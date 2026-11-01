// PSteroids5: native PS5 CSSteroids. Vector Asteroids from CSSteroids-2 (public/js),
// with ships and high scores from the existing CSSteroids server over HTTP(S).
// Rendering uses the PS5 Vulkan Template and PS5_VKHomebrewUI, via the BattleStar shell.
#include "kit.hpp"

namespace {
// MULTIPLAYER = two-pilot campaign rooms (co-op); MMO = the drop-in shared world.
enum class Screen { title, hangar, name, game, lobby, multiplayer, editor };
const char *const menu_items[] = {"PLAY", "MULTIPLAYER", "MMO", "HANGAR", "SHIP EDITOR", "PILOT NAME", "SOUND", "REFRESH SCORES"};
constexpr int menu_count = 8, menu_sound = 6;
// Ship editor (docs/ship-design-space.md): tools, its OPTIONS menu, canvas placement.
enum class Tool { line, move, erase, paint, thruster, weapon, fill };
const char *const tool_names[] = {"LINE", "MOVE", "ERASE", "PAINT", "THRUSTER", "WEAPON", "FILL"};
constexpr int tool_count = 7;
const char *const editor_menu[] = {"SAVE", "SAVE AS NEW SHIP", "TEST FLIGHT", "RENAME SHIP", "CUSTOM COLOUR", "PUBLIC IN GALLERY", "WEAPON SOUND", "RESTORE LOCAL DRAFT", "SCALE DOWN 90%", "SCALE UP 110%", "CLEAR ALL", "EXIT EDITOR"};
constexpr int editor_menu_count = 12, editor_menu_sound = 6;
constexpr float canvas_left = 300, canvas_top = 170, canvas_px = 800; // 400 design units square
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
    cssteroids::visual::Starfield stars;
    cssteroids::visual::Trail ownTrail;
    struct RemoteTrail { cssteroids::visual::Trail trail; double x=0,y=0; bool located=false; };
    std::map<std::string,RemoteTrail> remoteTrails;
    Screen effectsScreen = Screen::title;
    double effectsAccumulator = 0;
    int draftRevision = -1, savedRevision = -1;
    double draftDelay = 0;
    bool draftFailed = false;
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
    bool patternOpen=false, patternMenu=false, patternPending=false, patternGrabbing=false, patternDirty=false, nameForPattern=false;
    cssteroids::editor::Editor tileEditor;
    cssteroids::editor::Pt fillSeed, shipCursor, tileCursor, tileStart, tileGrab;
    cssteroids::editor::pattern::Region fillRegion;
    cssteroids::editor::pattern::Tile activeTile;
    std::vector<cssteroids::editor::pattern::Tile> patternLibrary;
    std::vector<cssteroids::editor::Line> patternPreview;
    std::string fillId, patternError, savedSnapshot;
    int patternTool=0, patternMenuIndex=0, patternPreset=0, patternLibraryIndex=-1, patternRevision=0, previewRevision=-1;
    double patternScale=5, patternWidth=1, tileZoom=1;
    cssteroids::editor::Pt tilePan;
    bool patternR2=false,patternL2=false;
    const std::array<std::string,7> patternNames={"grid","hex","triangle","diamond","ribs","crosshatch","blank"};
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
    // OPTIONS in play opens this menu (RESUME / quit); CIRCLE is kept free for later
    bool quitMenu = false;
    int quitIndex = 0;
    std::string nameDraft;
    float accumulator = 0, exitHold = 0, previewSpin = 0;
    // sound: retro effects and the thruster loop (sound.hpp, the same recipes as the browser)
    cssteroids::sound::Mixer sfx{cssteroids::sound::thrust_loop_pcm, cssteroids::sound::thrust_loop_frames};
    int soundLevel = 8;
    static void fillAudio(int16_t *frames, int count, void *user) { static_cast<cssteroids::sound::Mixer *>(user)->fill(frames, count); }

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
    void loadSound() {
        std::ifstream file("/data/cssteroids-ps5/sound.txt");
        if (int level; file >> level) soundLevel = std::clamp(level, 0, 10);
        sfx.set_volume(soundLevel / 10.f);
    }
    void saveSound() {
        mkdir("/data/cssteroids-ps5", 0777);
        std::ofstream("/data/cssteroids-ps5/sound.txt", std::ios::trunc) << soundLevel << "\n";
    }
    void setSoundLevel(int level) {
        soundLevel = std::clamp(level, 0, 10);
        sfx.set_volume(soundLevel / 10.f);
        saveSound();
        sfx.shot(ship.shotSound); // a sample at the new volume
    }
    // Sounds from the MMO / co-op world: the player's own at full volume, the rest quieter and
    // only near enough to be on screen
    void playCues() {
        for (const auto &c : mp.cues) {
            using Cue = cssteroids::mmo::SoundCue;
            const bool near = std::fabs(c.x - mp.x) < 1060 && std::fabs(c.y - mp.y) < 640;
            if (!c.mine && !near) continue;
            const float gain = c.mine ? 1.f : .6f;
            switch (c.kind) {
                case Cue::shot: sfx.shot(ship.shotSound); break;
                case Cue::explosion: sfx.explosion(gain); break;
                case Cue::asteroid: sfx.asteroid_break(c.size, gain); break;
                case Cue::boundary: sfx.boundary(wall_ms() / 1000); break;
            }
        }
        mp.cues.clear();
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
            scene.line(float(w.x1), float(w.y1), float(w.x2), float(w.y2), g.custom() ? w.width * std::max(1.f, scale * .6f) : width, Color::rgb(w.color));
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
    // PowerUp.draw: ring, then the type's art at 0.25 scale; blinks out near expiry
    void drawPowerUp(const cssteroids::powerups::PowerUp &p, float x, float y, double rotation) {
        using hui::gfx::Color;
        namespace pu = cssteroids::powerups;
        if (pu::blinked_out(p.ageMs)) return;
        scene.ring(x, y, float(pu::radius), 1.5f, Color::rgb(pu::def(p.type).color));
        for (const auto &l : pu::art_segments(p.type, x, y, rotation)) scene.line(float(l.x1), float(l.y1), float(l.x2), float(l.y2), 2, Color::rgb(l.color));
    }
    // Ship.drawShield
    void drawShield(float x, float y, double msLeft) {
        using hui::gfx::Color;
        namespace pu = cssteroids::powerups;
        if (!pu::shield_visible(msLeft)) return;
        const uint32_t c = pu::def(pu::Type::shield).color;
        scene.circle(x, y, float(pu::shield_radius), Color::rgb(c, .12f));
        scene.ring(x, y, float(pu::shield_radius), 2, Color::rgb(c));
    }
    // drawEffectsHUD: one "Label: Ns" line per active timed effect
    void drawEffectTimers(const cssteroids::powerups::Effects &e, float x, float y) {
        namespace pu = cssteroids::powerups;
        char line[48];
        for (auto t : pu::types) {
            if (!e.active(t)) continue;
            std::snprintf(line, sizeof line, "%s: %ds", pu::def(t).label, int(std::ceil(e.ms[int(t)] / 1000)));
            text(line, x, y, 26, pu::def(t).color);
            y += 34;
        }
    }
    void drawStars() {
        using hui::gfx::Color;
        for(const auto &layer:stars.layers)for(const auto &s:layer.stars) {
            const double twinkle=1-s.pulse*(.5+.5*std::sin(stars.time*s.speed+s.phase));
            const float size=float(std::max(.5,s.size*(.8+.2*twinkle)));
            for(double x=cssteroids::visual::wrap(s.x-stars.travelX*layer.depth);x<1920;x+=1024)
                for(double y=cssteroids::visual::wrap(s.y-stars.travelY*layer.depth);y<1080;y+=1024)
                    scene.rounded_rect({float(x)-size/2,float(y)-size/2,size,size},0,Color::rgb(s.color,float(layer.alpha*twinkle)));
        }
        for(const auto &s:stars.shooting)for(size_t i=1;i<s.tail.size();++i) {
            const float k=float(i)/s.tail.size(), alpha=float(std::min(1.0,s.life/s.maxLife*1.5)*.8)*k;
            scene.line(s.tail[i-1].x,s.tail[i-1].y,s.tail[i].x,s.tail[i].y,.5f+k*1.5f,Color::rgb(0xffffff,alpha));
        }
    }
    void drawTrail(const cssteroids::visual::Trail &trail, float cameraX=0, float cameraY=0) {
        using hui::gfx::Color;
        using cssteroids::visual::Style;
        const cssteroids::visual::Particle *prev[3]={};
        for(const auto &p:trail.particles) {
            const float t=float(p.life/p.maxLife), x=float(p.x)-cameraX, y=float(p.y)-cameraY;
            if(p.style==Style::ribbon) {
                const auto *a=prev[p.nozzle];prev[p.nozzle]=&p;
                if(!a || std::fabs(a->x-p.x)>50 || std::fabs(a->y-p.y)>50)continue;
                scene.line(float(a->x)-cameraX,float(a->y)-cameraY,x,y,float(p.size)*t*4,Color::rgb(p.color,t*.25f));
                scene.line(float(a->x)-cameraX,float(a->y)-cameraY,x,y,float(p.size)*t,Color::rgb(p.color,t*.9f));
            } else if(x>-20 && x<1940 && y>-20 && y<1100) {
                if(p.style==Style::embers) {
                    const float r=(float(p.size)*t+.5f)*1.6f;
                    scene.circle(x,y,r*2,Color::rgb(p.color,t*.08f));
                    scene.circle(x,y,r,Color::rgb(p.color,t*.25f));
                    scene.circle(x,y,std::max(.6f,r*.4f),Color::rgb(p.color,t*.9f));
                } else {
                    const float size=std::max(1.f,std::round(float(p.size)*t*1.5f));
                    scene.rounded_rect({std::round(x-size/2),std::round(y-size/2),size,size},0,Color::rgb(p.color,t>.5f?1:t*2));
                }
            }
        }
    }
    void updateSpace(float dt, bool intercepted) {
        using namespace cssteroids;
        if(effectsScreen!=screen) {
            ownTrail.clear();remoteTrails.clear();effectsAccumulator=0;effectsScreen=screen;
        }
        const bool single=screen==Screen::game, multi=screen==Screen::multiplayer;
        const bool frozen=single && (paused || intercepted);
        if(frozen)return;
        const bool flying=single && !game.over && !game.ship.exploding;
        const bool online=multi && !mp.dead && mp.phase==mmo::Phase::playing;
        const double vx=flying?game.ship.vx:online?mp.vx:0, vy=flying?game.ship.vy:online?mp.vy:0;
        // Remote rendered motion supplies speed only; emission positions remain in world space.
        std::map<std::string,std::pair<double,double>> velocities;
        if(multi && mp.phase!=mmo::Phase::playing)remoteTrails.clear();
        if(multi && mp.phase==mmo::Phase::playing) {
            std::erase_if(remoteTrails,[&](const auto &entry){
                const auto it=mp.players.find(entry.first);
                return it==mp.players.end() || it->second.dead || it->second.outOfView;
            });
            for(const auto &[id,p]:mp.players) {
                if(p.dead || p.outOfView || (remoteTrails.size()>=64 && !remoteTrails.count(id)))continue;
                auto &r=remoteTrails[id];
                const double frames=std::max(.001,double(dt)*60);
                if(r.located && std::hypot(p.x-r.x,p.y-r.y)>100)r.trail.clear();
                velocities[id]={r.located?(p.x-r.x)/frames:0,r.located?(p.y-r.y)/frames:0};
                r.x=p.x;r.y=p.y;r.located=true;
            }
        }
        effectsAccumulator=std::min(effectsAccumulator+dt,6*visual::tick);
        while(effectsAccumulator>=visual::tick) {
            stars.step(vx,vy);
            if(single)ownTrail.step(game.geometry,game.ship.x,game.ship.y,game.ship.angle,vx,vy,game.ship.thrusting,flying,game.ship.effects);
            else if(multi) {
                ownTrail.step(ship.geometry,mp.x,mp.y,mp.angle,vx,vy,mp.thrusting,online,mp.effects_left(mp.effects,wall_ms()));
                for(auto &[id,r]:remoteTrails) {
                    const auto &p=mp.players.at(id);const auto v=velocities.at(id);
                    r.trail.step(p.geometry,p.x,p.y,p.angle,v.first,v.second,p.thrust,!p.dead,mp.effects_left(p.effects,wall_ms()));
                }
            }
            effectsAccumulator-=visual::tick;
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
        for (const auto &p : game.powerUps) drawPowerUp(p, float(p.x), float(p.y), p.rotation);
        for (const auto &d : game.debris)
            scene.line(d.x, d.y, d.x + std::cos(d.rotation) * d.length, d.y + std::sin(d.rotation) * d.length, 2,
                       Color::rgb(d.color, d.life > .5f ? 1.f : d.life * 2));
        drawTrail(ownTrail);
        const auto &s = game.ship;
        if (!game.over && !s.exploding && (!s.invulnerable() || int(s.invulnerableTime * 10) % 2 == 0)) {
            drawShip(game.geometry, s.x, s.y, s.angle, 1, s.thrusting);
            drawShield(s.x, s.y, s.effects.ms[int(cssteroids::powerups::Type::shield)]);
        }
    }
    void drawTitle() {
        centered("PSTEROID5", 170, 96);
        centered("a CSSteroids port for PS5", 240, 28, 0x9fb3c8);
        for (int i = 0; i < menu_count; i++) {
            const bool on = i == menu;
            std::string label = menu_items[i];
            if (i == menu_sound) label += soundLevel ? ": " + std::to_string(soundLevel) : ": OFF";
            if (on) scene.rounded_rect({760, 285.f + i * 36 - 30, 400, 36}, 8, hui::gfx::Color::rgb(0x2a4a6a, .8f));
            centered(label, 285.f + i * 36, 26, on ? 0xffffff : 0x9fb3c8);
        }
        char line[128];
        std::snprintf(line, sizeof line, "PILOT %s     SHIP %s     LOCAL BEST %d", pilot.c_str(), ship.name.c_str(), scores.best);
        centered(line, 580, 24, 0x70e8a0);
        drawShip(ship.geometry, 960, 665, previewSpin, 2.5f, false);
        centered("SERVER HIGH SCORES", 760, 26, 0xffd700);
        if (board.empty()) centered(scoreFetch.pending() ? "..." : "No scores to show", 800, 22, 0x9fb3c8);
        for (size_t i = 0; i < board.size() && i < 5; i++) {
            std::snprintf(line, sizeof line, "%zu. %s  %d", i + 1, board[i].name.c_str(), board[i].score);
            centered(line, 800 + i * 32.f, 24, i == 0 ? 0xffffff : 0xc8d2dc);
        }
        centered(onlineStatus, 1000, 22, server.valid() ? 0x83bed4 : 0xf0c07d);
        centered(menu == menu_sound ? "LEFT/RIGHT  Volume     CROSS  Next level     Hold OPTIONS  Exit"
                                    : "UP/DOWN  Select     CROSS  Confirm     Hold OPTIONS  Exit", 1045, 20, 0x8293ab);
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
        centered(nameForPattern?"PATTERN NAME":nameForShip?"SHIP NAME":"PILOT NAME",160,56);
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
        if (!game.over) drawEffectTimers(game.ship.effects, 1700, 100);
        std::snprintf(line, sizeof line, testFlight ? "TEST FLIGHT  -  OPTIONS menu to return to the editor" : "Level %d", game.level); text(line, testFlight ? 700 : 900, 60, 26, 0x9fb3c8);
        if (game.levelBanner > 0 && !game.over) centered(line, 520, 64);
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     OPTIONS  Menu", 40, 1050, 20, 0x8293ab);
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
            centered("CROSS to play again / OPTIONS for menu", 670, 26, 0x9fb3c8);
            if (scores.failed) centered("Best score kept for this session; saving was unavailable", 720, 22, 0xf0c07d);
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
        quitMenu = false;
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
        if (quitMenuInput(pressed)) return;
        if (pressed & PAD_OPTIONS) { openQuitMenu(); return; }
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
        // power-up spin is time-based, as the browser's renderMMO
        const double spin = now / 1000 * cssteroids::powerups::rotation_speed * 60;
        for (const auto &[id, p] : mp.powerUps) if (visible(p.x, p.y, 20)) drawPowerUp(p, sx(p.x), sy(p.y), spin);
        for (const auto &bu : mp.bursts) {
            const float r = float(bu.age) * (bu.big ? 90.f : 40.f);
            scene.ring(sx(bu.x), sy(bu.y), r, 2, Color::rgb(bu.color ? bu.color : 0xffa040, float(1 - bu.age)));
        }
        drawTrail(ownTrail,camX,camY);
        for(const auto &[id,r]:remoteTrails)drawTrail(r.trail,camX,camY);
        const int shield = int(cssteroids::powerups::Type::shield);
        for (const auto &[id, p] : mp.players) {
            if (p.dead || p.outOfView || !visible(p.x, p.y, 40)) continue;
            drawShip(p.geometry, sx(p.x), sy(p.y), float(p.angle), 1, p.thrust);
            drawShield(sx(p.x), sy(p.y), mp.effects_left(p.effects, now).ms[shield]);
            centered(p.name, sy(p.y) - 30, 18, 0x9fd3ff, sx(p.x));
        }
        const bool blink = mp.invulnerableMs > 0 && int(now / 100) % 2;
        const auto myEffects = mp.effects_left(mp.effects, now);
        if (!mp.dead && !blink && mp.phase == cssteroids::mmo::Phase::playing) {
            drawShip(ship.geometry, sx(mp.x), sy(mp.y), float(mp.angle), 1, mp.thrusting);
            drawShield(sx(mp.x), sy(mp.y), myEffects.ms[shield]);
        }
        if (mp.phase == cssteroids::mmo::Phase::warping) scene.ring(sx(mp.x), sy(mp.y), 30 + 10 * std::sin(float(now) / 120), 2, Color::rgb(0x70caff));
        int mine = 0;
        for (const auto &e2 : mp.scores) if (e2.id == mp.myId) mine = e2.score;
        char line[128];
        std::snprintf(line, sizeof line, "Score: %d", mine); text(line, 40, 60, 30);
        if (!mp.dead) drawEffectTimers(myEffects, 40, 100);
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
            centered("CROSS  Play again in this room     OPTIONS  Menu", 600, 26, 0x9fb3c8);
        }
        centered(mpStatus, 1000, 22, mp.phase == cssteroids::mmo::Phase::playing ? 0x83bed4 : 0xf0c07d);
        text("STICK / D-PAD  Turn     R2 / UP  Thrust     CROSS / R1  Fire     OPTIONS  Menu", 40, 1050, 20, 0x8293ab);
    }

    // --- in-play menu (OPTIONS) ------------------------------------------------
    std::string quitLabel() const {
        if (screen == Screen::multiplayer) return coopMode ? "LEAVE ROOM" : "LEAVE WORLD";
        return testFlight ? "BACK TO EDITOR" : "QUIT TO TITLE";
    }
    void openQuitMenu() {
        quitMenu = true; quitIndex = 0;
        if (screen == Screen::game) paused = true; // single player waits; the shared world keeps going
    }
    // true while the menu has the controller
    bool quitMenuInput(uint32_t pressed) {
        if (!quitMenu) return false;
        if (pressed & (PAD_UP | PAD_DOWN)) quitIndex ^= 1;
        const bool resume = (pressed & PAD_OPTIONS) || ((pressed & PAD_CROSS) && quitIndex == 0);
        if (resume) { quitMenu = false; if (screen == Screen::game) paused = false; }
        else if ((pressed & PAD_CROSS) && quitIndex == 1) {
            quitMenu = false; paused = false;
            if (screen == Screen::multiplayer) stopMultiplayer();
            else if (testFlight) { testFlight = false; game.geometry = ship.geometry; screen = Screen::editor; }
            else { screen = Screen::title; requestScores(); }
        }
        return true;
    }
    void drawQuitMenu() {
        if (!quitMenu) return;
        using hui::gfx::Color;
        scene.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, screen == Screen::game ? .6f : .35f));
        scene.rounded_rect({710, 380, 500, 290}, 12, Color::rgb(0x08111f, .96f));
        centered(screen == Screen::game ? "PAUSED" : "MENU", 440, 40, 0xffd700);
        if (screen == Screen::multiplayer) centered("The game keeps running", 478, 20, 0x9fb3c8);
        const std::string items[2] = {"RESUME", quitLabel()};
        for (int i = 0; i < 2; i++) {
            if (i == quitIndex) scene.rounded_rect({760, 530 + i * 56.f - 34, 400, 46}, 8, Color::rgb(0x2a4a6a));
            centered(items[i], 530 + i * 56.f, 28, i == quitIndex ? 0xffffff : 0x9fb3c8);
        }
        centered("UP/DOWN  Select     CROSS  Confirm     OPTIONS  Resume", 650, 18, 0x8293ab);
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
    void openEditor(bool restoreDraft=false) {
        using namespace cssteroids::editor;
        if(saveRequest.pending()) { onlineStatus="Ship save is finishing; open the editor again shortly"; return; }
        Ship start;
        Ownership own = Ownership::fresh;
        std::string pass;
        cssteroids::json::Value v;
        if (!ship.source.empty() && cssteroids::json::parse(ship.source, v) && ship.geometry.custom() && from_server(v, start)) {
            const bool mine = !ship.passphrase.empty() && std::find(owned.begin(), owned.end(), ship.passphrase) != owned.end();
            own = mine ? Ownership::owned : Ownership::copy;
            if (mine) pass = ship.passphrase;
        } else start.name = (pilot + "'s ship").substr(0, max_name);
        isPublic=!v.get("isPublic") || v.get("isPublic")->b;
        lastSavedPass.clear();
        // A local completed draft survives exiting/restarting without a server write.
        cssteroids::json::Value pendingPattern;
        std::ifstream draft("/data/cssteroids-ps5/editor-draft.json");
        if(restoreDraft && draft) {
            std::stringstream buffer;buffer<<draft.rdbuf();cssteroids::json::Value dv;
            if(buffer.str().size()<(2u<<20) && cssteroids::json::parse(buffer.str(),dv) && from_server(dv,start)) {
                pass=dv.str("passphrase");
                own=!pass.empty() && std::find(owned.begin(),owned.end(),pass)!=owned.end()?Ownership::owned:Ownership::copy;
                if(own!=Ownership::owned)pass.clear();
                isPublic=!dv.get("isPublic") || dv.get("isPublic")->b;
                if(const auto *pending=dv.get("patternDraft"))pendingPattern=*pending;
            }
        }
        if(restoreDraft && !draft) { edStatus="No local draft yet";return; }
        ed = Editor(start, own, pass);
        patternOpen=patternMenu=false;patternRevision=0;nameForPattern=false;
        edGeometryRevision=-1;draftRevision=ed.revision;savedRevision=-1;draftFailed=false;draftDelay=0;
        cursor = {0, 0}; pending = grabbing = menuOpen = colorOpen = false; tool = Tool::line;
        edStatus = own == Ownership::owned ? "Editing your ship: SAVE updates it (" + pass + ")"
                 : own == Ownership::copy ? "Editing a copy of \"" + start.name + "\": SAVE makes a new ship"
                 : "New ship: LINE tool, CROSS to start and end each line";
        if (ed.legacyOutside) edStatus = "This ship was drawn beyond the editor's canvas; it can be edited and saved as it is";
        screen = Screen::editor;
        if(restoreDraft && pendingPattern.is_object()){
            if(const auto *seed=pendingPattern.get("seed"))openPattern({seed->num("x"),seed->num("y")});
            if(patternOpen && pendingPattern.get("tile")){
                auto tile=cssteroids::editor::pattern::read_tile(*pendingPattern.get("tile"));
                if(cssteroids::editor::pattern::valid(tile)){loadPatternTile(tile);patternScale=pendingPattern.num("scale",5);fillId=pendingPattern.str("fillId");patternDirty=true;previewRevision=-1;}
            }
        }
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
    cssteroids::editor::pattern::Tile currentTile() const {
        auto t=activeTile;t.original={};t.lines.clear();t.width=patternWidth;
        const double k=t.side/400.;
        for(auto l:tileEditor.ship.lines){
            l.a={(l.a.x+200)*k,(l.a.y+200)*k};l.b={(l.b.x+200)*k,(l.b.y+200)*k};
            if(l.curved)l.control={(l.control.x+200)*k,(l.control.y+200)*k};
            t.lines.push_back(std::move(l));
        }
        return t;
    }
    void loadPatternTile(cssteroids::editor::pattern::Tile t) {
        using namespace cssteroids::editor;
        activeTile=t;patternWidth=t.width;
        Ship s;s.name=t.name.substr(0,max_name);s.color=t.color;
        const double k=400./t.side;
        for(auto l:t.lines){l.a={l.a.x*k-200,l.a.y*k-200};l.b={l.b.x*k-200,l.b.y*k-200};if(l.curved)l.control={l.control.x*k-200,l.control.y*k-200};s.lines.push_back(std::move(l));}
        tileEditor=Editor(s);tileEditor.mirrorOn=false;tileCursor={};tilePan={};tileZoom=1;
        patternPending=patternGrabbing=false;previewRevision=-1;++patternRevision;
    }
    void openPattern(cssteroids::editor::Pt seed) {
        using namespace cssteroids::editor;
        std::string error;auto rs=pattern::regions(ed.ship.lines,error);
        if(!error.empty()){ed.message=error;return;}
        const auto *r=pattern::region_at(rs,seed);
        if(!r){ed.message="Choose a closed hull region; join any open endpoints first";return;}
        fillRegion=*r;fillSeed=seed;shipCursor=cursor;fillId.clear();patternScale=5;patternDirty=false;
        auto tile=pattern::preset("grid");tile.color=currentColor();for(auto &l:tile.lines)l.color=tile.color;
        for(const auto &f:ed.ship.fills)if(pattern::match(rs,f)==r){
            fillId=f.id;patternScale=f.scale;
            if(f.kind=="custom")tile=f.tile;
            else {tile=pattern::preset(f.kind);tile.color=f.color;for(auto &l:tile.lines){l.color=f.color;l.width=f.width;}patternScale=std::max(.05,f.scale/2);}
            break;
        }
        loadPatternTile(tile);patternOpen=true;patternMenu=false;patternMenuIndex=0;patternTool=0;
        patternError.clear();pending=grabbing=touchDown=false;lastTouchCount=0;
    }
    bool applyPattern(bool close=true) {
        using namespace cssteroids::editor;
        if(!ed.apply_fill(fillSeed,currentTile(),patternScale,fillId)){patternError=ed.message;return false;}
        patternDirty=false;patternError.clear();++patternRevision;
        if(close){patternOpen=patternMenu=false;cursor=shipCursor;pending=grabbing=touchDown=false;lastTouchCount=0;}
        saveDraft();return true;
    }
    void previewPattern() {
        const int revision=tileEditor.revision+patternRevision;
        if(previewRevision==revision)return;
        previewRevision=revision;patternError.clear();
        patternPreview=cssteroids::editor::pattern::repeat(fillRegion,currentTile(),patternScale,patternError);
    }
    void patternPress() {
        using namespace cssteroids::editor;
        auto at=clamp_canvas(snap(tileCursor,fineGrid?fine:grid));Pt vertex;
        if(patternTool<2 && tileEditor.nearest_vertex(tileCursor,vertex))at=vertex;
        const int before=tileEditor.revision;
        if(patternTool==0){
            if(!patternPending){tileStart=at;patternPending=true;return;}
            if(tileEditor.ship.lines.size()+(tileEditor.mirrorOn?2:1)>256){patternError="A pattern tile supports at most 256 lines";return;}
            tileEditor.add_line(tileStart,at,currentColor(),patternWidth);patternPending=false;
        }else if(patternTool==1){
            if(!patternGrabbing){if(tileEditor.nearest_vertex(tileCursor,vertex,10)){tileGrab=vertex;patternGrabbing=true;}else patternError="Choose a tile endpoint";return;}
            tileEditor.move_point(tileGrab,at);patternGrabbing=false;
        }else if(patternTool==2)tileEditor.erase_near(tileCursor);
        else tileEditor.paint_near(tileCursor,currentColor(),patternWidth);
        if(tileEditor.revision!=before){++patternRevision;patternDirty=true;previewRevision=-1;}
        else patternError=tileEditor.message;
    }
    void loadPatternLibrary() {
        std::ifstream f("/data/cssteroids-ps5/pattern-library.json");if(!f)return;
        std::stringstream data;data<<f.rdbuf();cssteroids::json::Value a;
        if(data.str().size()>(2u<<20) || !cssteroids::json::parse(data.str(),a) || !a.is_array())return;
        patternLibrary.clear();for(const auto &v:a.items){auto t=cssteroids::editor::pattern::read_tile(v);if(cssteroids::editor::pattern::valid(t))patternLibrary.push_back(t);if(patternLibrary.size()==32)break;}
    }
    void savePatternLibrary() {
        using namespace cssteroids;
        if(patternLibrary.empty())loadPatternLibrary();auto t=currentTile();
        if(!editor::pattern::valid(t)){patternError="Invalid tile; library unchanged";return;}
        auto next=patternLibrary;auto it=std::find_if(next.begin(),next.end(),[&](const auto &old){return old.name==t.name;});
        if(it!=next.end())*it=t;else if(next.size()<32)next.push_back(t);else {patternError="Local library is full (32 patterns)";return;}
        json::Value a;a.type=json::Value::Type::array;for(const auto &p:next)a.items.push_back(editor::pattern::tile_json(p));
        mkdir("/data/cssteroids-ps5",0777);const std::string path="/data/cssteroids-ps5/pattern-library.json",tmp=path+".tmp";
        std::ofstream f(tmp,std::ios::trunc);f<<json::dump(a);bool ok=bool(f.flush());f.close();
        if(!ok || std::rename(tmp.c_str(),path.c_str())!=0){patternError="Could not save local pattern library";return;}
        patternLibrary=next;patternError="Pattern saved to this console's library";
    }
    void patternInput(const struct pad &pad,float dt) {
        using namespace cssteroids::editor;
        const uint32_t pressed=pad.pressed;
        if(patternMenu){
            constexpr int count=13;
            if(pressed & PAD_UP)patternMenuIndex=(patternMenuIndex+count-1)%count;
            if(pressed & PAD_DOWN)patternMenuIndex=(patternMenuIndex+1)%count;
            if(pressed & (PAD_CIRCLE|PAD_OPTIONS)){patternMenu=false;return;}
            const int direction=(pressed & PAD_LEFT)?-1:(pressed & PAD_RIGHT)?1:0;
            const bool activate=pressed & PAD_CROSS;
            if(patternMenuIndex==2 && (activate || direction)){
                patternPreset=(patternPreset+(direction?direction:1)+7)%7;
                auto t=patternNames[patternPreset]=="blank"?pattern::Tile{}:pattern::preset(patternNames[patternPreset]);
                t.color=currentColor();for(auto &l:t.lines){l.color=t.color;l.width=patternWidth;}t.width=patternWidth;
                loadPatternTile(t);patternDirty=true;return;
            }
            if(patternMenuIndex==3 && (activate || direction)){
                patternScale=std::clamp(patternScale*(direction<0?.8:1.25),.05,10000.);++patternRevision;previewRevision=-1;patternDirty=true;return;
            }
            if(patternMenuIndex==4 && (activate || direction)){patternWidth=patternWidth==1?2:1;++patternRevision;patternDirty=true;return;}
            if(patternMenuIndex==5 && (activate || direction)){tileZoom=tileZoom>=4?1:tileZoom*2;double limit=200-200/tileZoom;tilePan={std::clamp(tileCursor.x,-limit,limit),std::clamp(tileCursor.y,-limit,limit)};return;}
            if(!activate)return;
            if(patternMenuIndex==0){applyPattern();return;}
            if(patternMenuIndex==1){patternOpen=patternMenu=false;patternPending=patternGrabbing=touchDown=false;cursor=shipCursor;++patternRevision;saveDraft();return;}
            if(patternMenuIndex==6){saveDraft();screen=Screen::name;nameForPattern=true;nameForShip=false;nameDraft=activeTile.name.substr(0,name_max);nameCursor=0;patternMenu=false;return;}
            if(patternMenuIndex==7){savePatternLibrary();return;}
            if(patternMenuIndex==8){loadPatternLibrary();if(patternLibrary.empty()){patternError="No local patterns saved yet";return;}patternLibraryIndex=(patternLibraryIndex+1)%int(patternLibrary.size());loadPatternTile(patternLibrary[patternLibraryIndex]);patternDirty=true;return;}
            if(patternMenuIndex==9){if(ed.remove_fill(fillSeed)){patternOpen=patternMenu=false;cursor=shipCursor;++patternRevision;saveDraft();}else patternError=ed.message;return;}
            if(patternMenuIndex==10){if(applyPattern()){testFlight=true;game.geometry=editorGeometry();startGame();}return;}
            if(patternMenuIndex==11){
                std::ifstream f("/data/cssteroids-ps5/pattern-code.txt");std::stringstream buffer;buffer<<f.rdbuf();auto code=buffer.str();while(!code.empty() && (code.back()=='\n' || code.back()=='\r' || code.back()==' '))code.pop_back();
                pattern::Tile tile;std::string error;
                if(!f || !pattern::decode(code,tile,error)){patternError=error.empty()?"No pattern-code.txt to import":error;return;}
                loadPatternTile(tile);patternDirty=true;patternError="CSP1 pattern imported";return;
            }
            std::string error;auto code=pattern::encode(currentTile(),error);
            if(!error.empty()){patternError=error;return;}
            mkdir("/data/cssteroids-ps5",0777);const std::string path="/data/cssteroids-ps5/pattern-code.txt",tmp=path+".tmp";
            std::ofstream f(tmp,std::ios::trunc);f<<code<<"\n";bool written=bool(f.flush());f.close();
            patternError=written && std::rename(tmp.c_str(),path.c_str())==0?"CSP1 exported to pattern-code.txt":"Could not export pattern code";
            return;
        }
        const double step=fineGrid?fine:grid;
        tileCursor.x+=pad.left_x*std::fabs(pad.left_x)*250*dt/tileZoom;tileCursor.y+=pad.left_y*std::fabs(pad.left_y)*250*dt/tileZoom;
        if(pressed & PAD_LEFT)tileCursor.x=snap(tileCursor,step).x-step;
        if(pressed & PAD_RIGHT)tileCursor.x=snap(tileCursor,step).x+step;
        if(pressed & PAD_UP)tileCursor.y=snap(tileCursor,step).y-step;
        if(pressed & PAD_DOWN)tileCursor.y=snap(tileCursor,step).y+step;
        const double panLimit=200-200/tileZoom;
        tilePan.x=std::clamp(tilePan.x+pad.right_x*200*dt,-panLimit,panLimit);tilePan.y=std::clamp(tilePan.y+pad.right_y*200*dt,-panLimit,panLimit);
        if(pad.touch_count>0){if(lastTouchCount>0 && pad.touch_id==lastTouchId){tileCursor.x+=(pad.touch_x-lastTouchX)*.26/tileZoom;tileCursor.y+=(pad.touch_y-lastTouchY)*.26/tileZoom;}lastTouchX=pad.touch_x;lastTouchY=pad.touch_y;lastTouchId=pad.touch_id;}
        lastTouchCount=pad.touch_count;tileCursor=clamp_canvas(tileCursor);
        if(pressed & PAD_TOUCH_PAD){touchDown=true;patternPress();}
        if(touchDown && !(pad.held & PAD_TOUCH_PAD)){touchDown=false;if(patternPending || patternGrabbing)patternPress();}
        if(pressed & PAD_CROSS)patternPress();
        if(pressed & PAD_CIRCLE){if(patternPending || patternGrabbing)patternPending=patternGrabbing=false;else patternMenu=true;}
        if(pressed & PAD_OPTIONS)patternMenu=true;
        if(pressed & PAD_SQUARE){if(tileEditor.undo()){++patternRevision;patternDirty=true;previewRevision=-1;}}
        if(pressed & PAD_TRIANGLE){if(tileEditor.redo()){++patternRevision;patternDirty=true;previewRevision=-1;}}
        if(pressed & PAD_L1){patternTool=(patternTool+3)%4;patternPending=patternGrabbing=false;}
        if(pressed & PAD_R1){patternTool=(patternTool+1)%4;patternPending=patternGrabbing=false;}
        const int n=int(palette.size())+1;
        if(pad.r2>.6 && !patternR2){colorIndex=(colorIndex+1)%n;patternR2=true;}else if(pad.r2<.3)patternR2=false;
        if(pad.l2>.6 && !patternL2){colorIndex=(colorIndex+n-1)%n;patternL2=true;}else if(pad.l2<.3)patternL2=false;
        if(pressed & PAD_R3)tileEditor.mirrorOn=!tileEditor.mirrorOn;
        if(pressed & PAD_L3)fineGrid=!fineGrid;
    }
    void drawPattern() {
        using namespace cssteroids::editor;using hui::gfx::Color;
        previewPattern();
        text("SHIP / PATTERN / TEST",60,80,32,0x00ffff);text("PATTERN: "+activeTile.name,700,80,30,0xff80ff);
        text("REPEATING TILE",80,155,26,0x00ffff);text("LIVE SHIP FILL",1000,155,26,0x00ffff);
        const float left=80,top=200,size=720,viewLeft=1010,viewSize=700;
        scene.rounded_rect({left,top,size,size},0,Color::rgb(0x050a12));scene.rounded_rect({viewLeft,top,viewSize,viewSize},0,Color::rgb(0x050a12));
        auto x=[&](double x){return left+size/2+float((x-tilePan.x)*size/400*tileZoom);};
        auto y=[&](double y){return top+size/2+float((y-tilePan.y)*size/400*tileZoom);};
        pattern::Region viewport{{{left,top},{left+size,top},{left+size,top+size},{left,top+size}},{},{},size*size};
        auto clipped=[&](Pt a,Pt b,float width,uint32_t color,float alpha=1){for(const auto &l:pattern::clip(Line{{x(a.x),y(a.y)},{x(b.x),y(b.y)},"white"},viewport))scene.line(float(l.a.x),float(l.a.y),float(l.b.x),float(l.b.y),width,Color::rgb(color,alpha));};
        for(int i=0;i<=32;++i){double u=-200+i*12.5;clipped({u,-200},{u,200},1,0x3e6580,i%4==0?.5f:.2f);clipped({-200,u},{200,u},1,0x3e6580,i%4==0?.5f:.2f);}
        for(const auto &l:tileEditor.ship.lines){auto pts=pattern::flatten(l);for(size_t i=1;i<pts.size();++i)clipped(pts[i-1],pts[i],float(l.width)*2,cssteroids::css_color(l.color));}
        auto at=clamp_canvas(snap(tileCursor,fineGrid?fine:grid));
        if(patternPending){clipped(tileStart,at,2,cssteroids::css_color(currentColor()),.8);if(tileEditor.mirrorOn)clipped(mirror(tileStart),mirror(at),2,cssteroids::css_color(currentColor()),.4);}
        if(patternGrabbing)clipped(tileGrab,at,2,0x00ffff,.7);
        if(x(at.x)>=left && x(at.x)<=left+size && y(at.y)>=top && y(at.y)<=top+size){scene.line(x(at.x)-10,y(at.y),x(at.x)+10,y(at.y),2,Color::rgb(0xffffff));scene.line(x(at.x),y(at.y)-10,x(at.x),y(at.y)+10,2,Color::rgb(0xffffff));}
        auto sx=[&](double u){return viewLeft+viewSize/2+float(u*viewSize/400);};auto sy=[&](double u){return top+viewSize/2+float(u*viewSize/400);};
        for(const auto &l:ed.ship.lines)if(l.meshGroup!=fillId || fillId.empty()){auto pts=pattern::flatten(l);for(size_t i=1;i<pts.size();++i)scene.line(sx(pts[i-1].x),sy(pts[i-1].y),sx(pts[i].x),sy(pts[i].y),float(l.width),Color::rgb(cssteroids::css_color(l.color),l.mesh?.45f:1.f));}
        for(const auto &l:patternPreview)scene.line(sx(l.a.x),sy(l.a.y),sx(l.b.x),sy(l.b.y),float(l.width),Color::rgb(cssteroids::css_color(l.color)));
        for(size_t i=0;i<fillRegion.points.size();++i){auto a=fillRegion.points[i],b=fillRegion.points[(i+1)%fillRegion.points.size()];scene.line(sx(a.x),sy(a.y),sx(b.x),sy(b.y),2,Color::rgb(0xff00ff,.6));}
        if(tileZoom>1){scene.rounded_rect({820,760,150,150},0,Color::rgb(0x0b1827));for(const auto &l:tileEditor.ship.lines)scene.line(820+float(l.a.x+200)*.375f,760+float(l.a.y+200)*.375f,820+float(l.b.x+200)*.375f,760+float(l.b.y+200)*.375f,1,Color::rgb(cssteroids::css_color(l.color)));scene.ring(895+float(tilePan.x)*.375f,835+float(tilePan.y)*.375f,65/float(tileZoom),1,Color::rgb(0xffffff));}
        char info[200];std::snprintf(info,sizeof info,"%s   %zu / 256 LINES   WIDTH %.0f   REPEAT %.2f   ZOOM %.0fx",tool_names[patternTool],tileEditor.ship.lines.size(),patternWidth,patternScale,tileZoom);
        text(info,80,960,23,0x9fb3c8);
        text("L1 R1 Tool   L2 R2 Colour   CROSS Place   SQUARE Undo   TRIANGLE Redo   R3 Mirror   L3 Fine",80,1000,19,0x8293ab);
        text(patternError.empty()?"OPTIONS Apply / Presets / Scale / Library / Test     RIGHT STICK Pan     CIRCLE Cancel stroke / Menu":patternError,80,1040,19,patternError.empty()?0x8293ab:0xf0c07d);
        if(patternMenu){
            std::array<std::string,13> labels={"APPLY AND RETURN TO SHIP","CANCEL TILE CHANGES","PRESET: "+patternNames[patternPreset],"REPEAT SCALE (LEFT/RIGHT)","LINE WIDTH: "+std::to_string(int(patternWidth)),"ZOOM: "+std::to_string(int(tileZoom))+"x","RENAME PATTERN","SAVE TO LOCAL PATTERNS","LOAD NEXT LOCAL PATTERN","REMOVE THIS REGION'S FILL","APPLY AND TEST FLIGHT","IMPORT CSP1 FROM pattern-code.txt","EXPORT CSP1 TO pattern-code.txt"};
            scene.rounded_rect({480,110,960,840},12,Color::rgb(0x08111f,.98));
            for(int i=0;i<13;++i){if(i==patternMenuIndex)scene.rounded_rect({505,135+i*60.f,910,53},6,Color::rgb(0x124466));centered(labels[i],171+i*60.f,25,i==patternMenuIndex?0x00ffff:0x9fb3c8);}
        }
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
            case Tool::fill: openPattern(at);return;
        }
        (void)ok;
    }
    void editorRelease() { // touchpad lift-off: completes a drag
        if (tool == Tool::line && pending) editorPress();
        else if (tool == Tool::move && grabbing) editorPress();
    }
    void saveDraft(float dt=0) {
        if(screen!=Screen::editor && !nameForShip && !testFlight)return;
        const int revision=ed.revision+patternRevision;
        if(draftRevision==revision)return;
        draftDelay+=dt;if(dt>0 && draftDelay<.5)return;
        mkdir("/data/cssteroids-ps5",0777);
        const std::string path="/data/cssteroids-ps5/editor-draft.json", tmp=path+".tmp";
        std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
        cssteroids::json::Value draft;cssteroids::json::parse(cssteroids::editor::post_body(ed.ship,ed.passphrase,isPublic),draft);
        if(patternOpen){auto pending=cssteroids::json::Value::object();pending.set("seed",cssteroids::editor::pattern::point(fillSeed));pending.set("tile",cssteroids::editor::pattern::tile_json(currentTile()));pending.set("scale",cssteroids::editor::pattern::number(patternScale));pending.set("fillId",cssteroids::json::Value::string_value(fillId));draft.set("patternDraft",pending);}
        f<<cssteroids::json::dump(draft);
        const bool written=bool(f.flush());f.close();
        if(written && std::rename(tmp.c_str(),path.c_str())==0){draftRevision=revision;draftFailed=false;draftDelay=0;}
        else {draftFailed=true;draftDelay=0;edStatus="Draft save failed: keep the editor open or save to server";}
    }
    void saveShip(bool asNew) {
        using namespace cssteroids::editor;
        if(saveRequest.pending()){edStatus="A save is already in progress";return;}
        if (!server.valid()) { edStatus = "Offline: cannot save to the server"; return; }
        if (const std::string why = ed.save_problem(); !why.empty()) { edStatus = "Cannot save yet: " + why; return; }
        const std::string pass = !asNew && ed.ownership == Ownership::owned ? ed.passphrase : std::string();
        cssteroids::json::Value snapshot;cssteroids::json::parse(post_body(ed.ship,{},isPublic),snapshot);snapshot.set("editorDocument",document(ed.ship));savedSnapshot=cssteroids::json::dump(snapshot);
        if (saveRequest.start(server, pass.empty()?"POST":"PUT",pass.empty()?"/api/ships/editor":cssteroids::api::passphrase_path(pass),save_body(ed.ship,isPublic))) {
            savedRevision=ed.revision;
            edStatus = pass.empty() ? "Saving a new ship ..." : "Updating " + pass + " ...";
        }
    }
    void pollSave() {
        cssteroids::net::Response r;
        if (!saveRequest.take(r)) return;
        cssteroids::json::Value v; cssteroids::ShipDesign d;
        cssteroids::json::Value snapshot;
        const bool parsed=r.ok() && cssteroids::json::parse(r.body,v) && !v.str("passphrase").empty() && cssteroids::json::parse(savedSnapshot,snapshot);
        if(parsed){snapshot.set("passphrase",*v.get("passphrase"));if(v.get("id"))snapshot.set("id",*v.get("id"));if(v.get("isPublic"))snapshot.set("isPublic",*v.get("isPublic"));}
        if (parsed && cssteroids::parse_ship(snapshot,d)) {
            ed.ownership = cssteroids::editor::Ownership::owned;
            ed.passphrase = d.passphrase;
            addOwned(d.passphrase);
            if(!testFlight)useShip(d, true);
            else { ship=d;shipStore.save(d.source); }
            draftRevision=-1;saveDraft();
            lastSavedPass = d.passphrase;
            edStatus = savedRevision==ed.revision ? "Saved. Passphrase: "+d.passphrase : "Snapshot saved; newer edits remain in the local draft. "+d.passphrase;
            say("PSteroid5 editor: saved \"%s\" as %s (%zu lines)", d.name.c_str(), d.passphrase.c_str(), ed.ship.lines.size());
        } else edStatus = "Save failed: " + describe(r);
    }
    void editorInput(const struct pad &pad, float dt) {
        using namespace cssteroids::editor;
        const uint32_t pressed = pad.pressed;
        if(patternOpen){patternInput(pad,dt);return;}
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
                case 3: screen = Screen::name; nameForPattern=false; nameForShip = true; nameDraft = ed.ship.name; nameCursor = 0; break;
                case 4: colorOpen = true; break;
                case 5: isPublic = !isPublic; ed.revision++; edStatus="Visibility applies to new copies; updates keep existing visibility"; break;
                case editor_menu_sound: {
                    using cssteroids::sound::shot_sounds;
                    const auto it = std::find(shot_sounds.begin(), shot_sounds.end(), cssteroids::sound::valid_shot(ed.ship.shotSound));
                    ed.ship.shotSound = shot_sounds[size_t((it - shot_sounds.begin() + 1) % long(shot_sounds.size()))];
                    ed.revision++;
                    sfx.shot(ed.ship.shotSound);
                    menuOpen = true; // stay on the item to hear the next one
                    break;
                }
                case 7: openEditor(true); break;
                case 8: ed.scale_ship(.9); break;
                case 9: ed.scale_ship(1.1); break;
                case 10: ed.clear(); break;
                default: saveDraft(); screen = Screen::title; game.geometry = ship.geometry; break;
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
        if(patternOpen){drawPattern();return;}
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
            const int steps=l.curved?16:1;
            auto prev=l.a;
            for(int j=1;j<=steps;++j) {
                const double t=double(j)/steps,u=1-t;
                const Pt next=l.curved?Pt{u*u*l.a.x+2*u*t*l.control.x+t*t*l.b.x,u*u*l.a.y+2*u*t*l.control.y+t*t*l.b.y}
                                      :Pt{l.a.x+(l.b.x-l.a.x)*t,l.a.y+(l.b.y-l.a.y)*t};
                scene.line(toScreenX(prev.x),toScreenY(prev.y),toScreenX(next.x),toScreenY(next.y),int(i)==hot?6.f:float(l.width)*1.5f,
                           Color::rgb(int(i)==hot?(tool==Tool::erase?0xff4040:cssteroids::css_color(currentColor())):col));
                prev=next;
            }
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
        // TV-safe rail, canvas and context inspector, adapted from the browser designer.
        text("SHIP DESIGNER",60,80,36,0x00ffff);
        text(ed.ship.name,380,80,30,0xffffff);
        text("SHIP / PATTERN / TEST",1360,80,26,0xff00ff);
        for(int i=0;i<tool_count;++i) {
            const bool selected=int(tool)==i;
            scene.rounded_rect({55,190+i*74.f,210,60},8,Color::rgb(selected?0x124466:0x091525));
            if(selected)scene.rounded_rect({55,190+i*74.f,5,60},0,Color::rgb(0x00ffff));
            text(tool_names[i],75,230+i*74.f,26,selected?0x00ffff:0x9fb3c8);
        }
        text("L1 / R1",75,770,22,0x8293ab);
        text("Change tool",75,802,20,0x8293ab);
        text("OPTIONS",75,865,22,0x00ffff);
        text("Save / Scale",75,897,20,0x8293ab);
        text("Test flight",75,929,20,0x8293ab);
        // Rotation centre is fixed, independent of cursor and mirroring.
        scene.ring(toScreenX(0),toScreenY(0),8,2,Color::rgb(0xff00ff));
        scene.line(toScreenX(0)-12,toScreenY(0),toScreenX(0)+12,toScreenY(0),1,Color::rgb(0xff00ff));
        scene.line(toScreenX(0),toScreenY(0)-12,toScreenX(0),toScreenY(0)+12,1,Color::rgb(0xff00ff));
        const float px=1160;
        char line[160];
        text("INSPECTOR",px,195,28,0x00ffff);
        text(tool_names[int(tool)],px,238,32);
        const char *help[]={"Place two endpoints to draw a hull line.","Pick a joint, then place its new position.",
                            "Remove the highlighted line and its mirror.","Apply this colour to the highlighted line.",
                            "Place up to three exhaust nozzles.","Place up to two weapon hardpoints.","Choose a closed hull region to design its pattern."};
        text(help[int(tool)],px,280,20,0x9fb3c8);
        for(int i=0;i<=int(palette.size());++i) {
            const auto c=i<int(palette.size())?palette[size_t(i)]:customColor;
            const float x=px+i*55.f;
            if(i==colorIndex)scene.rounded_rect({x-4,306,44,44},8,Color::rgb(0xffffff));
            scene.rounded_rect({x,310,36,36},6,Color::rgb(cssteroids::css_color(c)));
        }
        text("L2 / R2  Colour     OPTIONS  Custom RGB",px,384,20,0x8293ab);
        std::snprintf(line,sizeof line,"LINES %zu / 5000    THRUST %zu / 3    WEAPONS %zu / 2",ed.ship.lines.size(),ed.ship.thrusters.size(),ed.ship.weapons.size());
        text(line,px,422,20,0xc8d2dc);
        std::snprintf(line,sizeof line,"MIRROR %s    GRID %s    %s",ed.mirrorOn?"ON":"OFF",fineGrid?"FINE (1)":"5",isPublic?"PUBLIC":"PRIVATE");
        text(line,px,456,20,0x83bed4);
        const auto &g=editorGeometry();
        scene.rounded_rect({px,485,300,240},8,Color::rgb(0x091525));
        text("ACTUAL FLIGHT SIZE",px+12,515,18,0x8293ab);
        drawShip(g,px+150,620,float(now/1400.0),1,true);
        scene.rounded_rect({px+320,485,320,240},8,Color::rgb(0x091525));
        text("3x  HULL / PATTERN",px+332,515,18,0x8293ab);
        for(const auto &w:cssteroids::ship_segments(g,px+480,620,0,3))
            scene.line(float(w.x1),float(w.y1),float(w.x2),float(w.y2),w.width,Color::rgb(w.color));
        text("MAGENTA  Rotation centre",px,768,20,0xff80ff);
        text("STICK / TOUCHPAD  Cursor    D-PAD  Step",px,810,20,0x8293ab);
        text("CROSS  Place    SQUARE  Undo    TRIANGLE  Redo",px,844,20,0x8293ab);
        text("R3  Mirror    L3  Fine grid    OPTIONS  Save / Test",px,878,20,0x8293ab);
        text("Touchpad: hold press to drag a line or point",px,912,20,0x8293ab);
        text(ed.ship.lines.size()>2000?"Heavy design: online play sheds pattern lines first":ed.message,px,954,20,0xf0c07d);
        text(saveRequest.pending()?"SAVING TO SERVER...":draftFailed?"LOCAL DRAFT FAILED":draftRevision==ed.revision?"OPTIONS  Restore local draft":"LOCAL DRAFT PENDING",px,992,18,draftFailed?0xff4040:0x70e8a0);
        text(edStatus,60,1045,18,0x83bed4);
        // menus
        if (menuOpen) {
            scene.rounded_rect({620, 230, 680, 80 + editor_menu_count * 52.f}, 12, Color::rgb(0x08111f, .96f));
            for (int i = 0; i < editor_menu_count; i++) {
                std::string label = editor_menu[i];
                if (i == 0) label = ed.ownership == Ownership::owned ? "SAVE (update " + ed.passphrase + ")" : "SAVE (new ship)";
                if (i == 5) label += isPublic ? ": YES" : ": NO";
                if (i == editor_menu_sound) {
                    label += ": " + cssteroids::sound::valid_shot(ed.ship.shotSound);
                    for (auto &c : label) c = char(std::toupper((unsigned char)c));
                }
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
        screen = Screen::game; paused = false; quitMenu = false; savedRun = false; submitted = false; submitStatus.clear();
        game.reset(); ownTrail.clear(); remoteTrails.clear(); accumulator=0;
    }
    void titleInput(uint32_t pressed) {
        if (pressed & PAD_UP) menu = (menu + menu_count - 1) % menu_count;
        if (pressed & PAD_DOWN) menu = (menu + 1) % menu_count;
        if (menu == menu_sound) {
            if (pressed & PAD_LEFT) setSoundLevel(soundLevel - 1);
            if (pressed & PAD_RIGHT) setSoundLevel(soundLevel + 1);
            if (pressed & PAD_CROSS) setSoundLevel(soundLevel == 10 ? 0 : soundLevel + 1);
            return;
        }
        if (!(pressed & PAD_CROSS)) return;
        switch (menu) {
            case 0: startGame(); break;
            case 1: startCoopLobby(); break;
            case 2: startMmo(); break;
            case 3: screen = Screen::hangar; if (gallery.empty()) requestGallery(); break;
            case 4: openEditor(); break;
            case 5: screen = Screen::name; nameForPattern=false; nameForShip = false; nameDraft = pilot; nameCursor = 0; break;
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
        if (pressed & PAD_CIRCLE) {screen = (nameForShip || nameForPattern) ? Screen::editor : Screen::title;nameForPattern=false;}
        if (pressed & PAD_OPTIONS) {
            while (!nameDraft.empty() && nameDraft.back() == ' ') nameDraft.pop_back();
            if(nameForPattern){if(!nameDraft.empty()){activeTile.name=nameDraft;++patternRevision;}nameForPattern=false;screen=Screen::editor;return;}
            if (nameForShip) { if (!nameDraft.empty()) ed.set_name(nameDraft); screen = Screen::editor; return; }
            if (!nameDraft.empty()) { pilot = nameDraft; savePilot(); }
            screen = Screen::title;
        }
    }
    void gameInput(uint32_t pressed) {
        if (quitMenuInput(pressed)) return;
        if (pressed & PAD_OPTIONS) { openQuitMenu(); return; }
        if ((pressed & PAD_CROSS) && game.over) { if (testFlight) game.geometry = editorGeometry(); startGame(); }
        // Scores reach the production database only when the player asks.
        if ((pressed & PAD_TRIANGLE) && game.over && !testFlight && server.valid() && game.score > 0 && !submitted && !submitRequest.pending())
            submitRequest.start(server, "POST", cssteroids::api::high_scores_path,
                                cssteroids::api::high_score_body(pilot, game.score, ship.typeName, ship.colorName, ship.passphrase));
    }

public:
    ~VulkanExample() { audio_stop(); }
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
        loadSound();
        if (!audio_start(&VulkanExample::fillAudio, &sfx)) say("PSteroid5: no audio output");
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
        if (screen == Screen::multiplayer || screen == Screen::lobby) pumpMultiplayer(intercepted || quitMenu || screen == Screen::lobby ? cssteroids::Controls{} : in, dt);
        playCues();
        if (screen == Screen::game && !paused && !intercepted) {
            accumulator = std::min(accumulator + dt, 6 * cssteroids::step_seconds);
            while (accumulator >= cssteroids::step_seconds) { game.step(in); accumulator -= cssteroids::step_seconds; }
        } else game.time += dt; // keeps menu animations moving
        size_t broken = 0;
        for (const auto e : game.events) {
            using cssteroids::Event;
            if (e == Event::shot) sfx.shot(testFlight ? ed.ship.shotSound : ship.shotSound);
            else if (e == Event::ship_hit) sfx.explosion();
            else if (e == Event::asteroid_hit && broken < game.brokenSizes.size()) sfx.asteroid_break(game.brokenSizes[broken++]);
        }
        game.events.clear(); game.brokenSizes.clear();
        // the player's own thrusters, while the ship is flying
        sfx.set_thrust(!intercepted && ((screen == Screen::game && !paused && !game.over && game.ship.thrusting && !game.ship.exploding) ||
                                        (screen == Screen::multiplayer && mp.phase == cssteroids::mmo::Phase::playing && mp.thrusting && !mp.dead)));
        updateSpace(dt,intercepted);
        if(screen==Screen::editor)saveDraft(dt);
        if (game.over && !savedRun) { if (!testFlight) scores.record(game.score); savedRun = true; } // test flights do not count

        scene.clear();
        using hui::gfx::Color;
        drawStars();
        switch (screen) {
            case Screen::title: drawTitle(); break;
            case Screen::hangar: drawHangar(); break;
            case Screen::name: drawNameEntry(); break;
            case Screen::game: drawWorld(); drawHud(); drawQuitMenu(); break;
            case Screen::multiplayer: drawMultiplayer(); drawQuitMenu(); break;
            case Screen::lobby: drawLobby(); break;
            case Screen::editor: drawEditor(); break;
        }
        if (exitHold > .25f) centered("Hold OPTIONS to exit", 60, 25, 0xf0c07d);
        kit.renderer.begin(); kit.renderer.draw(scene);
        buildKitCommandBuffer(); submitFrame();
    }
};
VULKAN_EXAMPLE_MAIN()
