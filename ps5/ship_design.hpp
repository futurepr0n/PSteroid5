#pragma once
// Ships as the CSSteroids server stores them (models/Ship.js): name, type
// ('custom'|'default'|'triangle'|'diamond'), CSS colour, passphrase, and design-space
// customLines / thrusterPoints / weaponPoints. The browser draws them at 0.25 scale.
#include "asteroids_core.hpp"
#include "json_lite.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace cssteroids {
// CSS colour → 0xRRGGBB: #rgb, #rrggbb, rgb(r,g,b) and the names the customizer offers.
inline uint32_t css_color(std::string text, uint32_t fallback = 0xffffff) {
    for (auto &c : text) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (!text.empty() && text[0] == '#') {
        uint32_t v = 0;
        for (size_t i = 1; i < text.size(); i++) if (hex(text[i]) < 0) return fallback;
        if (text.size() == 7) { for (size_t i = 1; i < 7; i++) v = v << 4 | uint32_t(hex(text[i])); return v; }
        if (text.size() == 4) { for (size_t i = 1; i < 4; i++) v = v << 8 | uint32_t(hex(text[i]) * 17); return v; }
        return fallback;
    }
    int r, g, b;
    if (std::sscanf(text.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 || std::sscanf(text.c_str(), "rgb(%d, %d, %d)", &r, &g, &b) == 3)
        return uint32_t(std::clamp(r, 0, 255) << 16 | std::clamp(g, 0, 255) << 8 | std::clamp(b, 0, 255));
    static const std::pair<const char *, uint32_t> names[] = {
        {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"lime", 0x00ff00}, {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"cyan", 0x00ffff}, {"aqua", 0x00ffff}, {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff},
        {"orange", 0xffa500}, {"purple", 0x800080}, {"pink", 0xffc0cb}, {"gold", 0xffd700}, {"silver", 0xc0c0c0},
        {"gray", 0x808080}, {"grey", 0x808080}, {"black", 0x000000}, {"brown", 0xa52a2a}, {"violet", 0xee82ee},
        {"teal", 0x008080}, {"navy", 0x000080}, {"maroon", 0x800000}, {"olive", 0x808000}, {"orangered", 0xff4500}};
    for (const auto &n : names) if (text == n.first) return n.second;
    return fallback;
}

struct ShipDesign {
    std::string name = "Default", passphrase, typeName = "default", colorName = "white";
    ShipGeometry geometry;
    std::string source; // the server JSON it came from (saved for offline reload)
    bool fromServer() const { return !passphrase.empty(); }
};

namespace detail {
// JSON columns arrive as arrays, or (older rows) as JSON-encoded strings.
inline const json::Value *array_field(const json::Value &ship, const char *key, json::Value &scratch) {
    const json::Value *v = ship.get(key);
    if (v && v->type == json::Value::Type::string && json::parse(v->s, scratch)) v = &scratch;
    return v && v->is_array() ? v : nullptr;
}
inline bool finite_num(const json::Value &v, const char *key, float &out) {
    const json::Value *n = v.get(key);
    if (!n || n->type != json::Value::Type::number || !std::isfinite(n->n) || std::fabs(n->n) > 10000) return false;
    out = float(n->n);
    return true;
}
}

constexpr size_t max_ship_lines = 400, max_ship_points = 16;

inline bool parse_ship(const json::Value &v, ShipDesign &out) {
    if (!v.is_object()) return false;
    ShipDesign d;
    d.name = v.str("name", "Unnamed").substr(0, 32);
    d.passphrase = v.str("passphrase").substr(0, 64);
    d.typeName = v.str("type", "default");
    d.colorName = v.str("color", "white");
    auto &g = d.geometry;
    g.color = css_color(d.colorName);
    g.shape = d.typeName == "triangle" ? ShipShape::triangle : d.typeName == "diamond" ? ShipShape::diamond
            : d.typeName == "custom" ? ShipShape::custom : ShipShape::standard;
    json::Value scratch;
    if (const auto *lines = detail::array_field(v, "customLines", scratch)) {
        for (const auto &l : lines->items) {
            float x1, y1, x2, y2;
            if (!l.is_object() || !detail::finite_num(l, "startX", x1) || !detail::finite_num(l, "startY", y1) ||
                !detail::finite_num(l, "endX", x2) || !detail::finite_num(l, "endY", y2)) continue;
            g.lines.push_back({x1 * .25f, y1 * .25f, x2 * .25f, y2 * .25f, css_color(l.str("color"), g.color)});
            if (g.lines.size() == max_ship_lines) break;
        }
    }
    for (auto [key, into] : {std::pair{"weaponPoints", &g.weapons}, std::pair{"thrusterPoints", &g.thrusters}}) {
        json::Value pointScratch;
        if (const auto *points = detail::array_field(v, key, pointScratch)) {
            for (const auto &p : points->items) {
                Point pt;
                if (p.is_object() && detail::finite_num(p, "x", pt.x) && detail::finite_num(p, "y", pt.y)) {
                    into->push_back({pt.x * .25f, pt.y * .25f});
                    if (into->size() == max_ship_points) break;
                }
            }
        }
    }
    // A custom type with no usable lines draws and collides as the default ship.
    if (g.shape == ShipShape::custom && g.lines.empty()) g.shape = ShipShape::standard;
    d.source = json::dump(v);
    out = std::move(d);
    return true;
}

inline bool parse_ship_list(const std::string &body, std::vector<ShipDesign> &out, size_t limit = 60) {
    json::Value root;
    if (!json::parse(body, root) || !root.is_array()) return false;
    out.clear();
    for (const auto &item : root.items) {
        ShipDesign d;
        if (parse_ship(item, d)) out.push_back(std::move(d));
        if (out.size() == limit) break;
    }
    return true;
}

// The chosen ship is kept as the server's JSON, so it reloads offline unchanged.
struct ShipStore {
    std::string path;
    bool load(ShipDesign &out) const {
        std::ifstream file(path);
        if (!file) return false;
        std::stringstream text;
        text << file.rdbuf();
        json::Value v;
        return text.str().size() < (1u << 20) && json::parse(text.str(), v) && parse_ship(v, out);
    }
    bool save(const std::string &shipJson) const {
        if (const auto split = path.find_last_of('/'); split != std::string::npos) mkdir(path.substr(0, split).c_str(), 0777);
        const std::string temp = path + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!(file << shipJson) || !file.flush()) return false;
        }
        return std::rename(temp.c_str(), path.c_str()) == 0;
    }
};
}
