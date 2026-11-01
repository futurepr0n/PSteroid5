#pragma once
// Native port of editor-lab/geometry.mjs: planar regions, clipped procedural
// artwork and freeform repeating tiles. No renderer, network or console state.
#include "editor_geometry.hpp"
#include "json_lite.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <array>
#include <map>
#include <set>
#include <vector>
#include <limits>

namespace cssteroids::editor::pattern {
constexpr double eps=1e-7;
inline Pt sub(Pt a,Pt b){return {a.x-b.x,a.y-b.y};}
inline double cross(Pt a,Pt b){return a.x*b.y-a.y*b.x;}
inline Pt mix(Pt a,Pt b,double t){return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};}
inline Pt at(const Line &l,double t){double u=1-t;return l.curved?Pt{u*u*l.a.x+2*u*t*l.control.x+t*t*l.b.x,u*u*l.a.y+2*u*t*l.control.y+t*t*l.b.y}:mix(l.a,l.b,t);}
inline std::vector<Pt> flatten(const Line &l){std::vector<Pt> p;int n=l.curved?32:1;for(int i=0;i<=n;++i)p.push_back(at(l,double(i)/n));return p;}
inline bool contains(Pt p,const std::vector<Pt> &poly){
    bool hit=false;if(poly.empty())return false;
    for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++){
        auto a=poly[i],b=poly[j];
        if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)hit=!hit;
    }return hit;
}
inline bool intersection(Pt a,Pt b,Pt c,Pt d,double &t,double &u){
    auto v=sub(b,a),w=sub(d,c);double den=cross(v,w);if(std::fabs(den)<eps)return false;
    auto q=sub(c,a);t=cross(q,w)/den;u=cross(q,v)/den;
    if(t < -eps || t>1+eps || u < -eps || u>1+eps)return false;
    t=std::clamp(t,0.,1.);u=std::clamp(u,0.,1.);return true;
}
struct Region {std::vector<Pt> points;std::vector<std::vector<Pt>> holes;std::vector<std::string> boundary;double area=0;};
inline bool inside(Pt p,const Region &r){return contains(p,r.points) && std::none_of(r.holes.begin(),r.holes.end(),[&](const auto &h){return contains(p,h);});}
inline std::vector<Region> regions(const std::vector<Line> &lines,std::string &error){
    error.clear();
    struct Edge{Pt a,b;std::vector<double> cuts{0,1};std::string id;};
    std::vector<Edge> edges;
    for(size_t i=0;i<lines.size();++i)if(!lines[i].mesh){auto p=flatten(lines[i]);for(size_t j=1;j<p.size();++j)edges.push_back({p[j-1],p[j],{0,1},lines[i].id.empty()?"line-"+std::to_string(i):lines[i].id});}
    // A quadratic pairwise planarization has a deliberate work budget on console.
    if(edges.size()>2048){error="Hull too complex for region detection (2048 flattened edges)";return {};}
    for(size_t i=0;i<edges.size();++i)for(size_t j=i+1;j<edges.size();++j){double t,u;if(intersection(edges[i].a,edges[i].b,edges[j].a,edges[j].b,t,u)){edges[i].cuts.push_back(t);edges[j].cuts.push_back(u);}}
    struct Node{Pt p;std::vector<int> out;};struct Half{int from,to;std::string id;};
    std::vector<Node> nodes;std::vector<Half> halves;std::map<std::pair<long long,long long>,int> lookup;
    auto node=[&](Pt p){auto key=std::pair{std::llround(p.x*1000),std::llround(p.y*1000)};auto it=lookup.find(key);if(it!=lookup.end())return it->second;int n=int(nodes.size());nodes.push_back({p,{}});lookup[key]=n;return n;};
    std::set<std::pair<int,int>> seen;
    for(auto &e:edges){std::sort(e.cuts.begin(),e.cuts.end());for(size_t i=1;i<e.cuts.size();++i){int a=node(mix(e.a,e.b,e.cuts[i-1])),b=node(mix(e.a,e.b,e.cuts[i]));if(a==b || !seen.insert(std::minmax(a,b)).second)continue;int n=int(halves.size());halves.push_back({a,b,e.id});halves.push_back({b,a,e.id});nodes[a].out.push_back(n);nodes[b].out.push_back(n+1);}}
    for(auto &n:nodes)std::sort(n.out.begin(),n.out.end(),[&](int a,int b){auto p=nodes[halves[a].to].p,q=nodes[halves[b].to].p;return std::atan2(p.y-n.p.y,p.x-n.p.x)<std::atan2(q.y-n.p.y,q.x-n.p.x);});
    std::vector<bool> visited(halves.size());std::vector<Region> out;
    for(size_t first=0;first<halves.size();++first){if(visited[first])continue;size_t e=first;Region r;std::set<std::string> ids;
        while(!visited[e]){visited[e]=true;auto h=halves[e];r.points.push_back(nodes[h.from].p);ids.insert(h.id);auto &v=nodes[h.to].out;auto back=std::find(v.begin(),v.end(),int(e)^1);if(back==v.end())break;size_t index=size_t(back-v.begin());e=size_t(v[(index+v.size()-1)%v.size()]);}
        for(size_t i=0;i<r.points.size();++i)r.area+=cross(r.points[i],r.points[(i+1)%r.points.size()])/2;
        if(e==first && r.area>1){r.boundary.assign(ids.begin(),ids.end());out.push_back(std::move(r));}
    }
    for(size_t i=0;i<out.size();++i){std::vector<size_t> nested;for(size_t j=0;j<out.size();++j)if(j!=i && out[j].area<out[i].area && std::all_of(out[j].points.begin(),out[j].points.end(),[&](Pt p){return contains(p,out[i].points);}))nested.push_back(j);
        for(auto j:nested)if(std::none_of(nested.begin(),nested.end(),[&](size_t k){return k!=j && out[k].area>out[j].area && contains(out[j].points[0],out[k].points);}))out[i].holes.push_back(out[j].points);
    }return out;
}
inline const Region *region_at(const std::vector<Region> &rs,Pt p){const Region *best=nullptr;for(const auto &r:rs)if(inside(p,r) && (!best || r.area<best->area))best=&r;return best;}
inline std::vector<Line> clip(const Line &l,const Region &r){
    std::vector<double> ts{0,1};auto cut=[&](const auto &poly){for(size_t i=0;i<poly.size();++i){double t,u;if(intersection(l.a,l.b,poly[i],poly[(i+1)%poly.size()],t,u))ts.push_back(t);}};
    cut(r.points);for(const auto &h:r.holes)cut(h);std::sort(ts.begin(),ts.end());std::vector<Line> out;
    for(size_t i=1;i<ts.size();++i)if(ts[i]-ts[i-1]>eps && inside(mix(l.a,l.b,(ts[i]+ts[i-1])/2),r)){Line p=l;p.a=mix(l.a,l.b,ts[i-1]);p.b=mix(l.a,l.b,ts[i]);p.curved=false;out.push_back(std::move(p));}
    return out;
}
inline std::array<double,4> bounds(const Region &r){std::array<double,4>b{1e30,1e30,-1e30,-1e30};for(auto p:r.points){b[0]=std::min(b[0],p.x);b[1]=std::min(b[1],p.y);b[2]=std::max(b[2],p.x);b[3]=std::max(b[3],p.y);}return b;}
inline std::string key(const Line &l,int precision=7){
    auto a=l.a,b=l.b;if(a.x>b.x || (a.x==b.x && a.y>b.y))std::swap(a,b);
    char text[160];std::snprintf(text,sizeof text,"%.*f,%.*f|%.*f,%.*f",precision,a.x,precision,a.y,precision,b.x,precision,b.y);return text;
}
inline std::vector<Line> mesh(const Region &r,const std::string &kind,double spacing,std::string &error){
    error.clear();
    if(!std::isfinite(spacing) || spacing<.05 || spacing>10000){error="Pattern scale must be 0.05 to 10000";return {};}
    auto b=bounds(r);if((b[2]-b[0])*(b[3]-b[1])/(spacing*spacing)>25000){error="Pattern too dense; increase scale";return {};}
    const double minX=std::floor(b[0]/spacing)*spacing-spacing*2,maxX=b[2]+spacing*2,minY=std::floor(b[1]/spacing)*spacing-spacing*2,maxY=b[3]+spacing*2;
    std::vector<Line> out;std::set<std::string> seen;size_t attempts=0;
    auto add=[&](Pt a,Pt b){if(++attempts>250000)return;for(auto l:clip(Line{a,b,"#ffffff"},r))if(seen.insert(key(l,3)).second){if(out.size()>=5000){error="Pattern exceeds 5000 lines; increase scale";return;}out.push_back(std::move(l));}};
    const double sqrt3=std::sqrt(3.),sqrt2=std::sqrt(2.);
    if(kind=="hex"){
        double rad=spacing/sqrt3,dy=rad*sqrt3;
        for(int col=int(std::floor(minX/(rad*1.5)));col<=int(std::ceil(maxX/(rad*1.5)));++col)for(int row=int(std::floor(minY/dy))-1;row<=int(std::ceil(maxY/dy));++row){double x=col*rad*1.5,y=(row+(std::abs(col)%2)*.5)*dy;Pt p[6];for(int i=0;i<6;++i)p[i]={x+rad*std::cos(i*3.141592653589793/3),y+rad*std::sin(i*3.141592653589793/3)};for(int i=0;i<6;++i)add(p[i],p[(i+1)%6]);}
    }else if(kind=="ribs"){
        double cx=(minX+maxX)/2,cy=(minY+maxY)/2,rx=(maxX-minX)/2,ry=(maxY-minY)/2;
        for(double x=minX;x<=maxX;x+=spacing){Pt prev;bool have=false;for(double y=minY;y<=maxY;y+=6){Pt p{cx+(x-cx)*(.55+.45*std::sqrt(std::max(0.,1-std::pow((y-cy)/ry,2)))),y};if(have)add(prev,p);prev=p;have=true;}}
        for(double y=minY-60;y<=maxY;y+=spacing){Pt prev;bool have=false;for(double x=minX;x<=maxX;x+=6){Pt p{x,y+40*(1-std::pow((x-cx)/rx,2))};if(have)add(prev,p);prev=p;have=true;}}
    }else{
        auto diagonal=[&](double slope,double step){double lo=std::min(minY-slope*minX,minY-slope*maxX),hi=std::max(maxY-slope*minX,maxY-slope*maxX);for(double k=std::floor(lo/step)*step;k<=hi;k+=step)add({minX,slope*minX+k},{maxX,slope*maxX+k});};
        if(kind=="grid" || kind=="crosshatch"){for(double x=minX;x<=maxX;x+=spacing)add({x,minY},{x,maxY});for(double y=minY;y<=maxY;y+=spacing)add({minX,y},{maxX,y});}
        if(kind=="diamond" || kind=="crosshatch"){diagonal(1,spacing*sqrt2);diagonal(-1,spacing*sqrt2);}
        if(kind=="triangle"){diagonal(sqrt3,spacing*sqrt3);diagonal(-sqrt3,spacing*sqrt3);double step=spacing*sqrt3/2;for(double y=std::floor(minY/step)*step;y<=maxY;y+=step)add({minX,y},{maxX,y});}
    }
    if(attempts>250000)error="Pattern work budget exceeded; increase scale";
    if(!error.empty())out.clear();
    return out;
}
struct Tile {std::string name="PS5 Pattern",color="#00ffff";int side=8;double width=1;std::vector<Line> lines;json::Value original;};
inline bool hex_color(const std::string &c){return (c.size()==4 || c.size()==7) && c[0]=='#' && std::all_of(c.begin()+1,c.end(),[](unsigned char x){return std::isxdigit(x)!=0;});}
inline bool valid(const Tile &t){
    if(!hex_color(t.color))return false;
    if(t.side<2 || t.side>16 || t.name.empty() || t.name.size()>60 || (t.width!=1 && t.width!=2) || t.lines.size()>(t.original.is_object()?800:256))return false;
    for(const auto &l:t.lines){if(!hex_color(l.color) || (l.width!=1 && l.width!=2))return false;for(auto p:l.curved?std::vector<Pt>{l.a,l.b,l.control}:std::vector<Pt>{l.a,l.b})if(!std::isfinite(p.x+p.y) || p.x<0 || p.y<0 || p.x>t.side || p.y>t.side)return false;}return true;
}
inline std::vector<Line> starter(const std::string &kind,int side,double spacing,std::string &error) {
    double w=side,h=side,s=spacing,root3=std::sqrt(3.),root2=std::sqrt(2.);
    auto square=[](double x,double y){return Region{{{0,0},{x,0},{x,y},{0,y}},{},{},x*y};};
    std::vector<Line> raw;
    if(kind=="hex") {double rows=std::max(1.,std::round(h/s)),step=h/rows,periods=std::max(1.,std::round(w/(root3*step)));w=periods*root3*step;raw=mesh(square(w,h),kind,step,error);}
    else if(kind=="triangle"){double across=std::max(1.,std::round(w/s)),rows=std::max(1.,std::round(h/(s*root3)));w=across*s;h=rows*s*root3;raw=mesh(square(w,h),kind,s,error);}
    else if(kind=="diamond"){w=std::max(1.,std::round(w/(s*root2)))*s*root2;h=std::max(1.,std::round(h/(s*root2)))*s*root2;raw=mesh(square(w,h),kind,s,error);}
    else if(kind=="crosshatch"){double across=std::max(1.,std::round(w/s)),periods=std::max(1.,std::round(w/(s*root2)));raw=mesh(square(w,h),"grid",w/across,error);auto diagonal=mesh(square(w,h),"diamond",w/(periods*root2),error);raw.insert(raw.end(),diagonal.begin(),diagonal.end());}
    else if(kind=="grid"){raw=mesh(square(w,h),kind,w/std::max(1.,std::round(w/s)),error);}
    else raw=mesh(square(w,h),kind,s,error);
    std::vector<Line> out;auto target=square(side,side);
    for(auto l:raw){l.a.x*=side/w;l.b.x*=side/w;l.a.y*=side/h;l.b.y*=side/h;auto parts=clip(l,target);out.insert(out.end(),parts.begin(),parts.end());}
    return out;
}
inline Tile preset(const std::string &kind){
    Tile t;t.name=kind;std::string error;t.lines=starter(kind,t.side,2,error);
    for(size_t i=0;i<t.lines.size();++i){auto &l=t.lines[i];l.id="line-"+std::to_string(i);l.color=t.color;l.width=t.width;}
    return t;
}
inline std::vector<Line> repeat(const Region &r,const Tile &t,double scale,std::string &error){
    error.clear();
    if(!valid(t) || !std::isfinite(scale) || scale<.05 || scale>10000){error="Invalid pattern artwork or scale";return {};}
    if(t.lines.empty())return {};
    auto b=bounds(r);double step=t.side*scale;long long minCol=std::llround(std::floor(b[0]/step))-1,maxCol=std::llround(std::floor(b[2]/step)),minRow=std::llround(std::floor(b[1]/step))-1,maxRow=std::llround(std::floor(b[3]/step));
    if(double(maxCol-minCol+1)*double(maxRow-minRow+1)*t.lines.size()>250000){error="Pattern repeat too dense; increase scale";return {};}
    std::vector<Line> out;std::set<std::string> seen;
    for(auto col=minCol;col<=maxCol;++col)for(auto row=minRow;row<=maxRow;++row)for(const auto &l:t.lines){auto p=flatten(l);for(size_t i=1;i<p.size();++i){Line moved{{(col*t.side+p[i-1].x)*scale,(row*t.side+p[i-1].y)*scale},{(col*t.side+p[i].x)*scale,(row*t.side+p[i].y)*scale},l.color};moved.width=l.width;
        for(auto part:clip(moved,r))if(seen.insert(key(part)).second){if(out.size()==5000){error="Pattern exceeds 5000 lines; increase scale";return {};}out.push_back(std::move(part));}
    }}return out;
}
struct Fill {std::string id,kind="custom",color="#00ffff";std::vector<std::string> boundary;Pt seed;double scale=5,width=1;int tiles=0;Tile tile;};
inline const Region *match(const std::vector<Region> &rs,const Fill &f){
    const Region *best=nullptr;int count=0;for(const auto &r:rs){auto a=r.boundary,b=f.boundary;std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());if(a==b){++count;if(inside(f.seed,r))best=&r;}}
    if(count==1){for(const auto &r:rs){auto a=r.boundary,b=f.boundary;std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());if(a==b)return &r;}}return best;
}
inline std::vector<Line> bake(const Region &r,const Fill &f,std::string &error){
    double spacing=f.scale;if(f.tiles>=2 && f.tiles<=48){auto b=bounds(r);double factor=f.kind=="hex"?2/std::sqrt(3.):f.kind=="diamond"?1/std::sqrt(2.):1;spacing=std::max(.05,(b[2]-b[0])/f.tiles*factor);}
    auto lines=f.kind=="custom"?repeat(r,f.tile,f.scale,error):mesh(r,f.kind,spacing,error);
    for(size_t i=0;i<lines.size();++i){auto &l=lines[i];l.id=f.id+"-mesh-"+std::to_string(i);l.mesh=true;l.meshGroup=f.id;if(f.kind!="custom"){l.color=f.color;l.width=f.width;}}return lines;
}
inline json::Value number(double n){return json::Value::number_value(std::round(n*1048576)/1048576);}
inline json::Value point(Pt p){auto v=json::Value::object();v.set("x",number(p.x));v.set("y",number(p.y));return v;}
inline json::Value line_json(const Line &l){auto v=json::Value::object();v.set("startX",number(l.a.x));v.set("startY",number(l.a.y));v.set("endX",number(l.b.x));v.set("endY",number(l.b.y));v.set("color",json::Value::string_value(l.color));v.set("width",number(l.width));if(!l.id.empty())v.set("id",json::Value::string_value(l.id));if(l.curved){v.set("controlX",number(l.control.x));v.set("controlY",number(l.control.y));}if(l.mesh)v.set("mesh",json::Value::bool_value(true));if(!l.meshGroup.empty())v.set("meshGroup",json::Value::string_value(l.meshGroup));if(!l.pairId.empty())v.set("pairId",json::Value::string_value(l.pairId));return v;}
inline Line read_line(const json::Value &v){Line l{{v.num("startX"),v.num("startY")},{v.num("endX"),v.num("endY")},v.str("color","#ffffff")};l.id=v.str("id");l.width=v.num("width",2)==1?1:2;l.mesh=v.get("mesh") && v.get("mesh")->b;l.meshGroup=v.str("meshGroup");l.pairId=v.str("pairId");l.curved=v.get("controlX") && v.get("controlY");l.control={v.num("controlX"),v.num("controlY")};return l;}
inline json::Value tile_json(const Tile &t){if(t.original.is_object())return t.original;auto v=json::Value::object();v.set("version",number(1));v.set("name",json::Value::string_value(t.name));v.set("width",number(t.side));v.set("height",number(t.side));v.set("color",json::Value::string_value(t.color));v.set("strokeWidth",number(t.width));json::Value arr;arr.type=json::Value::Type::array;v.set("cells",arr);v.set("edges",arr);for(size_t i=0;i<t.lines.size();++i){auto l=t.lines[i];if(l.id.empty())l.id="line-"+std::to_string(i);arr.items.push_back(line_json(l));}v.set("lines",arr);return v;}
inline Tile read_tile(const json::Value &v){
    Tile t;t.original=v;t.name=v.str("name","PS5 Pattern");t.side=int(v.num("width",8));t.width=v.num("strokeWidth",1);t.color=v.str("color","#ffffff");
    if(const auto *ls=v.get("lines");ls && ls->is_array())for(const auto &l:ls->items)t.lines.push_back(read_line(l));
    // Older tiles can carry adjacent grid edges in addition to freeform artwork.
    if(const auto *es=v.get("edges");es && es->is_array())for(const auto &e:es->items){double x,y,u,w;if(std::sscanf(e.s.c_str(),"%lf,%lf|%lf,%lf",&x,&y,&u,&w)==4){Line l{{x,y},{u,w},t.color};l.width=t.width;t.lines.push_back(l);}}
    if(!v.str("preset").empty()){
        std::string err;auto generated=starter(v.str("preset"),t.side,v.num("presetSpacing",2),err);
        std::set<std::string> removed;if(const auto *a=v.get("removed");a && a->is_array())for(const auto &e:a->items)removed.insert(e.s);
        for(size_t i=0;i<generated.size();++i){auto id="preset-"+std::to_string(i);if(removed.count(id))continue;auto existing=std::find_if(t.lines.begin(),t.lines.end(),[&](const auto &l){return l.id==id;});if(existing!=t.lines.end())continue;auto l=generated[i];l.id=id;l.width=t.width;l.color=t.color;t.lines.push_back(std::move(l));}
    }
    for(size_t i=0;i<t.lines.size();++i)if(t.lines[i].id.empty())t.lines[i].id="tile-"+std::to_string(i);
    return t;
}
inline json::Value fill_json(const Fill &f){auto v=json::Value::object();v.set("id",json::Value::string_value(f.id));v.set("pattern",json::Value::string_value(f.kind));v.set("seed",point(f.seed));v.set("spacing",number(f.scale));v.set("color",json::Value::string_value(f.color));v.set("width",number(f.width));if(f.tiles)v.set("tiles",number(f.tiles));json::Value arr;arr.type=json::Value::Type::array;for(const auto &id:f.boundary)arr.items.push_back(json::Value::string_value(id));v.set("boundary",arr);if(f.kind=="custom"){v.set("scale",number(f.scale));v.set("tile",tile_json(f.tile));}return v;}
inline Fill read_fill(const json::Value &v){Fill f;f.id=v.str("id");f.kind=v.str("pattern","custom");f.scale=v.num("scale",v.num("spacing",5));f.color=v.str("color","#ffffff");f.width=v.num("width",1);f.tiles=int(v.num("tiles"));if(const auto *s=v.get("seed"))f.seed={s->num("x"),s->num("y")};if(const auto *b=v.get("boundary");b && b->is_array())for(const auto &id:b->items)f.boundary.push_back(id.s);if(const auto *t=v.get("tile"))f.tile=read_tile(*t);return f;}
inline std::string base64(const std::string &data){
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;uint32_t carry=0;int bits=0;
    for(unsigned char c:data){carry=(carry<<8)|c;bits+=8;while(bits>=6){bits-=6;out+=alphabet[(carry>>bits)&63];}}
    if(bits)out+=alphabet[(carry<<(6-bits))&63];
    return out;
}
inline std::string checksum(const std::string &payload){uint32_t h=2166136261u;for(unsigned char c:payload)h=(h^c)*16777619u;char hex[9];std::snprintf(hex,sizeof hex,"%08x",h);return hex;}
inline std::string encode(const Tile &t,std::string &error){
    error.clear();
    if(!valid(t)){error="Invalid pattern tile";return {};}
    auto payload=base64(json::dump(tile_json(t)));auto code="CSP1-"+payload+"."+checksum(payload);
    if(code.size()>4096){error="Pattern code exceeds 4096 characters; simplify artwork";return {};}return code;
}
inline bool decode(const std::string &code,Tile &out,std::string &error){
    error.clear();
    if(code.size()>4096 || code.size()<15 || code.substr(0,5)!="CSP1-" || code[code.size()-9]!='.') {error="Invalid CSP1 pattern code";return false;}
    auto payload=code.substr(5,code.size()-14);if(checksum(payload)!=code.substr(code.size()-8)){error="Pattern code checksum mismatch";return false;}
    std::string data,alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";uint32_t carry=0;int bits=0;
    for(char c:payload){auto index=alphabet.find(c);if(index==std::string::npos){error="Malformed pattern code encoding";return false;}carry=(carry<<6)|uint32_t(index);bits+=6;if(bits>=8){bits-=8;data+=char((carry>>bits)&255);}}
    if(base64(data)!=payload){error="Malformed pattern code padding";return false;}
    json::Value v;if(!json::parse(data,v) || !v.is_object() || v.num("version")!=1 || v.num("width")!=v.num("height") || v.num("width")!=std::floor(v.num("width"))){error="Invalid pattern code schema";return false;}
    const std::set<std::string> allowed={"version","name","width","height","cells","edges","lines","color","strokeWidth","preset","presetSpacing","removed"};
    for(auto &field:v.fields)if(!allowed.count(field.first)){error="Unknown pattern code field";return false;}
    for(auto key:{"version","name","width","height","cells","edges","color","strokeWidth"})if(!v.get(key)){error="Incomplete pattern code schema";return false;}
    auto t=read_tile(v);if(!valid(t)){error="Invalid pattern artwork";return false;}out=std::move(t);return true;
}

// Live ship preview framing: zoom 1 fits the whole ship; larger zooms magnify around a focus without leaving the ship
constexpr std::array<double,5> view_zooms={1,1.5,2,3,4};
// Fine view control: a tap changes one percentage point; a hold glides after a
// short delay. Time is integrated after the delay for consistent 30/60 Hz input.
struct SmoothZoom {
    static constexpr double minimum=.25,maximum=4,tap=.01,delay=.25,rate=.35;
    double value=1,heldSeconds=0;int direction=0;
    void reset(){value=1;heldSeconds=0;direction=0;}
    void release(){heldSeconds=0;direction=0;}
    bool update(bool up,bool down,double dt){
        const double before=value;const int next=int(up)-int(down);
        if(!next){release();return false;}
        if(next!=direction){direction=next;heldSeconds=0;value+=direction*tap;}
        const double old=heldSeconds;heldSeconds+=std::isfinite(dt)?std::clamp(dt,0.,.1):0;
        value+=direction*rate*(std::max(0.,heldSeconds-delay)-std::max(0.,old-delay));
        value=std::clamp(value,minimum,maximum);return value!=before;
    }
    double slider()const{return (value-minimum)/(maximum-minimum);}
};
struct View {Pt centre;double scale=1;};  // screen = view origin + (design point - centre) * scale
inline bool bounds(const std::vector<Line> &lines,Pt &lo,Pt &hi){
    bool any=false;
    for(const auto &l:lines)for(auto p:flatten(l)){
        if(!std::isfinite(p.x) || !std::isfinite(p.y))continue;
        if(!any){lo=hi=p;any=true;continue;}
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y)};
    }
    return any;
}
inline View fit_view(const std::vector<Line> &lines,double px,double zoom=1,Pt focus={},double margin=.06){
    Pt lo,hi;if(px<=0 || !bounds(lines,lo,hi))return {{0,0},px>0?px/400:1};
    const double span=std::max({hi.x-lo.x,hi.y-lo.y,1.});
    View v{{(lo.x+hi.x)/2,(lo.y+hi.y)/2},px*(1-2*margin)/span};
    if(!std::isfinite(zoom) || zoom<=0)zoom=1;
    if(zoom==1)return v;
    v.scale*=zoom;const double half=px/2/v.scale;
    auto keep=[&](double f,double a,double b){return b-a<=2*half?(a+b)/2:std::clamp(f,a+half,b-half);};
    v.centre={keep(focus.x,lo.x,hi.x),keep(focus.y,lo.y,hi.y)};
    return v;
}
inline Pt centre_of(const std::vector<Pt> &points){
    if(points.empty())return {};
    Pt lo=points[0],hi=points[0];
    for(auto p:points){lo={std::min(lo.x,p.x),std::min(lo.y,p.y)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y)};}
    return {(lo.x+hi.x)/2,(lo.y+hi.y)/2};
}
}
