// Host tests for JSON, HTTP parsing and a live loopback request.
// Usage: test-net [port-of-fixture_server.py]
#include "net_http.hpp"
#include "server_api.hpp"
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
    // Endpoint parsing
    {
        net::Endpoint e;
        CHECK(net::parse_endpoint("192.168.1.50:6161\r\n", e) && e.host == "192.168.1.50" && e.port == 6161);
        CHECK(net::parse_endpoint("http://10.0.0.2:80/", e) && e.port == 80);
        CHECK(net::parse_endpoint("10.0.0.3", e) && e.port == 6161);
        CHECK(!net::parse_endpoint("localhost:6161", e));
        CHECK(!net::parse_endpoint("10.0.0.3:99999", e));
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
    if (failures) { std::printf("%d net test failure(s)\n", failures); return 1; }
    std::puts("net tests passed");
    return 0;
}
