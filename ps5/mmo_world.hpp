#pragma once
// Client side of the CSSteroids shared world (MMO; co-op uses the same engine).
// Ports public/js/shared/world-sim.js and the MMO paths of public/js/core/game.js
// against the unchanged server (docs/multiplayer-protocol.md). Portable: the
// caller supplies the clock and moves events/emits through socketio.hpp.
#include "asteroids_core.hpp"
#include "json_lite.hpp"
#include "ship_design.hpp"
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Asteroid records must step exactly like Node's doubles: no fused multiply-add
// (clang may contract on FMA-capable targets; GCC on x86-64 does not without -mfma).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace cssteroids::mmo {
constexpr int tick_ms = 50, frames_per_tick = 3, protocol_version = 4, id_block = 8;
constexpr int max_sync_ticks = 600;
// Others are drawn this far behind the server clock. Players: 2 ticks (as the browser) when the
// server sends velocity, so late snapshots can glide; 3 ticks for servers without it.
constexpr double player_delay_ticks = 2, player_delay_no_velocity = 3, enemy_delay_ticks = 4, max_extrapolate_ticks = 2;
constexpr double ship_radius = 15, view_margin = 200, bullet_speed = 8, bullet_life = 1.8;
constexpr double tau = 6.283185307179586;

// --- world-sim.js ----------------------------------------------------------
struct AsteroidRecord {
    int64_t id = 0;
    double x = 0, y = 0, vx = 0, vy = 0, radius = 40, rotation = 0, rotationSpeed = 0;
    int size = 3;
    uint32_t seed = 0;
    int64_t tick = 0;        // the tick this record has been stepped to
    int vertices = 6;
    double offsets[8] = {};
    double hiddenUntilMs = 0; // optimistic hide after a local bullet hit
};

inline void size_props(int size, double &radius, double &speed) {
    if (size == 3) { radius = 40; speed = 1; } else if (size == 2) { radius = 25; speed = 1.5; } else { radius = 15; speed = 2; }
}

// Asteroid.buildShape with mulberry32(seed), in doubles like the browser.
inline void build_shape(AsteroidRecord &a) {
    uint32_t s = a.seed;
    auto next = [&] {
        s += 0x6D2B79F5u;
        uint32_t t = s;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return double(t ^ (t >> 14)) / 4294967296.0;
    };
    a.vertices = int(std::floor(next() * 3)) + 6;
    const double jag = next() * 0.2 + 0.1;
    for (int i = 0; i < a.vertices; i++) a.offsets[i] = a.radius * (1 - jag + next() * jag);
}

struct Bounds { double width = 2000, height = 1500; };

inline void step_asteroid(AsteroidRecord &a, const Bounds &b) {
    a.x += a.vx * frames_per_tick;
    a.y += a.vy * frames_per_tick;
    a.rotation += a.rotationSpeed * frames_per_tick;
    if (a.x < a.radius) { a.x = a.radius; a.vx = std::fabs(a.vx); }
    else if (a.x > b.width - a.radius) { a.x = b.width - a.radius; a.vx = -std::fabs(a.vx); }
    if (a.y < a.radius) { a.y = a.radius; a.vy = std::fabs(a.vy); }
    else if (a.y > b.height - a.radius) { a.y = b.height - a.radius; a.vy = -std::fabs(a.vy); }
}

inline void advance_to(AsteroidRecord &a, int64_t tick, const Bounds &b) {
    int64_t ticks = std::min<int64_t>(std::max<int64_t>(0, tick - a.tick), max_sync_ticks);
    for (int64_t i = 0; i < ticks; i++) step_asteroid(a, b);
    if (tick > a.tick) a.tick = tick;
}

inline uint32_t child_seed(uint32_t parent, int index) {
    return uint32_t(uint32_t(parent ^ uint32_t(index + 1)) * 2654435761u + uint32_t(index) * 40503u);
}
inline int64_t child_id(int64_t id, int index) {
    const int64_t offset = id % id_block;
    return id - offset + 2 * offset + 1 + index;
}

inline std::vector<AsteroidRecord> split_asteroid(const AsteroidRecord &p, int64_t tick) {
    std::vector<AsteroidRecord> out;
    if (p.size <= 1) return out;
    const int size = p.size - 1;
    double radius, speed;
    size_props(size, radius, speed);
    const double heading = std::atan2(p.vy, p.vx);
    const double spread = 0.5 + child_seed(p.seed, 2) / 4294967296.0 * 0.7;
    for (int i = 0; i < 2; i++) {
        const int side = i == 0 ? -1 : 1;
        const double angle = heading + side * spread;
        AsteroidRecord c;
        c.id = child_id(p.id, i);
        c.x = p.x + std::cos(angle) * radius * 0.5;
        c.y = p.y + std::sin(angle) * radius * 0.5;
        c.vx = std::cos(angle) * speed;
        c.vy = std::sin(angle) * speed;
        c.size = size; c.radius = radius;
        c.rotation = p.rotation;
        c.rotationSpeed = p.rotationSpeed * -side * 1.5;
        c.seed = child_seed(p.seed, i);
        c.tick = tick;
        build_shape(c);
        out.push_back(c);
    }
    return out;
}

// createShotBullets + stepBullet: angle 0 = up; per-frame motion as the browser.
struct Bullet {
    double x, y, angle, speed, vx, vy, life = 0, maxLife;
    std::string owner;  // player id ("" = local) or enemy id for enemy rounds
    bool hostile = false;
};
inline std::vector<Bullet> create_shot_bullets(double x, double y, double angle, double vx, double vy,
                                               const std::vector<Point> &weapons) {
    const double sn = std::sin(angle), cs = std::cos(angle);
    std::vector<std::pair<double, double>> origins;
    if (weapons.empty()) origins.push_back({x + sn * ship_radius, y - cs * ship_radius});
    for (const auto &p : weapons) origins.push_back({x + p.x * cs - p.y * sn, y + p.x * sn + p.y * cs});
    const bool multi = origins.size() > 1;
    const double speed = multi ? bullet_speed * 0.75 : bullet_speed, life = multi ? bullet_life * 0.7 : bullet_life;
    std::vector<Bullet> out;
    for (auto [ox, oy] : origins) out.push_back({ox, oy, angle, speed, sn * speed + vx * 0.5, -cs * speed + vy * 0.5, 0, life, {}, false});
    return out;
}
inline void step_bullet_frame(Bullet &b, double dt) {
    b.x += b.vx; b.y += b.vy;
    b.speed *= 0.99;
    b.vx = std::sin(b.angle) * b.speed;
    b.vy = -std::cos(b.angle) * b.speed;
    b.life += dt;
}

struct ClockSample { double sentAt, receivedAt, serverTime; };
// estimateClockOffset: the lowest-RTT half, medians of offset and RTT.
inline bool estimate_clock(std::vector<ClockSample> samples, double &offsetMs, double &rttMs) {
    if (samples.empty()) return false;
    std::sort(samples.begin(), samples.end(), [](auto &a, auto &b) { return a.receivedAt - a.sentAt < b.receivedAt - b.sentAt; });
    samples.resize((samples.size() + 1) / 2);
    std::vector<double> offsets, rtts;
    for (auto &s : samples) { rtts.push_back(s.receivedAt - s.sentAt); offsets.push_back(s.serverTime - (s.sentAt + s.receivedAt) / 2); }
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); size_t m = v.size() / 2; return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2; };
    offsetMs = median(offsets); rttMs = median(rtts);
    return true;
}

inline double lerp_angle(double from, double to, double t) {
    double diff = std::fmod(to - from, tau);
    if (diff > tau / 2) diff -= tau;
    if (diff < -tau / 2) diff += tau;
    return from + diff * t;
}

// SnapshotBuffer: per-entity history from delta snapshots; absent entries kept their value.
struct Sample { double tick, x, y, a; bool dead; bool thrust; double vx = 0, vy = 0; };
class SnapshotBuffer {
public:
    double delayTicks, intervalTicks;
    std::map<std::string, std::deque<Sample>> history;
    explicit SnapshotBuffer(double delayMs = 100, double interval = 1) : delayTicks(delayMs / tick_ms), intervalTicks(interval) {}
    void push(double tick, const std::string &id, double x, double y, double a, bool dead, bool thrust, double vx = 0, double vy = 0) {
        auto &samples = history[id];
        if (!samples.empty() && samples.back().tick >= tick) return;
        // absent from deltas = unchanged: hold still (no velocity) until this sample
        if (!samples.empty() && samples.back().tick < tick - intervalTicks) { Sample hold = samples.back(); hold.tick = tick - intervalTicks; hold.vx = hold.vy = 0; samples.push_back(hold); }
        samples.push_back({tick, x, y, a, dead, thrust, std::isfinite(vx) ? vx : 0, std::isfinite(vy) ? vy : 0});
        while (samples.size() > 64) samples.pop_front();
    }
    void remove(const std::string &id) { history.erase(id); }
    bool sample(const std::string &id, double renderTick, Sample &out) {
        auto it = history.find(id);
        if (it == history.end() || it->second.empty()) return false;
        auto &s = it->second;
        while (s.size() > 2 && s[1].tick <= renderTick) s.pop_front();
        const Sample &from = s[0];
        if (s.size() < 2 || renderTick <= from.tick || renderTick >= s[1].tick || from.dead != s[1].dead) {
            const Sample &pick = s.size() >= 2 && (renderTick >= s[1].tick || from.dead != s[1].dead) ? s[1] : from;
            out = pick;
            out.dead = renderTick >= pick.tick ? pick.dead : from.dead;
            // the next snapshot is late: a moving ship glides briefly instead of freezing
            if (!pick.dead && renderTick > pick.tick) {
                const double ahead = std::min(renderTick - pick.tick, max_extrapolate_ticks) * frames_per_tick;
                out.x += pick.vx * ahead; out.y += pick.vy * ahead;
            }
            return true;
        }
        const Sample &to = s[1];
        const double t = (renderTick - from.tick) / (to.tick - from.tick);
        out = {renderTick, from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, lerp_angle(from.a, to.a, t), from.dead, t < 0.5 ? from.thrust : to.thrust};
        return true;
    }
};

struct ScoreEntry { std::string id, name; int score = 0; };
// mergeScores: full replaces, left deletes, entries upsert, sorted by score.
inline void merge_scores(std::vector<ScoreEntry> &board, const json::Value &data) {
    if (!data.is_object()) return;
    std::map<std::string, ScoreEntry> byId;
    if (!(data.get("full") && data.get("full")->b)) for (auto &e : board) byId[e.id] = e;
    if (const auto *left = data.get("left"); left && left->is_array()) for (auto &id : left->items) byId.erase(id.s);
    if (const auto *players = data.get("players"); players && players->is_array())
        for (auto &p : players->items)
            if (p.is_array() && p.items.size() >= 3) byId[p.items[0].s] = {p.items[0].s, p.items[1].s.substr(0, 20), int(p.items[2].n)};
    board.clear();
    for (auto &kv : byId) board.push_back(kv.second);
    std::stable_sort(board.begin(), board.end(), [](auto &a, auto &b) { return a.score > b.score; });
}

// Ship data for mmo-join / mmo-ship-data, inside sanitizeMMOShipData's limits
// (helpers.js: ≤500 lines, ≤3 thruster points, ≤2 weapon points, strings ≤32).
inline json::Value ship_payload(const ShipDesign &d) {
    json::Value out = json::Value::object();
    auto design_space = [](float v) { return json::Value::number_value(std::round(double(v) * 4 * 100) / 100); };
    json::Value lines; lines.type = json::Value::Type::array;
    for (size_t i = 0; i < d.geometry.lines.size() && i < 500; i++) {
        const auto &l = d.geometry.lines[i];
        json::Value o = json::Value::object();
        o.set("startX", design_space(l.x1)); o.set("startY", design_space(l.y1));
        o.set("endX", design_space(l.x2)); o.set("endY", design_space(l.y2));
        char hex[8]; std::snprintf(hex, sizeof hex, "#%06x", l.color & 0xffffff);
        o.set("color", json::Value::string_value(hex));
        lines.items.push_back(std::move(o));
    }
    auto points = [&](const std::vector<Point> &pts, size_t limit) {
        json::Value arr; arr.type = json::Value::Type::array;
        for (size_t i = 0; i < pts.size() && i < limit; i++) {
            json::Value o = json::Value::object();
            o.set("x", design_space(pts[i].x)); o.set("y", design_space(pts[i].y));
            arr.items.push_back(std::move(o));
        }
        return arr;
    };
    out.set("customLines", std::move(lines));
    out.set("thrusterPoints", points(d.geometry.thrusters, 3));
    out.set("weaponPoints", points(d.geometry.weapons, 2));
    out.set("shipType", json::Value::string_value(d.typeName.substr(0, 32)));
    out.set("shipColor", json::Value::string_value(d.colorName.substr(0, 32)));
    out.set("shipName", json::Value::string_value(d.name.substr(0, 32)));
    return out;
}
// The other way: a player's shipData from the server into drawable geometry.
inline ShipGeometry geometry_from(const json::Value &shipData) {
    ShipDesign d;
    if (!shipData.is_object()) return d.geometry;
    json::Value v = shipData;
    if (!v.get("type") && v.get("shipType")) v.set("type", *v.get("shipType"));
    if (!v.get("color") && v.get("shipColor")) v.set("color", *v.get("shipColor"));
    parse_ship(v, d);
    if (d.geometry.weapons.size() > 2) d.geometry.weapons.resize(2);
    return d.geometry;
}

// --- session ---------------------------------------------------------------
struct Outgoing { std::string name; json::Value data; bool hasData = true; };

struct RemotePlayer {
    std::string id, name;
    ShipGeometry geometry;
    double x = 0, y = 0, angle = 0;
    bool dead = false, thrust = false, outOfView = false;
};
struct Enemy { std::string id; double x = 0, y = 0, a = 0; bool outOfView = false; };
struct Burst { double x, y, age = 0; bool big; };

enum class Phase { idle, joining, warping, playing, failed };

class Session {
public:
    Phase phase = Phase::idle;
    std::string failure, sessionId = "mmo_world", mode = "mmo", myId, pilot;
    int myIdx = -1, round = 0, maxRounds = 10, lives = 0, teamScore = 0;
    bool coopOver = false, coopWon = false;
    Bounds bounds;
    ShipDesign design;
    // own ship (client-predicted; game convention, px per 60 Hz frame)
    double x = 1000, y = 750, angle = 0, vx = 0, vy = 0;
    bool thrusting = false, dead = false;
    double invulnerableMs = 0, shootCooldown = 0, respawnCountdownMs = 0;
    std::map<int64_t, AsteroidRecord> asteroids;
    std::map<std::string, RemotePlayer> players;
    std::map<std::string, Enemy> enemies;
    std::vector<Bullet> bullets;      // own, remote replays and enemy rounds
    std::vector<Burst> bursts;
    std::vector<ScoreEntry> scores;
    SnapshotBuffer playerBuf{100, 1}, enemyBuf{100, 2};
    std::vector<Outgoing> outbox;
    std::string banner;
    double bannerMs = 0;
    // stats for the HUD / tests
    int statesReceived = 0, shotsSent = 0;
    bool velocityEntries = false; // the server appends vx, vy to player entries
    // Where in the server tick our updates should land (0..1). 0.35 + up to one 60 Hz frame of
    // send delay centres arrivals around mid-tick (measured with tools/sio-fixture JITTER_MS).
    double sendPhase = 0.35;

    void begin_join(double nowMs, const std::string &name, const ShipDesign &ship, const std::string &session = "mmo_world") {
        *this = Session{};
        pilot = name.substr(0, 20); design = ship; sessionId = session;
        json::Value join = json::Value::object();
        join.set("sessionId", json::Value::string_value(sessionId));
        json::Value pd = json::Value::object();
        pd.set("name", json::Value::string_value(pilot));
        pd.set("shipData", ship_payload(design));
        join.set("playerData", std::move(pd));
        join.set("protocol", json::Value::number_value(protocol_version));
        json::Value vp = json::Value::object();
        vp.set("w", json::Value::number_value(1920)); vp.set("h", json::Value::number_value(1080));
        join.set("viewport", std::move(vp));
        emit("mmo-join", std::move(join));
        phase = Phase::joining;
        joinStartMs = nowMs;
    }
    void leave() { if (phase == Phase::warping || phase == Phase::playing) emit_bare("mmo-leave"); phase = Phase::idle; }

    double server_tick(double nowMs) const { return (nowMs + offsetMs) / tick_ms; }
    bool clock_known() const { return clockKnown; }
    double rtt() const { return rttMs; }

    // One server event.
    void handle(const std::string &name, const json::Value &d, double nowMs) {
        if (name == "mmo-join-success") on_join(d, nowMs);
        else if (name == "mmo-join-error") { phase = Phase::failed; failure = d.str("error", "join refused"); }
        else if (name == "mmo-reload") { phase = Phase::failed; failure = "server protocol changed; update PSteroids5"; }
        else if (name == "mmo-time-sync") on_time(d, nowMs);
        else if (name == "mmo-state") on_state(d);
        else if (name == "mmo-asteroid-spawn") { AsteroidRecord a; if (read_asteroid(d, a)) asteroids[a.id] = a; }
        else if (name == "mmo-asteroid-destroyed") on_asteroid_destroyed(d);
        else if (name == "mmo-asteroid-check") on_asteroid_check(d);
        else if (name == "mmo-enemy-spawn") {
            if (d.is_array() && d.items.size() >= 3) { Enemy e; e.id = id_of(d.items[0]); e.x = d.items[1].n; e.y = d.items[2].n; enemies[e.id] = e; }
        } else if (name == "mmo-enemy-destroyed") {
            if (d.is_array() && !d.items.empty()) { const auto id = id_of(d.items[0]); if (auto it = enemies.find(id); it != enemies.end()) bursts.push_back({it->second.x, it->second.y, 0, true}); enemies.erase(id); enemyBuf.remove(id); }
        } else if (name == "mmo-enemy-shoot") {
            auto shots = create_shot_bullets(d.num("x"), d.num("y"), d.num("angle") + tau / 4, 0, 0, {});
            for (auto &b : shots) { b.hostile = true; b.owner = id_of_field(d, "enemyId"); bullets.push_back(b); }
        } else if (name == "mmo-bullet-fired") {
            const auto *b = d.get("bullet");
            const std::string shooter = d.str("playerId");
            if (b && b->is_object() && shooter != myId) {
                const auto it = players.find(shooter);
                auto shots = create_shot_bullets(b->num("x"), b->num("y"), b->num("angle"), b->num("vx"), b->num("vy"),
                                                 it != players.end() ? it->second.geometry.weapons : std::vector<Point>{});
                for (auto &s : shots) { s.owner = shooter; bullets.push_back(s); }
            }
        } else if (name == "mmo-player-joined") {
            RemotePlayer p; p.id = d.str("playerId"); p.name = d.str("playerName", "Pilot").substr(0, 20);
            p.x = d.num("x"); p.y = d.num("y"); p.angle = d.num("angle");
            if (const auto *sd = d.get("shipData")) p.geometry = geometry_from(*sd);
            if (!p.id.empty() && p.id != myId) players[p.id] = p;
        } else if (name == "mmo-player-left") { players.erase(d.str("playerId")); playerBuf.remove(d.str("playerId")); }
        else if (name == "mmo-player-died") {
            const std::string who = d.str("playerId");
            bursts.push_back({d.num("x"), d.num("y"), 0, true});
            if (who == myId) { dead = true; respawnCountdownMs = 3000; thrusting = false; }
            else if (auto it = players.find(who); it != players.end()) it->second.dead = true;
        } else if (name == "mmo-respawn") {
            x = d.num("x", x); y = d.num("y", y); angle = d.num("angle", 0); vx = vy = 0;
            dead = false; invulnerableMs = 3000; respawnCountdownMs = 0;
        } else if (name == "mmo-player-respawned") {
            playerBuf.remove(d.str("playerId"));
            if (auto it = players.find(d.str("playerId")); it != players.end()) { it->second.dead = false; it->second.x = d.num("x"); it->second.y = d.num("y"); }
        } else if (name == "mmo-ship-data") {
            if (auto it = players.find(d.str("playerId")); it != players.end() && d.get("shipData")) it->second.geometry = geometry_from(*d.get("shipData"));
        } else if (name == "mmo-scores") merge_scores(scores, d);
        else if (name == "mmo-round-start") { round = int(d.num("round", round)); show("ROUND " + std::to_string(round)); }
        else if (name == "mmo-round-cleared") show("ROUND " + std::to_string(int(d.num("round"))) + " CLEARED");
        else if (name == "mmo-lives") lives = int(d.num("lives", lives));
        else if (name == "mmo-game-over") { coopOver = true; teamScore = int(d.num("teamScore")); show("GAME OVER"); }
        else if (name == "mmo-game-complete") { coopOver = true; coopWon = true; teamScore = int(d.num("teamScore")); show("MISSION COMPLETE"); }
    }

    // One rendered frame: local physics at 60 Hz steps, clocks, outgoing traffic.
    void update(const Controls &in, double nowMs, double dtSeconds) {
        dtSeconds = std::clamp(dtSeconds, 0.0, 0.1);
        if (bannerMs > 0) bannerMs -= dtSeconds * 1000;
        for (auto &b : bursts) b.age += dtSeconds;
        std::erase_if(bursts, [](const Burst &b) { return b.age > 1; });
        if (phase == Phase::joining && nowMs - joinStartMs > 10000) { phase = Phase::failed; failure = "no answer from the world"; }
        if (phase != Phase::warping && phase != Phase::playing) return;
        // time sync: 5 samples at 100 ms spacing after join success
        if (syncSent < 5 && nowMs - joinedAtMs >= syncSent * 100.0) {
            json::Value s = json::Value::object(); s.set("sentAt", json::Value::number_value(nowMs));
            emit("mmo-time-sync", std::move(s)); syncSent++;
        }
        if (!clockKnown && (samples.size() >= 5 || (samples.size() >= 3 && nowMs - joinedAtMs > 900))) clockKnown = estimate_clock(samples, offsetMs, rttMs);
        // warp ends once the clock is known and the world has been seen (or 3 s passed)
        if (phase == Phase::warping && clockKnown && nowMs - joinedAtMs >= 1500 && (statesReceived > 0 || nowMs - joinedAtMs >= 3000)) {
            emit_bare("mmo-ready"); phase = Phase::playing; invulnerableMs = 0;
        }
        accumulator = std::min(accumulator + dtSeconds, 6.0 / 60);
        while (accumulator >= 1.0 / 60) { step_frame(in, nowMs); accumulator -= 1.0 / 60; }
        if (invulnerableMs > 0) invulnerableMs -= dtSeconds * 1000;
        if (respawnCountdownMs > 0) respawnCountdownMs -= dtSeconds * 1000;
        // asteroid records follow the server tick; optimistic hides expire
        const int64_t tick = int64_t(std::floor(server_tick(nowMs)));
        for (auto &[id, a] : asteroids) { advance_to(a, tick, bounds); if (a.hiddenUntilMs && nowMs > a.hiddenUntilMs) a.hiddenUntilMs = 0; }
        // remote players / enemies from the snapshot buffers, 2 ticks behind
        // Others are drawn a little in the past so there is a later sample to blend toward:
        // players update every tick, enemies every 2; the extra tick absorbs network jitter.
        const double playerTick = server_tick(nowMs) - (velocityEntries ? player_delay_ticks : player_delay_no_velocity), enemyTick = server_tick(nowMs) - enemy_delay_ticks;
        for (auto &[id, p] : players) if (Sample s; playerBuf.sample(id, playerTick, s)) { p.x = s.x; p.y = s.y; p.angle = s.a; p.dead = s.dead; p.thrust = s.thrust; }
        for (auto &[id, e] : enemies) if (Sample s; enemyBuf.sample(id, enemyTick, s)) { e.x = s.x; e.y = s.y; e.a = s.a; }
        // Own pose once per server tick, timed to ARRIVE mid-tick: the server keeps the last update
        // it got before each 50 ms broadcast, so arrivals near the boundary jitter into the
        // wrong tick (two in one, none in the next) and other players see the ship stall.
        // Arrival tick = now + one-way delay; send as it crosses sendPhase. Unchanged poses
        // still go out at least once a second.
        const double oneWayTicks = clockKnown ? rttMs / 2 / tick_ms : 0;
        const int64_t sendTick = int64_t(std::floor(server_tick(nowMs) + oneWayTicks - sendPhase));
        if (phase == Phase::playing && !dead && sendTick != lastUpdateTick) {
            const double ax = std::round(x), ay = std::round(y), aa = round2(angle), avx = round2(vx), avy = round2(vy);
            const bool same = ax == sentPose[0] && ay == sentPose[1] && aa == sentPose[2] && avx == sentPose[3] && avy == sentPose[4] && thrusting == sentThrust;
            if (!same || nowMs - lastSentMs >= 1000) {
                json::Value arr; arr.type = json::Value::Type::array;
                for (double v : {ax, ay, aa, avx, avy, thrusting ? 1.0 : 0.0}) arr.items.push_back(json::Value::number_value(v));
                emit("mmo-player-update", std::move(arr));
                sentPose[0] = ax; sentPose[1] = ay; sentPose[2] = aa; sentPose[3] = avx; sentPose[4] = avy; sentThrust = thrusting;
                lastSentMs = nowMs;
            }
            lastUpdateTick = sendTick;
        }
    }

    std::vector<Outgoing> take_outbox() { std::vector<Outgoing> out; out.swap(outbox); return out; }
    bool asteroid_visible(const AsteroidRecord &a) const { return a.hiddenUntilMs == 0; }
    // Asteroid position extrapolated inside the current tick (game.js:2337-2348).
    void asteroid_render_pos(const AsteroidRecord &a, double nowMs, double &rx, double &ry, double &rot) const {
        const double frac = std::clamp(server_tick(nowMs) - double(a.tick), 0.0, 1.0) * frames_per_tick;
        rx = a.x + a.vx * frac; ry = a.y + a.vy * frac; rot = a.rotation + a.rotationSpeed * frac;
    }

private:
    double joinStartMs = 0, joinedAtMs = 0, offsetMs = 0, rttMs = 0, accumulator = 0;
    int64_t lastUpdateTick = -1;
    double lastSentMs = -1e9, sentPose[5] = {1e18, 0, 0, 0, 0};
    bool sentThrust = false, clockKnown = false;
    int syncSent = 0;
    std::vector<ClockSample> samples;

    static double round2(double v) { return std::round(v * 100) / 100; }
    static std::string id_of(const json::Value &v) {
        if (v.type == json::Value::Type::string) return v.s;
        char buf[32]; std::snprintf(buf, sizeof buf, "%.0f", v.n); return buf;
    }
    static std::string id_of_field(const json::Value &d, const char *key) { const auto *v = d.get(key); return v ? id_of(*v) : std::string{}; }
    void emit(const char *name, json::Value data) { outbox.push_back({name, std::move(data), true}); }
    void emit_bare(const char *name) { outbox.push_back({name, {}, false}); }
    void show(std::string text) { banner = std::move(text); bannerMs = 2500; }

    static bool read_asteroid(const json::Value &d, AsteroidRecord &a) {
        if (!d.is_object() || !d.get("id")) return false;
        a.id = int64_t(d.num("id")); a.x = d.num("x"); a.y = d.num("y"); a.vx = d.num("vx"); a.vy = d.num("vy");
        a.size = std::clamp(int(d.num("size", 3)), 1, 3);
        double radius, speed; size_props(a.size, radius, speed);
        a.radius = d.num("radius", radius);
        a.rotation = d.num("rotation"); a.rotationSpeed = d.num("rotationSpeed");
        a.seed = uint32_t(int64_t(d.num("seed")));
        a.tick = int64_t(d.num("spawnTick"));
        build_shape(a);
        return true;
    }
    void on_join(const json::Value &d, double nowMs) {
        const auto *p = d.get("player");
        if (!p || !p->is_object()) { phase = Phase::failed; failure = "malformed join reply"; return; }
        myId = p->str("id"); myIdx = int(p->num("idx", -1));
        x = p->num("x", x); y = p->num("y", y); angle = p->num("angle", 0); vx = vy = 0;
        mode = d.str("mode", "mmo"); round = int(d.num("round")); maxRounds = int(d.num("maxRounds", 10)); lives = int(d.num("lives"));
        if (const auto *wb = d.get("worldBounds")) { bounds.width = wb->num("width", 2000); bounds.height = wb->num("height", 1500); }
        const double tick = d.num("tick");
        offsetMs = d.num("serverTime", tick * tick_ms) - nowMs; // until time sync refines it
        asteroids.clear();
        if (const auto *list = d.get("asteroids"); list && list->is_array())
            for (const auto &item : list->items) { AsteroidRecord a; if (read_asteroid(item, a)) { if (!item.get("spawnTick")) a.tick = int64_t(tick); asteroids[a.id] = a; } }
        enemies.clear();
        if (const auto *list = d.get("enemies"); list && list->is_array())
            for (const auto &item : list->items) { Enemy e; e.id = id_of_field(item, "id"); e.x = item.num("x"); e.y = item.num("y"); e.a = item.num("angle"); enemies[e.id] = e; }
        players.clear();
        if (const auto *list = d.get("players"); list && list->is_array())
            for (const auto &item : list->items) {
                RemotePlayer rp; rp.id = item.str("id");
                if (rp.id.empty() || rp.id == myId) continue;
                rp.name = item.str("name", "Pilot").substr(0, 20); rp.x = item.num("x"); rp.y = item.num("y"); rp.angle = item.num("angle");
                rp.dead = item.get("dead") && item.get("dead")->b;
                if (const auto *sd = item.get("shipData")) rp.geometry = geometry_from(*sd);
                players[rp.id] = rp;
            }
        phase = Phase::warping; joinedAtMs = nowMs; invulnerableMs = 1e9;
        show(mode == "coop" ? "CO-OP ROUND " + std::to_string(round) : "ENTERING THE SHARED WORLD");
    }
    void on_time(const json::Value &d, double nowMs) {
        if (!d.is_object() || d.num("sentAt") < joinedAtMs) return;
        samples.push_back({d.num("sentAt"), nowMs, d.num("serverTime")});
    }
    void on_state(const json::Value &d) {
        if (!d.is_object()) return;
        const double t = d.num("t");
        statesReceived++;
        if (const auto *p = d.get("p"); p && p->is_array())
            for (const auto &e : p->items) {
                if (!e.is_array() || e.items.size() < 4) continue;
                const std::string id = id_of(e.items[0]);
                if (id == myId) continue;
                const bool isDead = e.items.size() > 4 && e.items[4].b;
                const bool thrust = e.items.size() > 5 && e.items[5].n == 1;
                // servers with smoothing append velocity: [id, x, y, a, dead, thrust, vx, vy]
                const double evx = e.items.size() > 7 ? e.items[6].n : 0, evy = e.items.size() > 7 ? e.items[7].n : 0;
                if (e.items.size() > 7) velocityEntries = true;
                playerBuf.push(t, id, e.items[1].n, e.items[2].n, e.items[3].n, isDead, thrust, evx, evy);
                if (auto it = players.find(id); it != players.end()) it->second.outOfView = false;
            }
        if (const auto *en = d.get("e"); en && en->is_array())
            for (const auto &e : en->items) {
                if (!e.is_array() || e.items.size() < 4) continue;
                const std::string id = id_of(e.items[0]);
                enemyBuf.push(t, id, e.items[1].n, e.items[2].n, e.items[3].n, false, false);
                auto &enemy = enemies[id]; enemy.id = id; enemy.outOfView = false;
            }
        if (const auto *g = d.get("g"); g && g->is_array())
            for (const auto &v : g->items) {
                const std::string id = id_of(v);
                if (auto it = players.find(id); it != players.end()) { it->second.outOfView = true; playerBuf.remove(id); }
                if (auto it = enemies.find(id); it != enemies.end()) { it->second.outOfView = true; enemyBuf.remove(id); }
            }
    }
    void on_asteroid_destroyed(const json::Value &d) {
        if (!d.is_array() || d.items.size() < 2) return;
        const int64_t id = int64_t(d.items[0].n), tick = int64_t(d.items[1].n);
        const int killer = d.items.size() > 2 ? int(d.items[2].n) : -1;
        auto it = asteroids.find(id);
        if (it == asteroids.end()) return;
        AsteroidRecord parent = it->second;
        advance_to(parent, tick, bounds);
        asteroids.erase(it);
        bursts.push_back({parent.x, parent.y, 0, parent.size >= 2});
        for (auto &c : split_asteroid(parent, tick)) asteroids[c.id] = c;
        // our kill that we did not predict: retire our closest round, as the browser does
        if (killer == myIdx && parent.hiddenUntilMs == 0) {
            size_t best = SIZE_MAX; double bestD = 1e18;
            for (size_t i = 0; i < bullets.size(); i++) if (bullets[i].owner.empty() && !bullets[i].hostile) {
                const double dd = std::hypot(bullets[i].x - parent.x, bullets[i].y - parent.y);
                if (dd < bestD) { bestD = dd; best = i; }
            }
            if (best != SIZE_MAX && bestD < parent.radius + 60) bullets.erase(bullets.begin() + long(best));
        }
    }
    void on_asteroid_check(const json::Value &d) {
        const auto *a = d.get("a");
        if (!a || !a->is_array()) return;
        const int64_t tick = int64_t(d.num("tick"));
        for (const auto &e : a->items) {
            if (!e.is_array() || e.items.size() < 3) continue;
            auto it = asteroids.find(int64_t(e.items[0].n));
            if (it == asteroids.end()) continue;
            AsteroidRecord &r = it->second;
            if (r.tick > tick) continue; // compare at the same tick only
            advance_to(r, tick, bounds);
            if (std::hypot(r.x - e.items[1].n, r.y - e.items[2].n) > 1.5) { r.x = e.items[1].n; r.y = e.items[2].n; }
        }
    }

    // One 60 Hz frame of the local ship and every bullet (ship.js / bullet.js in a world).
    void step_frame(const Controls &in, double nowMs) {
        const double dt = 1.0 / 60;
        if (!dead) {
            angle += std::clamp(double(in.turn), -1.0, 1.0) * 0.08;
            thrusting = in.thrust;
            if (thrusting) {
                vx += 0.1 * std::sin(angle); vy -= 0.1 * std::cos(angle);
                const double sp = std::hypot(vx, vy);
                if (sp > 5) { vx *= 5 / sp; vy *= 5 / sp; }
            } else {
                vx *= 0.99; vy *= 0.99;
                if (std::fabs(vx) < 0.01) vx = 0;
                if (std::fabs(vy) < 0.01) vy = 0;
            }
            x = std::clamp(x + vx, ship_radius, bounds.width - ship_radius);
            y = std::clamp(y + vy, ship_radius, bounds.height - ship_radius);
            if (shootCooldown > 0) shootCooldown -= dt;
            if (phase == Phase::playing && in.fire && shootCooldown <= 0) {
                shootCooldown = 0.25;
                for (auto &b : create_shot_bullets(x, y, angle, vx, vy, design.geometry.weapons)) bullets.push_back(b);
                json::Value s = json::Value::object();
                s.set("x", json::Value::number_value(x)); s.set("y", json::Value::number_value(y));
                s.set("angle", json::Value::number_value(angle));
                s.set("vx", json::Value::number_value(vx)); s.set("vy", json::Value::number_value(vy));
                s.set("tick", json::Value::number_value(std::floor(server_tick(nowMs))));
                emit("mmo-shoot", std::move(s)); shotsSent++;
            }
        }
        for (auto &b : bullets) step_bullet_frame(b, dt);
        std::erase_if(bullets, [&](const Bullet &b) {
            return b.life >= b.maxLife || b.speed <= 0.5 || b.x < -3 || b.x > bounds.width + 3 || b.y < -3 || b.y > bounds.height + 3;
        });
        // own rounds: hide what they hit until the server confirms (or not)
        for (size_t i = bullets.size(); i-- > 0;) {
            const Bullet &b = bullets[i];
            if (b.hostile) continue;
            bool hit = false;
            for (auto &[id, a] : asteroids) {
                if (!asteroid_visible(a) || std::hypot(b.x - a.x, b.y - a.y) >= 3 + a.radius) continue;
                if (b.owner.empty()) a.hiddenUntilMs = nowMs + 300 + (clockKnown ? rttMs : 700);
                hit = true; break;
            }
            if (!hit) for (auto &[id, e] : enemies) if (!e.outOfView && std::hypot(b.x - e.x, b.y - e.y) < 3 + 15) { hit = true; break; }
            if (hit) bullets.erase(bullets.begin() + long(i));
        }
        if (phase != Phase::playing || dead || invulnerableMs > 0) return;
        // own death: report what we touched; the server checks plausibility
        auto report = [&](const char *type, json::Value objectId, bool collision) {
            json::Value r = json::Value::object();
            r.set("type", json::Value::string_value(type)); r.set("objectId", std::move(objectId));
            r.set("x", json::Value::number_value(std::round(x))); r.set("y", json::Value::number_value(std::round(y)));
            if (collision) emit("mmo-ship-collision", r);
            emit("mmo-player-died", std::move(r));
            dead = true; thrusting = false; respawnCountdownMs = 3000;
        };
        ShipGeometry g = design.geometry;
        auto touches = [&](double cx, double cy, double r) {
            if (!g.custom()) return std::hypot(x - cx, y - cy) < ship_radius + r;
            const double c = std::cos(angle), sn = std::sin(angle);
            for (const auto &l : g.lines)
                if (segment_hits_circle(float(x + l.x1 * c - l.y1 * sn), float(y + l.x1 * sn + l.y1 * c),
                                        float(x + l.x2 * c - l.y2 * sn), float(y + l.x2 * sn + l.y2 * c), float(cx), float(cy), float(r))) return true;
            return std::hypot(x - cx, y - cy) < ship_radius;
        };
        for (auto &[id, a] : asteroids) if (asteroid_visible(a) && touches(a.x, a.y, a.radius)) { report("asteroid", json::Value::number_value(double(id)), true); return; }
        for (auto &[id, e] : enemies) if (!e.outOfView && touches(e.x, e.y, 15)) {
            json::Value v = json::Value::string_value(id);
            char *end = nullptr; const double num = std::strtod(id.c_str(), &end);
            if (end && *end == 0) v = json::Value::number_value(num);
            report("enemy", std::move(v), true); return;
        }
        for (size_t i = 0; i < bullets.size(); i++) if (bullets[i].hostile && touches(bullets[i].x, bullets[i].y, 3)) {
            json::Value v = json::Value::string_value(bullets[i].owner);
            char *end = nullptr; const double num = std::strtod(bullets[i].owner.c_str(), &end);
            if (end && *end == 0) v = json::Value::number_value(num);
            bullets.erase(bullets.begin() + long(i));
            report("enemyBullet", std::move(v), false); return;
        }
    }
};
}

#if defined(__clang__)
#pragma clang fp contract(on)
#endif
