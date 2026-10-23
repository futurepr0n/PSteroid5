#pragma once
// Controller-first ship editor model (docs/ship-design-space.md). Design units are the browser
// customizer's: a 400x400 canvas, origin at the centre, nose up; the game draws at 0.25.
// Every edit goes through apply(), which refuses anything the server would reject, so the
// editor cannot produce an invalid ship. Portable and host-tested.
#include "json_lite.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cssteroids::editor {
constexpr double half = 200;          // canvas half-size (±200 design units)
constexpr double grid = 5, fine = 1;  // 5 units = 1.25 px in game
constexpr size_t max_lines = 500, max_thrusters = 3, max_weapons = 2, max_name = 32, max_undo = 200;
constexpr double snap_radius = 6;     // endpoints this close are treated as one
constexpr double pick_radius = 10;    // cursor reach for picking lines/points

struct Pt { double x = 0, y = 0; };
inline bool same_pt(Pt a, Pt b, double eps = 1e-9) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps; }
struct Line { Pt a, b; std::string color; };

// The browser's preset swatches (index.html .color-option) as the hex it stores per line.
inline const std::vector<std::string> palette = {"#ffffff", "#00ffff", "#00ff00", "#ffff00", "#ff9900", "#ff00ff", "#ff0000", "#0066ff"};

struct Ship {
    std::string name = "PS5 Ship", color = "white";
    std::vector<Line> lines;
    std::vector<Pt> thrusters, weapons;
};

enum class Ownership { fresh, owned, copy }; // copy: from the gallery, saved without its passphrase

inline bool inside(Pt p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::fabs(p.x) <= half && std::fabs(p.y) <= half; }
inline Pt mirror(Pt p) { return {p.x == 0 ? 0 : -p.x, p.y}; }
inline Pt snap(Pt p, double step) { return {std::round(p.x / step) * step, std::round(p.y / step) * step}; }
inline Pt clamp_canvas(Pt p) { return {std::clamp(p.x, -half, half), std::clamp(p.y, -half, half)}; }

// The server's acceptance rules (validateShipData + sanitizeMMOShipData limits) plus, unless
// the ship was drawn elsewhere already beyond it, the editor's ±200 canvas.
inline std::string invalid_reason(const Ship &s, bool canvas = true) {
    std::string n = s.name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty()) return "a name is required";
    if (s.name.size() > max_name) return "name longer than 32";
    if (s.lines.empty()) return "draw at least one line";
    if (s.lines.size() > max_lines) return "more than 500 lines";
    if (s.thrusters.size() > max_thrusters) return "more than 3 thrusters";
    if (s.weapons.size() > max_weapons) return "more than 2 weapons";
    for (const auto &l : s.lines) {
        if (l.color.size() > max_name) return "colour name longer than 32";
        if (canvas && (!inside(l.a) || !inside(l.b))) return "a line leaves the canvas";
        if (!std::isfinite(l.a.x + l.a.y + l.b.x + l.b.y)) return "a point is not a number";
    }
    if (canvas) {
        for (const auto &p : s.thrusters) if (!inside(p)) return "a thruster leaves the canvas";
        for (const auto &p : s.weapons) if (!inside(p)) return "a weapon leaves the canvas";
    }
    return {};
}
inline bool within_canvas(const Ship &s) {
    for (const auto &l : s.lines) if (!inside(l.a) || !inside(l.b)) return false;
    for (const auto &p : s.thrusters) if (!inside(p)) return false;
    for (const auto &p : s.weapons) if (!inside(p)) return false;
    return true;
}
// A ship still being drawn may have no lines yet or no name; everything else holds at every step.
inline std::string edit_problem(const Ship &s, bool canvas = true) {
    Ship t = s;
    if (t.lines.empty()) t.lines.push_back({{0, 0}, {0, 1}, "#ffffff"});
    if (t.name.empty()) t.name = "x";
    return invalid_reason(t, canvas);
}
inline bool editable(const Ship &s, bool canvas = true) { return edit_problem(s, canvas).empty(); }

class Editor {
public:
    Ship ship;
    Ownership ownership = Ownership::fresh;
    std::string passphrase; // set when owned
    bool mirrorOn = true;
    std::string message;    // why the last edit was refused (for the HUD)

    // A ship drawn in the browser beyond ±200 stays editable; only new placements are held to
    // the canvas (the cursor cannot leave it).
    bool legacyOutside = false;
    int revision = 0; // bumps on every change (for cached previews)

    explicit Editor(Ship s = {}, Ownership o = Ownership::fresh, std::string pass = {})
        : ship(std::move(s)), ownership(o), passphrase(std::move(pass)), legacyOutside(!within_canvas(ship)) {}

    bool can_undo() const { return !undoStack.empty(); }
    bool can_redo() const { return !redoStack.empty(); }
    bool undo() { if (undoStack.empty()) return false; redoStack.push_back(ship); ship = undoStack.back(); undoStack.pop_back(); revision++; return true; }
    bool redo() { if (redoStack.empty()) return false; undoStack.push_back(ship); ship = redoStack.back(); redoStack.pop_back(); revision++; return true; }
    // What saving would be refused for (empty = ready to save).
    std::string save_problem() const { return invalid_reason(ship, !legacyOutside); }

    // --- edits (each one undoable; false + message when refused) -------------
    // a == b makes a dot: the browser stores a click without a drag the same way (3% of
    // gallery lines) and draws it as the endpoint dots of a zero-length line.
    bool add_line(Pt a, Pt b, const std::string &color) {
        Ship next = ship;
        next.lines.push_back({a, b, color});
        if (mirrorOn) {
            Line m{mirror(a), mirror(b), color};
            if (!line_exists(next, m)) next.lines.push_back(m);
        }
        return commit(std::move(next));
    }
    // Delete the line nearest the cursor (and its mirror image when mirroring).
    bool erase_near(Pt c) {
        const int i = nearest_line(c);
        if (i < 0) return refuse("no line here");
        Ship next = ship;
        const Line gone = next.lines[size_t(i)];
        next.lines.erase(next.lines.begin() + i);
        if (mirrorOn) erase_line(next, {mirror(gone.a), mirror(gone.b), gone.color});
        return commit(std::move(next));
    }
    bool paint_near(Pt c, const std::string &color) {
        const int i = nearest_line(c);
        if (i < 0) return refuse("no line here");
        Ship next = ship;
        const Line was = next.lines[size_t(i)];
        next.lines[size_t(i)].color = color;
        if (mirrorOn) for (auto &l : next.lines) if (same_line(l, {mirror(was.a), mirror(was.b), was.color})) l.color = color;
        return commit(std::move(next));
    }
    // Move every endpoint at `from` (and its mirror) to `to`: the whole joint moves.
    bool move_point(Pt from, Pt to) {
        if (same_pt(from, to)) return false;
        Ship next = ship;
        bool moved = false;
        auto shift = [&](Pt f, Pt t) {
            for (auto &l : next.lines) {
                if (same_pt(l.a, f, 1e-6)) { l.a = t; moved = true; }
                if (same_pt(l.b, f, 1e-6)) { l.b = t; moved = true; }
            }
            for (auto *pts : {&next.thrusters, &next.weapons}) for (auto &p : *pts) if (same_pt(p, f, 1e-6)) { p = t; moved = true; }
        };
        shift(from, to);
        if (mirrorOn && from.x != 0) shift(mirror(from), mirror(to));
        // a line whose ends meet becomes a dot, as in the browser (erase removes it)
        if (!moved) return refuse("nothing to move here");
        return commit(std::move(next));
    }
    // Toggle a thruster/weapon at the cursor: remove the nearest one, else add (mirrored pair when room).
    bool toggle_point(bool weapon, Pt c) {
        Ship next = ship;
        auto &pts = weapon ? next.weapons : next.thrusters;
        const size_t limit = weapon ? max_weapons : max_thrusters;
        const int i = nearest_point(pts, c);
        if (i >= 0) {
            const Pt gone = pts[size_t(i)];
            pts.erase(pts.begin() + i);
            if (mirrorOn && gone.x != 0) { const int j = nearest_point(pts, mirror(gone), 1e-6); if (j >= 0) pts.erase(pts.begin() + j); }
            return commit(std::move(next));
        }
        if (pts.size() >= limit) return refuse(weapon ? "2 weapons is the maximum" : "3 thrusters is the maximum");
        pts.push_back(c);
        if (mirrorOn && c.x != 0 && pts.size() < limit) pts.push_back(mirror(c));
        return commit(std::move(next));
    }
    bool set_name(std::string name) {
        if (name.size() > max_name) name.resize(max_name);
        Ship next = ship; next.name = std::move(name);
        return commit(std::move(next));
    }
    bool set_ship_color(const std::string &color) { Ship next = ship; next.color = color; return commit(std::move(next)); }
    bool clear() { Ship next = ship; next.lines.clear(); next.thrusters.clear(); next.weapons.clear(); return commit(std::move(next)); }

    // --- queries ------------------------------------------------------------
    int nearest_line(Pt c, double reach = pick_radius) const {
        int best = -1; double bd = reach;
        for (size_t i = 0; i < ship.lines.size(); i++) {
            const double d = seg_dist(c, ship.lines[i].a, ship.lines[i].b);
            if (d <= bd) { bd = d; best = int(i); }
        }
        return best;
    }
    // The nearest existing endpoint or point within reach (for snapping and MOVE).
    bool nearest_vertex(Pt c, Pt &out, double reach = snap_radius) const {
        double bd = reach; bool found = false;
        auto consider = [&](Pt p) { const double d = std::hypot(p.x - c.x, p.y - c.y); if (d <= bd) { bd = d; out = p; found = true; } };
        for (const auto &l : ship.lines) { consider(l.a); consider(l.b); }
        for (const auto &p : ship.thrusters) consider(p);
        for (const auto &p : ship.weapons) consider(p);
        return found;
    }

private:
    std::vector<Ship> undoStack, redoStack;

    bool refuse(const char *why) { message = why; return false; }
    bool commit(Ship next) {
        if (const std::string why = edit_problem(next, !legacyOutside); !why.empty()) { message = why; return false; }
        undoStack.push_back(ship);
        if (undoStack.size() > max_undo) undoStack.erase(undoStack.begin());
        redoStack.clear();
        ship = std::move(next);
        message.clear();
        revision++;
        return true;
    }
    static double seg_dist(Pt p, Pt a, Pt b) {
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0;
        return std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y);
    }
    static bool same_line(const Line &l, const Line &m) {
        return (same_pt(l.a, m.a, 1e-6) && same_pt(l.b, m.b, 1e-6)) || (same_pt(l.a, m.b, 1e-6) && same_pt(l.b, m.a, 1e-6));
    }
    static bool line_exists(const Ship &s, const Line &m) { for (const auto &l : s.lines) if (same_line(l, m)) return true; return false; }
    static void erase_line(Ship &s, const Line &m) {
        for (size_t i = 0; i < s.lines.size(); i++) if (same_line(s.lines[i], m)) { s.lines.erase(s.lines.begin() + long(i)); return; }
    }
    static int nearest_point(const std::vector<Pt> &pts, Pt c, double reach = pick_radius) {
        int best = -1; double bd = reach;
        for (size_t i = 0; i < pts.size(); i++) { const double d = std::hypot(pts[i].x - c.x, pts[i].y - c.y); if (d <= bd) { bd = d; best = int(i); } }
        return best;
    }
};

// --- interchange with the server ---------------------------------------------
inline json::Value num(double v) {
    // design coordinates: keep up to 1/1024 like the browser's own fractional values
    return json::Value::number_value(std::round(v * 1024) / 1024);
}
// POST /api/ships body, as the browser customizer sends it. A passphrase updates that ship.
inline std::string post_body(const Ship &s, const std::string &passphrase, bool isPublic = true) {
    json::Value v = json::Value::object();
    v.set("name", json::Value::string_value(s.name));
    v.set("type", json::Value::string_value("custom"));
    v.set("color", json::Value::string_value(s.color));
    json::Value lines; lines.type = json::Value::Type::array;
    for (const auto &l : s.lines) {
        json::Value o = json::Value::object();
        o.set("startX", num(l.a.x)); o.set("startY", num(l.a.y)); o.set("endX", num(l.b.x)); o.set("endY", num(l.b.y));
        o.set("color", json::Value::string_value(l.color));
        lines.items.push_back(std::move(o));
    }
    v.set("customLines", std::move(lines));
    for (auto [key, pts] : {std::pair{"thrusterPoints", &s.thrusters}, std::pair{"weaponPoints", &s.weapons}}) {
        json::Value arr; arr.type = json::Value::Type::array;
        for (const auto &p : *pts) { json::Value o = json::Value::object(); o.set("x", num(p.x)); o.set("y", num(p.y)); arr.items.push_back(std::move(o)); }
        v.set(key, std::move(arr));
    }
    v.set("isPublic", json::Value::bool_value(isPublic));
    if (!passphrase.empty()) v.set("passphrase", json::Value::string_value(passphrase));
    return json::dump(v);
}

// A server ship (object from /api/ships/...) into the editor, exactly: coordinates and colours kept.
inline bool from_server(const json::Value &v, Ship &out) {
    if (!v.is_object()) return false;
    Ship s;
    s.name = v.str("name", "Ship").substr(0, max_name);
    s.color = v.str("color", "white").substr(0, max_name);
    auto arr = [&](const char *key, json::Value &scratch) -> const json::Value * {
        const json::Value *a = v.get(key);
        if (a && a->type == json::Value::Type::string && json::parse(a->s, scratch)) a = &scratch;
        return a && a->is_array() ? a : nullptr;
    };
    json::Value scratch;
    if (const auto *lines = arr("customLines", scratch))
        for (const auto &l : lines->items) {
            if (!l.is_object()) continue;
            Line line{{l.num("startX"), l.num("startY")}, {l.num("endX"), l.num("endY")}, l.str("color", s.color)};
            if (std::isfinite(line.a.x) && std::isfinite(line.a.y) && std::isfinite(line.b.x) && std::isfinite(line.b.y)) s.lines.push_back(line);
        }
    for (auto [key, into] : {std::pair{"thrusterPoints", &s.thrusters}, std::pair{"weaponPoints", &s.weapons}}) {
        json::Value sc;
        if (const auto *pts = arr(key, sc)) for (const auto &p : pts->items) if (p.is_object()) into->push_back({p.num("x"), p.num("y")});
    }
    out = std::move(s);
    return true;
}
}
