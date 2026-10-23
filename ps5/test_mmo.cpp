// Host tests for the shared-world client.
// Usage: test-mmo <testdata/world_vectors.json> [http://host:port of tools/sio-fixture/mmo-server.js]
#include "coop_lobby.hpp"
#include "mmo_world.hpp"
#include "socketio.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace cssteroids;
using namespace cssteroids::mmo;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static AsteroidRecord rec(const json::Value &v) {
    AsteroidRecord a;
    a.id = int64_t(v.num("id")); a.x = v.num("x"); a.y = v.num("y"); a.vx = v.num("vx"); a.vy = v.num("vy");
    a.size = int(v.num("size")); a.radius = v.num("radius"); a.rotation = v.num("rotation"); a.rotationSpeed = v.num("rotationSpeed");
    a.seed = uint32_t(int64_t(v.num("seed"))); a.tick = int64_t(v.num("spawnTick"));
    build_shape(a);
    return a;
}
// ids, seeds and sizes must be exact. Positions/velocities may differ in the last bit after
// a split: glibc's cos/sin/atan2 and V8's fdlibm can round differently (seen: 2.2e-16 in vx).
// The server's mmo-asteroid-check corrects anything beyond 1.5 px, so 1e-9 is a strict bound.
static bool same(const AsteroidRecord &a, const json::Value &v, double tol = 1e-9) {
    auto near = [&](double x, double y) { return std::fabs(x - y) <= tol; };
    return a.id == int64_t(v.num("id")) && near(a.x, v.num("x")) && near(a.y, v.num("y")) && near(a.vx, v.num("vx")) && near(a.vy, v.num("vy")) &&
           near(a.rotation, v.num("rotation")) && near(a.rotationSpeed, v.num("rotationSpeed")) && a.seed == uint32_t(int64_t(v.num("seed"))) &&
           a.size == int(v.num("size")) && a.radius == v.num("radius");
}
static json::Value parse_or_die(const std::string &text) { json::Value v; if (!json::parse(text, v)) { std::puts("bad json"); std::exit(2); } return v; }
static json::Value arr(std::initializer_list<double> xs) { json::Value a; a.type = json::Value::Type::array; for (double x : xs) a.items.push_back(json::Value::number_value(x)); return a; }

static void golden(const char *path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    json::Value v = parse_or_die(ss.str());
    Bounds b{v.get("bounds")->num("width"), v.get("bounds")->num("height")};
    int trees = 0;
    for (const auto &t : v.get("asteroids")->items) {
        AsteroidRecord a = rec(*t.get("start"));
        // shape (mulberry32 in doubles)
        CHECK(a.vertices == int(t.get("shape")->num("vertices")));
        for (int i = 0; i < a.vertices; i++) CHECK(a.offsets[i] == t.get("shape")->get("offsets")->items[size_t(i)].n);
        for (int i = 0; i < 250; i++) step_asteroid(a, b);
        CHECK(same(a, *t.get("stepped"), 0)); // stepping is plain arithmetic: bit-exact
        auto kids = split_asteroid(a, 1250);
        CHECK(kids.size() == t.get("children")->items.size());
        for (size_t k = 0; k < kids.size(); k++) {
            CHECK(same(kids[k], t.get("children")->items[k]));
            CHECK(kids[k].tick == 1250);
            CHECK(kids[k].vertices == int(t.get("childShape")->items[k].num("vertices")));
        }
        if (kids.size() == 2) {
            AsteroidRecord c = kids[1];
            for (int i = 0; i < 40; i++) step_asteroid(c, b);
            auto grand = split_asteroid(c, 1290);
            CHECK(grand.size() == t.get("grandchildren")->items.size());
            for (size_t k = 0; k < grand.size(); k++) CHECK(same(grand[k], t.get("grandchildren")->items[k]));
        }
        trees++;
    }
    for (const auto &bt : v.get("bullets")->items) {
        const auto *shot = bt.get("shot");
        std::vector<Point> weapons;
        for (const auto &w : bt.get("weapons")->items) weapons.push_back({float(w.num("x") * 0.25), float(w.num("y") * 0.25)});
        auto made = create_shot_bullets(shot->num("x"), shot->num("y"), shot->num("angle"), shot->num("vx"), shot->num("vy"), weapons);
        CHECK(made.size() == bt.get("created")->items.size());
        for (size_t i = 0; i < made.size(); i++) {
            const auto &c = bt.get("created")->items[i];
            CHECK(made[i].x == c.num("x") && made[i].y == c.num("y") && made[i].vx == c.num("vx") && made[i].vy == c.num("vy"));
            CHECK(made[i].speed == c.num("speed") && made[i].maxLife == c.num("maxLife"));
            mmo::Bullet s = made[i];
            for (int f2 = 0; f2 < 30; f2++) step_bullet_frame(s, 1.0 / 60);
            const auto &st = bt.get("stepped")->items[i];
            CHECK(s.x == st.num("x") && s.y == st.num("y") && s.speed == st.num("speed"));
        }
    }
    {
        std::vector<ClockSample> samples;
        for (const auto &s : v.get("clock")->get("samples")->items) samples.push_back({s.num("sentAt"), s.num("receivedAt"), s.num("serverTime")});
        double off, rtt;
        CHECK(estimate_clock(samples, off, rtt));
        CHECK(off == v.get("clock")->get("result")->num("offsetMs") && rtt == v.get("clock")->get("result")->num("rttMs"));
    }
    {
        std::vector<ScoreEntry> board;
        for (const auto &step : v.get("scores")->items) {
            merge_scores(board, *step.get("msg"));
            const auto &want = step.get("board")->items;
            CHECK(board.size() == want.size());
            for (size_t i = 0; i < board.size() && i < want.size(); i++) CHECK(board[i].id == want[i].str("id") && board[i].score == int(want[i].num("score")));
        }
    }
    std::printf("golden vectors: %d asteroid trees match world-sim.js (stepping bit-exact)\n", trees);
}

static void offline() {
    ShipDesign ship;
    ship.name = "Spike"; ship.typeName = "custom"; ship.colorName = "#00ff00";
    ship.geometry.shape = ShipShape::custom;
    ship.geometry.lines = {{0, -15, 10, 10, 0x00ff00}};
    ship.geometry.weapons = {{-5, -5}, {5, -5}};
    Session s;
    s.begin_join(0, "PS5 Tester", ship);
    auto out = s.take_outbox();
    CHECK(out.size() == 1 && out[0].name == "mmo-join");
    const auto &j = out[0].data;
    CHECK(j.str("sessionId") == "mmo_world" && j.num("protocol") == 4 && j.get("viewport")->num("w") == 1920);
    const auto *sd = j.get("playerData")->get("shipData");
    CHECK(j.get("playerData")->str("name") == "PS5 Tester" && sd->str("shipType") == "custom");
    CHECK(sd->get("customLines")->items.size() == 1 && sd->get("customLines")->items[0].num("startY") == -60);
    CHECK(sd->get("weaponPoints")->items.size() == 2 && sd->get("weaponPoints")->items[1].num("x") == 20);
    // join success with one asteroid at spawn tick 100 and another player
    s.handle("mmo-join-success", parse_or_die(R"({"player":{"id":"me","idx":0,"x":1000,"y":700,"angle":0},"mode":"mmo","round":0,"lives":0,
        "worldBounds":{"width":2400,"height":1800},"asteroids":[{"id":8,"x":300,"y":300,"vx":1,"vy":0,"size":3,"radius":40,"rotation":0,"rotationSpeed":0,"seed":5,"spawnTick":100}],
        "enemies":[],"players":[{"id":"me","name":"PS5 Tester","x":1000,"y":700,"angle":0},{"id":"bob","name":"Bob","x":1200,"y":700,"angle":1,"shipData":{"shipType":"triangle","shipColor":"red"}}],
        "tick":100,"serverTime":5000})"), 1000);
    CHECK(s.phase == Phase::warping && s.myId == "me" && s.bounds.width == 2400 && s.players.size() == 1 && s.players["bob"].geometry.shape == ShipShape::triangle);
    // time sync replies: offset = serverTime - midpoint
    double now = 1000;
    for (int i = 0; i < 5; i++) { s.update({}, now, 0.1); now += 100; }
    out = s.take_outbox();
    int syncs = 0; for (auto &o : out) syncs += o.name == "mmo-time-sync";
    CHECK(syncs == 5);
    for (int i = 0; i < 5; i++) {
        json::Value r = json::Value::object();
        r.set("sentAt", json::Value::number_value(1000 + i * 100)); r.set("serverTime", json::Value::number_value(5000 + i * 100 + 20));
        s.handle("mmo-time-sync", r, 1000 + i * 100 + 40);
    }
    s.handle("mmo-state", parse_or_die(R"({"t":110,"p":[["bob",1210,700,1,false,1]]})"), now);
    s.handle("mmo-state", parse_or_die(R"({"t":111,"p":[["bob",1220,710,1.2,false,1]]})"), now);
    for (int i = 0; i < 12; i++) { s.update({}, now, 0.1); now += 100; } // warp lasts at least 1.5 s
    CHECK(s.clock_known() && std::fabs(s.server_tick(now) - (now + 4000) / 50) < 1e-6);
    CHECK(s.phase == Phase::playing);
    out = s.take_outbox();
    bool ready = false; int updates = 0;
    for (auto &o : out) { ready |= o.name == "mmo-ready"; if (o.name == "mmo-player-update") { updates++; CHECK(o.data.items.size() == 6 && o.data.items[0].n == 1000); } }
    CHECK(ready && updates >= 1 && updates <= 2); // unchanged pose: heartbeat only
    // thrusting changes the pose every 50 ms
    Controls go; go.thrust = true;
    for (int i = 0; i < 20; i++) { s.update(go, now, 0.05); now += 50; }
    out = s.take_outbox(); updates = 0;
    for (auto &o : out) if (o.name == "mmo-player-update") { updates++; CHECK(o.data.items[5].n == 1); }
    CHECK(updates >= 15 && s.y < 700);
    // two weapon points: one mmo-shoot per trigger, two local rounds, cooldown 0.25 s
    Controls fire; fire.fire = true;
    const size_t before = s.bullets.size();
    for (int i = 0; i < 6; i++) { s.update(fire, now, 0.05); now += 50; }
    out = s.take_outbox();
    int shots = 0; for (auto &o : out) if (o.name == "mmo-shoot") { shots++; CHECK(o.data.get("tick") && o.data.num("angle") == s.angle); }
    CHECK(shots == 2 && s.bullets.size() >= before + 2);
    // remote player is interpolated from the buffer (old-format entries: no velocity, so it holds)
    CHECK(std::fabs(s.players["bob"].x - 1220) < 1e-9);
    {   // velocity entries glide past the newest sample, at most max_extrapolate_ticks, as world-sim.js
        SnapshotBuffer buf(100, 1);
        buf.push(10, "a", 0, 0, 0, false, true, 2, -1);
        buf.push(11, "a", 6, -3, 0, false, true, 2, -1);
        Sample g;
        CHECK(buf.sample("a", 11.5, g) && g.x == 6 + 2 * 3 * 0.5 && g.y == -3 - 3 * 0.5);
        CHECK(buf.sample("a", 20, g) && g.x == 6 + 2 * 3 * max_extrapolate_ticks);
        buf.push(30, "a", 6, -3, 0, false, false, 2, -1); // the gap inserts a held sample with no velocity
        CHECK(buf.sample("a", 25, g) && g.x == 6);
        SnapshotBuffer dead(100, 1);
        dead.push(10, "d", 5, 5, 0, true, false, 3, 3);
        CHECK(dead.sample("d", 11, g) && g.x == 5);
    }
    // asteroid destroyed at tick 150: two children with world-sim ids/ticks
    s.handle("mmo-asteroid-destroyed", arr({8, 150, 1}), now);
    CHECK(s.asteroids.count(8) == 0 && s.asteroids.count(child_id(8, 0)) == 1 && s.asteroids.count(child_id(8, 1)) == 1);
    CHECK(s.asteroids[child_id(8, 0)].size == 2);
    // scores merge, enemy spawn/shoot, respawn
    s.handle("mmo-scores", parse_or_die(R"({"full":true,"players":[["me","PS5 Tester",300],["bob","Bob",900]]})"), now);
    CHECK(s.scores.size() == 2 && s.scores[0].id == "bob");
    s.handle("mmo-enemy-spawn", arr({41, 1500, 1500, 0, 0}), now);
    CHECK(s.enemies.count("41") == 1);
    // an enemy round fired straight at us kills us and reports enemyBullet with the numeric id
    s.invulnerableMs = 0; s.vx = s.vy = 0;
    json::Value shot = json::Value::object();
    shot.set("enemyId", json::Value::number_value(41)); shot.set("x", json::Value::number_value(s.x - 60));
    shot.set("y", json::Value::number_value(s.y)); shot.set("angle", json::Value::number_value(0)); // math 0 = right
    s.handle("mmo-enemy-shoot", shot, now);
    for (int i = 0; i < 10 && !s.dead; i++) { s.update({}, now, 1.0 / 60); now += 1000.0 / 60; }
    CHECK(s.dead);
    out = s.take_outbox();
    bool reported = false;
    for (auto &o : out) if (o.name == "mmo-player-died") { reported = true; CHECK(o.data.str("type") == "enemyBullet" && o.data.num("objectId") == 41); }
    CHECK(reported);
    s.handle("mmo-respawn", parse_or_die(R"({"x":900,"y":800,"angle":0.5})"), now);
    CHECK(!s.dead && s.x == 900 && s.invulnerableMs > 0);
    std::puts("offline session checks done");
}

static void live(const char *server) {
    net::Endpoint e;
    CHECK(net::parse_endpoint(server, e));
    sio::Client c;
    c.connect(e);
    for (int i = 0; i < 800 && c.state() == sio::Client::State::connecting; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(c.state() == sio::Client::State::connected);
    ShipDesign ship; ship.name = "Fixture"; ship.typeName = "default"; ship.colorName = "white";
    Session s;
    json::Value lastStats;
    std::map<int64_t, AsteroidRecord> atStats;
    double poseAtStats[2] = {0, 0}; // our pose when the server snapshot arrived (the ship keeps coasting)
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    auto pump = [&](const Controls &in, double seconds) {
        const double end = now() + seconds * 1000;
        double last = now();
        while (now() < end) {
            sio::Event ev;
            while (c.poll(ev)) {
                if (ev.name == "fixture-stats") { lastStats = ev.data; atStats = s.asteroids; poseAtStats[0] = s.x; poseAtStats[1] = s.y; }
                else s.handle(ev.name, ev.data, now());
            }
            const double t = now();
            s.update(in, t, (t - last) / 1000);
            last = t;
            for (auto &o : s.take_outbox()) { if (o.hasData) c.emit(o.name, o.data); else c.emit(o.name); }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };
    s.begin_join(now(), "PSteroids5 test", ship);
    pump({}, 4);
    CHECK(s.phase == Phase::playing && s.clock_known());
    std::printf("live: joined as %s idx %d, world %.0fx%.0f, %zu asteroids, rtt %.1f ms\n", s.myId.c_str(), s.myIdx, s.bounds.width, s.bounds.height, s.asteroids.size(), s.rtt());
    // Fly a little; the server's copy of our ship must agree
    Controls go; go.thrust = true; go.turn = 0.2f;
    pump(go, 1.5);
    pump({}, 0.6);
    c.emit("fixture-stats");
    pump({}, 0.4);
    CHECK(lastStats.is_object() && lastStats.get("player"));
    if (lastStats.is_object() && lastStats.get("player") && lastStats.get("player")->is_object()) {
        const auto *p = lastStats.get("player");
        CHECK(!p->get("warping")->b);
        const double dx = p->num("x") - poseAtStats[0], dy = p->num("y") - poseAtStats[1];
        std::printf("live: server pose (%.0f, %.0f) vs ours when it arrived (%.1f, %.1f)\n", p->num("x"), p->num("y"), poseAtStats[0], poseAtStats[1]);
        CHECK(std::hypot(dx, dy) < 30);
        // asteroid records agree with the server at its tick
        int compared = 0, close = 0;
        const int64_t tick = int64_t(lastStats.num("tick"));
        for (const auto &a : lastStats.get("asteroids")->items) {
            auto it = atStats.find(int64_t(a.num("id")));
            if (it == atStats.end()) continue;
            AsteroidRecord r = it->second;
            if (r.tick > tick) continue;
            advance_to(r, tick, s.bounds);
            compared++;
            if (std::hypot(r.x - a.num("x"), r.y - a.num("y")) <= 1.5) close++;
        }
        std::printf("live: %d/%d asteroid records within 1.5 px of the server\n", close, compared);
        CHECK(compared > 0 && close == compared);
    }
    // Aim at the nearest asteroid and shoot; the server sweep must award points
    int myScore = 0;
    for (int attempt = 0; attempt < 12 && myScore == 0; attempt++) {
        const AsteroidRecord *target = nullptr; double best = 1e18;
        for (auto &[id, a] : s.asteroids) { const double d = std::hypot(a.x - s.x, a.y - s.y); if (d < best) { best = d; target = &a; } }
        if (!target) break;
        Controls aim; aim.fire = true;
        const double want = std::atan2(target->x - s.x, -(target->y - s.y)); // game convention
        s.angle = want; // snap the aim for the test
        aim.thrust = best > 350;
        pump(aim, 1.0);
        for (auto &e2 : s.scores) if (e2.id == s.myId) myScore = e2.score;
        if (s.dead) pump({}, 3.5);
    }
    std::printf("live: server score after shooting: %d (shots sent %d)\n", myScore, s.shotsSent);
    CHECK(myScore > 0);
    s.leave();
    for (auto &o : s.take_outbox()) c.emit(o.name);
    pump({}, 0.3);
    c.close();
}

// Flies for `seconds` and reports the share of server ticks that got a fresh position from us
// (what other players' interpolation needs). Run the fixture with JITTER_MS to add network jitter.
static void fresh_bench(const char *server, double phase, bool ignoreRtt, double seconds) {
    net::Endpoint e; net::parse_endpoint(server, e);
    sio::Client c; c.connect(e);
    for (int i = 0; i < 800 && c.state() == sio::Client::State::connecting; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ShipDesign ship; ship.typeName = "default"; ship.colorName = "white";
    Session s;
    json::Value stats;
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    auto pump = [&](const Controls &in, double secs) {
        const double end = now() + secs * 1000; double last = now();
        while (now() < end) {
            sio::Event ev;
            while (c.poll(ev)) { if (ev.name == "fixture-stats") stats = ev.data; else s.handle(ev.name, ev.data, now()); }
            const double t = now();
            s.update(in, t, (t - last) / 1000); last = t;
            for (auto &o : s.take_outbox()) { if (o.hasData) c.emit(o.name, o.data); else c.emit(o.name); }
            // render-loop cadence: 60 Hz frames
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::microseconds(16667));
        }
    };
    s.begin_join(now(), "fresh bench", ship);
    s.sendPhase = phase;
    pump({}, 3.5);
    if (ignoreRtt) s.sendPhase = phase; // the phase alone; one-way delay still applies unless rtt is unknown
    c.emit("fixture-reset-fresh");
    Controls circle; circle.thrust = true; circle.turn = 0.25f;  // keeps moving inside the world
    pump(circle, seconds);
    c.emit("fixture-stats");
    pump({}, 0.5);
    const auto *f = stats.is_object() ? stats.get("fresh") : nullptr;
    if (f && f->is_object()) std::printf("fresh-bench phase %.2f: %d of %d server ticks had a new position (%.1f%%), rtt %.1f ms\n",
        phase, int(f->num("ticksWithUpdate")), int(f->num("ticksElapsed")), 100.0 * f->num("ticksWithUpdate") / std::max(1.0, f->num("ticksElapsed")), s.rtt());
    s.leave(); for (auto &o : s.take_outbox()) c.emit(o.name);
    pump({}, 0.2); c.close();
}

static void coop_offline() {
    using namespace cssteroids::coop;
    Room r; std::string err;
    CHECK(parse_room_reply(R"({"success":true,"session":{"id":"session_ab12cd34","hostPlayerId":"sockA","maxPlayers":2,"currentPlayers":0,"gameState":"waiting"}})", r, err));
    CHECK(r.id == "session_ab12cd34" && r.maxPlayers == 2 && room_code(r.id) == "ab12cd34" && room_id_from_code("AB12 CD34") == r.id);
    CHECK(!parse_room_reply(R"({"success":false,"error":"Session not found"})", r, err) && err == "Session not found");
    std::vector<Room> rooms;
    CHECK(parse_room_list(R"({"success":true,"sessions":[{"id":"session_a","currentPlayers":1,"maxPlayers":2},{"nope":1}]})", rooms) && rooms.size() == 1 && rooms[0].currentPlayers == 1);
    CHECK(create_body("sockA") == R"({"hostPlayerId":"sockA","maxPlayers":2,"worldWidth":2000,"worldHeight":1500})");
    LobbyState l; l.roomId = "session_a";
    l.handle("session-joined", parse_or_die(R"({"sessionId":"session_a","playerId":"x","playerCount":1})"));
    l.handle("player-joined", parse_or_die(R"({"playerId":"y"})"));
    CHECK(l.joined && l.playerCount == 2 && !l.started);
    l.handle("game-started", parse_or_die(R"({"sessionId":"session_other"})"));
    CHECK(!l.started);
    l.handle("game-started", parse_or_die(R"({"sessionId":"session_a"})"));
    CHECK(l.started);
    std::puts("co-op lobby offline checks done");
}

// Two pilots: A creates a room over REST, both join it, A starts, both play the co-op world.
static void coop_live(const char *server) {
    using namespace cssteroids::coop;
    net::Endpoint e; CHECK(net::parse_endpoint(server, e));
    sio::Client a, b; a.connect(e); b.connect(e);
    for (int i = 0; i < 800 && (a.state() != sio::Client::State::connected || b.state() != sio::Client::State::connected); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(a.state() == sio::Client::State::connected && b.state() == sio::Client::State::connected);
    auto created = net::request(e, "POST", create_path, create_body(a.sid()));
    Room room; std::string err;
    CHECK(created.ok() && parse_room_reply(created.body, room, err));
    a.emit("join-simple-session", json::Value::string_value(room.id));
    // B finds the room in the open list, as the lobby screen does
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::vector<Room> open;
    auto listed = net::request(e, "GET", rooms_path);
    CHECK(listed.ok() && parse_room_list(listed.body, open));
    bool found = false; for (auto &r : open) found |= r.id == room.id;
    CHECK(found);
    b.emit("join-simple-session", json::Value::string_value(room.id));
    LobbyState la, lb; la.roomId = lb.roomId = room.id;
    ShipDesign ship; ship.typeName = "default"; ship.colorName = "white";
    Session sa, sb;
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    bool startSent = false;
    double last = now();
    for (double end = now() + 9000; now() < end;) {
        for (auto [c, l, s] : {std::tuple{&a, &la, &sa}, std::tuple{&b, &lb, &sb}}) {
            sio::Event ev;
            while (c->poll(ev)) if (!l->handle(ev.name, ev.data)) s->handle(ev.name, ev.data, now());
            if (l->started && s->phase == Phase::idle) s->begin_join(now(), c == &a ? "Host A" : "Guest B", ship, room.id);
        }
        if (la.joined && lb.joined && la.playerCount == 2 && !startSent) { a.emit("start-multiplayer-game"); startSent = true; }
        const double t = now();
        for (auto [c, s] : {std::pair{&a, &sa}, std::pair{&b, &sb}}) {
            s->update({}, t, (t - last) / 1000);
            for (auto &o : s->take_outbox()) { if (o.hasData) c->emit(o.name, o.data); else c->emit(o.name); }
        }
        last = t;
        if (sa.phase == Phase::playing && sb.phase == Phase::playing && sa.players.size() == 1 && sb.players.size() == 1 && sa.round >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    CHECK(la.playerCount == 2 && la.started && lb.started);
    CHECK(sa.phase == Phase::playing && sb.phase == Phase::playing);
    CHECK(sa.mode == "coop" && sb.mode == "coop" && sa.sessionId == room.id);
    CHECK(sa.lives == 3 && sa.maxRounds == 10 && sa.round >= 1);
    CHECK(sa.players.count(sb.myId) == 1 && sb.players.count(sa.myId) == 1);
    std::printf("co-op live: room %s, both pilots in round %d with %d shared lives, %zu asteroids\n",
                room_code(room.id).c_str(), sa.round, sa.lives, sa.asteroids.size());
    // B flies; a server with remote-player smoothing appends velocity, which A's client picks up
    Controls fly; fly.thrust = true; fly.turn = 0.3f;
    for (double end = now() + 1500; now() < end;) {
        for (auto [c, s, in] : {std::tuple{&a, &sa, Controls{}}, std::tuple{&b, &sb, fly}}) {
            sio::Event ev;
            while (c->poll(ev)) s->handle(ev.name, ev.data, now());
            s->update(in, now(), 1.0 / 60);
            for (auto &o : s->take_outbox()) { if (o.hasData) c->emit(o.name, o.data); else c->emit(o.name); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("co-op live: server sends player velocity: %s\n", sa.velocityEntries ? "yes" : "no (server without smoothing)");
    sa.leave(); sb.leave();
    for (auto &o : sa.take_outbox()) a.emit(o.name);
    for (auto &o : sb.take_outbox()) b.emit(o.name);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    a.close(); b.close();
}

int main(int argc, char **argv) {
    if (argc >= 4 && std::string(argv[1]) == "fresh") {
        for (int i = 3; i < argc; i++) fresh_bench(argv[2], std::atof(argv[i]), false, 12);
        return 0;
    }
    if (argc < 2) { std::puts("usage: test-mmo world_vectors.json [server] | test-mmo fresh <server> <phase>..."); return 2; }
    golden(argv[1]);
    offline();
    coop_offline();
    if (argc > 2) { live(argv[2]); coop_live(argv[2]); }
    if (failures) { std::printf("%d mmo test failure(s)\n", failures); return 1; }
    std::puts("mmo tests passed");
    return 0;
}
