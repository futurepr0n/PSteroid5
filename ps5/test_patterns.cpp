#include "ship_editor.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace cssteroids;
using namespace cssteroids::editor;
namespace pat=cssteroids::editor::pattern;
static editor::Ship square(double half=80) {
    editor::Ship s;s.name="Pattern test";
    s.lines={{{-half,-half},{half,-half},"cyan"},{{half,-half},{half,half},"cyan"},
             {{half,half},{-half,half},"cyan"},{{-half,half},{-half,-half},"cyan"}};
    return s;
}
int main(int argc,char **argv){
    if(argc==3 && std::string(argv[1])=="decode"){
        std::ifstream in(argv[2]);std::string text((std::istreambuf_iterator<char>(in)),{});json::Value fixtures;
        assert(json::parse(text,fixtures) && fixtures.is_array());
        for(const auto &f:fixtures.items){pat::Tile t;std::string error;assert(pat::decode(f.str("code"),t,error));
            const auto *expected=f.get("lines");assert(expected && expected->items.size()==t.lines.size());
            std::multiset<std::string> want,got;
            for(const auto &l:expected->items)want.insert(pat::key(pat::read_line(l),4));
            for(const auto &l:t.lines)got.insert(pat::key(l,4));
            assert(want==got);
        }
        std::cout<<"Browser-authored CSP1 codes decoded with matching artwork\n";return 0;
    }
    std::string error;Editor ed(square());ed.mirrorOn=false;
    auto regions=pat::regions(ed.ship.lines,error);assert(error.empty() && regions.size()==1 && regions[0].area==25600);
    assert(pat::region_at(regions,{0,0}) && !pat::region_at(regions,{100,0}));
    pat::Tile blank;
    pat::Region large{{{-5000,-5000},{5000,-5000},{5000,5000},{-5000,5000}},{},{},100000000};
    assert(pat::repeat(large,blank,.05,error).empty() && error.empty());
    auto tile=pat::preset("grid");
    assert(ed.apply_fill({0,0},tile,5));assert(ed.ship.fills.size()==1 && ed.ship.lines.size()>4);
    const auto count=ed.ship.lines.size();const auto id=ed.ship.fills[0].id;
    assert(ed.undo() && ed.ship.fills.empty() && ed.ship.lines.size()==4);
    assert(ed.redo() && ed.ship.fills[0].id==id && ed.ship.lines.size()==count);
    assert(ed.move_point({80,80},{60,70}));assert(ed.ship.fills.size()==1);
    assert(ed.undo() && ed.ship.lines.size()==count);
    assert(ed.scale_ship(.5) && ed.ship.fills[0].scale==2.5 && ed.ship.fills[0].tile.side==8);
    assert(ed.undo());
    assert(!ed.apply_fill({0,0},tile,.05));assert(ed.ship.lines.size()==count && ed.ship.fills[0].scale==5);
    assert(ed.remove_fill({0,0}) && ed.ship.lines.size()==4 && ed.ship.fills.empty());
    assert(ed.undo() && ed.ship.fills.size()==1);
    // Opening a hull removes the derived fill atomically; undo restores the recipe and art.
    assert(ed.erase_near({80,0}) && ed.ship.fills.empty() && ed.ship.lines.size()==3);
    assert(ed.undo() && ed.ship.fills.size()==1);
    auto oversized=ed.ship;oversized.fills.resize(200,ed.ship.fills.front());
    for(auto &fill:oversized.fills)fill.tile.lines.resize(256,fill.tile.lines.front());
    assert(invalid_reason(oversized).find("document too large")!=std::string::npos);
    auto posted=post_body(ed.ship,{},false);json::Value legacy;assert(json::parse(posted,legacy));
    editor::Ship restored;assert(from_server(legacy,restored) && restored.fills.size()==1);
    auto doc=document(ed.ship);auto server=legacy;server.set("editorDocument",json::Value::string_value(json::dump(doc)));
    assert(from_server(server,restored) && restored.fills[0].tile.lines.size()==tile.lines.size());
    assert(restored.lines[0].id==ed.ship.lines[0].id);
    auto code=pat::encode(tile,error);assert(error.empty() && code.substr(0,5)=="CSP1-");
    pat::Tile decoded;assert(pat::decode(code,decoded,error) && decoded.lines.size()==tile.lines.size());
    code.back()=code.back()=='0'?'1':'0';assert(!pat::decode(code,decoded,error));error.clear();
    auto nested=square();auto inner=square(25);nested.lines.insert(nested.lines.end(),inner.lines.begin(),inner.lines.end());
    Editor holes(nested);auto rs=pat::regions(holes.ship.lines,error);assert(rs.size()==2);
    const auto *outer=pat::region_at(rs,{60,0});assert(outer && outer->holes.size()==1 && !pat::inside({0,0},*outer));
    auto repeated=pat::repeat(*outer,tile,5,error);assert(error.empty());
    for(const auto &l:repeated)assert(pat::inside(pat::mix(l.a,l.b,.5),*outer));
    auto divided=square();divided.lines.push_back({{0,-80},{0,80},"white"});
    Editor split(divided);auto faces=pat::regions(split.ship.lines,error);assert(faces.size()==2);
    auto open=square();open.lines.pop_back();assert(pat::regions(open.lines,error).empty());
    // Produce reference-judgeable fixtures, not expected values copied from our implementation.
    json::Value fixtures;fixtures.type=json::Value::Type::array;
    for(const auto &kind:std::vector<std::string>{"grid","hex","triangle","diamond","ribs","crosshatch"}){
        auto t=pat::preset(kind);assert(pat::valid(t));
        for(int which=0;which<4;++which){
            auto shape=which==1?nested:which==2?divided:square();if(which==3){shape.lines[0].curved=true;shape.lines[0].control={0,-160};}
            Editor e(shape);auto seed=which==1?Pt{60,0}:which==2?Pt{-40,0}:Pt{0,0};
            auto regions=pat::regions(e.ship.lines,error);auto *r=pat::region_at(regions,seed);assert(r);
            auto repeated=pat::repeat(*r,t,5,error);assert(error.empty());
            auto nativeMesh=pat::mesh(*r,kind,20,error);assert(error.empty());
            json::Value f=json::Value::object(),hull;hull.type=json::Value::Type::array;
            for(const auto &l:e.ship.lines)hull.items.push_back(pat::line_json(l));
            f.set("hull",hull);f.set("seed",pat::point(seed));f.set("tile",pat::tile_json(t));f.set("preset",json::Value::string_value(kind));f.set("scale",pat::number(5));
            json::Value output;output.type=json::Value::Type::array;for(const auto &l:repeated)output.items.push_back(pat::line_json(l));f.set("repeated",output);
            output.items.clear();for(const auto &l:nativeMesh)output.items.push_back(pat::line_json(l));f.set("mesh",output);
            assert(e.apply_fill(seed,t,5));f.set("document",document(e.ship));
            f.set("code",json::Value::string_value(pat::encode(t,error)));fixtures.items.push_back(f);
        }
    }
    if(argc>1)std::ofstream(argv[1])<<json::dump(fixtures);
    std::cout<<"pattern regions, holes, fills, atomic limits, undo/refit, metadata and CSP1 tests passed\n";
}
