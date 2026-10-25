// Host tests for power-ups (g++ -std=c++20). Values mirror CSSteroids-2 test/powerups.test.js;
// the long-run vectors were printed by the real public/js/shared/powerups.js in Node.
#include "asteroids_core.hpp"
#include "mmo_world.hpp"
#include <cstdio>
#include <cstdlib>

using namespace cssteroids;
namespace pu = cssteroids::powerups;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static auto seq(std::vector<double> v) { return [v, i = size_t(0)]() mutable { return v[i++]; }; }
static json::Value parse(const char *text) { json::Value v; if (!json::parse(text, v)) { std::puts("bad json"); std::exit(2); } return v; }
static pu::PowerUp at(double x, double y, double vx, double vy) { pu::PowerUp p; p.x = x; p.y = y; p.vx = vx; p.vy = vy; return p; }

static void rules() {
    // type list and definitions line up; art fits inside the ring at 0.25 scale
    CHECK(pu::types[0] == pu::Type::life && pu::types[3] == pu::Type::shield);
    for (auto t : pu::types) {
        CHECK(pu::type_from(pu::def(t).key) == t);
        CHECK(!pu::lines(t).empty());
        for (const auto &l : pu::lines(t)) for (double v : {l.x1, l.y1, l.x2, l.y2}) CHECK(std::fabs(v) * 0.25 <= pu::radius);
    }
    CHECK(!pu::type_from("bogus"));
    CHECK(pu::def(pu::Type::life).durationMs == 0 && pu::def(pu::Type::shield).durationMs == 30000 && pu::def(pu::Type::rapid).durationMs == 20000);
    CHECK(pu::lines(pu::Type::life).size() == 10 && pu::lines(pu::Type::rapid).size() == 4 && pu::lines(pu::Type::shield).size() == 6);
    {   // FAN: Math.round of the spray angles
        const double ends[5][2] = {{-36, -15}, {-19, -25}, {0, -28}, {19, -25}, {36, -15}};
        const auto &fan = pu::lines(pu::Type::spray);
        CHECK(fan.size() == 5);
        for (size_t i = 0; i < 5 && i < fan.size(); i++) CHECK(fan[i].x1 == 0 && fan[i].y1 == 28 && fan[i].x2 == ends[i][0] && fan[i].y2 == ends[i][1]);
        CHECK(pu::spray_angles[0] == -0.6981317007977318 && pu::spray_angles[1] == -0.3490658503988659 && pu::spray_angles[4] == 0.6981317007977318);
    }
    // rollDrop honours the chance boundary per source
    CHECK(pu::roll_drop(pu::Source::asteroid, seq({0.0999, 0})) == pu::Type::life);
    CHECK(!pu::roll_drop(pu::Source::asteroid, seq({0.1, 0})));
    CHECK(pu::roll_drop(pu::Source::enemy, seq({0.2499, 0})) == pu::Type::life);
    CHECK(!pu::roll_drop(pu::Source::enemy, seq({0.25, 0})));
    // rollDrop picks uniformly among allowed types
    CHECK(pu::roll_drop(pu::Source::enemy, seq({0, 0.999})) == pu::Type::shield);
    CHECK(pu::roll_drop(pu::Source::enemy, seq({0, 0.3})) == pu::Type::rapid);
    const std::vector<pu::Type> allowed = {pu::Type::rapid, pu::Type::spray, pu::Type::shield};
    CHECK(pu::roll_drop(pu::Source::enemy, seq({0, 0}), allowed) == pu::Type::rapid);
    CHECK(pu::roll_drop(pu::Source::enemy, seq({0, 0.999}), allowed) == pu::Type::shield);
    CHECK(!pu::roll_drop(pu::Source::enemy, seq({0}), {}));

    // stepPowerUp drifts by frames
    { auto p = at(100, 100, 0.5, -0.25); pu::step(p, 800, 600, 3); CHECK(p.x == 101.5 && p.y == 99.25); }
    {   // stepPowerUp bounces off every edge
        const double r = pu::radius;
        auto p = at(r + 0.1, 300, -0.5, 0); pu::step(p, 800, 600); CHECK(p.x == r && p.vx == 0.5);
        p = at(800 - r - 0.1, 300, 0.5, 0); pu::step(p, 800, 600); CHECK(p.x == 800 - r && p.vx == -0.5);
        p = at(300, r + 0.1, 0, -0.5); pu::step(p, 800, 600); CHECK(p.y == r && p.vy == 0.5);
        p = at(300, 600 - r - 0.1, 0, 0.5); pu::step(p, 800, 600); CHECK(p.y == 600 - r && p.vy == -0.5);
    }
    {   // one frames=3 step per server tick, bit-exact with Node over many bounces
        auto p = at(20, 500, -0.37, 0.29);
        for (int i = 0; i < 3000; i++) pu::step(p, 2000, 1500, 3);
        CHECK(p.x == 635.1300000000818 && p.y == 165.38000000000045 && p.vx == -0.37 && p.vy == 0.29);
        p = at(1985.9, 14.2, 0.49, -0.13);
        for (int i = 0; i < 7; i++) pu::step(p, 2000, 1500, 3);
        CHECK(p.x == 1977.1799999999998 && p.y == 16.340000000000003 && p.vx == -0.49 && p.vy == 0.13);
    }
    {   // syncPowerUpToTick steps once per tick with the given frames, never backwards
        auto p = at(20, 500, -0.5, 0.25), expected = p;
        p.simTick = 10;
        for (int i = 0; i < 5; i++) pu::step(expected, 1000, 1000, 3);
        pu::sync_to_tick(p, 15, 1000, 1000, 3);
        CHECK(p.x == expected.x && p.y == expected.y && p.vx == expected.vx && p.vy == expected.vy && p.simTick == 15);
        pu::sync_to_tick(p, 12, 1000, 1000, 3);
        CHECK(p.simTick == 15);
    }
    {   // isPickup reaches exactly ship radius plus power-up radius
        const double reach = 15 + pu::radius;
        CHECK(pu::is_pickup(100, 100, at(100 + reach, 100, 0, 0)));
        CHECK(!pu::is_pickup(100, 100, at(100 + reach + 0.01, 100, 0, 0)));
        CHECK(pu::is_pickup(100, 100, at(100, 100, 0, 0)));
        CHECK(!pu::is_pickup(100, 100, at(100 + reach, 101, 0, 0)));
    }
    {   // effectsMsLeft converts end ticks to milliseconds and drops lapsed ones
        pu::EffectTicks t;
        t.until[int(pu::Type::rapid)] = 110; t.until[int(pu::Type::shield)] = 100; t.until[int(pu::Type::spray)] = 90;
        auto e = t.left(100.5, 50);
        CHECK(e.ms[int(pu::Type::rapid)] == 475 && !e.active(pu::Type::shield) && !e.active(pu::Type::spray));
        CHECK(!t.left(NAN, 50).active(pu::Type::rapid));
        CHECK(!pu::EffectTicks{}.left(5, 50).active(pu::Type::rapid));
    }
    // blink: hidden on alternate 150 ms in the last 3 s
    CHECK(!pu::blinked_out(8999) && !pu::blinked_out(9000) && pu::blinked_out(9150) && !pu::blinked_out(9300));
    CHECK(pu::shield_visible(5000) && !pu::shield_visible(150) && pu::shield_visible(300) && !pu::shield_visible(0));
}

static void weapons() {
    using mmo::create_shot_bullets;
    {   // without effects bullets are unchanged
        auto b = create_shot_bullets(100, 100, 0.3, 2, -1, {});
        auto out = pu::apply_weapon_effects(b, {});
        CHECK(out.size() == 1 && out[0].x == b[0].x && out[0].angle == b[0].angle && out[0].speed == b[0].speed);
        CHECK(std::fabs(out[0].vx - b[0].vx) < 1e-12 && std::fabs(out[0].vy - b[0].vy) < 1e-12);
    }
    {   // rapid doubles speed and keeps maxLife
        auto base = create_shot_bullets(100, 100, 0, 0, 0, {});
        auto fast = pu::apply_weapon_effects(base, {true, false});
        CHECK(base[0].speed == 8 && fast[0].speed == 16 && fast[0].maxLife == base[0].maxLife && fast[0].vy == -16);
    }
    {   // rapid keeps the ship velocity carry
        auto fast = pu::apply_weapon_effects(create_shot_bullets(100, 100, 0, 4, 0, {}), {true, false});
        CHECK(std::fabs(fast[0].vx - 2) < 1e-9);
    }
    {   // spray yields a 5-way fan per origin (weapon points (-20, 0), (20, 0) in design units)
        auto origins = create_shot_bullets(0, 0, 0, 0, 0, {{-5, 0}, {5, 0}});
        auto out = pu::apply_weapon_effects(origins, {false, true});
        CHECK(out.size() == 10);
        const int degrees[5] = {-40, -20, 0, 20, 40};
        for (int i = 0; i < 5; i++) CHECK(int(std::lround(out[size_t(i)].angle * 180 / 3.141592653589793)) == degrees[i]);
        CHECK(out[0].x == origins[0].x && out[5].x == origins[1].x);
        const auto &side = out[4];
        CHECK(std::fabs(side.vx - std::sin(side.angle) * side.speed) < 1e-9 && std::fabs(side.vy + std::cos(side.angle) * side.speed) < 1e-9);
    }
    {   // spray and rapid together, against Node's applyWeaponEffects
        auto out = pu::apply_weapon_effects(create_shot_bullets(100, 200, 1.234, 3.1, -2.2, {}), {true, true});
        const double js[5][4] = {
            {0.5358682992022682, 16, 9.71940508325938, -14.857209767449786},
            {0.884934149601134, 16, 13.931972529374194, -11.23344740361457},
            {1.234, 16, 16.65109134999414, -6.387441729147678},
            {1.583065850398866, 16, 17.548795685432616, -0.9036925478165443},
            {1.9321317007977319, 16, 16.516809144130775, 4.55637905759183}};
        CHECK(out.size() == 5);
        for (size_t i = 0; i < 5 && i < out.size(); i++) {
            CHECK(out[i].angle == js[i][0] && out[i].speed == js[i][1] && out[i].life == 0);
            CHECK(std::fabs(out[i].vx - js[i][2]) < 1e-12 && std::fabs(out[i].vy - js[i][3]) < 1e-12); // libm vs V8 sin/cos
        }
    }
}

static void single_player() {
    {   // a drop drifts, blinks, and lapses after 12 s
        Game g(1); g.asteroids = {Asteroid{}}; g.asteroids[0].x = 100; g.asteroids[0].y = 100;
        pu::PowerUp p = at(1500, 300, 0.25, 0); g.powerUps = {p};
        for (int i = 0; i < 60; i++) { g.asteroids[0].x = 100; g.asteroids[0].y = 100; g.step({}); }
        CHECK(g.powerUps.size() == 1 && std::fabs(g.powerUps[0].x - 1515) < 1e-9 && std::fabs(g.powerUps[0].rotation - 1.2) < 1e-9);
        for (int i = 0; i < 660; i++) { g.asteroids[0].x = 100; g.asteroids[0].y = 100; g.step({}); }
        CHECK(g.powerUps.empty());
    }
    {   // pickups: life adds a life; timed effects start at their full duration
        Game g(1); g.asteroids = {Asteroid{}}; g.asteroids[0].x = 100; g.asteroids[0].y = 100;
        auto life = at(g.ship.x + 20, g.ship.y, 0, 0); life.type = pu::Type::life;
        auto shield = at(g.ship.x - 20, g.ship.y, 0, 0); shield.type = pu::Type::shield;
        g.powerUps = {life, shield};
        g.step({});
        CHECK(g.powerUps.empty() && g.lives == 4 && g.debris.size() == 20);
        CHECK(std::fabs(g.ship.effects.ms[int(pu::Type::shield)] - 30000) < 20 && !g.ship.effects.active(pu::Type::life));
        // the shield holds off an asteroid
        g.asteroids[0].x = g.ship.x; g.asteroids[0].y = g.ship.y; g.asteroids[0].vx = g.asteroids[0].vy = 0;
        for (int i = 0; i < 30; i++) g.step({});
        CHECK(!g.ship.exploding && g.lives == 4);
        // it lapses, then the hit lands and clears every effect
        g.ship.effects.ms[int(pu::Type::shield)] = 10; g.ship.effects.add(pu::Type::rapid);
        g.step({}); g.step({});
        CHECK(g.ship.exploding && !g.ship.effects.active(pu::Type::rapid));
        g.reset();
        CHECK(g.powerUps.empty() && !g.ship.effects.active(pu::Type::rapid));
    }
    {   // spray fans five rounds; rapid doubles their speed
        Game g(1); g.asteroids = {Asteroid{}}; g.asteroids[0].x = 100; g.asteroids[0].y = 100;
        g.ship.effects.add(pu::Type::spray); g.ship.effects.add(pu::Type::rapid);
        Controls c; c.fire = true; g.step(c);
        CHECK(g.bullets.size() == 5 && g.bullets[0].initialSpeed == 12 && std::fabs(g.bullets[0].angle + 0.6981317f) < 1e-6f);
        // timers run down at 60 Hz and lapse
        for (int i = 0; i < 1200; i++) { g.asteroids[0].x = 100; g.asteroids[0].y = 100; g.step({}); }
        CHECK(!g.ship.effects.active(pu::Type::spray) && !g.ship.effects.active(pu::Type::rapid));
    }
    {   // bullet kills drop at about 10% per asteroid, at the asteroid
        int drops = 0, kills = 0;
        for (uint32_t seed = 1; seed <= 400; seed++) {
            Game g(seed); g.asteroids.resize(1);
            g.asteroids[0].x = 960; g.asteroids[0].y = 300; g.asteroids[0].vx = g.asteroids[0].vy = 0; g.asteroids[0].size = 1; g.asteroids[0].radius = 15;
            Controls c; c.fire = true;
            for (int i = 0; i < 90 && g.score == 0; i++) g.step(i == 0 ? c : Controls{});
            if (g.score) kills++;
            if (!g.powerUps.empty()) { drops++; CHECK(std::hypot(g.powerUps[0].x - 960, g.powerUps[0].y - 300) < 2); CHECK(std::hypot(g.powerUps[0].vx, g.powerUps[0].vy) <= pu::max_drift + 1e-6); }
        }
        CHECK(kills == 400 && drops > 20 && drops < 65);
    }
}

static void shared_world() {
    using namespace cssteroids::mmo;
    Session s;
    ShipDesign ship;
    s.begin_join(0, "PS5 Tester", ship);
    auto out = s.take_outbox();
    CHECK(out.size() == 1 && out[0].data.num("protocol") == 5);
    json::Value d = parse(R"({"player":{"id":"me","idx":0,"x":1000,"y":700,"angle":0},"mode":"mmo","worldBounds":{"width":2000,"height":1500},
        "asteroids":[],"enemies":[],"players":[{"id":"bob","name":"Bob","x":1200,"y":700,"angle":0,"effects":{"shield":160,"spray":90}}],
        "powerUps":[{"id":3,"type":"spray","x":1020,"y":700,"vx":0.5,"vy":0,"tick":100,"expireTick":300}],"tick":100,"serverTime":5000})");
    s.handle("mmo-join-success", d, 0);
    CHECK(s.powerUps.size() == 1 && s.powerUps[3].type == pu::Type::spray && s.powerUps[3].expireTick == 300);
    CHECK(s.players["bob"].effects.until[int(pu::Type::shield)] == 160);
    CHECK(s.effects_left(s.players["bob"].effects, 0).ms[int(pu::Type::shield)] == 3000 && !s.effects_left(s.players["bob"].effects, 0).active(pu::Type::spray));
    // records step once per tick (frames = 3)
    double now = 0;
    for (int i = 0; i < 4; i++) { s.update({}, now, 0.05); now += 50; }
    CHECK(s.powerUps[3].simTick == 103 && s.powerUps[3].x == 1020 + 0.5 * 3 * 3);
    // warping: no claims yet
    for (auto &o : s.take_outbox()) CHECK(o.name != "mmo-powerup-pickup");
    for (int i = 0; i < 5; i++) {
        json::Value r = json::Value::object();
        r.set("sentAt", json::Value::number_value(i * 100)); r.set("serverTime", json::Value::number_value(5000 + i * 100 + 20));
        s.handle("mmo-time-sync", r, i * 100 + 40);
    }
    for (int i = 0; i < 64; i++) { s.update({}, now, 0.05); now += 50; } // no state seen: warp lasts 3 s
    CHECK(s.phase == Phase::playing);
    // touching it sends one claim, retried after 500 ms while it is still there
    s.powerUps[3].x = s.x + 10; s.powerUps[3].y = s.y; s.powerUps[3].vx = 0; s.powerUps[3].claimedAtMs = -1e18;
    int claims = 0;
    for (int i = 0; i < 12; i++) {
        s.update({}, now, 0.05); now += 50;
        for (auto &o : s.take_outbox()) if (o.name == "mmo-powerup-pickup") { claims++; CHECK(o.data.num("id") == 3 && json::dump(o.data) == R"({"id":3})"); }
    }
    CHECK(claims == 2);
    // a claim does not grant: only 'taken' with our id does
    CHECK(!s.effects_left(s.effects, now).active(pu::Type::spray));
    const double tickNow = s.server_tick(now);
    json::Value taken = json::Value::object();
    taken.set("id", json::Value::number_value(3)); taken.set("playerId", json::Value::string_value("me"));
    taken.set("type", json::Value::string_value("spray")); taken.set("untilTick", json::Value::number_value(std::floor(tickNow) + 400));
    s.handle("mmo-powerup-taken", taken, now);
    CHECK(s.powerUps.empty() && !s.bursts.empty() && s.bursts.back().color == pu::def(pu::Type::spray).color);
    CHECK(s.effects_left(s.effects, now).active(pu::Type::spray));
    // own shots fan out; the mmo-shoot payload is unchanged
    Controls fire; fire.fire = true;
    const size_t before = s.bullets.size();
    s.update(fire, now, 1.0 / 60); now += 1000.0 / 60;
    CHECK(s.bullets.size() == before + 5);
    for (auto &o : s.take_outbox()) if (o.name == "mmo-shoot") CHECK(!o.data.get("effects"));
    // remote shots replay with their effects field
    s.bullets.clear();
    d = parse(R"({"bullet":{"x":1200,"y":700,"angle":0,"vx":0,"vy":0},"playerId":"bob","effects":{"rapid":true,"spray":true}})");
    s.handle("mmo-bullet-fired", d, now);
    CHECK(s.bullets.size() == 5 && s.bullets[0].speed == 16 && s.bullets[0].owner == "bob");
    d = parse(R"({"bullet":{"x":1200,"y":700,"angle":0,"vx":0,"vy":0},"playerId":"bob"})");
    s.handle("mmo-bullet-fired", d, now);
    CHECK(s.bullets.size() == 6 && s.bullets[5].speed == 8);
    // someone else's pickup: their effect, not ours; life only removes it
    s.handle("mmo-powerup-spawned", parse(R"({"id":4,"type":"shield","x":500,"y":500,"vx":0,"vy":0,"tick":1,"expireTick":99999999})"), now);
    s.handle("mmo-powerup-spawned", parse(R"({"id":5,"type":"life","x":500,"y":500,"vx":0,"vy":0,"tick":1,"expireTick":99999999})"), now);
    s.handle("mmo-powerup-spawned", parse(R"({"id":6,"type":"rapid","x":500,"y":500,"vx":0,"vy":0,"tick":1,"expireTick":2})"), now);
    CHECK(s.powerUps.size() == 3);
    taken.set("id", json::Value::number_value(4)); taken.set("playerId", json::Value::string_value("bob"));
    taken.set("type", json::Value::string_value("shield"));
    s.handle("mmo-powerup-taken", taken, now);
    CHECK(s.effects_left(s.players["bob"].effects, now).active(pu::Type::shield) && !s.effects_left(s.effects, now).active(pu::Type::shield));
    taken.set("id", json::Value::number_value(5)); taken.set("type", json::Value::string_value("life")); taken.set("untilTick", json::Value{});
    s.handle("mmo-powerup-taken", taken, now);
    CHECK(s.powerUps.size() == 1);
    s.update({}, now, 0.0); // id 6 lapsed at its expireTick without an event
    CHECK(s.powerUps.empty());
    s.handle("mmo-powerup-spawned", parse(R"({"id":7,"type":"rapid","x":500,"y":500,"vx":0,"vy":0,"tick":1,"expireTick":99999999})"), now);
    s.handle("mmo-powerup-expired", parse(R"({"id":7})"), now);
    CHECK(s.powerUps.empty());
    // own shield: an asteroid on the ship sends no death or collision claim
    taken.set("playerId", json::Value::string_value("me")); taken.set("type", json::Value::string_value("shield"));
    taken.set("untilTick", json::Value::number_value(std::floor(s.server_tick(now)) + 600));
    s.handle("mmo-powerup-taken", taken, now);
    AsteroidRecord rock; rock.id = 8; rock.x = s.x; rock.y = s.y; rock.tick = int64_t(s.server_tick(now)) + 1000; build_shape(rock);
    s.asteroids[8] = rock; s.invulnerableMs = 0; s.take_outbox();
    for (int i = 0; i < 20; i++) { s.update({}, now, 1.0 / 60); now += 1000.0 / 60; }
    CHECK(!s.dead);
    for (auto &o : s.take_outbox()) CHECK(o.name != "mmo-player-died" && o.name != "mmo-ship-collision");
    // a respawn reply keeps effects; our death clears them, and a remote death clears theirs
    s.handle("mmo-respawn", parse(R"({"x":900,"y":800,"angle":0})"), now);
    CHECK(s.effects_left(s.effects, now).active(pu::Type::shield));
    s.handle("mmo-player-died", parse(R"({"playerId":"me","x":900,"y":800})"), now);
    CHECK(!s.effects_left(s.effects, now).active(pu::Type::shield) && !s.effects_left(s.effects, now).active(pu::Type::spray));
    s.handle("mmo-player-died", parse(R"({"playerId":"bob","x":1,"y":1})"), now);
    CHECK(!s.effects_left(s.players["bob"].effects, now).active(pu::Type::shield));
}

int main() {
    rules();
    weapons();
    single_player();
    shared_world();
    if (failures) { std::printf("%d power-up checks failed\n", failures); return 1; }
    std::puts("power-up tests passed");
    return 0;
}
