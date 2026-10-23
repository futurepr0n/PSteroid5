// Host tests for the portable simulation (g++ -std=c++20).
#include "asteroids_core.hpp"
#include <cstdio>
#include <cstdlib>

using namespace cssteroids;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    // mulberry32 matches Asteroid.seededRandom(1): first value 0.6270739405881613
    { Rng r(1); CHECK(std::fabs(r.next() - 0.62707394f) < 1e-6f); }

    // Level 1: three large asteroids, none spawned on the ship
    {
        Game g(7);
        CHECK(g.asteroids.size() == 3);
        for (auto &a : g.asteroids) {
            CHECK(a.size == 3 && a.radius == 40);
            CHECK(std::hypot(a.x - g.ship.x, a.y - g.ship.y) >= board_height * .2f);
            CHECK(a.vertices >= 6 && a.vertices <= 8);
        }
    }
    // Rotation and thrust are per-frame like ship.js, capped at max speed 5
    {
        Game g(1); g.asteroids.clear();
        Controls c; c.turn = 1;
        g.step(c);
        CHECK(std::fabs(g.ship.angle - .08f) < 1e-6f);
        g.ship.angle = 0;
        c = {}; c.thrust = true;
        for (int i = 0; i < 200; i++) { g.asteroids.clear(); g.step(c); }
        CHECK(std::fabs(std::hypot(g.ship.vx, g.ship.vy) - 5) < 1e-3f);
        CHECK(g.ship.vy < 0); // angle 0 points up
    }
    // Ship wraps around the board edges
    {
        Game g(1); g.ship.x = -20; g.ship.vx = -1; g.asteroids = {Asteroid{}}; g.asteroids[0].x = 900; g.asteroids[0].y = 900;
        g.step({});
        CHECK(g.ship.x > board_width);
    }
    // Firing: 0.25 s cooldown, nose origin, bullet expires by 1.5 s
    {
        Game g(1);
        Controls c; c.fire = true;
        g.asteroids.clear(); g.step(c);
        CHECK(g.bullets.size() == 1);
        CHECK(std::fabs(g.bullets[0].x - board_width / 2) < 1e-3f);
        int shots = 1;
        for (int i = 0; i < 59; i++) { g.asteroids = {Asteroid{}}; g.asteroids[0].x = 50; g.asteroids[0].y = 50; size_t n = g.bullets.size(); g.step(c); if (g.bullets.size() > n) shots++; }
        CHECK(shots == 4 || shots == 5);
    }
    // A hit splits large -> two medium and scores (4 - size) * 100
    {
        Game g(3);
        g.asteroids.resize(1);
        g.asteroids[0].x = 960; g.asteroids[0].y = 300; g.asteroids[0].vx = g.asteroids[0].vy = 0;
        Controls c; c.fire = true;
        for (int i = 0; i < 90 && g.score == 0; i++) g.step(i == 0 ? c : Controls{});
        CHECK(g.score == 100);
        CHECK(g.asteroids.size() == 2);
        for (auto &a : g.asteroids) CHECK(a.size == 2 && a.radius == 25);
        CHECK(!g.debris.empty());
    }
    // Clearing the board advances the level with 2 + level asteroids
    {
        Game g(4);
        g.asteroids.resize(1);
        g.asteroids[0] = Asteroid{}; g.asteroids[0].size = 1; g.asteroids[0].radius = 15;
        g.asteroids[0].x = 960; g.asteroids[0].y = 400;
        Controls c; c.fire = true;
        for (int i = 0; i < 90 && g.level == 1; i++) g.step(c);
        CHECK(g.level == 2);
        CHECK(g.asteroids.size() == 4);
        CHECK(g.score == 300);
    }
    // Collision costs exactly one life after the 1 s explosion; respawn is invulnerable
    {
        Game g(5);
        g.asteroids.resize(1);
        g.asteroids[0].x = g.ship.x; g.asteroids[0].y = g.ship.y; g.asteroids[0].vx = g.asteroids[0].vy = 0;
        g.step({});
        CHECK(g.ship.exploding);
        for (int i = 0; i < 70; i++) g.step({});
        CHECK(g.lives == 2);
        CHECK(g.ship.invulnerable());
        CHECK(!g.ship.exploding);
    }
    // Three hits end the game
    {
        Game g(6);
        for (int hit = 0; hit < 3; hit++) {
            g.ship.invulnerableTime = 0;
            g.asteroids.resize(1);
            g.asteroids[0].x = g.ship.x; g.asteroids[0].y = g.ship.y; g.asteroids[0].vx = g.asteroids[0].vy = 0;
            for (int i = 0; i < 70; i++) g.step({});
        }
        CHECK(g.over && g.lives == 0);
    }
    // Weapon points: one round per point, rotated with the ship, 0.75x speed when several
    {
        Game g(11); g.asteroids.clear();
        g.geometry.weapons = {{-10, 0}, {10, 0}};
        g.ship.angle = pi / 2; // facing right: ship-space +x maps to screen +y
        Controls c; c.fire = true;
        g.step(c);
        CHECK(g.bullets.size() == 2);
        CHECK(std::fabs(g.bullets[0].y - (board_height / 2 - 10)) < 1e-3f && std::fabs(g.bullets[1].y - (board_height / 2 + 10)) < 1e-3f);
        CHECK(g.bullets[0].initialSpeed == 4.5f && g.bullets[0].maxLife == 1.05f);
        g.geometry.weapons = {{0, -20}};
        g.bullets.clear(); g.ship.shootTimer = 0; g.step(c);
        CHECK(g.bullets.size() == 1 && g.bullets[0].initialSpeed == 6.f);
    }
    // Custom ships collide by their rotated lines, not the 15 px circle
    {
        Game g(12);
        g.geometry.shape = ShipShape::custom;
        g.geometry.lines = {{0, -60, 0, 0, 0xffffff}}; // a long nose spike straight up
        g.asteroids.resize(1);
        auto &a = g.asteroids[0]; a.vx = a.vy = 0; a.radius = 15;
        a.x = g.ship.x; a.y = g.ship.y - 65;        // touches the spike tip, far outside 15+15
        g.step({});
        CHECK(g.ship.exploding);
        Game h(12);
        h.geometry = g.geometry; h.ship.angle = pi;  // spike now points down
        h.asteroids.resize(1);
        auto &b = h.asteroids[0]; b.vx = b.vy = 0; b.radius = 15; b.x = h.ship.x; b.y = h.ship.y - 65;
        h.step({});
        CHECK(!h.ship.exploding);
    }
    // Long seeded runs stay finite and bounded
    {
        Game g(9);
        Controls c; c.thrust = true; c.fire = true; c.turn = .3f;
        for (int i = 0; i < 60 * 120; i++) {
            g.step(c);
            CHECK(std::isfinite(g.ship.x) && std::isfinite(g.ship.y));
            if (g.over) g.reset();
            g.events.clear();
        }
        CHECK(g.bullets.size() < 16);
    }
    if (failures) { std::printf("%d core test failure(s)\n", failures); return 1; }
    std::puts("core tests passed");
    return 0;
}
