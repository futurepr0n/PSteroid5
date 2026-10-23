#pragma once
// Ship outline and thruster-flame anchors in world space, mirroring CSSteroids-2 ship.js
// (draw / drawCustomShip / drawDefaultShip / drawTriangleShip / drawDiamondShip / drawThrust /
// findLowestPoints). The renderer draws these; tools/ship-evidence.js compares them with the
// browser's own ship.js run in Node.
#include "asteroids_core.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace cssteroids {
struct WorldSeg { double x1, y1, x2, y2; uint32_t color; };

// ctx.translate(x, y); ctx.rotate(angle); then ship-space points, scaled for display.
inline std::vector<WorldSeg> ship_segments(const ShipGeometry &g, double x, double y, double angle, double scale = 1) {
    const double c = std::cos(angle), s = std::sin(angle), r = 15;
    auto at = [&](double px, double py, double &ox, double &oy) { ox = x + (px * c - py * s) * scale; oy = y + (px * s + py * c) * scale; };
    std::vector<WorldSeg> out;
    auto seg = [&](double x1, double y1, double x2, double y2, uint32_t color) {
        WorldSeg w; at(x1, y1, w.x1, w.y1); at(x2, y2, w.x2, w.y2); w.color = color; out.push_back(w);
    };
    if (g.custom()) {
        for (const auto &l : g.lines) seg(l.x1, l.y1, l.x2, l.y2, l.color);
    } else if (g.shape == ShipShape::triangle) {
        seg(0, -r, -r, r, g.color); seg(-r, r, r, r, g.color); seg(r, r, 0, -r, g.color);
    } else if (g.shape == ShipShape::diamond) {
        seg(0, -r, r, 0, g.color); seg(r, 0, 0, r, g.color); seg(0, r, -r, 0, g.color); seg(-r, 0, 0, -r, g.color);
    } else {
        seg(0, -r, -r * .7, r * .7, g.color); seg(-r * .7, r * .7, r * .7, r * .7, g.color); seg(r * .7, r * .7, 0, -r, g.color);
    }
    return out;
}

// drawCustomShip also fills a radius-1 dot at both ends of every line (all a zero-length
// "line" shows); centre and radius in world space, plus colour.
struct WorldDot { double x, y, r; uint32_t color; };
inline std::vector<WorldDot> ship_dots(const ShipGeometry &g, double x, double y, double angle, double scale = 1) {
    std::vector<WorldDot> out;
    if (!g.custom()) return out;
    const double c = std::cos(angle), s = std::sin(angle);
    for (const auto &l : g.lines)
        for (auto [px, py] : {std::pair{double(l.x1), double(l.y1)}, std::pair{double(l.x2), double(l.y2)}})
            out.push_back({x + (px * c - py * s) * scale, y + (px * s + py * c) * scale, scale, l.color});
    return out;
}

// Ship-space flame anchors: thruster points, else (custom) up to three endpoints within 20
// design units (5 px) of the lowest, else the default rear point.
inline std::vector<Point> flame_anchors(const ShipGeometry &g) {
    if (!g.thrusters.empty()) return g.thrusters;
    if (g.custom()) {
        std::vector<Point> pts;
        for (const auto &l : g.lines) { pts.push_back({l.x1, l.y1}); pts.push_back({l.x2, l.y2}); }
        std::stable_sort(pts.begin(), pts.end(), [](const Point &a, const Point &b) { return a.y > b.y; });
        std::vector<Point> out;
        for (const auto &p : pts) if (p.y >= pts[0].y - 5 && out.size() < 3) out.push_back(p);
        return out;
    }
    return {{0, 15 * .7f}};
}
}
