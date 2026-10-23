// PSteroids5: native PS5 CSSteroids. Vector Asteroids from CSSteroids-2 (public/js),
// with ships and high scores from the existing CSSteroids server over HTTP(S).
// Rendering uses the PS5 Vulkan Template and PS5_VKHomebrewUI, via the BattleStar shell.
#include "kit.hpp"

namespace {
enum class Screen { title, hangar, name, game };
const char *const menu_items[] = {"PLAY", "HANGAR", "PILOT NAME", "REFRESH SCORES"};
constexpr int menu_count = 4;
const std::string name_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
constexpr int name_columns = 10, name_max = 16;
}

class VulkanExample : public ps5ui::KitExample {
    cssteroids::Game game{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    cssteroids::ScoreStore scores;
    cssteroids::ShipStore shipStore{"/data/cssteroids-ps5/ship.json"};
    cssteroids::ShipDesign ship;
    std::string pilot = "PS5 Pilot";
    cssteroids::net::Endpoint server;
    cssteroids::net::AsyncRequest scoreFetch, galleryFetch, passFetch, submitRequest;
    std::vector<cssteroids::api::ScoreRow> board;
    std::vector<cssteroids::ShipDesign> gallery;
    std::string onlineStatus, hangarStatus, submitStatus;
    hui::gfx::DrawList scene;
    Screen screen = Screen::title;
    int menu = 0, galleryIndex = 0, wheel[3] = {0, 0, 0}, wheelFocus = 0, nameCursor = 0;
    bool hangarPassphrase = false, paused = false, savedRun = false, submitted = false;
    std::string nameDraft;
    float accumulator = 0, exitHold = 0, previewSpin = 0;

    // --- text helpers -------------------------------------------------------
    void text(const std::string &s, float x, float y, float size, uint32_t color = 0xffffff, float alpha = 1) {
        hui::ui::text(scene, kit.fonts.regular, s, x, y, size, hui::gfx::Color::rgb(color, alpha));
    }
    float width_of(const std::string &s, float size) { return kit.fonts.regular.font->measure(s, size); }
    void centered(const std::string &s, float y, float size, uint32_t color = 0xffffff, float cx = 960) {
        text(s, cx - width_of(s, size) * .5f, y, size, color);
    }

    // --- persistence --------------------------------------------------------
    void loadPilot() {
        std::ifstream file("/data/cssteroids-ps5/pilot.txt");
        std::string line;
        if (std::getline(file, line)) {
            std::string clean;
            for (char c : line) if (name_chars.find(char(std::toupper((unsigned char)c))) != std::string::npos || (c >= 'a' && c <= 'z')) clean += c;
            if (!clean.empty()) pilot = clean.substr(0, name_max);
        }
    }
    void savePilot() {
        mkdir("/data/cssteroids-ps5", 0777);
        std::ofstream("/data/cssteroids-ps5/pilot.txt", std::ios::trunc) << pilot << "\n";
    }
    void useShip(const cssteroids::ShipDesign &d, bool persist) {
        ship = d;
        game.geometry = d.geometry;
        if (persist && !d.source.empty() && !shipStore.save(d.source)) hangarStatus = "Ship selected (could not save it for next time)";
    }

    // --- network ------------------------------------------------------------
    void requestScores() {
        if (!server.valid()) { onlineStatus = "OFFLINE - add server.txt (see server.txt.example)"; return; }
        if (scoreFetch.start(server, "GET", cssteroids::api::high_scores_path)) onlineStatus = "Contacting " + server.label() + " ...";
    }
    void requestGallery() {
        if (!server.valid()) { hangarStatus = "Offline: no server configured"; return; }
        if (galleryFetch.start(server, "GET", cssteroids::api::public_ships_path)) hangarStatus = "Loading public ships ...";
    }
    std::string wheelPassphrase() const {
        using namespace cssteroids::api;
        return adjectives[wheel[0]] + "-" + nouns[wheel[1]] + "-" + verbs[wheel[2]];
    }
    static std::string describe(const cssteroids::net::Response &r) {
        if (r.status) { cssteroids::json::Value v; if (cssteroids::json::parse(r.body, v) && v.is_object() && !v.str("message").empty()) return v.str("message"); return "server error " + std::to_string(r.status); }
        return r.error;
    }
    void pollNetwork() {
        cssteroids::net::Response r;
        if (scoreFetch.take(r)) {
            if (r.ok() && cssteroids::api::parse_high_scores(r.body, board)) onlineStatus = "ONLINE  " + server.label();
            else onlineStatus = r.ok() ? "Server sent unreadable scores" : "Scores unavailable: " + describe(r);
            say("PSteroids5 scores: status %d %s", r.status, r.error.c_str());
        }
        if (galleryFetch.take(r)) {
            if (r.ok() && cssteroids::parse_ship_list(r.body, gallery)) {
                hangarStatus = std::to_string(gallery.size()) + " public ships";
                galleryIndex = std::clamp(galleryIndex, 0, std::max(0, int(gallery.size()) - 1));
            } else hangarStatus = r.ok() ? "Server sent unreadable ships" : "Gallery unavailable: " + describe(r);
            say("PSteroids5 gallery: status %d %s", r.status, r.error.c_str());
        }
        if (passFetch.take(r)) {
            cssteroids::json::Value v;
            cssteroids::ShipDesign d;
            if (r.ok() && cssteroids::json::parse(r.body, v) && cssteroids::parse_ship(v, d)) {
                useShip(d, true);
                hangarStatus = "Loaded \"" + d.name + "\"";
            } else hangarStatus = r.status == 404 ? "No ship with that passphrase" : "Lookup failed: " + describe(r);
        }
        if (submitRequest.take(r)) {
            submitted = r.ok();
            submitStatus = r.ok() ? "Score submitted to " + server.label() : "Submit failed: " + describe(r) + "  (CROSS to retry)";
            if (r.ok()) requestScores();
        }
    }

    // --- drawing ------------------------------------------------------------
    // Ship outlines from ship.js (angle 0 = up), at the browser's on-screen scale.
    void drawShip(const cssteroids::ShipGeometry &g, float x, float y, float angle, float scale, bool thrusting) {
        using hui::gfx::Color;
        const float r = 15, c = std::cos(angle), sn = std::sin(angle);
        auto at = [&](float px, float py) { return std::array<float, 2>{x + (px * c - py * sn) * scale, y + (px * sn + py * c) * scale}; };
        auto seg = [&](float x1, float y1, float x2, float y2, uint32_t color) {
            auto a = at(x1, y1), b = at(x2, y2);
            scene.line(a[0], a[1], b[0], b[1], 2.f * std::max(1.f, scale * .6f), Color::rgb(color));
        };
        std::vector<cssteroids::Point> flames = g.thrusters;
        if (g.custom()) {
            for (const auto &l : g.lines) seg(l.x1, l.y1, l.x2, l.y2, l.color);
            if (flames.empty()) { // findLowestPoints: up to three endpoints within 5 px (20 design px) of the lowest
                std::vector<cssteroids::Point> pts;
                for (const auto &l : g.lines) { pts.push_back({l.x1, l.y1}); pts.push_back({l.x2, l.y2}); }
                std::sort(pts.begin(), pts.end(), [](auto &a, auto &b) { return a.y > b.y; });
                for (const auto &p : pts) if (p.y >= pts[0].y - 5 && flames.size() < 3) flames.push_back(p);
            }
        } else {
            const float k = g.shape == cssteroids::ShipShape::triangle ? 1.f : .7f;
            if (g.shape == cssteroids::ShipShape::diamond) {
                seg(0, -r, r, 0, g.color); seg(r, 0, 0, r, g.color); seg(0, r, -r, 0, g.color); seg(-r, 0, 0, -r, g.color);
            } else {
                seg(0, -r, -r * k, r * k, g.color); seg(-r * k, r * k, r * k, r * k, g.color); seg(r * k, r * k, 0, -r, g.color);
            }
            if (flames.empty()) flames.push_back({0, r * .7f});
        }
        if (thrusting) {
            const float flicker = .7f + .3f * std::fabs(std::sin(game.time * 47));
            for (const auto &f : flames) {
                auto base = at(f.x, f.y), tip = at(f.x, f.y + r * .6f * flicker);
                scene.line(base[0], base[1], tip[0], tip[1], 3 * std::max(1.f, scale * .6f), Color::rgb(0xffa500, .9f));
                scene.circle(tip[0], tip[1], 2 * scale, Color::rgb(0xff3000, .6f));
            }
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
        if (!game.over && !s.exploding && (!s.invulnerable() || int(s.invulnerableTime * 10) % 2 == 0))
            drawShip(game.geometry, s.x, s.y, s.angle, 1, s.thrusting);
    }
    void drawTitle() {
        centered("CSSTEROIDS", 170, 96);
        centered("PS5 edition", 240, 28, 0x9fb3c8);
        for (int i = 0; i < menu_count; i++) {
            const bool on = i == menu;
            if (on) scene.rounded_rect({760, 300.f + i * 56 - 36, 400, 48}, 8, hui::gfx::Color::rgb(0x2a4a6a, .8f));
            centered(menu_items[i], 300.f + i * 56, 32, on ? 0xffffff : 0x9fb3c8);
        }
        char line[128];
        std::snprintf(line, sizeof line, "PILOT %s     SHIP %s     LOCAL BEST %d", pilot.c_str(), ship.name.c_str(), scores.best);
        centered(line, 560, 24, 0x70e8a0);
        drawShip(ship.geometry, 960, 640, previewSpin, 2.5f, false);
        centered("SERVER HIGH SCORES", 760, 26, 0xffd700);
        if (board.empty()) centered(scoreFetch.pending() ? "..." : "No scores to show", 800, 22, 0x9fb3c8);
        for (size_t i = 0; i < board.size() && i < 5; i++) {
            std::snprintf(line, sizeof line, "%zu. %s  %d", i + 1, board[i].name.c_str(), board[i].score);
            centered(line, 800 + i * 32.f, 24, i == 0 ? 0xffffff : 0xc8d2dc);
        }
        centered(onlineStatus, 1000, 22, server.valid() ? 0x83bed4 : 0xf0c07d);
        centered("UP/DOWN  Select     CROSS  Confirm     Hold OPTIONS  Exit", 1045, 20, 0x8293ab);
    }
    void drawHangar() {
        using hui::gfx::Color;
        centered("HANGAR", 120, 56);
        centered(hangarPassphrase ? "ENTER A SHIP PASSPHRASE" : "PUBLIC SHIP GALLERY", 175, 26, 0xffd700);
        if (!hangarPassphrase) {
            if (gallery.empty()) centered(galleryFetch.pending() ? "Loading ..." : "No ships loaded", 500, 30, 0x9fb3c8);
            else {
                const auto &d = gallery[size_t(galleryIndex)];
                scene.bordered_rect({560, 220, 800, 560}, 16, Color::rgb(0x0b1622, .9f), 2, Color::rgb(0x2a4a6a));
                drawShip(d.geometry, 960, 500, previewSpin, 4, std::fmod(game.time, 2.f) < 1);
                centered(d.name, 830, 36);
                char line[96]; std::snprintf(line, sizeof line, "%d / %zu    %s    %zu weapons", galleryIndex + 1, gallery.size(),
                                             d.typeName.c_str(), d.geometry.weapons.empty() ? size_t(1) : d.geometry.weapons.size());
                centered(line, 875, 24, 0x9fb3c8);
                text("<", 480, 520, 64, 0x9fb3c8); text(">", 1410, 520, 64, 0x9fb3c8);
            }
            centered("LEFT/RIGHT  Browse     CROSS  Fly this ship     SQUARE  Passphrase     TRIANGLE  Reload     CIRCLE  Back", 1045, 20, 0x8293ab);
        } else {
            using namespace cssteroids::api;
            const std::vector<std::string> *lists[3] = {&adjectives, &nouns, &verbs};
            for (int w = 0; w < 3; w++) {
                const float cx = 560 + w * 400.f;
                const auto &list = *lists[w];
                scene.bordered_rect({cx - 170, 260, 340, 420}, 12, Color::rgb(0x0b1622, .9f), w == wheelFocus ? 3.f : 1.f,
                                    Color::rgb(w == wheelFocus ? 0x70caff : 0x2a4a6a));
                for (int k = -3; k <= 3; k++) {
                    const int i = (wheel[w] + k + int(list.size()) * 4) % int(list.size());
                    centered(list[size_t(i)], 480.f + k * 56, k == 0 ? 38.f : 26.f, k == 0 ? 0xffffff : 0x56657a, cx);
                }
            }
            centered(wheelPassphrase(), 760, 40, 0xffd700);
            centered("LEFT/RIGHT  Word     UP/DOWN  Change     CROSS  Load ship     SQUARE  Gallery     CIRCLE  Back", 1045, 20, 0x8293ab);
        }
        centered(hangarStatus, 960, 24, 0x83bed4);
        char line[96]; std::snprintf(line, sizeof line, "Current ship: %s", ship.name.c_str());
        centered(line, 1000, 22, 0x70e8a0);
    }
    void drawNameEntry() {
        using hui::gfx::Color;
        centered("PILOT NAME", 160, 56);
        centered(nameDraft + (int(game.time * 2) % 2 ? "_" : " "), 260, 48, 0xffd700);
        for (size_t i = 0; i < name_chars.size(); i++) {
            const float x = 660 + float(i % name_columns) * 64, y = 380 + float(i / name_columns) * 72;
            const bool on = int(i) == nameCursor;
            if (on) scene.rounded_rect({x - 26, y - 44, 52, 60}, 8, Color::rgb(0x2a4a6a));
            const std::string label = name_chars[i] == ' ' ? "SP" : std::string(1, name_chars[i]);
            centered(label, y, label.size() > 1 ? 22.f : 36.f, on ? 0xffffff : 0x9fb3c8, x);
        }
        centered("D-PAD  Move     CROSS  Add     SQUARE  Delete     OPTIONS  Save     CIRCLE  Cancel", 1045, 20, 0x8293ab);
    }
    void drawHud() {
        char line[160];
        std::snprintf(line, sizeof line, "Score: %d", game.score); text(line, 40, 60, 30);
        std::snprintf(line, sizeof line, "Lives: %d", game.lives); text(line, 1700, 60, 30);
        std::snprintf(line, sizeof line, "Level %d", game.level); text(line, 900, 60, 26, 0x9fb3c8);
        if (game.levelBanner > 0 && !game.over) centered(line, 520, 64);
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     OPTIONS  Pause     CIRCLE  Title", 40, 1050, 20, 0x8293ab);
        if (game.over) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, hui::gfx::Color::rgb(0x000000, .7f));
            centered("GAME OVER", 420, 72, 0xf47366);
            std::snprintf(line, sizeof line, "Final score %d     Best %d", game.score, scores.best); centered(line, 500, 32);
            if (server.valid() && game.score > 0) {
                if (submitted) centered(submitStatus, 570, 26, 0x70e8a0);
                else if (submitRequest.pending()) centered("Submitting ...", 570, 26, 0x83bed4);
                else {
                    std::snprintf(line, sizeof line, "TRIANGLE  Submit score as %s", pilot.c_str());
                    centered(line, 570, 26, 0xffd700);
                    if (!submitStatus.empty()) centered(submitStatus, 610, 22, 0xf0c07d);
                }
            }
            centered("CROSS to play again / CIRCLE for title", 670, 26, 0x9fb3c8);
            if (scores.failed) centered("Best score kept for this session; saving was unavailable", 720, 22, 0xf0c07d);
        }
        if (paused && !game.over) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, hui::gfx::Color::rgb(0x000000, .6f));
            centered("PAUSED", 515, 56);
            centered("OPTIONS to resume", 575, 26, 0x9fb3c8);
        }
    }

    // --- input --------------------------------------------------------------
    void startGame() {
        screen = Screen::game; paused = false; savedRun = false; submitted = false; submitStatus.clear();
        game.reset();
    }
    void titleInput(uint32_t pressed) {
        if (pressed & PAD_UP) menu = (menu + menu_count - 1) % menu_count;
        if (pressed & PAD_DOWN) menu = (menu + 1) % menu_count;
        if (!(pressed & PAD_CROSS)) return;
        switch (menu) {
            case 0: startGame(); break;
            case 1: screen = Screen::hangar; if (gallery.empty()) requestGallery(); break;
            case 2: screen = Screen::name; nameDraft = pilot; nameCursor = 0; break;
            default: requestScores(); break;
        }
    }
    void hangarInput(uint32_t pressed) {
        if (pressed & PAD_CIRCLE) { screen = Screen::title; return; }
        if (pressed & PAD_SQUARE) { hangarPassphrase = !hangarPassphrase; return; }
        if (!hangarPassphrase) {
            const int n = int(gallery.size());
            if (n && (pressed & PAD_LEFT)) galleryIndex = (galleryIndex + n - 1) % n;
            if (n && (pressed & PAD_RIGHT)) galleryIndex = (galleryIndex + 1) % n;
            if (pressed & PAD_TRIANGLE) requestGallery();
            if (n && (pressed & PAD_CROSS)) { useShip(gallery[size_t(galleryIndex)], true); hangarStatus = "Flying \"" + ship.name + "\""; }
            return;
        }
        using namespace cssteroids::api;
        const int sizes[3] = {int(adjectives.size()), int(nouns.size()), int(verbs.size())};
        if (pressed & PAD_LEFT) wheelFocus = (wheelFocus + 2) % 3;
        if (pressed & PAD_RIGHT) wheelFocus = (wheelFocus + 1) % 3;
        if (pressed & PAD_UP) wheel[wheelFocus] = (wheel[wheelFocus] + sizes[wheelFocus] - 1) % sizes[wheelFocus];
        if (pressed & PAD_DOWN) wheel[wheelFocus] = (wheel[wheelFocus] + 1) % sizes[wheelFocus];
        if (pressed & PAD_CROSS) {
            if (!server.valid()) hangarStatus = "Offline: no server configured";
            else if (passFetch.start(server, "GET", passphrase_path(wheelPassphrase()))) hangarStatus = "Looking up " + wheelPassphrase() + " ...";
        }
    }
    void nameInput(uint32_t pressed) {
        const int n = int(name_chars.size());
        if (pressed & PAD_LEFT) nameCursor = (nameCursor + n - 1) % n;
        if (pressed & PAD_RIGHT) nameCursor = (nameCursor + 1) % n;
        if (pressed & PAD_UP) nameCursor = (nameCursor + n - name_columns) % n;
        if (pressed & PAD_DOWN) nameCursor = (nameCursor + name_columns) % n;
        if ((pressed & PAD_CROSS) && int(nameDraft.size()) < name_max) nameDraft += name_chars[size_t(nameCursor)];
        if ((pressed & PAD_SQUARE) && !nameDraft.empty()) nameDraft.pop_back();
        if (pressed & PAD_CIRCLE) screen = Screen::title;
        if (pressed & PAD_OPTIONS) {
            while (!nameDraft.empty() && nameDraft.back() == ' ') nameDraft.pop_back();
            if (!nameDraft.empty()) { pilot = nameDraft; savePilot(); }
            screen = Screen::title;
        }
    }
    void gameInput(uint32_t pressed) {
        if ((pressed & PAD_CROSS) && game.over) startGame();
        if (pressed & PAD_CIRCLE) { screen = Screen::title; paused = false; requestScores(); }
        if ((pressed & PAD_OPTIONS) && !game.over) paused = !paused;
        // Scores reach the production database only when the player asks.
        if ((pressed & PAD_TRIANGLE) && game.over && server.valid() && game.score > 0 && !submitted && !submitRequest.pending())
            submitRequest.start(server, "POST", cssteroids::api::high_scores_path,
                                cssteroids::api::high_score_body(pilot, game.score, ship.typeName, ship.colorName, ship.passphrase));
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
        loadPilot();
        if (cssteroids::ShipDesign saved; shipStore.load(saved)) useShip(saved, false);
        server = cssteroids::net::load_endpoint({"/data/cssteroids-ps5/server.txt", "/app0/server.txt"});
        requestScores();
        if (ps5.frameBudget > 0) startGame();
        prepared = true;
        say("PSteroids5 ready: %ux%u output, server %s, ship %s", width, height,
            server.valid() ? server.label().c_str() : "(none)", ship.name.c_str());
    }
    void render() override {
        if (!prepared) return;
        prepareFrame();
        const auto &pad = ps5_pad();
        const float dt = std::clamp(frameTimer, 0.f, .1f);
        const bool intercepted = (pad.held & PAD_INTERCEPTED) != 0;
        pollNetwork();
        previewSpin += dt * .8f;
        if (!intercepted && !ps5.frameBudget) {
            const uint32_t pressed = pad.pressed;
            switch (screen) {
                case Screen::title: titleInput(pressed); break;
                case Screen::hangar: hangarInput(pressed); break;
                case Screen::name: nameInput(pressed); break;
                case Screen::game: gameInput(pressed); break;
            }
            exitHold = (pad.held & PAD_OPTIONS) && screen != Screen::name ? exitHold + dt : 0;
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
        if (screen == Screen::game && !paused && !intercepted) {
            accumulator = std::min(accumulator + dt, 6 * cssteroids::step_seconds);
            while (accumulator >= cssteroids::step_seconds) { game.step(in); accumulator -= cssteroids::step_seconds; }
        } else game.time += dt; // keeps menu animations moving
        game.events.clear();
        if (game.over && !savedRun) { scores.record(game.score); savedRun = true; }

        scene.clear();
        using hui::gfx::Color;
        for (int i = 0; i < 140; i++)
            scene.circle(float((i * 811 + 137) % 1920), float((i * 373 + 71) % 1080), i % 7 == 0 ? 1.6f : .8f, Color::rgb(0xffffff, .25f + (i % 5) * .1f));
        switch (screen) {
            case Screen::title: drawTitle(); break;
            case Screen::hangar: drawHangar(); break;
            case Screen::name: drawNameEntry(); break;
            case Screen::game: drawWorld(); drawHud(); break;
        }
        if (exitHold > .25f) centered("Hold OPTIONS to exit", 60, 25, 0xf0c07d);
        kit.renderer.begin(); kit.renderer.draw(scene);
        buildKitCommandBuffer(); submitFrame();
    }
};
VULKAN_EXAMPLE_MAIN()
