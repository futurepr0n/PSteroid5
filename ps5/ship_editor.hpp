#pragma once
// Controller-first ship editor model (docs/ship-design-space.md). Design units are the browser
// customizer's: a 400x400 canvas, origin at the centre, nose up; the game draws at 0.25.
// Every edit goes through apply(), which refuses anything the server would reject, so the
// editor cannot produce an invalid ship. Portable and host-tested.
#include "json_lite.hpp"
#include "pattern_design.hpp"
#include "ship_design.hpp"
#include "sound.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cssteroids::editor {
constexpr double half = 200;          // canvas half-size (±200 design units)
constexpr double grid = 5, fine = 1;  // 5 units = 1.25 px in game
constexpr size_t max_lines = 5000, max_thrusters = 3, max_weapons = 2, max_name = 32, max_undo = 200;
constexpr double snap_radius = 6;     // endpoints this close are treated as one
constexpr double pick_radius = 10;    // cursor reach for picking lines/points

// The browser's preset swatches (index.html .color-option) as the hex it stores per line.
inline const std::vector<std::string> palette = {"#ffffff", "#00ffff", "#00ff00", "#ffff00", "#ff9900", "#ff00ff", "#ff0000", "#0066ff"};

struct Ship {
    std::string name = "PS5 Ship", color = "white";
    std::vector<Line> lines;
    std::vector<Pt> thrusters, weapons;
    std::string shotSound = "pew"; // the player's own shots (pew, zap, laser, arcade)
    std::vector<pattern::Fill> fills{};
};

enum class Hull { classic, triangle, diamond, meridian, manta, delta, blank };
inline constexpr std::array<const char *,7> hull_names={"CLASSIC","TRIANGLE","DIAMOND","MERIDIAN","MANTA","DELTA","BLANK"};
// Browser sample() outlines/cockpits, uniformly fitted inside the editable square.
inline Ship starter_hull(Hull kind) {
    Ship s;std::vector<Pt> outline,cockpit;
    switch(kind) {
        case Hull::classic: outline={{0,-60},{-42,42},{42,42}};s.name="Classic";break;
        case Hull::triangle: outline={{0,-60},{-60,60},{60,60}};s.name="Triangle";break;
        case Hull::diamond: outline={{0,-60},{60,0},{0,60},{-60,0}};s.name="Diamond";break;
        case Hull::meridian: outline={{0,-250},{45,-90},{90,-20},{220,140},{210,170},{70,100},{42,210},{0,175},{-42,210},{-70,100},{-210,170},{-220,140},{-90,-20},{-45,-90}};s.name="Meridian study";break;
        case Hull::manta: outline={{0,-180},{60,-100},{150,-70},{250,30},{260,140},{170,80},{65,130},{0,220},{-65,130},{-170,80},{-260,140},{-250,30},{-150,-70},{-60,-100}};s.name="Manta study";break;
        case Hull::delta: outline={{0,-250},{240,200},{80,155},{0,205},{-80,155},{-240,200}};s.name="Delta study";break;
        case Hull::blank: s.name="Untitled ship";break;
    }
    s.color="#e5edf2";
    if(kind==Hull::meridian || kind==Hull::manta || kind==Hull::delta)cockpit={{0,-150},{20,-60},{15,40},{0,65},{-15,40},{-20,-60}};
    for(const auto *points:{&outline,&cockpit})for(size_t i=0;i<points->size();++i){
        Line l{(*points)[i],(*points)[(i+1)%points->size()],s.color};
        l.id="hull-"+std::to_string(int(kind))+"-"+std::to_string(s.lines.size());s.lines.push_back(std::move(l));
    }
    double extent=0;for(const auto &l:s.lines)for(auto p:{l.a,l.b})extent=std::max({extent,std::fabs(p.x),std::fabs(p.y)});
    if(extent>half){const double k=180/extent;for(auto &l:s.lines){l.a.x*=k;l.a.y*=k;l.b.x*=k;l.b.y*=k;}}
    return s;
}

inline std::string post_body(const Ship &,const std::string &,bool);

enum class Ownership { fresh, owned, copy }; // copy: from the gallery, saved without its passphrase

inline bool inside(Pt p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::fabs(p.x) <= half+1e-7 && std::fabs(p.y) <= half+1e-7; }
inline Pt mirror(Pt p) { return {p.x == 0 ? 0 : -p.x, p.y}; }
inline Pt snap(Pt p, double step) { return {std::round(p.x / step) * step, std::round(p.y / step) * step}; }
inline Pt clamp_canvas(Pt p) { return {std::clamp(p.x, -half, half), std::clamp(p.y, -half, half)}; }
// Position 0..1 along a line (curves sampled like nearest_line) closest to p.
inline double param_at(const Line &l, Pt p) {
    const int n = l.curved ? 16 : 1;
    double best = 1e30, bt = 0; Pt a = l.a;
    for (int j = 1; j <= n; ++j) {
        const double t = double(j) / n, u = 1 - t;
        const Pt b = l.curved ? Pt{u * u * l.a.x + 2 * u * t * l.control.x + t * t * l.b.x, u * u * l.a.y + 2 * u * t * l.control.y + t * t * l.b.y} : l.b;
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double k = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0;
        const double d = std::hypot(a.x + k * dx - p.x, a.y + k * dy - p.y);
        if (d < best) { best = d; bt = (j - 1 + k) / n; }
        a = b;
    }
    return bt;
}
// The browser's curveThrough: bend l so the point grabbed at `grab` passes through `to`, endpoints fixed.
inline Line curve_through(Line l, Pt grab, Pt to) {
    const double t = std::clamp(param_at(l, grab), .08, .92), u = 1 - t;
    l.control = clamp_canvas({(to.x - u * u * l.a.x - t * t * l.b.x) / (2 * u * t), (to.y - u * u * l.a.y - t * t * l.b.y) / (2 * u * t)});
    l.curved = true;
    return l;
}


// The server's acceptance rules (validateShipData + sanitizeMMOShipData limits) plus, unless
// the ship was drawn elsewhere already beyond it, the editor's ±200 canvas.
inline std::string invalid_reason(const Ship &s, bool canvas = true) {
    std::string n = s.name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty()) return "a name is required";
    if (s.name.size() > max_name) return "name longer than 32";
    if (s.lines.empty()) return "draw at least one line";
    if (s.fills.size()>200) return "more than 200 pattern fills";
    if (s.lines.size() > max_lines) return "more than 5000 lines";
    if (s.thrusters.size() > max_thrusters) return "more than 3 thrusters";
    if (s.weapons.size() > max_weapons) return "more than 2 weapons";
    for (const auto &l : s.lines) {
        if (l.color.size() > max_name) return "colour name longer than 32";
        if (l.width != 1 && l.width != 2) return "line width must be 1 or 2";
        if (l.curved && (!std::isfinite(l.control.x) || !std::isfinite(l.control.y))) return "invalid curve";
        if (canvas && (!inside(l.a) || !inside(l.b) || (l.curved && !inside(l.control)))) return "a line leaves the canvas";
        if (!std::isfinite(l.a.x + l.a.y + l.b.x + l.b.y)) return "a point is not a number";
    }
    if (canvas) {
        for (const auto &p : s.thrusters) if (!inside(p)) return "a thruster leaves the canvas";
        for (const auto &p : s.weapons) if (!inside(p)) return "a weapon leaves the canvas";
    }
    if(post_body(s,{},false).size()>1900000)return "Editor document too large; simplify pattern artwork";
    return {};
}
inline bool within_canvas(const Ship &s) {
    for (const auto &l : s.lines) if (!inside(l.a) || !inside(l.b) || (l.curved && !inside(l.control))) return false;
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
        : ship(std::move(s)), ownership(o), passphrase(std::move(pass)), legacyOutside(!within_canvas(ship)) { ensure_ids(ship); }

    bool can_undo() const { return !undoStack.empty(); }
    bool can_redo() const { return !redoStack.empty(); }
    bool undo() { if (undoStack.empty()) return false; redoStack.push_back(ship); ship = undoStack.back(); undoStack.pop_back(); legacyOutside=!within_canvas(ship); revision++; return true; }
    bool redo() { if (redoStack.empty()) return false; undoStack.push_back(ship); ship = redoStack.back(); redoStack.pop_back(); legacyOutside=!within_canvas(ship); revision++; return true; }
    // What saving would be refused for (empty = ready to save).
    std::string save_problem() const { return invalid_reason(ship, !legacyOutside); }

    bool choose_hull(Hull kind) {
        Ship next=starter_hull(kind);next.shotSound=ship.shotSound;
        const bool wasOutside=legacyOutside;
        legacyOutside=!within_canvas(next);
        if(commit(std::move(next)))return true;
        legacyOutside=wasOutside;return false;
    }

    // --- edits (each one undoable; false + message when refused) -------------
    // a == b makes a dot: the browser stores a click without a drag the same way (3% of
    // gallery lines) and draws it as the endpoint dots of a zero-length line.
    bool add_line(Pt a, Pt b, const std::string &color, double width=2) {
        Ship next = ship;
        next.lines.push_back({a, b, color});next.lines.back().width=width;
        if (mirrorOn) {
            Line m{mirror(a), mirror(b), color};m.width=width;
            if (!line_exists(next, m)) next.lines.push_back(m);
        }
        return commit(std::move(next));
    }
    // Delete the line nearest the cursor (and its mirror image when mirroring).
    bool erase_near(Pt c) {
        const int i = nearest_line(c);
        if (i < 0) return refuse("no line here");
        if(ship.lines[size_t(i)].mesh)return refuse("Use FILL to edit or remove pattern artwork");
        Ship next = ship;
        const Line gone = next.lines[size_t(i)];
        next.lines.erase(next.lines.begin() + i);
        if (mirrorOn) erase_line(next, {mirror(gone.a), mirror(gone.b), gone.color});
        return commit(std::move(next));
    }
    bool paint_near(Pt c, const std::string &color, double width=0) {
        const int i = nearest_line(c);
        if (i < 0) return refuse("no line here");
        if(ship.lines[size_t(i)].mesh)return refuse("Use FILL to edit or remove pattern artwork");
        Ship next = ship;
        const Line was = next.lines[size_t(i)];
        next.lines[size_t(i)].color = color;if(width>0)next.lines[size_t(i)].width=width;
        if (mirrorOn) for (auto &l : next.lines) if (same_line(l, {mirror(was.a), mirror(was.b), was.color})) {l.color = color;if(width>0)l.width=width;}
        return commit(std::move(next));
    }
    // Bend the line under `grab` so it passes through `to`; endpoints stay fixed (browser curveThrough).
    bool bend_line(Pt grab, Pt to) {
        const int i = nearest_line(grab);
        if (i < 0) return refuse("no line here");
        if (ship.lines[size_t(i)].mesh) return refuse("Use FILL to edit or remove pattern artwork");
        Ship next = ship;
        const Line was = next.lines[size_t(i)];
        Line &l = next.lines[size_t(i)];
        l = curve_through(was, grab, to);
        // a quadratic is the same curve either way round, so the partner just takes the mirrored control point
        if (mirrorOn) for (size_t j = 0; j < next.lines.size(); j++)
            if (j != size_t(i) && same_line(next.lines[j], {mirror(was.a), mirror(was.b), was.color})) { next.lines[j].control = mirror(l.control); next.lines[j].curved = true; }
        return commit(std::move(next));
    }
    // Index of the line that mirrors line i across the centre axis, or -1.
    int mirror_of(int i) const {
        if (i < 0 || size_t(i) >= ship.lines.size()) return -1;
        const Line &l = ship.lines[size_t(i)];
        for (size_t j = 0; j < ship.lines.size(); j++) {
            const Line &m = ship.lines[j];
            if (j == size_t(i)) continue;
            if (!l.pairId.empty() && m.pairId == l.pairId) return int(j);
            if (same_line(m, {mirror(l.a), mirror(l.b), l.color})) return int(j);
        }
        return -1;
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
    bool scale_ship(double factor) {
        if(!std::isfinite(factor) || factor < .1 || factor > 3) return refuse("scale must be 10 to 300 percent");
        Ship next=ship;
        auto scale=[&](Pt &p){p.x*=factor;p.y*=factor;};
        for(auto &l:next.lines){scale(l.a);scale(l.b);if(l.curved)scale(l.control);}
        for(auto *pts:{&next.thrusters,&next.weapons})for(auto &p:*pts)scale(p);
        for(auto &f:next.fills){scale(f.seed);f.scale*=factor;if(f.scale<.05 || f.scale>10000)return refuse("Pattern scale would leave valid range");}
        return commit(std::move(next));
    }
    bool apply_fill(Pt seed,const pattern::Tile &tile,double scale,const std::string &replace={}) {
        Ship next=ship;ensure_ids(next);std::string error;auto rs=pattern::regions(next.lines,error);
        if(!error.empty()){message=error;return false;}
        const auto *area=pattern::region_at(rs,seed);if(!area)return refuse("Choose a closed hull region (gaps must be joined)");
        std::string id=replace;
        if(id.empty())for(const auto &f:next.fills)if(pattern::match(rs,f)==area){id=f.id;break;}
        if(id.empty()) {size_t n=1;do{id="ps5-fill-"+std::to_string(n++);}while(std::any_of(next.fills.begin(),next.fills.end(),[&](const auto &f){return f.id==id;}));}
        pattern::Fill f;f.id=id;f.boundary=area->boundary;f.seed=seed;f.tile=tile;f.scale=scale;f.color=tile.color;f.width=tile.width;
        auto generated=pattern::bake(*area,f,error);if(!error.empty()){message=error;return false;}
        if(generated.empty())return refuse("Draw pattern artwork before applying");
        std::erase_if(next.lines,[&](const auto &l){return l.meshGroup==id;});
        std::erase_if(next.fills,[&](const auto &old){return old.id==id;});
        next.lines.insert(next.lines.end(),generated.begin(),generated.end());next.fills.push_back(f);
        return commit(std::move(next));
    }
    bool remove_fill(Pt seed) {
        Ship next=ship;std::string error;auto rs=pattern::regions(next.lines,error);const auto *r=pattern::region_at(rs,seed);
        if(!r)return refuse("Choose a patterned closed region");
        auto f=std::find_if(next.fills.begin(),next.fills.end(),[&](const auto &f){return pattern::match(rs,f)==r;});
        if(f==next.fills.end())return refuse("No editable fill here");
        auto id=f->id;next.fills.erase(f);std::erase_if(next.lines,[&](const auto &l){return l.meshGroup==id;});return commit(std::move(next));
    }
    bool clear() { Ship next = ship; next.lines.clear(); next.thrusters.clear(); next.weapons.clear(); next.fills.clear(); return commit(std::move(next)); }

    // --- queries ------------------------------------------------------------
    int nearest_line(Pt c, double reach = pick_radius) const {
        int best = -1; double bd = reach;
        for (size_t i = 0; i < ship.lines.size(); i++) {
            const auto &l=ship.lines[i];
            if(l.mesh)continue;
            double d=seg_dist(c,l.a,l.b);
            if(l.curved) {
                d=1e30;Pt a=l.a;
                for(int j=1;j<=16;++j){const double t=double(j)/16,u=1-t;
                    Pt b{u*u*l.a.x+2*u*t*l.control.x+t*t*l.b.x,u*u*l.a.y+2*u*t*l.control.y+t*t*l.b.y};
                    d=std::min(d,seg_dist(c,a,b));a=b;}
            }
            if (d <= bd) { bd = d; best = int(i); }
        }
        return best;
    }
    // The nearest existing endpoint or point within reach (for snapping and MOVE).
    bool nearest_vertex(Pt c, Pt &out, double reach = snap_radius) const {
        double bd = reach; bool found = false;
        auto consider = [&](Pt p) { const double d = std::hypot(p.x - c.x, p.y - c.y); if (d <= bd) { bd = d; out = p; found = true; } };
        for (const auto &l : ship.lines) if(!l.mesh) { consider(l.a); consider(l.b); }
        for (const auto &p : ship.thrusters) consider(p);
        for (const auto &p : ship.weapons) consider(p);
        return found;
    }

private:
    std::vector<Ship> undoStack, redoStack;

    bool refuse(const char *why) { message = why; return false; }
    static void ensure_ids(Ship &s) {
        std::set<std::string> used;for(const auto &l:s.lines)if(!l.id.empty())used.insert(l.id);
        size_t i=1;for(auto &l:s.lines)if(l.id.empty()){std::string id;do{id="ps5-line-"+std::to_string(i++);}while(used.count(id));l.id=id;used.insert(id);}
    }
    bool commit(Ship next) {
        ensure_ids(next);
        if(!next.fills.empty()) {
            std::string error;auto rs=pattern::regions(next.lines,error);if(!error.empty()){message=error;return false;}
            std::set<std::string> groups;for(const auto &f:next.fills)groups.insert(f.id);
            std::erase_if(next.lines,[&](const auto &l){return groups.count(l.meshGroup)>0;});
            std::vector<pattern::Fill> active;
            for(auto f:next.fills)if(const auto *r=pattern::match(rs,f)){
                auto generated=pattern::bake(*r,f,error);if(!error.empty()){message=error;return false;}
                if(next.lines.size()+generated.size()>max_lines)return refuse("Pattern exceeds 5000 lines; increase scale");
                next.lines.insert(next.lines.end(),generated.begin(),generated.end());f.boundary=r->boundary;active.push_back(std::move(f));
            }
            next.fills=std::move(active);
        }
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
        o.set("width",num(l.width));
        if(!l.id.empty())o.set("id",json::Value::string_value(l.id));
        if(l.mesh)o.set("mesh",json::Value::bool_value(true));
        if(l.curved){o.set("controlX",num(l.control.x));o.set("controlY",num(l.control.y));}
        if(!l.meshGroup.empty())o.set("meshGroup",json::Value::string_value(l.meshGroup));
        if(!l.pairId.empty())o.set("pairId",json::Value::string_value(l.pairId));
        lines.items.push_back(std::move(o));
    }
    v.set("customLines", std::move(lines));
    for (auto [key, pts] : {std::pair{"thrusterPoints", &s.thrusters}, std::pair{"weaponPoints", &s.weapons}}) {
        json::Value arr; arr.type = json::Value::Type::array;
        for (const auto &p : *pts) { json::Value o = json::Value::object(); o.set("x", num(p.x)); o.set("y", num(p.y)); arr.items.push_back(std::move(o)); }
        v.set(key, std::move(arr));
    }
    json::Value fills;fills.type=json::Value::Type::array;for(const auto &f:s.fills)fills.items.push_back(pattern::fill_json(f));
    v.set("editorFills",fills);
    v.set("shotSound", json::Value::string_value(s.shotSound));
    v.set("isPublic", json::Value::bool_value(isPublic));
    if (!passphrase.empty()) v.set("passphrase", json::Value::string_value(passphrase));
    return json::dump(v);
}

// A server ship (object from /api/ships/...) into the editor, exactly: coordinates and colours kept.
inline bool from_server(const json::Value &v, Ship &out) {
    if (!v.is_object()) return false;
    Ship s;
    json::Value parsedDoc;const json::Value *doc=v.get("editorDocument"),*data=&v;
    if(doc && doc->type==json::Value::Type::string && json::parse(doc->s,parsedDoc))doc=&parsedDoc;
    if(doc && doc->is_object() && doc->get("ship") && doc->get("ship")->is_object())data=doc->get("ship");
    s.name = data->str("name", "Ship").substr(0, max_name);
    s.color = data->str("color", "white").substr(0, max_name);
    s.shotSound = sound::valid_shot(data->str("shotSound"));
    auto arr = [&](const char *key, json::Value &scratch) -> const json::Value * {
        const json::Value *a = data->get(key);
        if (a && a->type == json::Value::Type::string && json::parse(a->s, scratch)) a = &scratch;
        return a && a->is_array() ? a : nullptr;
    };
    json::Value scratch;
    if (const auto *lines = arr("customLines", scratch))
        for (const auto &l : lines->items) {
            if (!l.is_object()) continue;
            Line line{{l.num("startX"), l.num("startY")}, {l.num("endX"), l.num("endY")}, l.str("color", s.color)};
            line.width=l.num("width")==1?1:2;
            line.mesh=l.get("mesh") && l.get("mesh")->b;
            line.curved=l.get("controlX") && l.get("controlY") &&
                l.get("controlX")->type==json::Value::Type::number && l.get("controlY")->type==json::Value::Type::number;
            line.control={l.num("controlX"),l.num("controlY")};
            line.meshGroup=l.str("meshGroup");line.pairId=l.str("pairId");line.id=l.str("id");
            if (std::isfinite(line.a.x) && std::isfinite(line.a.y) && std::isfinite(line.b.x) && std::isfinite(line.b.y)) s.lines.push_back(line);
        }
    for (auto [key, into] : {std::pair{"thrusterPoints", &s.thrusters}, std::pair{"weaponPoints", &s.weapons}}) {
        json::Value sc;
        if (const auto *pts = arr(key, sc)) for (const auto &p : pts->items) if (p.is_object()) into->push_back({p.num("x"), p.num("y")});
    }
    const auto *fills=doc && doc->is_object()?doc->get("fills"):v.get("editorFills");
    if(fills && fills->is_array())for(const auto &f:fills->items)if(f.is_object())s.fills.push_back(pattern::read_fill(f));
    out = std::move(s);
    return true;
}
inline json::Value document(const Ship &s) {
    json::Value ship;json::parse(post_body(s,{},false),ship);
    std::erase_if(ship.fields,[](const auto &f){return f.first=="isPublic" || f.first=="editorFills";});
    auto hex=[](const std::string &c){char text[8];std::snprintf(text,sizeof text,"#%06x",cssteroids::css_color(c));return std::string(text);};
    ship.set("color",json::Value::string_value(hex(s.color)));
    for(auto &field:ship.fields)if(field.first=="customLines")for(auto &l:field.second.items)l.set("color",json::Value::string_value(hex(l.str("color"))));
    auto d=json::Value::object();d.set("version",num(1));d.set("ship",ship);json::Value fills;fills.type=json::Value::Type::array;
    for(const auto &f:s.fills)fills.items.push_back(pattern::fill_json(f));
    d.set("fills",fills);return d;
}
inline std::string save_body(const Ship &s,bool isPublic) {auto body=json::Value::object();body.set("document",document(s));body.set("isPublic",json::Value::bool_value(isPublic));return json::dump(body);}

}
