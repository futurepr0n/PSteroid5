#pragma once
// Minimal HTTP/1.1 client over BSD sockets for the CSSteroids server API.
// Builds unchanged on host Linux (tests) and the PS5 payload SDK. Numeric IPv4
// only: the server address comes from server.txt, so no resolver is needed.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace cssteroids::net {
struct Endpoint {
    std::string host;
    uint16_t port = 0;
    bool valid() const { return port != 0; }
    std::string label() const { return host + ":" + std::to_string(port); }
};

// "192.168.1.50:6161", "http://192.168.1.50:6161/" or "192.168.1.50" (port 6161)
inline bool parse_endpoint(std::string text, Endpoint &out) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '/')) text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    if (text.rfind("http://", 0) == 0) text.erase(0, 7);
    std::string host = text;
    long port = 6161;
    if (auto colon = text.find(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        char *stop = nullptr;
        port = std::strtol(text.c_str() + colon + 1, &stop, 10);
        if (!stop || *stop || port <= 0 || port > 65535) return false;
    }
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return false;
    out.host = host;
    out.port = uint16_t(port);
    return true;
}

// First readable, valid line of the first file that exists.
inline Endpoint load_endpoint(std::initializer_list<const char *> paths) {
    for (const char *path : paths) {
        std::ifstream file(path);
        std::string line;
        while (file && std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            Endpoint e;
            if (parse_endpoint(line, e)) return e;
        }
    }
    return {};
}

struct Response {
    int status = 0;          // 0 = transport failure; see error
    std::string body, error;
    bool ok() const { return status >= 200 && status < 300; }
};

inline std::string url_encode(const std::string &text) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Splits a raw HTTP/1.1 response (status line, headers, Content-Length or chunked body).
inline bool parse_response(const std::string &raw, Response &out) {
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos || raw.rfind("HTTP/1.", 0) != 0) { out.error = "malformed response"; return false; }
    const size_t space = raw.find(' ');
    out.status = std::atoi(raw.c_str() + space + 1);
    std::string head = raw.substr(0, head_end);
    for (auto &c : head) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::string body = raw.substr(head_end + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t at = 0;
        for (;;) {
            const size_t line = body.find("\r\n", at);
            if (line == std::string::npos) { out.error = "truncated chunk"; return false; }
            const unsigned long size = std::strtoul(body.c_str() + at, nullptr, 16);
            at = line + 2;
            if (size == 0) break;
            if (body.size() < at + size + 2) { out.error = "truncated chunk"; return false; }
            decoded.append(body, at, size);
            at += size + 2;
        }
        out.body = std::move(decoded);
    } else if (auto cl = head.find("content-length:"); cl != std::string::npos) {
        const unsigned long length = std::strtoul(head.c_str() + cl + 15, nullptr, 10);
        if (body.size() < length) { out.error = "truncated body"; return false; }
        out.body = body.substr(0, length);
    } else {
        out.body = std::move(body); // Connection: close delimits it
    }
    return true;
}

inline Response request(const Endpoint &to, const char *method, const std::string &path,
                        const std::string &json_body = {}, int timeout_ms = 4000) {
    Response out;
    if (!to.valid()) { out.error = "no server configured"; return out; }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { out.error = "socket() failed: " + std::to_string(errno); return out; }
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    inet_pton(AF_INET, to.host.c_str(), &addr.sin_addr);
    // Non-blocking connect bounded by poll, then blocking I/O with socket timeouts.
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        if (errno != EINPROGRESS) { out.error = "connect failed: " + std::to_string(errno); return out; }
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) != 1) { out.error = "connect timed out"; return out; }
        int err = 0; socklen_t len = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { out.error = "connect refused: " + std::to_string(err); return out; }
    }
    fcntl(fd, F_SETFL, flags);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + to.label() +
        "\r\nUser-Agent: CSSteroids-PS5/0.1\r\nAccept: application/json\r\nConnection: close\r\n";
    if (!json_body.empty() || std::strcmp(method, "POST") == 0)
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(json_body.size()) + "\r\n";
    req += "\r\n" + json_body;
    for (size_t sent = 0; sent < req.size();) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { out.error = "send failed: " + std::to_string(errno); return out; }
        sent += size_t(n);
    }
    std::string raw;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n == 0) break;
        if (n < 0) { out.error = errno == EAGAIN || errno == EWOULDBLOCK ? "response timed out" : "recv failed: " + std::to_string(errno); return out; }
        raw.append(buffer, size_t(n));
        if (raw.size() > (1u << 20)) { out.error = "response too large"; return out; }
    }
    if (!parse_response(raw, out)) out.status = 0;
    return out;
}

// One request on a worker thread; the render loop polls ready() and never blocks.
class AsyncRequest {
    std::thread worker;
    std::atomic<bool> done{false}, busy{false};
    Response result;
public:
    ~AsyncRequest() { if (worker.joinable()) worker.join(); }
    bool start(Endpoint to, std::string method, std::string path, std::string body = {}) {
        if (busy) return false;
        if (worker.joinable()) worker.join();
        busy = true; done = false;
        worker = std::thread([this, to = std::move(to), method = std::move(method), path = std::move(path), body = std::move(body)] {
            result = request(to, method.c_str(), path, body);
            done = true;
        });
        return true;
    }
    bool pending() const { return busy && !done; }
    // True once, when a result is available; the worker has finished with it.
    bool take(Response &out) {
        if (!busy || !done) return false;
        worker.join();
        out = std::move(result);
        busy = false;
        return true;
    }
};
}
