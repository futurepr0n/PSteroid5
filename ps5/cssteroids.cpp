// PSteroids5: native PS5 CSSteroids. Vector Asteroids from CSSteroids-2 (public/js),
// with ships and high scores from the existing CSSteroids server over HTTP(S).
// Rendering uses the PS5 Vulkan Template and PS5_VKHomebrewUI, via the BattleStar shell.
#include "kit.hpp"

namespace {
// MULTIPLAYER = two-pilot campaign rooms (co-op); MMO = the drop-in shared world.
enum class Screen { title, hangar, name, game, lobby, multiplayer };
const char *const menu_items[] = {"PLAY", "MULTIPLAYER", "MMO", "HANGAR", "PILOT NAME", "REFRESH SCORES"};
constexpr int menu_count = 6;
enum class LobbyStep { menu, list, code, waiting };
const char *const lobby_items[] = {"CREATE ROOM", "BROWSE OPEN ROOMS", "ENTER ROOM CODE"};
const std::string code_chars = "abcdefghijklmnopqrstuvwxyz0123456789";
double wall_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
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
    cssteroids::sio::Client socket;
    cssteroids::mmo::Session mp;
    bool mpWantJoin = false, coopMode = false;
    std::string mpStatus;
    // co-op rooms
    cssteroids::coop::LobbyState lobby;
    std::vector<cssteroids::coop::Room> rooms;
    cssteroids::net::AsyncRequest roomRequest;
    enum class RoomAction { none, create, list, verify } roomAction = RoomAction::none;
    LobbyStep lobbyStep = LobbyStep::menu;
    int lobbyMenu = 0, roomIndex = 0, joinedStarts = 0;
    std::string codeDraft, lobbyStatus;
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
            say("PSteroid5 scores: status %d %s", r.status, r.error.c_str());
        }
        if (galleryFetch.take(r)) {
            if (r.ok() && cssteroids::parse_ship_list(r.body, gallery)) {
                hangarStatus = std::to_string(gallery.size()) + " public ships";
                galleryIndex = std::clamp(galleryIndex, 0, std::max(0, int(gallery.size()) - 1));
            } else hangarStatus = r.ok() ? "Server sent unreadable ships" : "Gallery unavailable: " + describe(r);
            say("PSteroid5 gallery: status %d %s", r.status, r.error.c_str());
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
        centered("PSTEROID5", 170, 96);
        centered("a CSSteroids port for PS5", 240, 28, 0x9fb3c8);
        for (int i = 0; i < menu_count; i++) {
            const bool on = i == menu;
            if (on) scene.rounded_rect({760, 290.f + i * 44 - 33, 400, 42}, 8, hui::gfx::Color::rgb(0x2a4a6a, .8f));
            centered(menu_items[i], 290.f + i * 44, 28, on ? 0xffffff : 0x9fb3c8);
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

    // --- shared world (socket.io to the same server as the browser) ---------
    void startMmo() {
        screen = Screen::multiplayer;
        coopMode = false;
        mp = {};
        if (!server.valid()) { mpStatus = "Offline: no server configured"; return; }
        socket.connect(server);
        mpWantJoin = true;
        mpStatus = "Connecting to " + server.label() + " ...";
    }
    void startCoopLobby() {
        screen = Screen::lobby;
        coopMode = true;
        mp = {}; lobby = {}; rooms.clear();
        lobbyStep = LobbyStep::menu; lobbyMenu = 0; joinedStarts = 0;
        mpWantJoin = false;
        if (!server.valid()) { lobbyStatus = "Offline: no server configured"; return; }
        socket.connect(server);
        lobbyStatus = "Connecting to " + server.label() + " ...";
    }
    void stopMultiplayer() {
        mp.leave();
        for (auto &o : mp.take_outbox()) { if (o.hasData) socket.emit(o.name, o.data); else socket.emit(o.name); }
        socket.close();
        mpWantJoin = false;
        coopMode = false;
        screen = Screen::title;
        requestScores();
    }
    void enterRoom(const std::string &roomId) {
        lobby = {};
        lobby.roomId = roomId;
        socket.emit("join-simple-session", cssteroids::json::Value::string_value(roomId));
        lobbyStep = LobbyStep::waiting;
        lobbyStatus = "Joining room " + cssteroids::coop::room_code(roomId) + " ...";
        say("PSteroid5 co-op: joining room %s", roomId.c_str());
    }
    void leaveRoom() {
        if (!lobby.roomId.empty()) socket.emit("leave-session");
        lobby = {};
        lobbyStep = LobbyStep::menu;
        lobbyStatus.clear();
    }
    // Room REST replies (create / list / verify a typed code).
    void pollRooms() {
        cssteroids::net::Response r;
        if (!roomRequest.take(r)) return;
        const RoomAction action = roomAction;
        roomAction = RoomAction::none;
        std::string error;
        cssteroids::coop::Room room;
        if (action == RoomAction::create) {
            if (r.ok() && cssteroids::coop::parse_room_reply(r.body, room, error)) enterRoom(room.id);
            else lobbyStatus = "Could not create a room: " + (r.ok() ? error : describe(r));
        } else if (action == RoomAction::list) {
            if (r.ok() && cssteroids::coop::parse_room_list(r.body, rooms)) {
                roomIndex = 0;
                lobbyStatus = rooms.empty() ? "No open rooms right now - create one and share its code" : std::to_string(rooms.size()) + " open rooms";
            } else lobbyStatus = "Room list unavailable: " + describe(r);
        } else if (action == RoomAction::verify) {
            // The server creates rooms on the fly for unknown ids, so a typo must be caught here.
            if (r.ok() && cssteroids::coop::parse_room_reply(r.body, room, error)) {
                if (room.gameState == "playing") lobbyStatus = "That room's campaign has already started";
                else enterRoom(room.id);
            } else lobbyStatus = r.status == 404 ? "No room with code " + codeDraft : "Lookup failed: " + describe(r);
        }
    }
    void lobbyInput(uint32_t pressed) {
        using cssteroids::sio::Client;
        const bool online = socket.state() == Client::State::connected;
        if (socket.state() == Client::State::closed && (pressed & PAD_CROSS)) { startCoopLobby(); return; }
        switch (lobbyStep) {
            case LobbyStep::menu:
                if (pressed & PAD_CIRCLE) { stopMultiplayer(); return; }
                if (pressed & PAD_UP) lobbyMenu = (lobbyMenu + 2) % 3;
                if (pressed & PAD_DOWN) lobbyMenu = (lobbyMenu + 1) % 3;
                if ((pressed & PAD_CROSS) && online && !roomRequest.pending()) {
                    if (lobbyMenu == 0) {
                        roomAction = RoomAction::create;
                        roomRequest.start(server, "POST", cssteroids::coop::create_path, cssteroids::coop::create_body(socket.sid()));
                        lobbyStatus = "Creating a room ...";
                    } else if (lobbyMenu == 1) {
                        lobbyStep = LobbyStep::list; rooms.clear();
                        roomAction = RoomAction::list;
                        roomRequest.start(server, "GET", cssteroids::coop::rooms_path);
                        lobbyStatus = "Looking for open rooms ...";
                    } else { lobbyStep = LobbyStep::code; codeDraft.clear(); nameCursor = 0; lobbyStatus.clear(); }
                }
                break;
            case LobbyStep::list: {
                if (pressed & PAD_CIRCLE) { lobbyStep = LobbyStep::menu; lobbyStatus.clear(); return; }
                const int n = int(rooms.size());
                if (n && (pressed & PAD_UP)) roomIndex = (roomIndex + n - 1) % n;
                if (n && (pressed & PAD_DOWN)) roomIndex = (roomIndex + 1) % n;
                if ((pressed & PAD_TRIANGLE) && !roomRequest.pending()) {
                    roomAction = RoomAction::list;
                    roomRequest.start(server, "GET", cssteroids::coop::rooms_path);
                    lobbyStatus = "Refreshing ...";
                }
                if (n && (pressed & PAD_CROSS)) enterRoom(rooms[size_t(roomIndex)].id);
                break;
            }
            case LobbyStep::code: {
                const int n = int(code_chars.size());
                if (pressed & PAD_LEFT) nameCursor = (nameCursor + n - 1) % n;
                if (pressed & PAD_RIGHT) nameCursor = (nameCursor + 1) % n;
                if (pressed & PAD_UP) nameCursor = (nameCursor + n - name_columns) % n;
                if (pressed & PAD_DOWN) nameCursor = (nameCursor + name_columns) % n;
                if ((pressed & PAD_CROSS) && codeDraft.size() < 16) codeDraft += code_chars[size_t(nameCursor % n)];
                if ((pressed & PAD_SQUARE) && !codeDraft.empty()) codeDraft.pop_back();
                if (pressed & PAD_CIRCLE) { lobbyStep = LobbyStep::menu; lobbyStatus.clear(); }
                if ((pressed & PAD_OPTIONS) && !codeDraft.empty() && online && !roomRequest.pending()) {
                    roomAction = RoomAction::verify;
                    roomRequest.start(server, "GET", std::string(cssteroids::coop::rooms_path) + "/" +
                                      cssteroids::net::url_encode(cssteroids::coop::room_id_from_code(codeDraft)));
                    lobbyStatus = "Checking room " + codeDraft + " ...";
                }
                break;
            }
            case LobbyStep::waiting:
                if (pressed & PAD_CIRCLE) { leaveRoom(); return; }
                if ((pressed & PAD_CROSS) && lobby.joined) { socket.emit("start-multiplayer-game"); lobbyStatus = "Starting the campaign ..."; }
                break;
        }
    }
    void drawLobby() {
        using hui::gfx::Color;
        centered("MULTIPLAYER", 150, 60);
        centered("Two-pilot campaign: 10 rounds, 3 shared lives", 205, 26, 0x9fb3c8);
        char line[160];
        switch (lobbyStep) {
            case LobbyStep::menu:
                for (int i = 0; i < 3; i++) {
                    const bool on = i == lobbyMenu;
                    if (on) scene.rounded_rect({700, 330.f + i * 60 - 38, 520, 52}, 8, Color::rgb(0x2a4a6a, .8f));
                    centered(lobby_items[i], 330.f + i * 60, 32, on ? 0xffffff : 0x9fb3c8);
                }
                centered("UP/DOWN  Select     CROSS  Confirm     CIRCLE  Back", 1045, 20, 0x8293ab);
                break;
            case LobbyStep::list:
                centered("OPEN ROOMS", 300, 32, 0xffd700);
                if (rooms.empty()) centered(roomRequest.pending() ? "..." : "None", 380, 28, 0x9fb3c8);
                for (size_t i = 0; i < rooms.size() && i < 10; i++) {
                    const bool on = int(i) == roomIndex;
                    if (on) scene.rounded_rect({660, 360.f + i * 50 - 34, 600, 46}, 8, Color::rgb(0x2a4a6a, .8f));
                    std::snprintf(line, sizeof line, "ROOM %s     %d / %d pilots", cssteroids::coop::room_code(rooms[i].id).c_str(), rooms[i].currentPlayers, rooms[i].maxPlayers);
                    centered(line, 360.f + i * 50, 28, on ? 0xffffff : 0x9fb3c8);
                }
                centered("UP/DOWN  Select     CROSS  Join     TRIANGLE  Refresh     CIRCLE  Back", 1045, 20, 0x8293ab);
                break;
            case LobbyStep::code:
                centered("ROOM CODE", 300, 32, 0xffd700);
                centered(codeDraft + (int(game.time * 2) % 2 ? "_" : " "), 380, 48);
                for (size_t i = 0; i < code_chars.size(); i++) {
                    const float x = 660 + float(i % name_columns) * 64, y = 480 + float(i / name_columns) * 72;
                    const bool on = int(i) == nameCursor;
                    if (on) scene.rounded_rect({x - 26, y - 44, 52, 60}, 8, Color::rgb(0x2a4a6a));
                    centered(std::string(1, char(std::toupper((unsigned char)code_chars[i]))), y, 36, on ? 0xffffff : 0x9fb3c8, x);
                }
                centered("D-PAD  Move     CROSS  Add     SQUARE  Delete     OPTIONS  Join     CIRCLE  Back", 1045, 20, 0x8293ab);
                break;
            case LobbyStep::waiting: {
                std::string code = cssteroids::coop::room_code(lobby.roomId);
                for (auto &c : code) c = char(std::toupper((unsigned char)c));
                centered("ROOM CODE", 330, 30, 0xffd700);
                centered(code, 420, 72);
                centered("Share this code: the other pilot picks MULTIPLAYER > ENTER ROOM CODE (or finds it in BROWSE)", 480, 22, 0x9fb3c8);
                std::snprintf(line, sizeof line, "%d / %d PILOTS IN THE ROOM", std::max(lobby.playerCount, lobby.joined ? 1 : 0), cssteroids::coop::room_size);
                centered(line, 580, 34, lobby.playerCount >= 2 ? 0x70e8a0 : 0xffffff);
                centered(lobby.playerCount >= 2 ? "CROSS  Start the campaign" : "CROSS  Start solo (or wait for a second pilot)", 660, 28, 0xffd700);
                centered("CIRCLE  Leave room", 1045, 20, 0x8293ab);
                break;
            }
        }
        if (!lobby.error.empty()) centered("Server: " + lobby.error, 950, 22, 0xf0c07d);
        centered(lobbyStatus, 1000, 22, 0x83bed4);
    }
    void pumpMultiplayer(const cssteroids::Controls &in, float dt) {
        using cssteroids::sio::Client;
        const double now = wall_ms();
        const auto state = socket.state();
        if (state == Client::State::connected && mpWantJoin) {
            mp.begin_join(now, pilot, ship);
            mpWantJoin = false;
            say("PSteroid5 mmo: socket %s, joining as %s", socket.sid().c_str(), pilot.c_str());
        }
        cssteroids::sio::Event ev;
        while (socket.poll(ev)) {
            if (coopMode && lobby.handle(ev.name, ev.data)) continue;
            mp.handle(ev.name, ev.data, wall_ms());
        }
        if (coopMode) {
            pollRooms();
            if (screen == Screen::lobby && state == Client::State::connected && lobbyStatus.rfind("Connecting", 0) == 0) lobbyStatus = "Connected";
            if (lobbyStep == LobbyStep::waiting && lobby.joined && lobbyStatus.rfind("Joining room", 0) == 0) lobbyStatus = "In room";
            // game-started (first start or a replay): everyone joins the room's co-op world
            if (lobby.starts > joinedStarts) {
                joinedStarts = lobby.starts;
                mp = {};
                mp.begin_join(now, pilot, ship, lobby.roomId);
                screen = Screen::multiplayer;
                say("PSteroid5 co-op: campaign started in room %s", lobby.roomId.c_str());
            }
        }
        mp.update(in, now, dt);
        auto out = mp.take_outbox();
        if (state == Client::State::connected) for (auto &o : out) { if (o.hasData) socket.emit(o.name, o.data); else socket.emit(o.name); }
        if (screen == Screen::lobby) return; // the lobby shows its own status
        // status line; every change also goes to klog
        std::string status;
        if (!server.valid()) status = "Offline: no server configured";
        else if (state == Client::State::closed) status = "Disconnected: " + socket.error() + "   CROSS to reconnect";
        else if (state == Client::State::connecting) status = "Connecting to " + server.label() + " ...";
        else if (mp.phase == cssteroids::mmo::Phase::failed) status = "Could not join: " + mp.failure + "   CROSS to retry";
        else if (mp.phase == cssteroids::mmo::Phase::joining) status = "Joining the world ...";
        else if (mp.phase == cssteroids::mmo::Phase::warping) status = "Warping in ...";
        else { char line[96]; std::snprintf(line, sizeof line, "ONLINE  %zu pilots   %.0f ms", mp.players.size() + 1, mp.rtt()); status = line; }
        if (status != mpStatus) { say("PSteroid5 %s: %s", coopMode ? "co-op" : "mmo", status.c_str()); mpStatus = status; }
    }
    void multiplayerInput(uint32_t pressed) {
        using cssteroids::sio::Client;
        if (pressed & PAD_CIRCLE) { stopMultiplayer(); return; }
        if (coopMode && mp.coopOver && (pressed & PAD_CROSS)) { socket.emit("start-multiplayer-game"); return; } // replay in the same room
        if ((pressed & PAD_CROSS) && server.valid() && (socket.state() == Client::State::closed || mp.phase == cssteroids::mmo::Phase::failed))
            coopMode ? startCoopLobby() : startMmo();
    }
    void drawMultiplayer() {
        using hui::gfx::Color;
        const double now = wall_ms();
        // camera: follow the ship, clamped to the world (centred when the world is smaller)
        const float W = float(mp.bounds.width), H = float(mp.bounds.height);
        const float camX = W <= 1920 ? (W - 1920) / 2 : std::clamp(float(mp.x) - 960, 0.f, W - 1920);
        const float camY = H <= 1080 ? (H - 1080) / 2 : std::clamp(float(mp.y) - 540, 0.f, H - 1080);
        auto sx = [&](double x) { return float(x) - camX; };
        auto sy = [&](double y) { return float(y) - camY; };
        auto visible = [&](double x, double y, double r) { return sx(x) > -r && sx(x) < 1920 + r && sy(y) > -r && sy(y) < 1080 + r; };
        const uint32_t edge = 0x3a6d99;
        scene.line(sx(0), sy(0), sx(W), sy(0), 3, Color::rgb(edge)); scene.line(sx(W), sy(0), sx(W), sy(H), 3, Color::rgb(edge));
        scene.line(sx(W), sy(H), sx(0), sy(H), 3, Color::rgb(edge)); scene.line(sx(0), sy(H), sx(0), sy(0), 3, Color::rgb(edge));
        for (const auto &[id, a] : mp.asteroids) {
            if (!mp.asteroid_visible(a)) continue;
            double ax, ay, rot;
            mp.asteroid_render_pos(a, now, ax, ay, rot);
            if (!visible(ax, ay, a.radius)) continue;
            float pts[16];
            for (int i = 0; i < a.vertices; i++) {
                const double ang = i * cssteroids::mmo::tau / a.vertices + rot;
                pts[i * 2] = sx(ax + a.offsets[i] * std::cos(ang)); pts[i * 2 + 1] = sy(ay + a.offsets[i] * std::sin(ang));
            }
            for (int i = 0; i < a.vertices; i++) { const int j = (i + 1) % a.vertices; scene.line(pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], 2.5f, Color::rgb(0xffffff)); }
        }
        // enemies: enemy.js outline; their angle is math convention (0 = right)
        for (const auto &[id, e] : mp.enemies) {
            if (e.outOfView || !visible(e.x, e.y, 20)) continue;
            const float c = std::cos(float(e.a)), sn = std::sin(float(e.a));
            auto at = [&](float px, float py) { return std::array<float, 2>{sx(e.x) + px * c - py * sn, sy(e.y) + px * sn + py * c}; };
            const std::array<float, 2> hull[4] = {at(15, 0), at(-10, -10), at(-5, 0), at(-10, 10)};
            for (int i = 0; i < 4; i++) scene.line(hull[i][0], hull[i][1], hull[(i + 1) % 4][0], hull[(i + 1) % 4][1], 2, Color::rgb(0xff3030));
            scene.circle(sx(e.x), sy(e.y), 2, Color::rgb(0xff0000));
        }
        for (const auto &b : mp.bullets) {
            if (!visible(b.x, b.y, 4)) continue;
            const uint32_t col = b.hostile ? 0xff2020 : b.owner.empty() ? 0xff4500 : 0xffd700;
            scene.line(sx(b.x), sy(b.y), sx(b.x - std::sin(b.angle) * 10), sy(b.y + std::cos(b.angle) * 10), 3, Color::rgb(col, .6f));
            scene.circle(sx(b.x), sy(b.y), 3, Color::rgb(col));
        }
        for (const auto &bu : mp.bursts) {
            const float r = float(bu.age) * (bu.big ? 90.f : 40.f);
            scene.ring(sx(bu.x), sy(bu.y), r, 2, Color::rgb(0xffa040, float(1 - bu.age)));
        }
        for (const auto &[id, p] : mp.players) {
            if (p.dead || p.outOfView || !visible(p.x, p.y, 40)) continue;
            drawShip(p.geometry, sx(p.x), sy(p.y), float(p.angle), 1, p.thrust);
            centered(p.name, sy(p.y) - 30, 18, 0x9fd3ff, sx(p.x));
        }
        const bool blink = mp.invulnerableMs > 0 && int(now / 100) % 2;
        if (!mp.dead && !blink && mp.phase == cssteroids::mmo::Phase::playing) drawShip(ship.geometry, sx(mp.x), sy(mp.y), float(mp.angle), 1, mp.thrusting);
        if (mp.phase == cssteroids::mmo::Phase::warping) scene.ring(sx(mp.x), sy(mp.y), 30 + 10 * std::sin(float(now) / 120), 2, Color::rgb(0x70caff));
        int mine = 0;
        for (const auto &e2 : mp.scores) if (e2.id == mp.myId) mine = e2.score;
        char line[128];
        std::snprintf(line, sizeof line, "Score: %d", mine); text(line, 40, 60, 30);
        text(mp.mode == "coop" ? "CO-OP" : "SHARED WORLD", 860, 60, 26, 0x9fb3c8);
        if (mp.mode == "coop") { std::snprintf(line, sizeof line, "Round %d/%d   Lives %d", mp.round, mp.maxRounds, mp.lives); text(line, 1500, 60, 26); }
        text("LEADERBOARD", 1640, 120, 20, 0xffd700);
        for (size_t i = 0; i < mp.scores.size() && i < 6; i++) {
            std::snprintf(line, sizeof line, "%zu. %s  %d", i + 1, mp.scores[i].name.c_str(), mp.scores[i].score);
            text(line, 1640, 150 + i * 26.f, 18, mp.scores[i].id == mp.myId ? 0x70e8a0 : 0xc8d2dc);
        }
        if (mp.dead && mp.phase == cssteroids::mmo::Phase::playing) {
            std::snprintf(line, sizeof line, "RESPAWNING IN %d", std::max(1, int(std::ceil(mp.respawnCountdownMs / 1000))));
            centered(line, 520, 48, 0xf47366);
        }
        if (mp.bannerMs > 0) centered(mp.banner, 300, 44, 0x83bed4);
        if (coopMode && mp.coopOver) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, .65f));
            centered(mp.coopWon ? "MISSION COMPLETE" : "GAME OVER", 440, 68, mp.coopWon ? 0x70e8a0 : 0xf47366);
            std::snprintf(line, sizeof line, "TEAM SCORE %d     ROUND %d / %d", mp.teamScore, mp.round, mp.maxRounds);
            centered(line, 520, 32);
            centered("CROSS  Play again in this room     CIRCLE  Leave", 600, 26, 0x9fb3c8);
        }
        centered(mpStatus, 1000, 22, mp.phase == cssteroids::mmo::Phase::playing ? 0x83bed4 : 0xf0c07d);
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     CIRCLE  Leave world", 40, 1050, 20, 0x8293ab);
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
            case 1: startCoopLobby(); break;
            case 2: startMmo(); break;
            case 3: screen = Screen::hangar; if (gallery.empty()) requestGallery(); break;
            case 4: screen = Screen::name; nameDraft = pilot; nameCursor = 0; break;
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
        title = "PSteroid5"; name = "cssteroids";
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
        say("PSteroid5 ready: %ux%u output, server %s, ship %s", width, height,
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
                case Screen::multiplayer: multiplayerInput(pressed); break;
                case Screen::lobby: lobbyInput(pressed); break;
            }
            exitHold = (pad.held & PAD_OPTIONS) && screen != Screen::name && !(screen == Screen::lobby && lobbyStep == LobbyStep::code) ? exitHold + dt : 0;
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
        if (screen == Screen::multiplayer || screen == Screen::lobby) pumpMultiplayer(intercepted || screen == Screen::lobby ? cssteroids::Controls{} : in, dt);
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
            case Screen::multiplayer: drawMultiplayer(); break;
            case Screen::lobby: drawLobby(); break;
        }
        if (exitHold > .25f) centered("Hold OPTIONS to exit", 60, 25, 0xf0c07d);
        kit.renderer.begin(); kit.renderer.draw(scene);
        buildKitCommandBuffer(); submitFrame();
    }
};
VULKAN_EXAMPLE_MAIN()
