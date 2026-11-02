#pragma once
// Client-only effects adapted from CSSteroids-2 starfield.js / trail.js (4a9fa48).
// Fixed 60 Hz updates, independent RNG, bounded storage; never affect gameplay RNG.
#include "ship_render.hpp"
#include <array>
#include <map>

namespace cssteroids::visual {
constexpr double tick = 1.0 / 60;
inline double wrap(double x, double size = 1024) { return x - std::floor(x / size) * size; }
struct Star { double x, y, size, phase, speed, pulse; uint32_t color; };
struct Layer { double depth, alpha; std::vector<Star> stars; };
struct Streak {
    double x, y, heading, turn, speed, life, maxLife;
    std::vector<Point> tail;
};
class Starfield {
    Rng rng;
    double between(double a, double b) { return a + rng.next() * (b - a); }
public:
    double time = 0, travelX = 0, travelY = 0, nextShooting;
    std::array<Layer, 3> layers;
    std::vector<Streak> shooting;
    explicit Starfield(uint32_t seed = 713) : rng(seed), nextShooting(between(6, 16)) {
        const double depths[] = {.06, .15, .30}, alphas[] = {.35, .55, .8};
        const int counts[] = {70, 40, 22};
        const double low[] = {.6, .9, 1.3}, high[] = {1.1, 1.6, 2.2};
        const uint32_t tints[] = {0x9ec9ff, 0xffe9a8, 0xffb3c7};
        for (int i = 0; i < 3; ++i) {
            layers[i].depth = depths[i]; layers[i].alpha = alphas[i];
            for (int n = 0; n < counts[i]; ++n) {
                const bool pulsar = rng.next() < .08;
                Star s{between(0,1024), between(0,1024), between(low[i], high[i]) * (pulsar ? 1.3 : 1),
                       between(0, 6.283185307), between(pulsar ? 1.5 : .5, pulsar ? 2.5 : 2), pulsar ? .6 : .25, 0xffffff};
                if (pulsar) s.color = tints[std::min(2, int(rng.next() * 3))];
                layers[i].stars.push_back(s);
            }
        }
    }
    void step(double vx = 0, double vy = 0, double width = 1920, double height = 1080) {
        time += tick;
        if (std::isfinite(vx) && std::isfinite(vy)) {
            travelX = wrap(travelX + vx, 1024000); travelY = wrap(travelY + vy, 1024000);
        }
        nextShooting -= tick;
        if (nextShooting <= 0 && width > 0 && height > 0) {
            nextShooting = between(6, 16);
            const int dir = rng.next() < .5 ? 1 : -1;
            const double angle = between(.2, .5), life = between(.7, 1.2);
            Streak s{between(0,width), between(0,height*.5), dir == 1 ? angle : 3.141592653589793-angle,
                     dir * between(.012,.025), between(6,10), life, life, {}};
            s.tail.reserve(14); s.tail.push_back({float(s.x),float(s.y)});
            if (shooting.size() < 4) shooting.push_back(std::move(s));
        }
        for (auto &s : shooting) {
            s.heading += s.turn; s.x += std::cos(s.heading)*s.speed; s.y += std::sin(s.heading)*s.speed;
            if (s.tail.size() == 14) s.tail.erase(s.tail.begin());
            s.tail.push_back({float(s.x),float(s.y)}); s.life -= tick;
        }
        std::erase_if(shooting, [](const auto &s) { return s.life <= 0; });
    }
};

enum class Style { pixels, ribbon, embers };
inline Style active_style(const powerups::Effects &fx) {
    if (fx.active(powerups::Type::rapid)) return Style::ribbon;
    if (fx.active(powerups::Type::spray)) return Style::embers;
    return Style::pixels;
}
struct Nozzle { Point point; uint32_t color; std::vector<uint32_t> accents; };
struct Particle {
    double x, y, vx, vy, life, maxLife, size;
    size_t nozzle; uint32_t color; Style style;
};
class Trail {
    Rng rng;
    const ShipGeometry *source = nullptr;
    uint64_t revision = 0;
    double carry = 0, lastX = 0, lastY = 0;
    bool positioned = false;
public:
    std::vector<Nozzle> nozzles;
    std::vector<Particle> particles;
    explicit Trail(uint32_t seed = 911) : rng(seed) { particles.reserve(180); }
    void clear() { particles.clear(); carry = 0; positioned = false; }
    void configure(const ShipGeometry &g) {
        if (source == &g && revision == g.revision) return;
        clear(); source = &g; revision = g.revision; nozzles.clear();
        std::map<uint32_t,size_t> counts;
        if (g.custom()) for (const auto &l : g.lines) ++counts[l.color];
        std::vector<std::pair<uint32_t,size_t>> palette(counts.begin(),counts.end());
        std::stable_sort(palette.begin(),palette.end(),[](auto a,auto b){return a.second>b.second;});
        for (const auto &p : flame_anchors(g)) {
            if (nozzles.size() == 3) break;
            uint32_t color = g.color; double best = 1e30;
            if (g.custom()) for (const auto &l : g.lines) {
                const double dx=l.x2-l.x1, dy=l.y2-l.y1, len=dx*dx+dy*dy;
                const double t=len>0?std::clamp(((p.x-l.x1)*dx+(p.y-l.y1)*dy)/len,0.0,1.0):0;
                const double d=std::hypot(l.x1+t*dx-p.x,l.y1+t*dy-p.y);
                if (d<best) {best=d;color=l.color;}
            }
            Nozzle n{p,color,{}};
            for (auto [c,count] : palette) { (void)count; if(c!=color)n.accents.push_back(c); }
            nozzles.push_back(std::move(n));
        }
    }
    void step(const ShipGeometry &g, double x, double y, double angle, double vx, double vy,
              bool thrusting, bool alive, const powerups::Effects &fx) {
        configure(g);
        if (!alive || !std::isfinite(x+y+angle+vx+vy)) { clear(); return; }
        if (positioned && std::hypot(x-lastX,y-lastY)>100) clear(); // wrap/warp/respawn
        lastX=x;lastY=y;positioned=true;
        if (thrusting) {
            const double speed=std::min(std::hypot(vx,vy)/5,1.0), sn=std::sin(angle), cs=std::cos(angle);
            const Style style=active_style(fx);
            carry += style==Style::ribbon?1:(1+speed*2)*.7;
            const int count=int(carry);carry-=count;
            const double life=std::clamp((.35+speed)*.3,.1,.4);
            for(size_t n=0;n<nozzles.size();++n) {
                const auto &nozzle=nozzles[n]; const auto p=nozzle.point;
                for(int i=0;i<count;++i) {
                    const double spread=style==Style::ribbon?0:(rng.next()-.5)*.6, kick=.6+rng.next()*.8;
                    const double size=std::clamp((1+speed*2+rng.next())*.7,1.0,3.0);
                    uint32_t color=nozzle.color;
                    if(!nozzle.accents.empty() && rng.next()>=.55)
                        color=nozzle.accents[std::min(nozzle.accents.size()-1,size_t(rng.next()*nozzle.accents.size()))];
                    if(particles.size()==180)particles.erase(particles.begin());
                    particles.push_back({x+p.x*cs-p.y*sn,y+p.x*sn+p.y*cs,-sn*kick+spread*cs,cs*kick+spread*sn,
                                         life,life,size,n,color,style});
                }
            }
        }
        for(auto &p:particles){p.x+=p.vx;p.y+=p.vy;p.vx*=.96;p.vy*=.96;p.life-=tick;}
        std::erase_if(particles,[](const auto &p){return p.life<=0;});
    }
};
}
