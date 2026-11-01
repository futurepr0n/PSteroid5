#include "space_effects.hpp"
#include "ship_design.hpp"
#include "ship_editor.hpp"
#include <cassert>
#include <cstdio>
using namespace cssteroids;
int main() {
    visual::Starfield stars(1);
    assert(stars.layers[0].stars.size()==70 && stars.layers[1].stars.size()==40 && stars.layers[2].stars.size()==22);
    for(int i=0;i<60;++i)stars.step(2,-3);
    assert(std::fabs(stars.travelX-120)<1e-8 && std::fabs(visual::wrap(stars.travelY,1024000)-1023820)<1e-8);
    const double previous=stars.travelX;stars.step(NAN,0);assert(stars.travelX==previous);
    stars.nextShooting=0;stars.step();assert(stars.shooting.size()==1);
    const double heading=stars.shooting[0].heading;
    for(int i=0;i<15;++i)stars.step();
    assert(stars.shooting[0].heading!=heading && stars.shooting[0].tail.size()==14);
    for(int i=0;i<100;++i)stars.step();
    assert(stars.shooting.empty());
    assert(visual::wrap(-1)==1023);

    json::Value v;
    assert(json::parse(R"({"name":"Curve","type":"custom","customLines":[
      {"startX":-40,"startY":0,"endX":40,"endY":0,"controlX":0,"controlY":160,"width":1,"color":"#00ffff"},
      {"startX":-100,"startY":160,"endX":100,"endY":160,"mesh":true,"meshGroup":"fill-1","color":"#ff00ff"}],
      "thrusterPoints":[{"x":0,"y":80}]})",v));
    ShipDesign ship;assert(parse_ship(v,ship));
    assert(ship.geometry.lines.size()==2 && ship.geometry.lines[0].curved && ship.geometry.lines[0].width==1);
    assert(ship_segments(ship.geometry,0,0,0).size()==17);
    assert(ship_hits_circle(ship.geometry,0,0,0,0,20,.1)); // curve midpoint
    assert(!ship_hits_circle(ship.geometry,0,0,0,20,40,.1)); // decoration, not hull
    editor::Ship document;assert(editor::from_server(v,document));
    editor::Editor ed(document);assert(ed.scale_ship(.5));
    assert(ed.ship.lines[0].control.y==80 && ed.ship.thrusters[0].y==40);
    assert(ed.undo() && ed.ship.lines[0].control.y==160);
    json::Value back;assert(json::parse(editor::post_body(ed.ship,{}),back));
    const auto &line=back.get("customLines")->items[1];
    assert(line.get("mesh")->b && line.str("meshGroup")=="fill-1");
    editor::Editor edge;edge.mirrorOn=false;edge.add_line({190,190},{200,200},"white");
    assert(!edge.scale_ship(1.1) && edge.ship.lines[0].b.x==200);

    visual::Trail trail(5);powerups::Effects fx;
    for(int i=0;i<60;++i)trail.step(ship.geometry,100+i,100,0,5,0,true,true,fx);
    assert(!trail.particles.empty() && trail.particles.size()<=180 && trail.nozzles.size()==1);
    bool cyan=false,magenta=false;
    for(const auto &p:trail.particles){cyan|=p.color==0x00ffff;magenta|=p.color==0xff00ff;assert(p.style==visual::Style::pixels);}
    assert(cyan && magenta);
    fx.add(powerups::Type::spray);assert(visual::active_style(fx)==visual::Style::embers);
    trail.step(ship.geometry,160,100,0,5,0,true,true,fx);assert(trail.particles.back().style==visual::Style::embers);
    fx.add(powerups::Type::rapid);assert(visual::active_style(fx)==visual::Style::ribbon);
    trail.step(ship.geometry,161,100,0,5,0,true,true,fx);assert(trail.particles.back().style==visual::Style::ribbon);
    fx.clear();assert(visual::active_style(fx)==visual::Style::pixels);
    for(int i=0;i<30;++i)trail.step(ship.geometry,161,100,0,0,0,false,true,fx);
    assert(trail.particles.empty());
    trail.step(ship.geometry,161,100,0,5,0,true,true,fx);
    trail.step(ship.geometry,1000,100,0,0,0,false,true,fx);assert(trail.particles.empty());
    trail.step(ship.geometry,1000,100,0,5,0,true,true,fx);
    trail.step(ship.geometry,1000,100,0,0,0,false,false,fx);assert(trail.particles.empty());
    ++ship.geometry.revision;ship.geometry.lines[0].color=0xff0000;
    trail.configure(ship.geometry);assert(trail.particles.empty());
    puts("space effects, curved hull/pattern collision, scale and interchange tests passed");
}
