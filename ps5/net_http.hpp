#pragma once
// Minimal HTTP/1.1 client over BSD sockets for the CSSteroids server API, with
// optional TLS (mbedTLS, bundled Let's Encrypt roots) for the production HTTPS
// server. Builds unchanged on host Linux (tests) and the PS5 payload SDK.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> // error codes only; MBEDTLS_NET_C is off
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include "ca_bundle.hpp"

namespace cssteroids::net {
struct Endpoint {
    bool tls = false;
    std::string host;      // certificate/SNI/Host-header name, or a numeric IPv4
    uint16_t port = 0;
    std::string connectIp; // optional numeric override (LAN hairpin), else DNS
    bool valid() const { return port != 0 && !host.empty(); }
    std::string label() const {
        const bool default_port = port == (tls ? 443 : 80);
        return std::string(tls ? "https://" : "http://") + host + (default_port ? "" : ":" + std::to_string(port));
    }
    std::string host_header() const {
        return host + (port == (tls ? 443 : 80) ? "" : ":" + std::to_string(port));
    }
};

inline bool numeric_ipv4(const std::string &text) {
    in_addr addr{};
    return inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

inline bool valid_hostname(const std::string &host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-') return false;
    for (char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}

// Accepted forms (one per line in server.txt):
//   https://asteroids.example.com            TLS, port 443, DNS
//   https://asteroids.example.com 192.168.1.10   same, but connect to that address
//   http://192.168.1.50:6161                 plain HTTP (LAN development server)
//   192.168.1.50:6161 / 192.168.1.50         plain HTTP, port 6161 by default
inline bool parse_endpoint(std::string text, Endpoint &out) {
    auto trim = [](std::string &s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    };
    trim(text);
    Endpoint e;
    if (auto space = text.find_first_of(" \t"); space != std::string::npos) {
        e.connectIp = text.substr(space + 1);
        trim(e.connectIp);
        text.resize(space);
        if (!numeric_ipv4(e.connectIp)) return false;
    }
    long port = 6161;
    if (text.rfind("https://", 0) == 0) { e.tls = true; text.erase(0, 8); port = 443; }
    else if (text.rfind("http://", 0) == 0) { text.erase(0, 7); port = 80; }
    while (!text.empty() && text.back() == '/') text.pop_back();
    if (text.find('/') != std::string::npos) return false;
    e.host = text;
    if (auto colon = text.find(':'); colon != std::string::npos) {
        e.host = text.substr(0, colon);
        char *stop = nullptr;
        port = std::strtol(text.c_str() + colon + 1, &stop, 10);
        if (!stop || *stop || port <= 0 || port > 65535) return false;
    }
    if (!valid_hostname(e.host)) return false;
    e.port = uint16_t(port);
    out = e;
    return true;
}

// First valid, non-comment line of the first file that has one.
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

// True once a buffered response holds its whole body, so a keep-alive proxy
// that ignores "Connection: close" cannot stall the read until the timeout.
inline bool response_complete(const std::string &raw) {
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) return false;
    std::string head = raw.substr(0, head_end);
    for (auto &c : head) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    const size_t body = raw.size() - head_end - 4;
    if (head.find("transfer-encoding: chunked") != std::string::npos)
        return raw.size() >= 5 && raw.compare(raw.size() - 5, 5, "0\r\n\r\n") == 0;
    if (auto cl = head.find("content-length:"); cl != std::string::npos)
        return body >= std::strtoul(head.c_str() + cl + 15, nullptr, 10);
    const int status = std::atoi(raw.c_str() + raw.find(' ') + 1);
    return status == 204 || status == 304;
}

#if defined(__PROSPERO__)
// The SDK's static getaddrinfo is FreeBSD's, which wants /etc/resolv.conf; the
// console's own resolver (libSceNet) uses the system network settings.
extern "C" {
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int memid);
int sceNetResolverCreate(const char *name, int memid, int flags);
int sceNetResolverStartNtoa(int rid, const char *hostname, in_addr *addr, int timeout_us, int retries, int flags);
int sceNetResolverDestroy(int rid);
}
inline bool console_resolve(const std::string &host, in_addr &out, std::string &error) {
    const int pool = sceNetPoolCreate("psteroids5-dns", 4 * 1024, 0);
    if (pool < 0) { error = "resolver pool " + std::to_string(pool); return false; }
    const int rid = sceNetResolverCreate("psteroids5", pool, 0);
    int rc = rid;
    if (rid >= 0) {
        rc = sceNetResolverStartNtoa(rid, host.c_str(), &out, 5 * 1000 * 1000, 2, 0);
        sceNetResolverDestroy(rid);
    }
    sceNetPoolDestroy(pool);
    if (rc < 0) { char hex[16]; std::snprintf(hex, sizeof hex, "%08x", unsigned(rc)); error = std::string("console resolver 0x") + hex; }
    return rc >= 0;
}
#endif

inline bool resolve(const Endpoint &to, sockaddr_in &addr, std::string &error) {
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    const std::string &literal = to.connectIp.empty() ? to.host : to.connectIp;
    if (inet_pton(AF_INET, literal.c_str(), &addr.sin_addr) == 1) return true;
#if defined(__PROSPERO__)
    std::string consoleError;
    if (console_resolve(to.host, addr.sin_addr, consoleError)) return true;
#endif
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const int rc = getaddrinfo(to.host.c_str(), nullptr, &hints, &found);
    if (rc != 0 || !found) {
        error = "DNS lookup failed for " + to.host + " (getaddrinfo " + std::to_string(rc) + ")";
#if defined(__PROSPERO__)
        error += ", " + consoleError + "; add its IP after the address in server.txt";
#endif
        return false;
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr;
    freeaddrinfo(found);
    return true;
}

// The socket, optionally wrapped in a TLS session. All failures land in error.
class Connection {
    int fd = -1;
    bool tls = false, tlsReady = false;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;

    static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
        const ssize_t n = ::send(*static_cast<int *>(ctx), buf, len, 0);
        if (n >= 0) return int(n);
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
        const ssize_t n = ::recv(*static_cast<int *>(ctx), buf, len, 0);
        if (n >= 0) return int(n);
        // A socket timeout is "no data yet": mbedTLS keeps its state and the call can be retried.
        return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    static std::string tls_error(const char *what, int rc) {
        char text[160];
        mbedtls_strerror(rc, text, sizeof text);
        return std::string(what) + ": " + text;
    }
public:
    std::string error;
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection() {
        if (tlsReady) mbedtls_ssl_close_notify(&ssl);
        if (tls) {
            mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&roots);
            mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy);
        }
        if (fd >= 0) ::close(fd);
    }

    bool open(const Endpoint &to, int timeout_ms) {
        if (!to.valid()) { error = "no server configured"; return false; }
        sockaddr_in addr;
        if (!resolve(to, addr, error)) return false;
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { error = "socket() failed: " + std::to_string(errno); return false; }
        // Non-blocking connect bounded by poll, then blocking I/O with socket timeouts.
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
            if (errno != EINPROGRESS) { error = "connect failed: " + std::to_string(errno); return false; }
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, timeout_ms) != 1) { error = "connect timed out"; return false; }
            int err = 0; socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) { error = "connect refused: " + std::to_string(err); return false; }
        }
        fcntl(fd, F_SETFL, flags);
        set_timeout(timeout_ms);
        return !to.tls || handshake(to, timeout_ms);
    }

    // Per-call socket timeout; receive() returns -2 when it expires.
    void set_timeout(int timeout_ms) {
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    bool handshake(const Endpoint &to, int timeout_ms) {
        tls = true;
        mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&roots);
        mbedtls_ctr_drbg_init(&drbg); mbedtls_entropy_init(&entropy);
        static std::once_flag crypto_once;
        static psa_status_t crypto_status;
        std::call_once(crypto_once, [] { crypto_status = psa_crypto_init(); });
        if (crypto_status != PSA_SUCCESS) { error = "TLS: crypto init failed"; return false; }
        static const char personal[] = "psteroids5";
        int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const unsigned char *>(personal), sizeof personal);
        if (rc) { error = tls_error("TLS: random seed failed", rc); return false; }
        rc = mbedtls_x509_crt_parse(&roots, reinterpret_cast<const unsigned char *>(certs::roots_pem), sizeof certs::roots_pem);
        if (rc) { error = tls_error("TLS: bad bundled roots", rc); return false; }
        rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc) { error = tls_error("TLS: config failed", rc); return false; }
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &roots, nullptr);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        if ((rc = mbedtls_ssl_setup(&ssl, &conf)) || (rc = mbedtls_ssl_set_hostname(&ssl, to.host.c_str()))) {
            error = tls_error("TLS: setup failed", rc); return false;
        }
        mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (std::chrono::steady_clock::now() < deadline) continue;
                error = "TLS handshake timed out";
                return false;
            }
            // TLS 1.3 servers may send a session ticket mid-handshake; nothing to do with it.
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (const uint32_t flags = mbedtls_ssl_get_verify_result(&ssl); flags && flags != uint32_t(-1)) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
                for (auto &c : why) if (c == '\n') c = ' ';
                error = std::string("TLS: certificate rejected:") + why;
            } else error = tls_error("TLS handshake failed", rc);
            return false;
        }
        tlsReady = true;
        return true;
    }

    bool send_all(const std::string &data) {
        for (size_t sent = 0; sent < data.size();) {
            int n;
            if (tls) {
                n = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char *>(data.data() + sent), data.size() - sent);
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            } else n = int(::send(fd, data.data() + sent, data.size() - sent, 0));
            if (n <= 0) { error = "send failed"; return false; }
            sent += size_t(n);
        }
        return true;
    }

    // Appends what arrives: >0 bytes, 0 = peer closed, -1 = error (see error), -2 = timeout.
    int receive(std::string &into) {
        char buffer[4096];
        for (;;) {
            if (!tls) {
                const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
                if (n < 0) { error = "recv failed: " + std::to_string(errno); return -1; }
                into.append(buffer, size_t(n));
                return int(n);
            }
            const int n = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char *>(buffer), sizeof buffer);
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return -2;
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) return 0;
            if (n < 0) { error = tls_error("TLS read failed", n); return -1; }
            into.append(buffer, size_t(n));
            return n;
        }
    }
};

inline Response request(const Endpoint &to, const char *method, const std::string &path,
                        const std::string &json_body = {}, int timeout_ms = 6000) {
    Response out;
    Connection conn;
    if (!conn.open(to, timeout_ms)) { out.error = conn.error; return out; }
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + to.host_header() +
        "\r\nUser-Agent: PSteroids5/0.2\r\nAccept: application/json\r\nConnection: close\r\n";
    if (!json_body.empty() || std::strcmp(method, "POST") == 0)
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(json_body.size()) + "\r\n";
    req += "\r\n" + json_body;
    if (!conn.send_all(req)) { out.error = conn.error; return out; }
    std::string raw;
    for (;;) {
        const int n = conn.receive(raw);
        if (n == -2) { out.error = "response timed out"; return out; }
        if (n < 0) { out.error = conn.error; return out; }
        if (n == 0 || response_complete(raw)) break;
        if (raw.size() > (1u << 21)) { out.error = "response too large"; return out; }
    }
    if (!parse_response(raw, out)) out.status = 0;
    return out;
}

// One request on a worker thread; the render loop polls take() and never blocks.
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
