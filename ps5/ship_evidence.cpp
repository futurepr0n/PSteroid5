// Generates the ship-editor evidence set for tools/ship-evidence.js (which judges it with the
// browser's own ship.js and the server's validators from CSSteroids-2).
// Usage: ship-evidence <testdata/ship_corpus.json> <out.json> [sessions]
#include "ship_editor.hpp"
#include "ship_design.hpp"
#include "ship_render.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace cssteroids;
using namespace cssteroids::editor;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct Rng64 { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 16); }
               double unit() { return next() / 4294967296.0; } int below(int n) { return int(next() % uint32_t(n)); } };

// What the PS5 does with a saved body: parse it as the server returns it, then draw and collide.
static json::Value ps5_view(const std::string &body, uint64_t seed) {
    json::Value v; json::parse(body, v);
    ShipDesign d; parse_ship(v, d);
    json::Value out = json::Value::object();
    json::Value poses; poses.type = json::Value::Type::array;
    const double pose[3][3] = {{500, 400, 0}, {300, 250, 1.2345}, {1000, 700, -2.5}};
    for (auto &p : pose) {
        json::Value o = json::Value::object();
        o.set("x", json::Value::number_value(p[0])); o.set("y", json::Value::number_value(p[1])); o.set("angle", json::Value::number_value(p[2]));
        json::Value segs; segs.type = json::Value::Type::array;
        for (const auto &w : ship_segments(d.geometry, p[0], p[1], p[2])) {
            json::Value s; s.type = json::Value::Type::array;
            for (double c : {w.x1, w.y1, w.x2, w.y2}) s.items.push_back(json::Value::number_value(c));
            char hex[8]; std::snprintf(hex, sizeof hex, "#%06x", w.color & 0xffffff);
            s.items.push_back(json::Value::string_value(hex));
            segs.items.push_back(std::move(s));
        }
        o.set("segments", std::move(segs));
        json::Value dots; dots.type = json::Value::Type::array;
        for (const auto &d2 : ship_dots(d.geometry, p[0], p[1], p[2])) {
            json::Value a; a.type = json::Value::Type::array;
            for (double c : {d2.x, d2.y, d2.r}) a.items.push_back(json::Value::number_value(c));
            char hex[8]; std::snprintf(hex, sizeof hex, "#%06x", d2.color & 0xffffff);
            a.items.push_back(json::Value::string_value(hex));
            dots.items.push_back(std::move(a));
        }
        o.set("dots", std::move(dots));
        json::Value flames; flames.type = json::Value::Type::array;
        const double c = std::cos(p[2]), sn = std::sin(p[2]);
        for (const auto &f : flame_anchors(d.geometry)) {
            json::Value a; a.type = json::Value::Type::array;
            a.items.push_back(json::Value::number_value(p[0] + f.x * c - f.y * sn));
            a.items.push_back(json::Value::number_value(p[1] + f.x * sn + f.y * c));
            flames.items.push_back(std::move(a));
        }
        o.set("flames", std::move(flames));
        // collision samples around the ship
        Rng64 r{seed * 2654435761u + 7};
        json::Value hits; hits.type = json::Value::Type::array;
        for (int i = 0; i < 120; i++) {
            const double cx = p[0] + (r.unit() - .5) * 140, cy = p[1] + (r.unit() - .5) * 140;
            static const double radii[4] = {3, 15, 25, 40};
            const double rad = radii[r.below(4)];
            json::Value h; h.type = json::Value::Type::array;
            for (double x : {cx, cy, rad}) h.items.push_back(json::Value::number_value(x));
            h.items.push_back(json::Value::bool_value(ship_hits_circle(d.geometry, p[0], p[1], p[2], cx, cy, rad)));
            hits.items.push_back(std::move(h));
        }
        o.set("hits", std::move(hits));
        poses.items.push_back(std::move(o));
    }
    out.set("poses", std::move(poses));
    return out;
}

static json::Value entry(const std::string &kind, const std::string &label, const std::string &body, uint64_t seed) {
    json::Value e = json::Value::object();
    e.set("kind", json::Value::string_value(kind));
    e.set("label", json::Value::string_value(label));
    e.set("body", json::Value::string_value(body));
    e.set("ps5", ps5_view(body, seed));
    return e;
}

int main(int argc, char **argv) {
    if (argc < 3) { std::puts("usage: ship-evidence corpus.json out.json [sessions]"); return 2; }
    const int sessions = argc > 3 ? std::atoi(argv[3]) : 400;
    std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
    json::Value corpus; CHECK(json::parse(ss.str(), corpus));
    json::Value out = json::Value::object(), list; list.type = json::Value::Type::array;

    // 1. property test: random editor sessions never leave the valid space
    int produced = 0, steps = 0, refused = 0;
    for (int seed = 1; seed <= sessions; seed++) {
        Rng64 r{uint64_t(seed) * 0x9E3779B97F4A7C15ull | 1};
        Editor ed;
        const int ops = 1 + r.below(80);
        auto rnd_pt = [&] { return clamp_canvas(snap({(r.unit() - .5) * 2 * (half + 30), (r.unit() - .5) * 2 * (half + 30)}, r.below(3) ? grid : fine)); };
        for (int k = 0; k < ops; k++) {
            Pt pick = rnd_pt();
            if (r.below(2) && !ed.ship.lines.empty()) { const auto &l = ed.ship.lines[size_t(r.below(int(ed.ship.lines.size())))]; pick = r.below(2) ? l.a : l.b; }
            bool ok = true;
            switch (r.below(12)) {
                case 0: case 1: case 2: case 3: ok = ed.add_line(rnd_pt(), rnd_pt(), palette[size_t(r.below(int(palette.size())))]); break;
                case 4: ok = ed.erase_near(pick); break;
                case 5: ok = ed.paint_near(pick, r.below(2) ? palette[0] : "#ca3f3f"); break;
                case 6: ok = ed.move_point(pick, rnd_pt()); break;
                case 7: ok = ed.toggle_point(true, rnd_pt()); break;
                case 8: ok = ed.toggle_point(false, rnd_pt()); break;
                case 9: ed.mirrorOn = !ed.mirrorOn; break;
                case 10: ok = r.below(2) ? ed.undo() : ed.redo(); break;
                case 11: ok = ed.set_name(r.below(4) ? std::string("Ship ") + std::to_string(seed) : std::string(40, 'x')); break;
            }
            refused += !ok;
            steps++;
            CHECK(editable(ed.ship)); // invariant after every step, accepted or refused
        }
        if (invalid_reason(ed.ship).empty()) {
            list.items.push_back(entry("property", "session " + std::to_string(seed), post_body(ed.ship, {}, false), uint64_t(seed)));
            produced++;
        }
    }
    std::printf("property: %d sessions, %d steps (%d refused), %d complete ships, editor invariant held: %s\n",
                sessions, steps, refused, produced, failures ? "NO" : "yes");

    // 2. corpus through the editor: load exactly, save back (lossless?)
    int loaded = 0, inCanvas = 0, lossless = 0, n = 0;
    // 3. corpus rebuilt from scratch with editor operations on the 5-unit grid
    int rebuilt = 0, fineSteps = 0, legacyEdits = 0; double worst = 0;
    for (const auto &s : corpus.get("ships")->items) {
        n++;
        editor::Ship ship;
        if (!from_server(s, ship) || ship.lines.empty()) continue;
        loaded++;
        const std::string why = invalid_reason(ship);
        if (why.empty()) inCanvas++;
        const std::string body = post_body(ship, {}, false);
        editor::Ship back; json::Value bv; json::parse(body, bv); from_server(bv, back);
        bool same = back.lines.size() == ship.lines.size() && back.thrusters.size() == ship.thrusters.size() && back.weapons.size() == ship.weapons.size();
        for (size_t i = 0; same && i < ship.lines.size(); i++)
            same = same_pt(back.lines[i].a, ship.lines[i].a, 1.0 / 2048) && same_pt(back.lines[i].b, ship.lines[i].b, 1.0 / 2048) && back.lines[i].color == ship.lines[i].color;
        lossless += same;
        list.items.push_back(entry("corpus", s.str("name"), body, uint64_t(n)));
        // a legacy ship beyond the canvas must still accept edits (no new violations)
        { Editor legacy(ship, Ownership::copy); legacy.mirrorOn = false;
          const bool edited = legacy.set_name("edited") && legacy.add_line({0, 0}, {10, 10}, "#ffffff");
          CHECK(edited || !legacy.legacyOutside); if (legacy.legacyOutside) legacyEdits += edited; }
        // rebuild: a player re-draws every line, mirror off, cursor on the 5-unit grid, switching
        // to the fine 1-unit step for strokes the coarse grid would collapse to a point
        Editor ed; ed.mirrorOn = false; ed.set_name(ship.name.empty() ? "Rebuilt" : ship.name);
        double dev = 0; bool ok = true;
        for (const auto &l : ship.lines) {
            Pt a = snap(clamp_canvas(l.a), grid), b = snap(clamp_canvas(l.b), grid);
            if (same_pt(a, b)) { a = snap(clamp_canvas(l.a), fine); b = snap(clamp_canvas(l.b), fine); fineSteps++; }
            dev = std::max({dev, std::hypot(a.x - l.a.x, a.y - l.a.y), std::hypot(b.x - l.b.x, b.y - l.b.y)});
            ok = ed.add_line(a, b, l.color) && ok; // zero-length = a dot, as drawn in the browser
        }
        for (const auto &p : ship.thrusters) ok = ed.toggle_point(false, snap(clamp_canvas(p), grid)) && ok;
        for (const auto &p : ship.weapons) ok = ed.toggle_point(true, snap(clamp_canvas(p), grid)) && ok;
        if (ok && why.empty() && invalid_reason(ed.ship).empty()) { rebuilt++; worst = std::max(worst, dev); }
        if (invalid_reason(ed.ship).empty()) list.items.push_back(entry("rebuild", s.str("name"), post_body(ed.ship, {}, false), uint64_t(n) + 1000));
        else std::printf("not rebuildable: \"%s\" (%s)\n", ship.name.c_str(), invalid_reason(ed.ship).c_str());
    }
    std::printf("legacy ships beyond the canvas that still accept edits: %d\n", legacyEdits);
    std::printf("corpus: %d ships, %d custom loaded, %d fit the editor's canvas and limits, %d round-trip losslessly\n", n, loaded, inCanvas, lossless);
    std::printf("rebuild on the 5-unit grid: %d of %d (worst endpoint shift %.2f design units = %.2f px in game; %d tiny strokes on the fine grid)\n", rebuilt, loaded, worst, worst * 0.25, fineSteps);

    json::Value summary = json::Value::object();
    for (auto [k, v] : {std::pair{"sessions", sessions}, {"propertyShips", produced}, {"steps", steps}, {"refused", refused}, {"corpus", n},
                        {"loaded", loaded}, {"inCanvas", inCanvas}, {"lossless", lossless}, {"rebuilt", rebuilt}})
        summary.set(k, json::Value::number_value(v));
    summary.set("worstShiftUnits", json::Value::number_value(worst));
    out.set("summary", std::move(summary));
    out.set("ships", std::move(list));
    std::ofstream(argv[2]) << json::dump(out);
    if (failures) { std::printf("%d evidence failure(s)\n", failures); return 1; }
    return 0;
}
