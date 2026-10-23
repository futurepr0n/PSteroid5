// Host tests for the WebSocket / socket.io client.
// Usage: test-sio [fixture-server-line]   e.g. http://127.0.0.1:41234 (tools/sio-fixture)
// PSTEROIDS_LIVE=<server line> adds a connect-only handshake with a real server (joins no world).
#include "socketio.hpp"
#include <cstdio>

using namespace cssteroids;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static bool wait_for(sio::Client &c, const std::string &name, sio::Event &out, int ms = 5000) {
    for (int waited = 0; waited < ms; waited += 5) {
        while (c.poll(out)) if (out.name == name) return true;
        if (c.state() == sio::Client::State::closed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
static bool wait_state(sio::Client &c, sio::Client::State s, int ms = 8000) {
    for (int waited = 0; waited < ms && c.state() != s; waited += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return c.state() == s;
}

int main(int argc, char **argv) {
    // RFC 6455 section 1.3 example
    CHECK(sio::detail::accept_for("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    // Client frames are masked; decoding them back yields the payload (7-, 16- and 64-bit lengths)
    for (size_t n : {size_t(5), size_t(125), size_t(126), size_t(70000)}) {
        std::string payload(n, 'a');
        for (size_t i = 0; i < n; i++) payload[i] = char('a' + i % 26);
        std::string wire = sio::detail::client_frame(1, payload, 0x12345678u);
        sio::detail::Frame f;
        std::string partial = wire.substr(0, wire.size() - 1);
        CHECK(sio::detail::take_frame(partial, f) == 0); // incomplete
        CHECK(sio::detail::take_frame(wire, f) == 1 && f.fin && f.opcode == 1 && f.payload == payload && wire.empty());
    }
    {   // Unmasked server frame and oversize rejection
        std::string wire = std::string("\x81\x05", 2) + "hello";
        sio::detail::Frame f;
        CHECK(sio::detail::take_frame(wire, f) == 1 && f.payload == "hello");
        std::string huge = std::string("\x81\x7f", 2) + std::string("\x00\x00\x00\x01\x00\x00\x00\x00", 8);
        CHECK(sio::detail::take_frame(huge, f) == -1);
    }
    {   // socket.io event packets
        sio::Event e;
        CHECK(sio::detail::parse_event(R"(2["mmo-state",{"t":5,"p":[]}])", e) && e.name == "mmo-state" && e.data.num("t") == 5);
        CHECK(sio::detail::parse_event(R"(2["mmo-ready"])", e) && e.name == "mmo-ready" && e.data.type == json::Value::Type::null);
        CHECK(sio::detail::parse_event(R"(212["ack",1])", e) && e.name == "ack");
        CHECK(!sio::detail::parse_event(R"(2[1,2])", e) && !sio::detail::parse_event("2{}", e));
        json::Value arr; arr.type = json::Value::Type::array;
        arr.items = {json::Value::number_value(1), json::Value::number_value(2.5)};
        CHECK(sio::detail::event_packet("mmo-player-update", &arr) == R"(42["mmo-player-update",[1,2.5]])");
        CHECK(sio::detail::event_packet("mmo-ready", nullptr) == R"(42["mmo-ready"])");
    }
    if (argc > 1) {
        net::Endpoint e;
        CHECK(net::parse_endpoint(argv[1], e));
        sio::Client c;
        c.connect(e);
        CHECK(wait_state(c, sio::Client::State::connected));
        sio::Event ev;
        CHECK(wait_for(c, "welcome", ev) && ev.data.str("sid") == c.sid() && !c.sid().empty());
        json::Value payload = json::Value::object();
        payload.set("text", json::Value::string_value("caf\xc3\xa9 \"quoted\""));
        payload.set("n", json::Value::number_value(-12.5));
        c.emit("echo", payload);
        CHECK(wait_for(c, "echo", ev) && json::dump(ev.data) == json::dump(payload));
        c.emit("big");
        CHECK(wait_for(c, "big", ev) && ev.data.s.size() == 300 * 1024);
        // The fixture pings every 300 ms with a 1.5 s timeout: still connected after 3 s means pongs work.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        CHECK(c.state() == sio::Client::State::connected);
        c.emit("echo", json::Value::number_value(7));
        CHECK(wait_for(c, "echo", ev) && ev.data.n == 7);
        c.emit("bye");
        CHECK(wait_state(c, sio::Client::State::closed, 3000));
        std::printf("fixture session ok; closed with: %s\n", c.error().c_str());
        // A refused port fails cleanly
        net::Endpoint dead; net::parse_endpoint("http://127.0.0.1:9", dead);
        sio::Client d; d.connect(dead);
        CHECK(wait_state(d, sio::Client::State::closed, 10000) && !d.error().empty());
    }
    if (const char *live = std::getenv("PSTEROIDS_LIVE"); live && *live) {
        net::Endpoint e;
        CHECK(net::parse_endpoint(live, e));
        sio::Client c;
        c.connect(e);
        CHECK(wait_state(c, sio::Client::State::connected));
        std::printf("live socket.io handshake %s: state %d sid %s %s\n", e.label().c_str(), int(c.state()), c.sid().c_str(), c.error().c_str());
        c.close();
    }
    if (failures) { std::printf("%d sio test failure(s)\n", failures); return 1; }
    std::puts("sio tests passed");
    return 0;
}
