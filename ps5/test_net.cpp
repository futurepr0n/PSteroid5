// Host tests for JSON, HTTP parsing and a live loopback request.
// Usage: test-net [http-fixture-port [https-self-signed-fixture-port]]
// PSTEROIDS_LIVE=<server line> adds read-only GETs against a real server.
#include "net_http.hpp"
#include "server_api.hpp"
#include "ship_design.hpp"
#include <cstdio>

using namespace cssteroids;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
    // JSON round trip, escapes and rejects
    {
        json::Value v;
        CHECK(json::parse(R"({"a":[1,2.5,-3e2],"s":"q\"\\é🚀","t":true,"n":null})", v));
        CHECK(v.get("a")->items.size() == 3 && v.get("a")->items[2].n == -300);
        CHECK(v.str("s") == "q\"\\\xc3\xa9\xf0\x9f\x9a\x80");
        CHECK(v.get("t")->b && v.get("n")->type == json::Value::Type::null);
        CHECK(json::dump(v) == "{\"a\":[1,2.5,-300],\"s\":\"q\\\"\\\\\xc3\xa9\xf0\x9f\x9a\x80\",\"t\":true,\"n\":null}");
        for (const char *bad : {"", "{", "[1,]", "{\"a\" 1}", "01x", "\"\x01\"", "[1] 2", "nul"}) CHECK(!json::parse(bad, v));
        std::string deep(200, '[');
        CHECK(!json::parse(deep + std::string(200, ']'), v));
    }
    // High score rows as Sequelize returns them; capped at 10
    {
        std::string body = "[";
        for (int i = 0; i < 12; i++) body += std::string(i ? "," : "") + "{\"id\":" + std::to_string(i) + ",\"name\":\"P" + std::to_string(i) +
            "\",\"score\":" + std::to_string(5000 - i) + ",\"shipType\":\"custom\",\"date\":\"2026-10-23T00:00:00.000Z\"}";
        body += "]";
        std::vector<api::ScoreRow> rows;
        CHECK(api::parse_high_scores(body, rows));
        CHECK(rows.size() == 10 && rows[0].name == "P0" && rows[0].score == 5000);
        CHECK(!api::parse_high_scores("{\"message\":\"db down\"}", rows));
        CHECK(api::high_score_body("Ace \"1\"", 1200, "default", "white") ==
              "{\"name\":\"Ace \\\"1\\\"\",\"score\":1200,\"shipType\":\"default\",\"shipColor\":\"white\"}");
    }
    // Ships as the server returns them: arrays or JSON-encoded string columns, CSS colours
    {
        CHECK(css_color("#ff0000") == 0xff0000 && css_color("#0f0") == 0x00ff00 && css_color("Magenta") == 0xff00ff);
        CHECK(css_color("rgb(1, 2, 3)") == 0x010203 && css_color("nonsense", 7) == 7 && css_color("#12", 7) == 7);
        const std::string body = R"([
          {"id":1,"name":"Spike","type":"custom","color":"magenta","passphrase":"phantom-talon-chasing","isPublic":true,
           "customLines":[{"startX":0,"startY":-60,"endX":40,"endY":40,"color":"#00ff00"},{"startX":"x","startY":0,"endX":1,"endY":1}],
           "thrusterPoints":[{"x":0,"y":40}],"weaponPoints":[{"x":-20,"y":-20},{"x":20,"y":-20}]},
          {"id":2,"name":"Legacy","type":"custom","color":"#00ffcc","passphrase":"red-star-flying",
           "customLines":"[{\"startX\":4,\"startY\":8,\"endX\":12,\"endY\":16}]","thrusterPoints":"[]","weaponPoints":null},
          {"id":3,"name":"Empty custom","type":"custom","color":"white","passphrase":"blue-moon-racing","customLines":[]},
          {"id":4,"name":"Tri","type":"triangle","color":"orange","passphrase":"swift-hawk-diving"}])";
        std::vector<ShipDesign> ships;
        CHECK(parse_ship_list(body, ships) && ships.size() == 4);
        const auto &s = ships[0].geometry;
        CHECK(s.custom() && s.lines.size() == 1 && s.lines[0].y1 == -15 && s.lines[0].x2 == 10 && s.lines[0].color == 0x00ff00);
        CHECK(s.weapons.size() == 2 && s.weapons[0].x == -5 && s.thrusters.size() == 1 && s.color == 0xff00ff);
        CHECK(ships[0].passphrase == "phantom-talon-chasing" && ships[0].fromServer());
        CHECK(ships[1].geometry.lines.size() == 1 && ships[1].geometry.lines[0].x2 == 3 && ships[1].geometry.lines[0].color == 0x00ffcc);
        CHECK(ships[2].geometry.shape == ShipShape::standard);
        CHECK(ships[3].geometry.shape == ShipShape::triangle && ships[3].geometry.color == 0xffa500);
        // Round trip through the local store keeps the design
        ShipStore store{"/tmp/psteroids5-test/ship.json"};
        CHECK(store.save(ships[0].source));
        ShipDesign back;
        CHECK(store.load(back) && back.name == "Spike" && back.geometry.lines.size() == 1 && back.geometry.weapons.size() == 2);
        CHECK(api::passphrase_path("phantom-talon-chasing") == "/api/ships/passphrase/phantom-talon-chasing");
        CHECK(api::passphrase_path("a b/../x") == "/api/ships/passphrase/a%20b%2F%2E%2E%2Fx");
        CHECK(api::high_score_body("P", 5, "custom", "magenta", "phantom-talon-chasing").find("\"shipPassphrase\":\"phantom-talon-chasing\"") != std::string::npos);
    }
    // Endpoint parsing
    {
        net::Endpoint e;
        CHECK(net::parse_endpoint("192.168.1.50:6161\r\n", e) && e.host == "192.168.1.50" && e.port == 6161 && !e.tls);
        CHECK(net::parse_endpoint("http://10.0.0.2:80/", e) && e.port == 80 && e.label() == "http://10.0.0.2");
        CHECK(net::parse_endpoint("10.0.0.3", e) && e.port == 6161);
        CHECK(net::parse_endpoint("https://asteroids.example.com", e) && e.tls && e.port == 443 && e.connectIp.empty());
        CHECK(e.host_header() == "asteroids.example.com" && e.label() == "https://asteroids.example.com");
        CHECK(net::parse_endpoint("https://asteroids.example.com/ 192.168.1.10", e) && e.connectIp == "192.168.1.10");
        CHECK(net::parse_endpoint("https://h.example:8443", e) && e.port == 8443 && e.host_header() == "h.example:8443");
        CHECK(!net::parse_endpoint("https://asteroids.example.com notanip", e));
        CHECK(!net::parse_endpoint("https://bad host", e));
        CHECK(!net::parse_endpoint("https://h.example/api", e));
        CHECK(!net::parse_endpoint("10.0.0.3:99999", e));
    }
    // Completion detection lets keep-alive proxies finish without waiting for a close
    {
        CHECK(net::response_complete("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]"));
        CHECK(!net::response_complete("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n[]"));
        CHECK(net::response_complete("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n"));
        CHECK(!net::response_complete("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n"));
        CHECK(!net::response_complete("HTTP/1.1 200 OK\r\n"));
    }
    // The bundled roots parse (ISRG Root X1 + X2)
    {
        mbedtls_x509_crt roots; mbedtls_x509_crt_init(&roots);
        CHECK(mbedtls_x509_crt_parse(&roots, reinterpret_cast<const unsigned char *>(certs::roots_pem), sizeof certs::roots_pem) == 0);
        int count = 0; for (auto *c = &roots; c && c->raw.len; c = c->next) count++;
        CHECK(count == 2);
        mbedtls_x509_crt_free(&roots);
    }
    // Response framing
    {
        net::Response r;
        CHECK(net::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]extra", r) && r.status == 200 && r.body == "[]");
        r = {};
        CHECK(net::parse_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\n[1,\r\n2\r\n2]\r\n0\r\n\r\n", r) && r.body == "[1,2]");
        r = {};
        CHECK(!net::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n[]", r));
        CHECK(!net::parse_response("garbage", r));
    }
    // Unreachable server fails quickly instead of hanging
    {
        net::Endpoint e; net::parse_endpoint("127.0.0.1:9", e);
        auto r = net::request(e, "GET", api::high_scores_path, {}, 1000);
        CHECK(r.status == 0 && !r.error.empty());
    }
    // Live loopback fixture: GET, chunked GET, POST echo, async worker
    if (argc > 1) {
        net::Endpoint e; CHECK(net::parse_endpoint(std::string("127.0.0.1:") + argv[1], e));
        auto r = net::request(e, "GET", api::high_scores_path);
        std::vector<api::ScoreRow> rows;
        CHECK(r.ok() && api::parse_high_scores(r.body, rows) && rows.size() == 3 && rows[0].name == "Fixture Ace");
        r = net::request(e, "GET", "/chunked");
        CHECK(r.ok() && api::parse_high_scores(r.body, rows) && rows.size() == 3);
        const std::string body = api::high_score_body("PS5", 4200, "default", "white");
        r = net::request(e, "POST", api::high_scores_path, body);
        CHECK(r.status == 201 && r.body == body);
        net::AsyncRequest async;
        CHECK(async.start(e, "GET", api::high_scores_path));
        net::Response got;
        for (int i = 0; i < 400 && !async.take(got); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(got.ok());
        std::printf("live fixture checks ran on port %s\n", argv[1]);
    }
    // HTTPS fixture with a self-signed certificate: the bundled roots must refuse it
    if (argc > 2) {
        net::Endpoint e;
        CHECK(net::parse_endpoint(std::string("https://localhost:") + argv[2] + " 127.0.0.1", e));
        auto r = net::request(e, "GET", api::high_scores_path);
        CHECK(r.status == 0 && r.error.find("certificate rejected") != std::string::npos);
        std::printf("untrusted certificate refused: %s\n", r.error.c_str());
    }
    // Optional, read-only checks against a real server: PSTEROIDS_LIVE=https://asteroids.example.com
    if (const char *live = std::getenv("PSTEROIDS_LIVE"); live && *live) {
        net::Endpoint e;
        CHECK(net::parse_endpoint(live, e));
        auto r = net::request(e, "GET", api::high_scores_path);
        std::vector<api::ScoreRow> rows;
        CHECK(r.ok() && api::parse_high_scores(r.body, rows));
        if (!r.ok()) std::printf("live GET failed: status %d %s\n", r.status, r.error.c_str());
        std::printf("live server %s: %zu high scores, top \"%s\" %d\n", e.label().c_str(), rows.size(),
                    rows.empty() ? "" : rows[0].name.c_str(), rows.empty() ? 0 : rows[0].score);
        // Public ship gallery, then one of those ships by its passphrase (both read-only)
        r = net::request(e, "GET", api::public_ships_path);
        std::vector<ShipDesign> ships;
        CHECK(r.ok() && parse_ship_list(r.body, ships));
        size_t custom = 0; for (auto &s : ships) custom += s.geometry.custom();
        std::printf("live public ships: %zu (%zu custom)\n", ships.size(), custom);
        for (const auto &s : ships) if (!s.passphrase.empty()) {
            auto one = net::request(e, "GET", api::passphrase_path(s.passphrase));
            json::Value v; ShipDesign d;
            CHECK(one.ok() && json::parse(one.body, v) && parse_ship(v, d) && d.name == s.name);
            std::printf("live passphrase lookup: \"%s\" -> %zu lines, %zu weapons\n", d.name.c_str(), d.geometry.lines.size(), d.geometry.weapons.size());
            break;
        }
        auto missing = net::request(e, "GET", api::passphrase_path("zz-no-such-ship"));
        CHECK(missing.status == 404);
        if (e.tls) {
            // Same address, wrong certificate name: verification must refuse it.
            sockaddr_in addr; std::string err;
            CHECK(net::resolve(e, addr, err));
            char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
            net::Endpoint wrong = e; wrong.host = "wrong-name.invalid"; wrong.connectIp = ip;
            auto bad = net::request(wrong, "GET", api::high_scores_path);
            CHECK(bad.status == 0 && bad.error.find("TLS") != std::string::npos);
            std::printf("wrong-name check: %s\n", bad.error.c_str());
        }
    }
    if (failures) { std::printf("%d net test failure(s)\n", failures); return 1; }
    std::puts("net tests passed");
    return 0;
}
