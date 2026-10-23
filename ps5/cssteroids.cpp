// CSSteroids native PS5 title: vector Asteroids from CSSteroids-2 (public/js),
// with high scores read from the existing CSSteroids server over HTTP.
// Rendering uses the PS5 Vulkan Template and PS5_VKHomebrewUI, via the BattleStar shell.
#include "kit.hpp"

class VulkanExample : public ps5ui::KitExample {
    cssteroids::Game game{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    cssteroids::ScoreStore scores;
    cssteroids::net::Endpoint server;
    cssteroids::net::AsyncRequest fetch;
    std::vector<cssteroids::api::ScoreRow> board;
    std::string onlineStatus;
    hui::gfx::DrawList scene;
    bool playing = false, paused = false, savedRun = false;
    float accumulator = 0, exitHold = 0;

    void text(const std::string &s, float x, float y, float size, uint32_t color = 0xffffff, float alpha = 1) {
        hui::ui::text(scene, kit.fonts.regular, s, x, y, size, hui::gfx::Color::rgb(color, alpha));
    }
    void centered(const std::string &s, float y, float size, uint32_t color = 0xffffff) {
        text(s, (1920.f - kit.fonts.regular.font->measure(s, size)) * .5f, y, size, color);
    }
    void requestScores() {
        if (!server.valid()) { onlineStatus = "OFFLINE - add server.txt (e.g. 192.168.1.50:6161)"; return; }
        if (fetch.start(server, "GET", cssteroids::api::high_scores_path)) onlineStatus = "Contacting " + server.label() + " ...";
    }
    void pollScores() {
        cssteroids::net::Response r;
        if (!fetch.take(r)) return;
        if (!r.ok()) {
            onlineStatus = r.status ? "Server error " + std::to_string(r.status) : "Server unreachable: " + r.error;
            say("CSSteroids high scores failed: status %d %s", r.status, r.error.c_str());
        } else if (!cssteroids::api::parse_high_scores(r.body, board)) {
            onlineStatus = "Server sent unreadable scores";
        } else {
            onlineStatus = "ONLINE  " + server.label();
            say("CSSteroids high scores: %zu rows from %s", board.size(), server.label().c_str());
        }
    }

    // Ship outlines from ship.js, in ship space (angle 0 = up), scaled 1.5x for 1080p.
    void drawShip(const cssteroids::Ship &s, uint32_t color) {
        using hui::gfx::Color;
        const float r = s.radius, k = 1.5f, c = std::cos(s.angle), sn = std::sin(s.angle);
        auto at = [&](float x, float y) { return std::array<float, 2>{s.x + (x * c - y * sn) * k, s.y + (x * sn + y * c) * k}; };
        const std::array<float, 2> hull[3] = {at(0, -r), at(-r * .7f, r * .7f), at(r * .7f, r * .7f)};
        for (int i = 0; i < 3; i++) scene.line(hull[i][0], hull[i][1], hull[(i + 1) % 3][0], hull[(i + 1) % 3][1], 3, Color::rgb(color));
        if (s.thrusting) {
            const float flicker = .7f + .3f * std::fabs(std::sin(game.time * 47));
            auto base = at(0, r * .7f), tip = at(0, r * .7f + r * .6f * flicker * 1.6f);
            scene.line(base[0], base[1], tip[0], tip[1], 5, Color::rgb(0xffa500, .85f));
            scene.glow({tip[0] - 6, tip[1] - 6, 12, 12}, 6, 10, Color::rgb(0xff4000, .6f));
        }
    }
    void drawWorld() {
        using hui::gfx::Color;
        for (const auto &a : game.asteroids) {
            float pts[16];
            for (int i = 0; i < a.vertices; i++) {
                const float angle = i * cssteroids::pi * 2 / a.vertices + a.rotation;
                pts[i * 2] = a.x + a.offsets[i] * std::cos(angle);
                pts[i * 2 + 1] = a.y + a.offsets[i] * std::sin(angle);
            }
            for (int i = 0; i < a.vertices; i++) {
                const int j = (i + 1) % a.vertices;
                scene.line(pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], 2.5f, Color::rgb(0xffffff));
            }
        }
        for (const auto &b : game.bullets) {
            const float ratio = b.speed / b.initialSpeed;
            const uint32_t color = ratio > .8f ? 0xff4500 : ratio > .6f ? 0xffa500 : ratio > .4f ? 0xffd700 : 0xffffff;
            const float tail = 12 * ratio;
            scene.line(b.x, b.y, b.x - std::sin(b.angle) * tail, b.y + std::cos(b.angle) * tail, 3, Color::rgb(color, .6f));
            scene.circle(b.x, b.y, 3 * (.8f + ratio * .4f), Color::rgb(color));
        }
        for (const auto &d : game.debris)
            scene.line(d.x, d.y, d.x + std::cos(d.rotation) * d.length, d.y + std::sin(d.rotation) * d.length, 2,
                       Color::rgb(d.color, d.life > .5f ? 1.f : d.life * 2));
        const auto &s = game.ship;
        if (!game.over && !s.exploding && (!s.invulnerable() || int(s.invulnerableTime * 10) % 2 == 0)) drawShip(s, 0xffffff);
    }
    void drawTitle() {
        centered("CSSTEROIDS", 210, 96);
        centered("Native PS5 edition", 290, 28, 0x9fb3c8);
        centered("Press CROSS to start", 400, 34);
        char best[64]; std::snprintf(best, sizeof best, "LOCAL BEST %d", scores.best); centered(best, 455, 26, 0x70e8a0);
        centered("SERVER HIGH SCORES", 560, 30, 0xffd700);
        if (board.empty()) centered(fetch.pending() ? "..." : "No scores to show", 610, 24, 0x9fb3c8);
        for (size_t i = 0; i < board.size(); i++) {
            char row[96]; std::snprintf(row, sizeof row, "%2zu.  %-24s %8d", i + 1, board[i].name.c_str(), board[i].score);
            text(row, 700, 610 + i * 34.f, 26);
        }
        centered(onlineStatus, 990, 22, server.valid() ? 0x83bed4 : 0xf0c07d);
        centered("TRIANGLE  Refresh scores      Hold OPTIONS  Exit", 1035, 20, 0x8293ab);
    }
    void drawHud() {
        char line[96];
        std::snprintf(line, sizeof line, "Score: %d", game.score); text(line, 40, 60, 30);
        std::snprintf(line, sizeof line, "Lives: %d", game.lives); text(line, 1700, 60, 30);
        std::snprintf(line, sizeof line, "Level %d", game.level); text(line, 900, 60, 26, 0x9fb3c8);
        if (game.levelBanner > 0 && !game.over) { std::snprintf(line, sizeof line, "Level %d", game.level); centered(line, 520, 64); }
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     OPTIONS  Pause     CIRCLE  Title", 40, 1050, 20, 0x8293ab);
        if (game.over) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, hui::gfx::Color::rgb(0x000000, .7f));
            centered("GAME OVER", 450, 72, 0xf47366);
            std::snprintf(line, sizeof line, "Final score %d     Best %d", game.score, scores.best); centered(line, 530, 32);
            centered("CROSS to play again / CIRCLE for title", 600, 26, 0x9fb3c8);
            if (scores.failed) centered("Best score kept for this session; saving was unavailable", 660, 22, 0xf0c07d);
        }
        if (paused && !game.over) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, hui::gfx::Color::rgb(0x000000, .6f));
            centered("PAUSED", 515, 56);
            centered("OPTIONS to resume", 575, 26, 0x9fb3c8);
        }
    }

public:
    VulkanExample() {
        title = "CSSteroids"; name = "cssteroids";
        settings.overlay = false; ps5.ownOverlay = true;
        defaultClearColor = {{0.f, 0.f, 0.f, 1.f}};
    }
    void prepare() override {
        VulkanExampleBase::prepare();
        prepareKit({.sound = false, .music = false, .covers = false});
        scores.init("/data/cssteroids-ps5/highscore.txt", "/app0/cssteroids-highscore.txt");
        server = cssteroids::net::load_endpoint({"/data/cssteroids-ps5/server.txt", "/app0/server.txt"});
        requestScores();
        playing = ps5.frameBudget > 0;
        prepared = true;
        say("CSSteroids ready: %ux%u output, server %s", width, height, server.valid() ? server.label().c_str() : "(none)");
    }
    void render() override {
        if (!prepared) return;
        prepareFrame();
        const auto &pad = ps5_pad();
        const float dt = std::clamp(frameTimer, 0.f, .1f);
        const bool intercepted = (pad.held & PAD_INTERCEPTED) != 0;
        pollScores();
        if (!intercepted && !ps5.frameBudget) {
            if (pad.pressed & PAD_CROSS) if (!playing || game.over) { playing = true; paused = false; game.reset(); savedRun = false; }
            if ((pad.pressed & PAD_CIRCLE) && playing) { playing = false; paused = false; requestScores(); }
            if ((pad.pressed & PAD_TRIANGLE) && !playing) requestScores();
            if ((pad.pressed & PAD_OPTIONS) && playing) paused = !paused;
            exitHold = (pad.held & PAD_OPTIONS) ? exitHold + dt : 0;
            if (exitHold >= 1.25f) quit = true;
        }
        if (intercepted) exitHold = 0;
        cssteroids::Controls in;
        if (ps5.frameBudget) { in.turn = .35f; in.thrust = int(game.time) % 3 == 0; in.fire = true; }
        else if (!intercepted) {
            in.turn = pad.left_x;
            if (pad.held & (PAD_LEFT | PAD_RIGHT)) in.turn = ((pad.held & PAD_RIGHT) ? 1.f : 0.f) - ((pad.held & PAD_LEFT) ? 1.f : 0.f);
            in.thrust = pad.r2 > .2f || (pad.held & PAD_UP) || pad.left_y < -.6f;
            in.fire = (pad.held & (PAD_CROSS | PAD_R1 | PAD_SQUARE)) != 0;
        }
        if (playing && !paused && !intercepted) {
            accumulator = std::min(accumulator + dt, 6 * cssteroids::step_seconds);
            while (accumulator >= cssteroids::step_seconds) { game.step(in); accumulator -= cssteroids::step_seconds; }
        }
        game.events.clear();
        if (game.over && !savedRun) { scores.record(game.score); savedRun = true; }

        scene.clear();
        using hui::gfx::Color;
        for (int i = 0; i < 140; i++)
            scene.circle(float((i * 811 + 137) % 1920), float((i * 373 + 71) % 1080), i % 7 == 0 ? 1.6f : .8f, Color::rgb(0xffffff, .25f + (i % 5) * .1f));
        if (playing) { drawWorld(); drawHud(); }
        else drawTitle();
        if (exitHold > .25f) centered("Hold OPTIONS to exit", 110, 25, 0xf0c07d);
        kit.renderer.begin(); kit.renderer.draw(scene);
        buildKitCommandBuffer(); submitFrame();
    }
};
VULKAN_EXAMPLE_MAIN()
