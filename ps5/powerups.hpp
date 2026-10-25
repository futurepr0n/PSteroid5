#pragma once
// Power-up rules and line art, translated from CSSteroids-2 public/js/shared/powerups.js
// (shared by the server engine and the browser). Single player and the shared world both
// use these; the numbers and the art must stay identical to the JS.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// stepPowerUp and applyWeaponEffects must round like Node's doubles (see mmo_world.hpp).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace cssteroids::powerups {
enum class Type { life, rapid, spray, shield };
constexpr std::array<Type, 4> types = {Type::life, Type::rapid, Type::spray, Type::shield};
enum class Source { asteroid, enemy };

constexpr double radius = 14, ship_radius = 15;
constexpr double life_ms = 12000, blink_ms = 3000;
constexpr double max_drift = 0.5; // px per frame
constexpr double asteroid_drop_chance = 0.10, enemy_drop_chance = 0.25;
constexpr double rapid_speed_mult = 2;
constexpr double rotation_speed = 0.02; // radians per frame
constexpr double js_pi = 3.141592653589793;
// [-40, -20, 0, 20, 40].map(d => d * Math.PI / 180)
const std::array<double, 5> spray_angles = {-40 * js_pi / 180, -20 * js_pi / 180, 0 * js_pi / 180, 20 * js_pi / 180, 40 * js_pi / 180};

struct Line { double x1, y1, x2, y2; uint32_t color; };

struct Def { const char *key, *label; uint32_t color; double durationMs; }; // durationMs 0: instant
inline const Def &def(Type t) {
    static const Def defs[4] = {
        {"life", "Extra Life", 0xff4d6d, 0},
        {"rapid", "Rapid", 0xffd60a, 20000},
        {"spray", "Spray", 0x4cc9f0, 20000},
        {"shield", "Shield", 0x80ffdb, 30000},
    };
    return defs[int(t)];
}
inline std::optional<Type> type_from(const std::string &key) {
    for (Type t : types) if (key == def(t).key) return t;
    return std::nullopt;
}

// Default line art in design units (drawn at 0.25 scale around the power-up's centre).
inline const std::vector<Line> &lines(Type t) {
    using Pts = std::vector<std::array<double, 2>>;
    auto chain = [](std::vector<Line> &out, const Pts &p, uint32_t c) {
        for (size_t i = 1; i < p.size(); i++) out.push_back({p[i - 1][0], p[i - 1][1], p[i][0], p[i][1], c});
    };
    static const std::array<std::vector<Line>, 4> art = [&] {
        std::array<std::vector<Line>, 4> a;
        chain(a[0], {{0, 32}, {-32, 0}, {-32, -16}, {-20, -30}, {-8, -30}, {0, -18}, {8, -30}, {20, -30}, {32, -16}, {32, 0}, {0, 32}}, def(Type::life).color);
        chain(a[1], {{-28, 0}, {0, -28}, {28, 0}}, def(Type::rapid).color);
        chain(a[1], {{-28, 24}, {0, -4}, {28, 24}}, def(Type::rapid).color);
        auto js_round = [](double v) { return std::floor(v + 0.5); };
        for (double s : spray_angles) a[2].push_back({0, 28, js_round(std::sin(s) * 56), js_round(28 - std::cos(s) * 56), def(Type::spray).color});
        chain(a[3], {{0, -34}, {28, -20}, {28, 6}, {0, 34}, {-28, 6}, {-28, -20}, {0, -34}}, def(Type::shield).color);
        return a;
    }();
    return art[int(t)];
}

// rand returns [0, 1); one draw for the chance, one for the type (as rollDrop).
template <class Rand>
inline std::optional<Type> roll_drop(Source source, Rand &&rand, const std::vector<Type> &allowed = {types.begin(), types.end()}) {
    const double chance = source == Source::asteroid ? asteroid_drop_chance : enemy_drop_chance;
    if (allowed.empty() || double(rand()) >= chance) return std::nullopt;
    return allowed[size_t(std::floor(double(rand()) * double(allowed.size())))];
}

struct PowerUp {
    int64_t id = 0;
    Type type = Type::rapid;
    double x = 0, y = 0, vx = 0, vy = 0;
    double rotation = 0, ageMs = 0;
    int64_t simTick = 0, expireTick = 0; // shared world: record tick and server expiry
    double claimedAtMs = -1e18;          // shared world: last pickup claim sent
};

// stepPowerUp: the asteroid edge bounce with the power-up radius. The server calls it once per
// tick with frames = 3 (not three calls of 1: the bounce differs).
inline void step(PowerUp &p, double width, double height, double frames = 1) {
    p.x += p.vx * frames;
    p.y += p.vy * frames;
    if (p.x < radius) { p.x = radius; p.vx = std::fabs(p.vx); }
    else if (p.x > width - radius) { p.x = width - radius; p.vx = -std::fabs(p.vx); }
    if (p.y < radius) { p.y = radius; p.vy = std::fabs(p.vy); }
    else if (p.y > height - radius) { p.y = height - radius; p.vy = -std::fabs(p.vy); }
}

// syncPowerUpToTick: one step of framesPerTick per server tick since simTick.
inline void sync_to_tick(PowerUp &p, int64_t tick, double width, double height, double framesPerTick) {
    for (; p.simTick < tick; p.simTick++) step(p, width, height, framesPerTick);
}

inline bool is_pickup(double shipX, double shipY, const PowerUp &p) {
    return std::hypot(shipX - p.x, shipY - p.y) <= ship_radius + radius;
}
inline bool blinked_out(double ageMs) {
    return ageMs > life_ms - blink_ms && int64_t(std::floor(ageMs / 150)) % 2 == 1;
}

struct WeaponEffects { bool rapid = false, spray = false; };

// applyWeaponEffects for bullets with x, y, angle, speed and vx/vy that include the ship carry:
// each base bullet becomes one per spray offset, at rapid speed, keeping the carry.
template <class B>
inline std::vector<B> apply_weapon_effects(const std::vector<B> &bullets, WeaponEffects e) {
    const double mult = e.rapid ? rapid_speed_mult : 1;
    std::vector<B> out;
    for (const auto &b : bullets) {
        const double carryX = b.vx - std::sin(b.angle) * b.speed, carryY = b.vy + std::cos(b.angle) * b.speed;
        for (size_t i = 0; i < spray_angles.size(); i++) {
            if (!e.spray && i != 2) continue;
            B n = b;
            n.angle = b.angle + spray_angles[i];
            n.speed = b.speed * mult;
            n.vx = std::sin(n.angle) * n.speed + carryX;
            n.vy = -std::cos(n.angle) * n.speed + carryY;
            out.push_back(n);
        }
    }
    return out;
}

// Timed effects: single player counts milliseconds down; the shared world keeps server end ticks.
struct Effects {
    double ms[4] = {};
    bool active(Type t) const { return ms[int(t)] > 0; }
    WeaponEffects weapons() const { return {active(Type::rapid), active(Type::spray)}; }
    void add(Type t) { ms[int(t)] = def(t).durationMs; }
    void clear() { *this = Effects{}; }
    void tick(double dtMs) { for (double &m : ms) if (m > 0) m = std::max(0.0, m - dtMs); }
};
struct EffectTicks {
    double until[4] = {}; // server tick each effect ends; 0 = none
    void clear() { *this = EffectTicks{}; }
    // effectsMsLeft
    Effects left(double tickTime, double tickMs) const {
        Effects e;
        if (!std::isfinite(tickTime)) return e;
        for (int i = 0; i < 4; i++) if (until[i] > tickTime) e.ms[i] = (until[i] - tickTime) * tickMs;
        return e;
    }
};

// Ring and art in world space for the renderer (PowerUp.draw: art at 0.25, rotated).
inline std::vector<Line> art_segments(Type t, double x, double y, double rotation) {
    const double c = std::cos(rotation), s = std::sin(rotation);
    std::vector<Line> out;
    for (const auto &l : lines(t)) {
        const double x1 = l.x1 * 0.25, y1 = l.y1 * 0.25, x2 = l.x2 * 0.25, y2 = l.y2 * 0.25;
        out.push_back({x + x1 * c - y1 * s, y + x1 * s + y1 * c, x + x2 * c - y2 * s, y + x2 * s + y2 * c, l.color});
    }
    return out;
}
// Ship.drawShield: hidden on alternate 150 ms in the last 3 s.
inline bool shield_visible(double msLeft) { return msLeft > 0 && !(msLeft < blink_ms && int64_t(std::floor(msLeft / 150)) % 2 == 1); }
constexpr double shield_radius = ship_radius + 8;
}

#if defined(__clang__)
#pragma clang fp contract(on)
#endif
