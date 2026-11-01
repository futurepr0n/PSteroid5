#pragma once
#include <string>
#include <utility>
#include <cmath>
namespace cssteroids::editor {
struct Pt { double x = 0, y = 0; };
inline bool same_pt(Pt a, Pt b, double eps = 1e-9) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps; }
struct Line {
    Pt a, b; std::string color;
    double width = 2; bool mesh = false, curved = false; Pt control;
    std::string meshGroup, pairId, id;
    Line(Pt from={},Pt to={},std::string c="white") : a(from),b(to),color(std::move(c)) {}
};

}
