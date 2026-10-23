// PSteroids5: native PS5 CSSteroids. Vector Asteroids from CSSteroids-2 (public/js),
// with ships and high scores from the existing CSSteroids server over HTTP(S).
// Rendering uses the PS5 Vulkan Template and PS5_VKHomebrewUI, via the BattleStar shell.
#include "kit.hpp"

namespace {
// MULTIPLAYER = two-pilot campaign rooms (co-op); MMO = the drop-in shared world.
enum class Screen { title, hangar, name, game, lobby, multiplayer, editor };
const char *const menu_items[] = {"PLAY", "MULTIPLAYER", "MMO", "HANGAR", "SHIP EDITOR", "PILOT NAME", "REFRESH SCORES"};
constexpr int menu_count = 7;
// Ship editor (docs/ship-design-space.md): tools, its OPTIONS menu, canvas placement.
enum class Tool { line, move, erase, paint, thruster, weapon };
const char *const tool_names[] = {"LINE", "MOVE", "ERASE", "PAINT", "THRUSTER", "WEAPON"};
constexpr int tool_count = 6;
const char *const editor_menu[] = {"SAVE", "SAVE AS NEW SHIP", "TEST FLIGHT", "RENAME SHIP", "CUSTOM COLOUR", "PUBLIC IN GALLERY", "CLEAR ALL", "EXIT EDITOR"};
constexpr int editor_menu_count = 8;
constexpr float canvas_left = 110, canvas_top = 150, canvas_px = 860; // 400 design units square
constexpr float canvas_scale = canvas_px / 400;
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
    // ship editor
    cssteroids::editor::Editor ed;
    cssteroids::ShipGeometry edGeometry;           // what the saved ship will look like in game
    int edGeometryRevision = -1;
    Tool tool = Tool::line;
    int colorIndex = 0, menuIndex = 0, rgbChannel = 0;
    std::string customColor = "#ca3f3f";
    bool fineGrid = false, menuOpen = false, colorOpen = false, isPublic = true, testFlight = false, nameForShip = false;
    bool pending = false, grabbing = false, touchDown = false, touchSeen = false;
    cssteroids::editor::Pt cursor, pendingStart, grabFrom;
    float lastTouchX = 0, lastTouchY = 0; int lastTouchId = -1, lastTouchCount = 0;
    cssteroids::net::AsyncRequest saveRequest;
    std::string edStatus, lastSavedPass;
    std::vector<std::string> owned; // passphrases of ships made or unlocked on this console
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
                addOwned(d.passphrase); // the passphrase is the key: this ship can be updated
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
    // Ship outline, endpoint dots and flames from ship_render.hpp: the same geometry that
    // tools/ship-evidence.js checks against the browser's ship.js.
    void drawShip(const cssteroids::ShipGeometry &g, float x, float y, float angle, float scale, bool thrusting) {
        using hui::gfx::Color;
        const float width = 2.f * std::max(1.f, scale * .6f);
        for (const auto &w : cssteroids::ship_segments(g, x, y, angle, scale))
            scene.line(float(w.x1), float(w.y1), float(w.x2), float(w.y2), width, Color::rgb(w.color));
        for (const auto &d : cssteroids::ship_dots(g, x, y, angle, scale)) scene.circle(float(d.x), float(d.y), std::max(1.f, float(d.r)), Color::rgb(d.color));
        if (!thrusting) return;
        const float flicker = .7f + .3f * std::fabs(std::sin(game.time * 47)), c = std::cos(angle), sn = std::sin(angle), r = 15;
        auto at = [&](float px, float py) { return std::array<float, 2>{x + (px * c - py * sn) * scale, y + (px * sn + py * c) * scale}; };
        for (const auto &f : cssteroids::flame_anchors(g)) {
            auto base = at(f.x, f.y), tip = at(f.x, f.y + r * .6f * flicker);
            scene.line(base[0], base[1], tip[0], tip[1], 3 * std::max(1.f, scale * .6f), Color::rgb(0xffa500, .9f));
            scene.circle(tip[0], tip[1], 2 * scale, Color::rgb(0xff3000, .6f));
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
            if (on) scene.rounded_rect({760, 285.f + i * 40 - 30, 400, 38}, 8, hui::gfx::Color::rgb(0x2a4a6a, .8f));
            centered(menu_items[i], 285.f + i * 40, 26, on ? 0xffffff : 0x9fb3c8);
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
        std::snprintf(line, sizeof line, testFlight ? "TEST FLIGHT  -  CIRCLE back to editor" : "Level %d", game.level); text(line, testFlight ? 700 : 900, 60, 26, 0x9fb3c8);
        if (game.levelBanner > 0 && !game.over) centered(line, 520, 64);
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     OPTIONS  Pause     CIRCLE  Title", 40, 1050, 20, 0x8293ab);
        if (game.over) {
            scene.rounded_rect({0, 0, 1920, 1080}, 0, hui::gfx::Color::rgb(0x000000, .7f));
            centered("GAME OVER", 420, 72, 0xf47366);
            std::snprintf(line, sizeof line, "Final score %d     Best %d", game.score, scores.best); centered(line, 500, 32);
            if (server.valid() && game.score > 0 && !testFlight) {
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

    // --- ship editor ----------------------------------------------------------
    std::string currentColor() const { return colorIndex < int(cssteroids::editor::palette.size()) ? cssteroids::editor::palette[size_t(colorIndex)] : customColor; }
    void loadOwned() {
        std::ifstream file("/data/cssteroids-ps5/owned.txt");
        for (std::string line; std::getline(file, line);) if (!line.empty()) owned.push_back(line.substr(0, 64));
    }
    void addOwned(const std::string &pass) {
        if (pass.empty() || std::find(owned.begin(), owned.end(), pass) != owned.end()) return;
        owned.push_back(pass);
        mkdir("/data/cssteroids-ps5", 0777);
        std::ofstream("/data/cssteroids-ps5/owned.txt", std::ios::app) << pass << "\n";
    }
    void openEditor() {
        using namespace cssteroids::editor;
        Ship start;
        Ownership own = Ownership::fresh;
        std::string pass;
        cssteroids::json::Value v;
        if (!ship.source.empty() && cssteroids::json::parse(ship.source, v) && ship.geometry.custom() && from_server(v, start)) {
            const bool mine = !ship.passphrase.empty() && std::find(owned.begin(), owned.end(), ship.passphrase) != owned.end();
            own = mine ? Ownership::owned : Ownership::copy;
            if (mine) pass = ship.passphrase;
        } else start.name = (pilot + "'s ship").substr(0, max_name);
        ed = Editor(start, own, pass);
        cursor = {0, 0}; pending = grabbing = menuOpen = colorOpen = false; tool = Tool::line;
        edStatus = own == Ownership::owned ? "Editing your ship: SAVE updates it (" + pass + ")"
                 : own == Ownership::copy ? "Editing a copy of \"" + start.name + "\": SAVE makes a new ship"
                 : "New ship: LINE tool, CROSS to start and end each line";
        if (ed.legacyOutside) edStatus = "This ship was drawn beyond the editor's canvas; it can be edited and saved as it is";
        screen = Screen::editor;
    }
    // The saved ship as the game will draw it: through the same JSON path as a server reply.
    const cssteroids::ShipGeometry &editorGeometry() {
        if (edGeometryRevision != ed.revision) {
            cssteroids::json::Value v; cssteroids::ShipDesign d;
            if (cssteroids::json::parse(cssteroids::editor::post_body(ed.ship, {}, false), v) && cssteroids::parse_ship(v, d)) edGeometry = d.geometry;
            edGeometryRevision = ed.revision;
        }
        return edGeometry;
    }
    float toScreenX(double x) const { return canvas_left + float(x + 200) * canvas_scale; }
    float toScreenY(double y) const { return canvas_top + float(y + 200) * canvas_scale; }
    cssteroids::editor::Pt snapped() const {
        using namespace cssteroids::editor;
        Pt p = clamp_canvas(snap(cursor, fineGrid ? fine : grid));
        Pt v;
        if ((tool == Tool::line || tool == Tool::move) && ed.nearest_vertex(cursor, v, snap_radius) && inside(v)) p = v; // join existing points
        return p;
    }
    void editorPress() {   // CROSS or touchpad press
        using namespace cssteroids::editor;
        const Pt at = snapped();
        bool ok = true;
        switch (tool) {
            case Tool::line:
                if (!pending) { pending = true; pendingStart = at; return; }
                ok = ed.add_line(pendingStart, at, currentColor()); pending = false; break;
            case Tool::move: {
                Pt v;
                if (!grabbing) { if (ed.nearest_vertex(cursor, v, pick_radius)) { grabbing = true; grabFrom = v; } else ed.message = "no point here to move"; return; }
                ok = ed.move_point(grabFrom, at); grabbing = false; break;
            }
            case Tool::erase: ok = ed.erase_near(cursor); break;
            case Tool::paint: ok = ed.paint_near(cursor, currentColor()); break;
            case Tool::thruster: ok = ed.toggle_point(false, at); break;
            case Tool::weapon: ok = ed.toggle_point(true, at); break;
        }
        (void)ok;
    }
    void editorRelease() { // touchpad lift-off: completes a drag
        if (tool == Tool::line && pending) editorPress();
        else if (tool == Tool::move && grabbing) editorPress();
    }
    void saveShip(bool asNew) {
        using namespace cssteroids::editor;
        if (!server.valid()) { edStatus = "Offline: cannot save to the server"; return; }
        if (const std::string why = ed.save_problem(); !why.empty()) { edStatus = "Cannot save yet: " + why; return; }
        const std::string pass = !asNew && ed.ownership == Ownership::owned ? ed.passphrase : std::string();
        if (saveRequest.start(server, "POST", "/api/ships", post_body(ed.ship, pass, isPublic)))
            edStatus = pass.empty() ? "Saving a new ship ..." : "Updating " + pass + " ...";
    }
    void pollSave() {
        cssteroids::net::Response r;
        if (!saveRequest.take(r)) return;
        cssteroids::json::Value v; cssteroids::ShipDesign d;
        if (r.ok() && cssteroids::json::parse(r.body, v) && cssteroids::parse_ship(v, d) && !d.passphrase.empty()) {
            ed.ownership = cssteroids::editor::Ownership::owned;
            ed.passphrase = d.passphrase;
            addOwned(d.passphrase);
            useShip(d, true);
            lastSavedPass = d.passphrase;
            edStatus = "Saved. Passphrase: " + d.passphrase + "  (load it in the browser or on any PS5)";
            say("PSteroid5 editor: saved \"%s\" as %s (%zu lines)", d.name.c_str(), d.passphrase.c_str(), ed.ship.lines.size());
        } else edStatus = "Save failed: " + describe(r);
    }
    void editorInput(const struct pad &pad, float dt) {
        using namespace cssteroids::editor;
        const uint32_t pressed = pad.pressed;
        if (colorOpen) { // custom colour: D-pad up/down channel, left/right value, CROSS use, CIRCLE back
            unsigned rgb = unsigned(std::strtoul(customColor.c_str() + 1, nullptr, 16));
            int ch[3] = {int(rgb >> 16 & 255), int(rgb >> 8 & 255), int(rgb & 255)};
            if (pressed & PAD_UP) rgbChannel = (rgbChannel + 2) % 3;
            if (pressed & PAD_DOWN) rgbChannel = (rgbChannel + 1) % 3;
            if (pad.held & PAD_LEFT) ch[rgbChannel] = std::max(0, ch[rgbChannel] - 3);
            if (pad.held & PAD_RIGHT) ch[rgbChannel] = std::min(255, ch[rgbChannel] + 3);
            ch[rgbChannel] = std::clamp(ch[rgbChannel] + int(pad.left_x * 6), 0, 255);
            char hex[8]; std::snprintf(hex, sizeof hex, "#%02x%02x%02x", ch[0], ch[1], ch[2]); customColor = hex;
            if (pressed & PAD_CROSS) { colorIndex = int(palette.size()); colorOpen = false; }
            if (pressed & PAD_CIRCLE) colorOpen = false;
            return;
        }
        if (menuOpen) {
            if (pressed & PAD_UP) menuIndex = (menuIndex + editor_menu_count - 1) % editor_menu_count;
            if (pressed & PAD_DOWN) menuIndex = (menuIndex + 1) % editor_menu_count;
            if (pressed & (PAD_CIRCLE | PAD_OPTIONS)) { menuOpen = false; return; }
            if (!(pressed & PAD_CROSS)) return;
            menuOpen = false;
            switch (menuIndex) {
                case 0: saveShip(false); break;
                case 1: saveShip(true); break;
                case 2:
                    if (const std::string why = ed.save_problem(); !why.empty() && why != "a name is required") { edStatus = "Cannot fly yet: " + why; break; }
                    testFlight = true; game.geometry = editorGeometry(); startGame(); break;
                case 3: screen = Screen::name; nameForShip = true; nameDraft = ed.ship.name; nameCursor = 0; break;
                case 4: colorOpen = true; break;
                case 5: isPublic = !isPublic; edStatus = isPublic ? "Saved ships appear in the public gallery" : "Saved ships stay private (load them by passphrase)"; break;
                case 6: ed.clear(); break;
                default: screen = Screen::title; game.geometry = ship.geometry; break;
            }
            return;
        }
        // cursor: stick (accelerating), D-pad one grid step, touchpad like a laptop trackpad
        const double step = fineGrid ? fine : grid;
        const double sx = pad.left_x, sy = pad.left_y;
        const double speed = 40 + 260 * std::min(1.0, std::hypot(sx, sy));
        cursor.x += sx * std::fabs(sx) * speed * dt; cursor.y += sy * std::fabs(sy) * speed * dt;
        if (pressed & PAD_LEFT) cursor.x = snap(cursor, step).x - step;
        if (pressed & PAD_RIGHT) cursor.x = snap(cursor, step).x + step;
        if (pressed & PAD_UP) cursor.y = snap(cursor, step).y - step;
        if (pressed & PAD_DOWN) cursor.y = snap(cursor, step).y + step;
        if (pad.touch_count > 0) {
            if (!touchSeen) { touchSeen = true; say("PSteroid5 editor: touchpad active (first finger at %.0f,%.0f)", pad.touch_x, pad.touch_y); }
            if (lastTouchCount > 0 && pad.touch_id == lastTouchId) { // same finger: move by its travel
                cursor.x += (pad.touch_x - lastTouchX) * 0.26; cursor.y += (pad.touch_y - lastTouchY) * 0.26;
            }
            lastTouchX = pad.touch_x; lastTouchY = pad.touch_y; lastTouchId = pad.touch_id;
        }
        lastTouchCount = pad.touch_count;
        cursor = clamp_canvas(cursor);
        // pressing the touchpad is the mouse button: press = down, release = up (drag to draw / move)
        if (pressed & PAD_TOUCH_PAD) { touchDown = true; editorPress(); }
        if (touchDown && !(pad.held & PAD_TOUCH_PAD)) { touchDown = false; editorRelease(); }
        if (pressed & PAD_CROSS) editorPress();
        if (pressed & PAD_CIRCLE) { if (pending || grabbing) pending = grabbing = false; else { menuOpen = true; menuIndex = 0; } }
        if (pressed & PAD_OPTIONS) { menuOpen = true; menuIndex = 0; }
        if (pressed & PAD_SQUARE) { if (!ed.undo()) ed.message = "nothing to undo"; }
        if (pressed & PAD_TRIANGLE) { if (!ed.redo()) ed.message = "nothing to redo"; }
        if (pressed & PAD_R1) { tool = Tool((int(tool) + 1) % tool_count); pending = grabbing = false; }
        if (pressed & PAD_L1) { tool = Tool((int(tool) + tool_count - 1) % tool_count); pending = grabbing = false; }
        const int colours = int(palette.size()) + 1;
        if (pad.r2 > .6f && !r2Latch) { colorIndex = (colorIndex + 1) % colours; r2Latch = true; } else if (pad.r2 < .3f) r2Latch = false;
        if (pad.l2 > .6f && !l2Latch) { colorIndex = (colorIndex + colours - 1) % colours; l2Latch = true; } else if (pad.l2 < .3f) l2Latch = false;
        if (pressed & PAD_R3) ed.mirrorOn = !ed.mirrorOn;
        if (pressed & PAD_L3) fineGrid = !fineGrid;
    }
    bool r2Latch = false, l2Latch = false;
    void drawEditor() {
        using hui::gfx::Color;
        using namespace cssteroids::editor;
        const double now = wall_ms();
        // canvas, grid every 5 units (brighter every 25), mirror axis, front marker
        scene.rounded_rect({canvas_left, canvas_top, canvas_px, canvas_px}, 6, Color::rgb(0x050a12));
        for (int u = -200; u <= 200; u += 5) {
            const bool major = u % 25 == 0;
            const float a = major ? .16f : .06f;
            scene.line(toScreenX(u), canvas_top, toScreenX(u), canvas_top + canvas_px, 1, Color::rgb(0x6f8fb0, u == 0 ? .35f : a));
            scene.line(canvas_left, toScreenY(u), canvas_left + canvas_px, toScreenY(u), 1, Color::rgb(0x6f8fb0, a));
        }
        if (ed.mirrorOn) scene.line(toScreenX(0), canvas_top, toScreenX(0), canvas_top + canvas_px, 2, Color::rgb(0x70caff, .5f));
        text("FRONT", toScreenX(0) - 28, canvas_top - 10, 18, 0x8293ab);
        // the ship at design scale
        const int hot = (tool == Tool::erase || tool == Tool::paint) ? ed.nearest_line(cursor) : -1;
        for (size_t i = 0; i < ed.ship.lines.size(); i++) {
            const auto &l = ed.ship.lines[i];
            const uint32_t col = cssteroids::css_color(l.color);
            scene.line(toScreenX(l.a.x), toScreenY(l.a.y), toScreenX(l.b.x), toScreenY(l.b.y), int(i) == hot ? 6.f : 3.f,
                       Color::rgb(int(i) == hot ? (tool == Tool::erase ? 0xff4040 : cssteroids::css_color(currentColor())) : col));
            scene.circle(toScreenX(l.a.x), toScreenY(l.a.y), 3, Color::rgb(col)); scene.circle(toScreenX(l.b.x), toScreenY(l.b.y), 3, Color::rgb(col));
        }
        for (const auto &t : ed.ship.thrusters) { scene.ring(toScreenX(t.x), toScreenY(t.y), 10, 3, Color::rgb(0xffa500)); scene.line(toScreenX(t.x), toScreenY(t.y), toScreenX(t.x), toScreenY(t.y + 14), 4, Color::rgb(0xff6000, .8f)); }
        for (const auto &w : ed.ship.weapons) { scene.ring(toScreenX(w.x), toScreenY(w.y), 10, 3, Color::rgb(0xff4040)); scene.line(toScreenX(w.x), toScreenY(w.y), toScreenX(w.x), canvas_top, 1, Color::rgb(0xff4040, .35f)); }
        // cursor, pending line and its mirror, grabbed point
        const Pt at = snapped();
        if (pending) {
            scene.line(toScreenX(pendingStart.x), toScreenY(pendingStart.y), toScreenX(at.x), toScreenY(at.y), 3, Color::rgb(cssteroids::css_color(currentColor()), .8f));
            if (ed.mirrorOn) scene.line(toScreenX(mirror(pendingStart).x), toScreenY(pendingStart.y), toScreenX(mirror(at).x), toScreenY(at.y), 2, Color::rgb(cssteroids::css_color(currentColor()), .4f));
        }
        if (grabbing) scene.line(toScreenX(grabFrom.x), toScreenY(grabFrom.y), toScreenX(at.x), toScreenY(at.y), 2, Color::rgb(0x70caff, .7f));
        const float cx = toScreenX(at.x), cy = toScreenY(at.y);
        scene.line(cx - 14, cy, cx + 14, cy, 2, Color::rgb(0xffffff)); scene.line(cx, cy - 14, cx, cy + 14, 2, Color::rgb(0xffffff));
        if (ed.mirrorOn && at.x != 0) scene.ring(toScreenX(-at.x), cy, 6, 1.5f, Color::rgb(0x70caff, .6f));
        // right panel
        const float px = 1030;
        char line[160];
        text("SHIP EDITOR", px, 190, 40);
        text("\"" + ed.ship.name + "\"", px, 230, 24, 0x9fd3ff);
        for (int i = 0; i < tool_count; i++) {
            const bool on = int(tool) == i;
            if (on) scene.rounded_rect({px - 8 + i * 135.f, 254, 128, 36}, 6, Color::rgb(0x2a4a6a));
            text(tool_names[i], px + i * 135.f, 280, 20, on ? 0xffffff : 0x8293ab);
        }
        for (int i = 0; i <= int(palette.size()); i++) {
            const std::string c = i < int(palette.size()) ? palette[size_t(i)] : customColor;
            const float x = px + i * 52.f;
            if (i == colorIndex) scene.rounded_rect({x - 4, 306, 44, 44}, 8, Color::rgb(0xffffff));
            scene.rounded_rect({x, 310, 36, 36}, 6, Color::rgb(cssteroids::css_color(c)));
        }
        text("custom", px + int(palette.size()) * 52.f - 4, 368, 14, 0x8293ab);
        std::snprintf(line, sizeof line, "LINES %zu / 500    THRUSTERS %zu / 3    WEAPONS %zu / 2", ed.ship.lines.size(), ed.ship.thrusters.size(), ed.ship.weapons.size());
        text(line, px, 410, 22, 0xc8d2dc);
        std::snprintf(line, sizeof line, "MIRROR %s    GRID %s    %s", ed.mirrorOn ? "ON" : "OFF", fineGrid ? "FINE (1)" : "5", isPublic ? "PUBLIC" : "PRIVATE");
        text(line, px, 442, 22, 0x83bed4);
        // live truth: in-game size (1:1, flying), and a 3x view with the collision outline
        scene.rounded_rect({px, 470, 360, 300}, 8, Color::rgb(0x0b1622));
        text("IN GAME (actual size)", px + 12, 495, 16, 0x8293ab);
        const auto &g = editorGeometry();
        const float spin = float(now / 1400.0);
        drawShip(g, px + 180, 630, spin, 1, true);
        for (const auto &w : g.weapons.empty() ? std::vector<cssteroids::Point>{{0, -15}} : g.weapons) { // firing directions
            const float c = std::cos(spin), sn = std::sin(spin), bx = px + 180 + (w.x * c - w.y * sn), by = 630 + (w.x * sn + w.y * c);
            scene.line(bx, by, bx + std::sin(spin) * 60, by - std::cos(spin) * 60, 1, Color::rgb(0xff4500, .5f));
        }
        scene.rounded_rect({px + 380, 470, 420, 300}, 8, Color::rgb(0x0b1622));
        text("3x  COLLISION OUTLINE", px + 392, 495, 16, 0x8293ab);
        for (const auto &w : cssteroids::ship_segments(g, px + 590, 630, 0, 3)) scene.line(float(w.x1), float(w.y1), float(w.x2), float(w.y2), 2, Color::rgb(0x70e8a0));
        scene.ring(px + 590, 630, 15 * 3, 1, Color::rgb(0x70e8a0, .35f)); // centre fallback circle
        // help and messages
        text("STICK / TOUCHPAD  Cursor    D-PAD  Step    CROSS  Place    L1 R1  Tool    L2 R2  Colour", px, 830, 18, 0x8293ab);
        text("SQUARE  Undo    TRIANGLE  Redo    R3  Mirror    L3  Fine grid    OPTIONS  Menu", px, 858, 18, 0x8293ab);
        text("Touchpad: slide to move, press-and-hold to drag a line or point", px, 886, 18, 0x8293ab);
        if (!ed.message.empty()) text(ed.message, px, 930, 22, 0xf0c07d);
        if (saveRequest.pending()) text("Saving ...", px, 965, 22, 0x83bed4);
        text(edStatus, canvas_left, 1050, 20, 0x83bed4);
        if (!lastSavedPass.empty()) { text("PASSPHRASE", px, 1000, 18, 0xffd700); text(lastSavedPass, px + 140, 1000, 26, 0xffd700); }
        // menus
        if (menuOpen) {
            scene.rounded_rect({620, 230, 680, 80 + editor_menu_count * 52.f}, 12, Color::rgb(0x08111f, .96f));
            for (int i = 0; i < editor_menu_count; i++) {
                std::string label = editor_menu[i];
                if (i == 0) label = ed.ownership == Ownership::owned ? "SAVE (update " + ed.passphrase + ")" : "SAVE (new ship)";
                if (i == 5) label += isPublic ? ": YES" : ": NO";
                if (i == menuIndex) scene.rounded_rect({640, 270 + i * 52.f - 34, 640, 46}, 8, Color::rgb(0x2a4a6a));
                centered(label, 270 + i * 52.f, 26, i == menuIndex ? 0xffffff : 0x9fb3c8);
            }
        }
        if (colorOpen) {
            scene.rounded_rect({660, 330, 600, 330}, 12, Color::rgb(0x08111f, .96f));
            centered("CUSTOM COLOUR", 380, 30, 0xffd700);
            scene.rounded_rect({900, 400, 120, 70}, 8, Color::rgb(cssteroids::css_color(customColor)));
            centered(customColor, 510, 26);
            const unsigned rgb = unsigned(std::strtoul(customColor.c_str() + 1, nullptr, 16));
            const char *names[3] = {"RED", "GREEN", "BLUE"};
            for (int i = 0; i < 3; i++) {
                const int v = int(rgb >> (16 - 8 * i) & 255);
                std::snprintf(line, sizeof line, "%s %3d", names[i], v);
                centered(line, 560 + i * 34.f, 24, i == rgbChannel ? 0xffffff : 0x8293ab);
            }
            centered("UP/DOWN channel  LEFT/RIGHT or stick adjust  CROSS use  CIRCLE back", 650, 16, 0x8293ab);
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
            case 1: startCoopLobby(); break;
            case 2: startMmo(); break;
            case 3: screen = Screen::hangar; if (gallery.empty()) requestGallery(); break;
            case 4: openEditor(); break;
            case 5: screen = Screen::name; nameForShip = false; nameDraft = pilot; nameCursor = 0; break;
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
        if (pressed & PAD_CIRCLE) screen = nameForShip ? Screen::editor : Screen::title;
        if (pressed & PAD_OPTIONS) {
            while (!nameDraft.empty() && nameDraft.back() == ' ') nameDraft.pop_back();
            if (nameForShip) { if (!nameDraft.empty()) ed.set_name(nameDraft); screen = Screen::editor; return; }
            if (!nameDraft.empty()) { pilot = nameDraft; savePilot(); }
            screen = Screen::title;
        }
    }
    void gameInput(uint32_t pressed) {
        if ((pressed & PAD_CROSS) && game.over) { if (testFlight) game.geometry = editorGeometry(); startGame(); }
        if (pressed & PAD_CIRCLE) {
            paused = false;
            if (testFlight) { testFlight = false; game.geometry = ship.geometry; screen = Screen::editor; return; }
            screen = Screen::title; requestScores();
        }
        if ((pressed & PAD_OPTIONS) && !game.over) paused = !paused;
        // Scores reach the production database only when the player asks.
        if ((pressed & PAD_TRIANGLE) && game.over && !testFlight && server.valid() && game.score > 0 && !submitted && !submitRequest.pending())
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
        loadOwned();
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
        pollSave();
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
                case Screen::editor: editorInput(pad, dt); break;
            }
            exitHold = (pad.held & PAD_OPTIONS) && screen != Screen::name && screen != Screen::editor && !(screen == Screen::lobby && lobbyStep == LobbyStep::code) ? exitHold + dt : 0;
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
        if (game.over && !savedRun) { if (!testFlight) scores.record(game.score); savedRun = true; } // test flights do not count

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
            case Screen::editor: drawEditor(); break;
        }
        if (exitHold > .25f) centered("Hold OPTIONS to exit", 60, 25, 0xf0c07d);
        kit.renderer.begin(); kit.renderer.draw(scene);
        buildKitCommandBuffer(); submitFrame();
    }
};
VULKAN_EXAMPLE_MAIN()
