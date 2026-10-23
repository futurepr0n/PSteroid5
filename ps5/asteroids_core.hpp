#pragma once
// Portable CSSteroids simulation, translated from public/js/entities/*.js and
// public/js/core/game.js (single-player). The JS physics is per animation frame,
// so the core steps at a fixed 60 Hz and the renderer interpolates.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cssteroids {
constexpr float board_width = 1920.f, board_height = 1080.f;
constexpr float step_seconds = 1.f / 60.f;
constexpr float pi = 3.14159265358979f;

// mulberry32, as Asteroid.seededRandom: same seed -> same sequence as the browser
struct Rng {
    uint32_t a;
    explicit Rng(uint32_t seed = 1) : a(seed) {}
    float next() {
        a += 0x6D2B79F5u;
        uint32_t t = a;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return float(double(t ^ (t >> 14)) / 4294967296.0);
    }
};

enum class ShipShape { standard, triangle, diamond, custom };

struct Point { float x = 0, y = 0; };
struct Segment { float x1, y1, x2, y2; uint32_t color; };

// A ship's look and gameplay geometry, in ship space (angle 0 = up, already at
// the browser's 0.25 design scale). Built from server/customizer JSON elsewhere.
struct ShipGeometry {
    ShipShape shape = ShipShape::standard;
    uint32_t color = 0xffffff;
    std::vector<Segment> lines;    // custom ships only
    std::vector<Point> weapons;    // empty: one round from the nose
    std::vector<Point> thrusters;  // empty: default flame position(s)
    bool custom() const { return shape == ShipShape::custom && !lines.empty(); }
};

inline bool segment_hits_circle(double x1, double y1, double x2, double y2, double cx, double cy, double r) {
    const double dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((cx - x1) * dx + (cy - y1) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(x1 + t * dx - cx, y1 + t * dy - cy) <= r;
}

// Ship.checkCollision: custom ships test each rotated line, then a centre-in-radius fallback;
// built-in shapes use the 15 px circle.
inline bool ship_hits_circle(const ShipGeometry &g, double sx, double sy, double angle, double cx, double cy, double r, double radius = 15) {
    if (!g.custom()) return std::hypot(sx - cx, sy - cy) < radius + r;
    const double c = std::cos(angle), s = std::sin(angle);
    for (const auto &l : g.lines)
        if (segment_hits_circle(sx + l.x1 * c - l.y1 * s, sy + l.x1 * s + l.y1 * c,
                                sx + l.x2 * c - l.y2 * s, sy + l.x2 * s + l.y2 * c, cx, cy, r)) return true;
    return std::hypot(sx - cx, sy - cy) < radius;
}

struct Controls {
    float turn = 0;      // -1 left .. 1 right; 1 == Ship.rotationSpeed per frame
    bool thrust = false;
    bool fire = false;
};

struct Ship {
    float x = board_width / 2, y = board_height / 2, angle = 0; // angle 0 = up
    float vx = 0, vy = 0;
    float radius = 15;
    bool thrusting = false, exploding = false;
    float shootTimer = 0, explodeTime = 0, invulnerableTime = 0;
    bool invulnerable() const { return invulnerableTime > 0; }
};

struct Asteroid {
    float x = 0, y = 0, vx = 0, vy = 0, radius = 40, rotation = 0;
    int size = 3, vertices = 6;
    float offsets[8] = {};
};

// Bullet.js: the first frame carries half the ship's velocity; update() then
// recomputes velocity from the angle alone, so later frames do not.
struct Bullet {
    float x, y, angle, speed, initialSpeed, life, maxLife;
    float vx0 = 0, vy0 = 0;
};

struct Debris {
    float x, y, vx, vy, rotation, rotationSpeed, length, life;
    uint32_t color;
};

enum class Event { shot, asteroid_hit, ship_hit, level_up, game_over };

inline void asteroid_size(int size, float &radius, float &speed) {
    if (size == 3) { radius = 40; speed = 1; }
    else if (size == 2) { radius = 25; speed = 1.5f; }
    else { radius = 15; speed = 2; }
}

class Game {
public:
    Rng rng;
    Ship ship;
    ShipGeometry geometry;
    std::vector<Asteroid> asteroids;
    std::vector<Bullet> bullets;
    std::vector<Debris> debris;
    std::vector<Event> events; // cleared by the caller each rendered frame
    int score = 0, lives = 3, level = 1;
    bool over = false;
    float levelBanner = 0, time = 0;

    explicit Game(uint32_t seed = 1) : rng(seed) { reset(); }

    void reset() {
        ship = Ship{};
        bullets.clear(); debris.clear(); events.clear();
        score = 0; lives = 3; level = 1; over = false; time = 0;
        levelBanner = 2;
        spawn_level();
    }

    Asteroid make_asteroid(float x, float y, int size) {
        Asteroid a;
        a.x = x; a.y = y; a.size = size;
        float speed;
        asteroid_size(size, a.radius, speed);
        const float heading = rng.next() * pi * 2;
        a.vx = std::cos(heading) * speed;
        a.vy = std::sin(heading) * speed;
        a.vertices = int(rng.next() * 3) + 6;
        const float jagged = rng.next() * .2f + .1f;
        for (int i = 0; i < a.vertices; i++) a.offsets[i] = a.radius * (1 - jagged + rng.next() * jagged);
        return a;
    }

    // createAsteroidsForLevel: 2 + level large asteroids, not within 20% of the height of the ship
    void spawn_level() {
        asteroids.clear();
        for (int i = 0; i < 2 + level; i++) {
            float x, y;
            do { x = rng.next() * board_width; y = rng.next() * board_height; }
            while (std::hypot(x - ship.x, y - ship.y) < board_height * .2f);
            asteroids.push_back(make_asteroid(x, y, 3));
        }
    }

    void step(const Controls &in) {
        time += step_seconds;
        if (levelBanner > 0) levelBanner -= step_seconds;
        for (auto &d : debris) {
            d.x += d.vx; d.y += d.vy; d.rotation += d.rotationSpeed; d.life -= step_seconds;
        }
        std::erase_if(debris, [](const Debris &d) { return d.life <= 0; });
        if (over) return;
        step_ship(in);
        for (auto &a : asteroids) {
            a.x += a.vx; a.y += a.vy;
            wrap(a.x, a.y, a.radius);
        }
        for (auto &b : bullets) {
            b.x += std::sin(b.angle) * b.speed + (b.life == 0 ? b.vx0 : 0);
            b.y += -std::cos(b.angle) * b.speed + (b.life == 0 ? b.vy0 : 0);
            b.speed *= .99f;
            b.life += step_seconds;
        }
        std::erase_if(bullets, [](const Bullet &b) {
            return b.life >= b.maxLife || b.speed <= .5f || b.x < 0 || b.x > board_width || b.y < 0 || b.y > board_height;
        });
        collide();
        if (!ship.exploding && asteroids.empty()) {
            level++;
            levelBanner = 2;
            spawn_level();
            events.push_back(Event::level_up);
        }
    }

private:
    void step_ship(const Controls &in) {
        Ship &s = ship;
        if (s.exploding) {
            s.explodeTime += step_seconds;
            if (s.explodeTime > 1) respawn();
            return;
        }
        s.angle += std::clamp(in.turn, -1.f, 1.f) * .08f;
        s.thrusting = in.thrust;
        if (s.thrusting) {
            s.vx += .1f * std::sin(s.angle);
            s.vy -= .1f * std::cos(s.angle);
            const float speed = std::hypot(s.vx, s.vy);
            if (speed > 5) { s.vx *= 5 / speed; s.vy *= 5 / speed; }
        } else {
            s.vx *= .99f; s.vy *= .99f;
            if (std::fabs(s.vx) < .01f) s.vx = 0;
            if (std::fabs(s.vy) < .01f) s.vy = 0;
        }
        s.x += s.vx; s.y += s.vy;
        wrap(s.x, s.y, s.radius);
        if (s.shootTimer > 0) s.shootTimer -= step_seconds;
        if (s.invulnerableTime > 0) s.invulnerableTime -= step_seconds;
        if (in.fire && s.shootTimer <= 0) {
            s.shootTimer = .25f;
            fire_from(s);
            events.push_back(Event::shot);
        }
    }

    // WorldSim.createShotBullets + Bullet (single-player speeds): one round from
    // the nose, or one per weapon point with 0.75x speed and 0.7x life when several.
    void fire_from(const Ship &s) {
        const float sn = std::sin(s.angle), cs = std::cos(s.angle);
        const bool multi = geometry.weapons.size() > 1;
        const float speed = multi ? 4.5f : 6.f, life = multi ? 1.05f : 1.5f;
        auto add = [&](float x, float y) {
            Bullet b{x, y, s.angle, speed, speed, 0, life};
            b.vx0 = s.vx * .5f; b.vy0 = s.vy * .5f;
            bullets.push_back(b);
        };
        if (geometry.weapons.empty()) add(s.x + sn * s.radius, s.y - cs * s.radius);
        for (const auto &p : geometry.weapons) add(s.x + p.x * cs - p.y * sn, s.y + p.x * sn + p.y * cs);
    }

    bool ship_hits(float cx, float cy, float r) const { return ship_hits_circle(geometry, ship.x, ship.y, ship.angle, cx, cy, r, ship.radius); }

    void respawn() {
        lives--;
        if (lives <= 0) {
            lives = 0; over = true; ship.exploding = false;
            events.push_back(Event::game_over);
            return;
        }
        ship = Ship{};
        ship.invulnerableTime = 3;
    }

    static void wrap(float &x, float &y, float r) {
        if (x < -r) x = board_width + r; else if (x > board_width + r) x = -r;
        if (y < -r) y = board_height + r; else if (y > board_height + r) y = -r;
    }

    void burst(float x, float y, int count, bool colourful) {
        for (int i = 0; i < count; i++) {
            const float angle = rng.next() * pi * 2, speed = .5f + rng.next() * 1.5f;
            uint32_t color = colourful ? (i % 2 ? 0xff0000u : 0xffa500u) : (rng.next() > .5f ? 0xffffffu : 0xff4400u);
            debris.push_back({x, y, std::cos(angle) * speed, std::sin(angle) * speed, angle,
                              (rng.next() - .5f) * .2f, 5 + rng.next() * 10, 1.f, color});
        }
    }

    void split(size_t index) {
        const Asteroid a = asteroids[index];
        score += (4 - a.size) * 100;
        burst(a.x, a.y, a.size * 5, false);
        asteroids.erase(asteroids.begin() + long(index));
        if (a.size > 1) for (int i = 0; i < 2; i++) asteroids.push_back(make_asteroid(a.x, a.y, a.size - 1));
        events.push_back(Event::asteroid_hit);
    }

    void collide() {
        if (!ship.exploding && !ship.invulnerable()) {
            for (const auto &a : asteroids) {
                if (ship_hits(a.x, a.y, a.radius)) {
                    ship.exploding = true; ship.explodeTime = 0;
                    burst(ship.x, ship.y, 20, true);
                    events.push_back(Event::ship_hit);
                    break;
                }
            }
        }
        for (size_t i = bullets.size(); i-- > 0;) {
            const Bullet &b = bullets[i];
            for (size_t j = asteroids.size(); j-- > 0;) {
                if (std::hypot(b.x - asteroids[j].x, b.y - asteroids[j].y) < 3 + asteroids[j].radius) {
                    bullets.erase(bullets.begin() + long(i));
                    split(j);
                    break;
                }
            }
        }
    }
};
}
