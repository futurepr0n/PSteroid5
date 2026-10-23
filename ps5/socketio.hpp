#pragma once
// socket.io v4 client (Engine.IO v4 over a WebSocket, RFC 6455) on net::Connection,
// so it works over plain HTTP and TLS alike. One thread owns the socket; the game
// queues emits and polls received events without blocking.
#include "json_lite.hpp"
#include "net_http.hpp"
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace cssteroids::sio {
struct Event {
    std::string name;
    json::Value data; // first argument (null when the event has none)
};

namespace detail {
// Masking keys and the handshake nonce only need to be unpredictable to
// intermediaries (RFC 6455 10.3), not secret.
struct Noise {
    uint64_t s;
    Noise() : s(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ uint64_t(uintptr_t(this)) ^ 0x9e3779b97f4a7c15ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
};

inline std::string base64(const unsigned char *data, size_t len) {
    unsigned char out[64];
    size_t written = 0;
    mbedtls_base64_encode(out, sizeof out, &written, data, len);
    return std::string(reinterpret_cast<char *>(out), written);
}

// Sec-WebSocket-Accept for a key: base64(SHA-1(key + GUID)).
inline std::string accept_for(const std::string &key) {
    const std::string joined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char *>(joined.data()), joined.size(), digest);
    return base64(digest, sizeof digest);
}

// A client frame: FIN, opcode, masked payload with 7/16/64-bit length.
inline std::string client_frame(int opcode, const std::string &payload, uint32_t mask) {
    std::string f;
    f += char(0x80 | opcode);
    const size_t n = payload.size();
    if (n < 126) f += char(0x80 | n);
    else if (n < 65536) { f += char(0x80 | 126); f += char(n >> 8); f += char(n & 0xff); }
    else { f += char(0x80 | 127); for (int i = 7; i >= 0; i--) f += char((uint64_t(n) >> (i * 8)) & 0xff); }
    const unsigned char key[4] = {uint8_t(mask >> 24), uint8_t(mask >> 16), uint8_t(mask >> 8), uint8_t(mask)};
    f.append(reinterpret_cast<const char *>(key), 4);
    for (size_t i = 0; i < n; i++) f += char(payload[i] ^ key[i & 3]);
    return f;
}

struct Frame { bool fin; int opcode; std::string payload; };

// Takes one complete frame off the front of buf. 0 = need more data, -1 = protocol error.
inline int take_frame(std::string &buf, Frame &out, size_t max_payload = 4u << 20) {
    if (buf.size() < 2) return 0;
    const auto b = [&](size_t i) { return uint8_t(buf[i]); };
    const bool masked = b(1) & 0x80;
    uint64_t len = b(1) & 0x7f;
    size_t at = 2;
    if (len == 126) { if (buf.size() < 4) return 0; len = uint64_t(b(2)) << 8 | b(3); at = 4; }
    else if (len == 127) { if (buf.size() < 10) return 0; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | b(2 + i); at = 10; }
    if (len > max_payload) return -1;
    unsigned char key[4] = {};
    if (masked) { if (buf.size() < at + 4) return 0; for (int i = 0; i < 4; i++) key[i] = b(at + i); at += 4; }
    if (buf.size() < at + len) return 0;
    out.fin = b(0) & 0x80;
    out.opcode = b(0) & 0x0f;
    out.payload.assign(buf, at, size_t(len));
    if (masked) for (size_t i = 0; i < out.payload.size(); i++) out.payload[i] = char(out.payload[i] ^ key[i & 3]);
    buf.erase(0, at + size_t(len));
    return 1;
}

// socket.io event packet "42[...]" -> name + first argument. Acks/namespaces are not used.
inline bool parse_event(const std::string &packet, Event &out) {
    if (packet.size() < 2 || packet[0] != '2') return false;
    size_t at = 1;
    while (at < packet.size() && packet[at] >= '0' && packet[at] <= '9') at++; // ack id
    json::Value arr;
    if (!json::parse(packet.substr(at), arr) || !arr.is_array() || arr.items.empty() || arr.items[0].type != json::Value::Type::string) return false;
    out.name = arr.items[0].s;
    out.data = arr.items.size() > 1 ? std::move(arr.items[1]) : json::Value{};
    return true;
}

inline std::string event_packet(const std::string &name, const json::Value *data) {
    std::string out = "42[";
    json::write_string(out, name);
    if (data) { out += ','; json::write(out, *data); }
    out += ']';
    return out;
}
}

class Client {
public:
    enum class State { idle, connecting, connected, closed };

    ~Client() { close(); }

    // Starts connecting on a worker thread; poll state() / events.
    void connect(net::Endpoint to, std::string path = "/socket.io/") {
        close();
        stopping = false;
        set_state(State::connecting, {});
        worker = std::thread([this, to = std::move(to), path = std::move(path)] { run(to, path); });
    }
    void close() {
        stopping = true;
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        outbox.clear();
        if (state_ != State::idle) state_ = State::closed;
    }
    void emit(const std::string &name, const json::Value &data) { queue(detail::event_packet(name, &data)); }
    void emit(const std::string &name) { queue(detail::event_packet(name, nullptr)); }
    bool poll(Event &out) {
        std::lock_guard<std::mutex> lock(mutex);
        if (inbox.empty()) return false;
        out = std::move(inbox.front());
        inbox.pop_front();
        return true;
    }
    State state() const { std::lock_guard<std::mutex> lock(mutex); return state_; }
    std::string error() const { std::lock_guard<std::mutex> lock(mutex); return error_; }
    std::string sid() const { std::lock_guard<std::mutex> lock(mutex); return sid_; }

private:
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::deque<std::string> outbox;   // complete Engine.IO messages ("42[...]")
    std::deque<Event> inbox;
    State state_ = State::idle;
    std::string error_, sid_;
    static constexpr size_t inbox_limit = 4096;

    void set_state(State s, std::string why) {
        std::lock_guard<std::mutex> lock(mutex);
        state_ = s;
        if (!why.empty()) error_ = std::move(why);
    }
    void queue(std::string packet) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state_ == State::connecting || state_ == State::connected) outbox.push_back(std::move(packet));
    }
    void deliver(Event e) {
        std::lock_guard<std::mutex> lock(mutex);
        if (inbox.size() >= inbox_limit) inbox.pop_front(); // a stalled game loop must not grow memory forever
        inbox.push_back(std::move(e));
    }

    void run(const net::Endpoint &to, const std::string &path) {
        net::Connection conn;
        detail::Noise noise;
        if (!conn.open(to, 8000)) { set_state(State::closed, conn.error); return; }
        unsigned char nonce[16];
        for (auto &c : nonce) c = uint8_t(noise.next());
        const std::string key = detail::base64(nonce, sizeof nonce);
        const std::string upgrade = "GET " + path + "?EIO=4&transport=websocket HTTP/1.1\r\nHost: " + to.host_header() +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
            "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: PSteroids5/0.3\r\n\r\n";
        if (!conn.send_all(upgrade)) { set_state(State::closed, conn.error); return; }
        std::string buf;
        size_t head_end;
        while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
            const int n = conn.receive(buf);
            if (n <= 0 || buf.size() > 16384) { set_state(State::closed, n == -2 ? "upgrade timed out" : n == 0 ? "server closed during upgrade" : conn.error); return; }
        }
        std::string head = buf.substr(0, head_end);
        buf.erase(0, head_end + 4);
        std::string lower = head;
        for (auto &c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (head.rfind("HTTP/1.1 101", 0) != 0) { set_state(State::closed, "upgrade refused: " + head.substr(0, head.find("\r\n"))); return; }
        if (lower.find("sec-websocket-accept: " + [&] { std::string a = detail::accept_for(key); for (auto &c : a) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return a; }()) == std::string::npos) {
            set_state(State::closed, "bad Sec-WebSocket-Accept"); return;
        }
        // Short receive waits keep queued emits from sitting behind a blocked read: position
        // updates must leave once per 50 ms server tick, not in clumps (console test 2026-10-23).
        conn.set_timeout(3);
        std::string message;      // reassembled fragments
        bool namespaceOpen = false;
        auto lastHeard = std::chrono::steady_clock::now();
        auto pingTimeout = std::chrono::milliseconds(25000 + 20000);
        auto send_text = [&](const std::string &text) { return conn.send_all(detail::client_frame(1, text, noise.next())); };
        while (!stopping) {
            // Outgoing packets wait until the namespace is joined ("40" acknowledged).
            if (namespaceOpen) {
                std::deque<std::string> pending;
                { std::lock_guard<std::mutex> lock(mutex); pending.swap(outbox); }
                for (const auto &packet : pending)
                    if (!send_text(packet)) { set_state(State::closed, conn.error); return; }
            }
            const int n = conn.receive(buf);
            if (n == 0) { set_state(State::closed, "server closed the connection"); return; }
            if (n == -1) { set_state(State::closed, conn.error); return; }
            if (std::chrono::steady_clock::now() - lastHeard > pingTimeout) { set_state(State::closed, "server stopped responding"); return; }
            detail::Frame frame;
            int got;
            while ((got = detail::take_frame(buf, frame)) == 1) {
                lastHeard = std::chrono::steady_clock::now();
                if (frame.opcode == 8) { conn.send_all(detail::client_frame(8, frame.payload.substr(0, 2), noise.next())); set_state(State::closed, "server closed the socket"); return; }
                if (frame.opcode == 9) { conn.send_all(detail::client_frame(10, frame.payload, noise.next())); continue; }
                if (frame.opcode == 10) continue;
                if (frame.opcode != 0) message.clear();
                message += frame.payload;
                if (!frame.fin) continue;
                const std::string text = std::move(message);
                message.clear();
                if (text.empty()) continue;
                switch (text[0]) {
                    case '0': { // Engine.IO open: {"sid","pingInterval","pingTimeout",...}
                        json::Value open;
                        if (json::parse(text.substr(1), open) && open.is_object())
                            pingTimeout = std::chrono::milliseconds(int64_t(open.num("pingInterval", 25000) + open.num("pingTimeout", 20000)));
                        if (!send_text("40")) { set_state(State::closed, conn.error); return; }
                        break;
                    }
                    case '2': if (!send_text("3" + text.substr(1))) { set_state(State::closed, conn.error); return; } break; // ping -> pong
                    case '1': set_state(State::closed, "server closed the session"); return;
                    case '4': {
                        const std::string packet = text.substr(1);
                        if (packet.empty()) break;
                        if (packet[0] == '0') { // namespace connected: 0{"sid":...}
                            json::Value ack;
                            std::lock_guard<std::mutex> lock(mutex);
                            if (json::parse(packet.substr(1), ack) && ack.is_object()) sid_ = ack.str("sid");
                            state_ = State::connected;
                            namespaceOpen = true;
                        } else if (packet[0] == '4') { // connect_error
                            json::Value why;
                            set_state(State::closed, json::parse(packet.substr(1), why) && why.is_object() ? "refused: " + why.str("message") : "namespace refused");
                            return;
                        } else if (packet[0] == '1') {
                            set_state(State::closed, "disconnected by server"); return;
                        } else if (Event e; detail::parse_event(packet, e)) deliver(std::move(e));
                        break;
                    }
                    default: break;
                }
            }
            if (got < 0) { set_state(State::closed, "bad WebSocket frame"); return; }
        }
        if (namespaceOpen) send_text("41");
        conn.send_all(detail::client_frame(8, std::string("\x03\xe8", 2), noise.next()));
    }
};
}
