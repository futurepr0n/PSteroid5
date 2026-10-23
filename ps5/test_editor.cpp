// Host tests for the ship editor model (ship_editor.hpp).
#include "ship_editor.hpp"
#include <cstdio>

using namespace cssteroids;
using namespace cssteroids::editor;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    {   // mirror on by default: a line and its reflection; erase and paint act on both
        Editor ed;
        CHECK(ed.mirrorOn);
        CHECK(ed.add_line({-20, -40}, {-50, 30}, "#ff0000"));
        CHECK(ed.ship.lines.size() == 2 && ed.ship.lines[1].a.x == 20 && ed.ship.lines[1].b.x == 50);
        CHECK(ed.add_line({0, -60}, {0, 60}, "#ffffff"));     // on the axis: no duplicate
        CHECK(ed.ship.lines.size() == 3);
        CHECK(ed.paint_near({-35, -5}, "#00ffff"));
        CHECK(ed.ship.lines[0].color == "#00ffff" && ed.ship.lines[1].color == "#00ffff");
        CHECK(ed.erase_near({35, -5}));
        CHECK(ed.ship.lines.size() == 1 && ed.ship.lines[0].a.x == 0);
    }
    {   // move drags the whole joint and its mirror; meeting ends leave a dot
        Editor ed;
        ed.add_line({-30, 0}, {-10, 40}, "#ffffff");
        ed.add_line({-10, 40}, {-40, 60}, "#ffffff");
        CHECK(ed.move_point({-10, 40}, {-15, 45}));
        for (const auto &l : ed.ship.lines) CHECK(!same_pt(l.a, {-10, 40}) && !same_pt(l.b, {-10, 40}) && !same_pt(l.a, {10, 40}) && !same_pt(l.b, {10, 40}));
        int moved = 0; for (const auto &l : ed.ship.lines) moved += same_pt(l.b, {-15, 45}) || same_pt(l.a, {-15, 45}) || same_pt(l.b, {15, 45}) || same_pt(l.a, {15, 45});
        CHECK(moved == 4);
        Editor dot; dot.mirrorOn = false;
        CHECK(dot.add_line({5, 5}, {5, 5}, "#ffffff") && dot.ship.lines.size() == 1); // a click without a drag
    }
    {   // limits cannot be exceeded; mirrored pairs fill the slots
        Editor ed;
        CHECK(ed.toggle_point(true, {-20, -80}) && ed.ship.weapons.size() == 2);
        CHECK(!ed.toggle_point(true, {0, -90}) && ed.message == "2 weapons is the maximum");
        CHECK(ed.toggle_point(false, {-15, 100}) && ed.ship.thrusters.size() == 2);
        CHECK(ed.toggle_point(false, {0, 110}) && ed.ship.thrusters.size() == 3);
        CHECK(!ed.toggle_point(false, {30, 90}));
        CHECK(ed.toggle_point(true, {20, -80}) && ed.ship.weapons.empty()); // removing one removes its mirror
        Editor many; many.mirrorOn = false;
        for (int i = 0; i < 500; i++) CHECK(many.add_line({-190.0 + i % 380, -100}, {-190.0 + i % 380, 100 + (i / 380) * 5.0}, "#ffffff"));
        CHECK(!many.add_line({0, 0}, {5, 5}, "#ffffff") && many.message == "more than 500 lines");
        CHECK(!many.set_name(std::string(40, 'x')) || many.ship.name.size() == 32);
    }
    {   // undo / redo restore exact states, and a new edit clears redo
        Editor ed;
        ed.add_line({-10, -10}, {-20, 20}, "#ffffff");
        const Ship one = ed.ship;
        ed.add_line({-30, -30}, {-40, 40}, "#ffffff");
        CHECK(ed.undo() && ed.ship.lines.size() == one.lines.size());
        CHECK(ed.redo() && ed.ship.lines.size() == 4);
        CHECK(ed.undo());
        ed.add_line({-50, 0}, {-60, 10}, "#ff0000");
        CHECK(!ed.can_redo());
        const int rev = ed.revision; ed.undo(); CHECK(ed.revision == rev + 1);
    }
    {   // save body: owned ships carry their passphrase (update), copies and new ships do not
        Editor ed(Ship{"Arrow", "white", {{{0, -50}, {-30, 40}, "#ff0000"}}, {}, {{0, -50}}}, Ownership::owned, "swift-hawk-diving");
        CHECK(ed.save_problem().empty());
        json::Value v; CHECK(json::parse(post_body(ed.ship, ed.passphrase, true), v));
        CHECK(v.str("passphrase") == "swift-hawk-diving" && v.str("type") == "custom" && v.get("isPublic")->b);
        CHECK(v.get("customLines")->items[0].num("startY") == -50 && v.get("customLines")->items[0].str("color") == "#ff0000");
        CHECK(v.get("weaponPoints")->items.size() == 1);
        json::Value w; CHECK(json::parse(post_body(ed.ship, {}, false), w) && !w.get("passphrase") && !w.get("isPublic")->b);
        Editor empty; empty.set_name("");
        CHECK(empty.save_problem() == "a name is required");
        Editor noLines; CHECK(noLines.save_problem() == "draw at least one line");
    }
    {   // loading a server ship is exact (fractional browser coordinates, custom hex, string columns)
        json::Value v;
        CHECK(json::parse(R"({"name":"two","type":"custom","color":"magenta","customLines":"[{\"startX\":-55.5,\"startY\":-34.59375,\"endX\":-69.5,\"endY\":-35.59375,\"color\":\"#ca3f3f\"}]","thrusterPoints":[{"x":-12.5,"y":101}],"weaponPoints":null})", v));
        Ship s; CHECK(from_server(v, s));
        CHECK(s.lines.size() == 1 && s.lines[0].a.y == -34.59375 && s.lines[0].color == "#ca3f3f" && s.thrusters[0].x == -12.5 && s.weapons.empty());
        json::Value back; CHECK(json::parse(post_body(s, {}), back) && back.get("customLines")->items[0].num("endY") == -35.59375);
    }
    {   // a ship drawn beyond the canvas stays editable; new placements are clamped by the cursor
        Ship wide; wide.lines.push_back({{-300, 0}, {300, 0}, "#ffffff"});
        Editor ed(wide, Ownership::copy);
        CHECK(ed.legacyOutside && ed.set_name("Wide") && ed.add_line({0, -10}, {0, 10}, "#ffffff"));
        CHECK(ed.save_problem().empty());
        Editor fresh;
        CHECK(!fresh.add_line({0, 0}, {250, 0}, "#ffffff") && fresh.message == "a line leaves the canvas");
        CHECK(same_pt(clamp_canvas({250, -999}), {200, -200}) && same_pt(snap({7.4, -2.6}, grid), {5, -5}));
    }
    if (failures) { std::printf("%d editor test failure(s)\n", failures); return 1; }
    std::puts("editor tests passed");
    return 0;
}
