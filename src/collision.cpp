// Reads live UE2 collision out of a map package: world BSP, terrain heightmaps,
// static-mesh collision faces and blocking-volume models. Movers are counted
// and left out. A tile with a missing package or an unsupported blocking class
// is not published. Render triangles are never used as a stand-in.
#include "collision.h"
#include "package.h"
#include "map_selection.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <windows.h>

namespace extractor::collision {
namespace {
using Properties=std::vector<Property>;
const Property* prop(const Properties& ps,const std::string& name) {
    const Property* result=nullptr;
    for (const auto& p:ps) if (p.name==name && p.element==0) { if (result) throw std::runtime_error("Duplicate collision property: "+name); result=&p; }
    return result;
}
double scalar(Reader& r) {
    const double n=std::bit_cast<float>(r.word());
    if (!std::isfinite(n) || std::abs(n)>16777216) throw std::runtime_error("Collision coordinate outside bounds"); return n;
}
Point point(Reader& r) { const auto x=scalar(r),y=scalar(r),z=scalar(r); return {x,y,z}; }
size_t count(Reader& r,size_t width=1,size_t maximum=4000000) {
    const auto n=r.compact(); if (n<0 || size_t(n)>maximum || r.offset>r.end || size_t(n)>(r.end-r.offset)/width) throw std::runtime_error("Collision array exceeds bounds at "+std::to_string(r.offset)+" count="+std::to_string(n)+" width="+std::to_string(width)); return size_t(n);
}
int reference(const Properties& ps,const std::string& name) {
    const auto p=prop(ps,name); if (!p) return 0;
    if (p->kind!=5) throw std::runtime_error("Invalid collision object reference: "+name);
    Reader r{p->value}; const auto ref=r.compact(); if (r.offset!=p->value.size()) throw std::runtime_error("Invalid collision reference size"); return ref;
}
bool flag(const Properties& ps,const std::string& name,bool fallback) {
    const auto p=prop(ps,name); if (!p) return fallback;
    if (p->kind!=3) throw std::runtime_error("Invalid collision boolean: "+name); return p->boolean;
}
int integer(const Properties& ps,const std::string& name,int fallback=0) {
    const auto p=prop(ps,name); if (!p) return fallback;
    Reader r{p->value}; if (p->kind==1 && p->value.size()==1) return r.byte();
    if (p->kind!=2 || p->value.size()!=4) throw std::runtime_error("Invalid collision integer: "+name); return int32_t(r.word());
}
Point vector(const Properties& ps,const std::string& name,Point fallback={}) {
    const auto p=prop(ps,name); if (!p) return fallback;
    if (p->structure!="Vector" || p->value.size()!=12) throw std::runtime_error("Invalid collision transform: "+name); Reader r{p->value}; return point(r);
}
bool collapsed(const Properties& ps) {
    if (auto p=prop(ps,"DrawScale")) {
        if (p->kind!=4 || p->value.size()!=4) throw std::runtime_error("Invalid DrawScale");
        Reader r{p->value}; if (scalar(r)==0) return true;
    }
    const auto scale=vector(ps,"DrawScale3D",{1,1,1});
    return scale.x==0 && scale.y==0 && scale.z==0;
}
Cylinder cylinder(const Properties& ps,uint32_t object) {
    auto dimension=[&](const char* name) {
        const auto p=prop(ps,name);if(!p)return 1.;
        if(p->kind!=4 || p->value.size()!=4)throw std::runtime_error(std::string("Invalid cylinder property: ")+name);
        Reader r{p->value};const auto value=scalar(r);
        if(value<0)throw std::runtime_error(std::string("Negative cylinder property: ")+name);
        return value;
    };
    Cylinder result;result.location=vector(ps,"Location");result.radius=dimension("CollisionRadius");result.halfHeight=dimension("CollisionHeight");result.object=object;
    const auto& v=result.location;
    result.bounds={v.x-result.radius,v.y-result.radius,v.x+result.radius,v.y+result.radius};
    for(double value:{result.bounds.minX,result.bounds.minY,result.bounds.maxX,result.bounds.maxY,v.z-result.halfHeight,v.z+result.halfHeight})if(!std::isfinite(value)||std::abs(value)>16777216)throw std::runtime_error("Cylinder world coordinate outside bounds");
    return result;
}
Point normal(Point a,Point b,Point c) { const auto u=b-a,v=c-a; return {u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x}; }
void triangle(std::vector<Triangle>& out,Point a,Point b,Point c) {
    const auto n=normal(a,b,c);
    if (n.x*n.x+n.y*n.y+n.z*n.z<1e-10) return;
    if (out.size()>=4000000) throw std::runtime_error("Native collision triangle budget exceeded"); out.push_back({a,b,c,n});
}
Reader native(const Package& p,size_t i) { size_t offset=0; p.properties(i,&offset); return {p.data,offset,p.exports.at(i).offset+p.exports.at(i).size}; }
struct Object { std::shared_ptr<Package> package; size_t index=0; };
class MissingMeshAsset : public std::runtime_error {
public:
    explicit MissingMeshAsset(const std::string& message):std::runtime_error(message){}
};
class Files {
    std::filesystem::path root;
    std::map<std::wstring,std::vector<std::filesystem::path>> paths;
    std::map<std::filesystem::path,std::shared_ptr<Package>> cache;
    std::map<std::filesystem::path,uint64_t> accesses;
    size_t cacheBytes=0,cacheLimit;
    uint64_t clock=0;
public:
    size_t hits=0,misses=0;
    explicit Files(const std::filesystem::path& map,size_t limit=256ull*1024*1024):cacheLimit(limit) {
        root=std::filesystem::canonical(map).parent_path();
        const auto clientRoot=[](const std::filesystem::path& folder) {
            return std::filesystem::is_directory(folder/"System") || (std::filesystem::is_directory(folder/"Maps") && std::filesystem::is_directory(folder/"StaticMeshes") && std::filesystem::is_directory(folder/"Textures"));
        };
        while (root.has_parent_path() && root!=root.parent_path() && !clientRoot(root)) root=root.parent_path();
        if (!clientRoot(root)) throw std::runtime_error("Cannot find this map's client root (System or Maps/StaticMeshes/Textures folders required)");
        for (const auto& dir:{"StaticMeshes","Textures","System","Maps"}) {
            const auto folder=root/dir; if (!std::filesystem::is_directory(folder)) continue;
            for (const auto& entry:std::filesystem::directory_iterator(folder)) {
                if (entry.is_symlink() || !entry.is_regular_file()) continue;
                const auto ext=folded(entry.path().extension().wstring());
                if (ext==L".usx" || ext==L".utx" || ext==L".u" || ext==L".unr") paths[folded(entry.path().stem().wstring())].push_back(entry.path());
            }
        }
    }
    std::shared_ptr<Package> open(const std::filesystem::path& path) {
        const auto key=std::filesystem::absolute(path).lexically_normal();
        if (auto found=cache.find(key); found!=cache.end()) { accesses[key]=++clock;++hits;return found->second; }
        ++misses;
        auto p=std::make_shared<Package>(Package::open(key));
        const auto cost=p->data.capacity();
        if(cost<=cacheLimit) {
            while(!cache.empty() && cacheBytes>cacheLimit-cost) {
                const auto oldest=std::min_element(accesses.begin(),accesses.end(),[](const auto& a,const auto& b){return a.second<b.second;});
                cacheBytes-=cache.at(oldest->first)->data.capacity();cache.erase(oldest->first);accesses.erase(oldest);
            }
            cache[key]=p;accesses[key]=++clock;cacheBytes+=cost;
        }
        return p;
    }

    Object resolve(const std::shared_ptr<Package>& p,int ref,const std::string& expected) {
        if (!ref) throw std::runtime_error("Missing "+expected+" reference");
        if (ref>0) {
            const auto i=size_t(ref-1); if (p->className(i)!=expected) throw std::runtime_error("Collision reference class mismatch"); return {p,i};
        }
        const auto path=p->objectPath(ref); const auto dot=path.find('.');
        if (dot==std::string::npos) throw std::runtime_error("Invalid external collision reference: "+path);
        const auto key=folded(std::filesystem::path(path.substr(0,dot)).wstring());
        const auto found=paths.find(key);
        if (found==paths.end()) {
            const auto message="Missing client package: "+path.substr(0,dot);
            if(expected=="Engine.StaticMesh")throw MissingMeshAsset(message);
            throw std::runtime_error(message);
        }
        const auto target=folded(std::filesystem::path(path.substr(dot+1)).wstring());
        Object result; unsigned matches=0;
        auto candidates=found->second;
        const auto preferred=expected=="Engine.StaticMesh" ? L".usx" : expected=="Engine.Texture" ? L".utx" : L"";
        std::stable_sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) { return (folded(a.extension().wstring())==preferred)>(folded(b.extension().wstring())==preferred); });
        bool preferredMatch=false;
        for (const auto& file:candidates) {
            const bool primary=folded(file.extension().wstring())==preferred;
            if (preferredMatch && !primary) break;
            const auto package=open(file);
            for (size_t i=0;i<package->exports.size();++i) if (package->className(i)==expected && folded(std::filesystem::path(package->objectPath(int(i+1))).wstring())==target) { result={package,i}; ++matches; preferredMatch|=primary; }
        }
        if (matches!=1) {
            std::string candidateNames;
            for (const auto& file:found->second) { if (!candidateNames.empty()) candidateNames+=", "; candidateNames+=file.filename().string(); }
            const auto message=(matches ? "Ambiguous client object: " : "Missing client object: ")+path+" ("+std::to_string(matches)+" matching "+expected+" exports; packages: "+candidateNames+")";
            if(!matches && expected=="Engine.StaticMesh")throw MissingMeshAsset(message);
            throw std::runtime_error(message);
        }
        return result;
    }
};
class DegenerateCollisionTransform : public std::runtime_error {
public:
    DegenerateCollisionTransform():std::runtime_error("Draw scale collapses a collision axis; the transform is not invertible"){}
};
struct Transform {
    Point location,pivot,scale,rotation; double uniform=1;
    explicit Transform(const Properties& ps):location(vector(ps,"Location")),pivot(vector(ps,"PrePivot")),scale(vector(ps,"DrawScale3D",{1,1,1})) {
        if (auto p=prop(ps,"DrawScale")) { Reader r{p->value}; if (p->kind!=4 || p->value.size()!=4) throw std::runtime_error("Invalid DrawScale"); uniform=scalar(r); }
        if (auto p=prop(ps,"Rotation")) { if (p->structure!="Rotator" || p->value.size()!=12) throw std::runtime_error("Invalid Rotation"); Reader r{p->value}; rotation={double(int32_t(r.word())),double(int32_t(r.word())),double(int32_t(r.word()))}; }
        if (!uniform || !scale.x || !scale.y || !scale.z) throw DegenerateCollisionTransform();
    }
    Point apply(Point p) const {
        p={(p.x-pivot.x)*scale.x*uniform,(p.y-pivot.y)*scale.y*uniform,(p.z-pivot.z)*scale.z*uniform};
        auto angle=[](double n) { return double((uint32_t(int32_t(n))>>2)&16383)*6.283185307179586/16384.; };
        const auto cp=std::cos(angle(rotation.x)),sp=std::sin(angle(rotation.x)),cy=std::cos(angle(rotation.y)),sy=std::sin(angle(rotation.y)),cr=std::cos(angle(rotation.z)),sr=std::sin(angle(rotation.z));
        Point out{float(location.x+p.x*cp*cy+p.y*(cy*sp*sr-cr*sy)-p.z*(cr*cy*sp+sr*sy)),float(location.y+p.x*cp*sy+p.y*(cr*cy+sp*sr*sy)+p.z*(cy*sr-cr*sp*sy)),float(location.z+p.x*sp-p.y*cp*sr+p.z*cp*cr)};
        for (double n:{out.x,out.y,out.z}) if (!std::isfinite(n) || std::abs(n)>16777216) throw std::runtime_error("World collision coordinate outside bounds"); return out;
    }
};
// BSP model. Vectors are skipped. Points, nodes, surface flags and the vertex pool are read.
// NotSolid (0x8) and Portal (0x04000000) faces are dropped. Winding follows the node plane.
std::vector<Triangle> model(const Object& object) {
    const auto& p=*object.package; auto r=native(p,object.index); r.skip(41);
    auto n=count(r,12); r.skip(n*12); n=count(r,12); std::vector<Point> points;
    for (size_t i=0;i<n;++i) points.push_back(point(r));
    struct Node { int first=0,surf=0; unsigned vertices=0; Point normal; };
    std::vector<Node> nodes; n=count(r,64,2000000);
    for (size_t i=0;i<n;++i) {
        Node node; node.normal=point(r); r.skip(13); node.first=r.compact(); node.surf=r.compact();
        for (int j=0;j<5;++j) r.compact(); r.skip(34); node.vertices=r.byte(); r.skip(20); nodes.push_back(node);
    }
    std::vector<unsigned> flags; n=count(r,20);
    for (size_t i=0;i<n;++i) { r.compact(); flags.push_back(r.word()); for (int j=0;j<6;++j) r.compact(); r.skip(20+(p.version>=123 && p.licensee>=23 ? 4 : 0)); }
    std::vector<int> pool; n=count(r,2); for (size_t i=0;i<n;++i) { pool.push_back(r.compact()); r.compact(); }
    std::vector<Triangle> result;
    for (const auto& node:nodes) {
        if (node.vertices<3) continue;
        if (node.surf<0 || size_t(node.surf)>=flags.size()) throw std::runtime_error("Invalid BSP surface");
        if (flags[node.surf]&(0x8u|0x04000000u)) continue;
        if (node.first<0 || size_t(node.first)>pool.size() || node.vertices>pool.size()-size_t(node.first)) throw std::runtime_error("Invalid BSP face span");
        auto vertex=[&](unsigned i) { const auto index=pool[size_t(node.first)+i]; if (index<0 || size_t(index)>=points.size()) throw std::runtime_error("Invalid BSP point"); return points[index]; };
        for (unsigned i=1;i+1<node.vertices;++i) {
            auto a=vertex(0),b=vertex(i),c=vertex(i+1); const auto norm=normal(a,b,c);
            if (norm.x*node.normal.x+norm.y*node.normal.y+norm.z*node.normal.z<0) std::swap(b,c);
            triangle(result,a,b,c);
        }
    }
    return result;
}
// Static-mesh collision faces. UseSimpleBoxCollision with a collision Model uses that model.
// A material slot contributes only when EnableCollision is set.
std::vector<Triangle> mesh(Files& files,const Object& object) {
    const auto& p=*object.package; auto ps=p.properties(object.index); auto r=native(p,object.index);
    std::vector<bool> collides;
    if (const auto materials=prop(ps,"Materials")) {
        if (materials->kind!=9) throw std::runtime_error("Invalid mesh material array");
        Reader fields{materials->value,0,materials->value.size()}; const auto slots=count(fields,1,65536);
        for (size_t i=0;i<slots;++i) collides.push_back(flag(p.tags(fields),"EnableCollision",false));
        if (fields.offset!=fields.end) throw std::runtime_error("Mesh material tag boundary mismatch");
    }
    r.skip(41); const auto sections=count(r,14); r.skip(sections*14+25);
    const auto n=count(r,24); std::vector<Point> vertices;
    for (size_t i=0;i<n;++i) { vertices.push_back(point(r)); r.skip(12); } r.skip(4);
    for (int i=0;i<2;++i) { const auto colors=count(r,4); r.skip(colors*4+4); }
    const auto uvs=count(r,1,16);
    for (size_t i=0;i<uvs;++i) { const auto entries=count(r,8); r.skip(entries*8+8); }
    for (int i=0;i<2;++i) { const auto indices=count(r,2,16000000); r.skip(indices*2+4); }
    const int collisionModel=r.compact();
    if (collisionModel && flag(ps,"UseSimpleBoxCollision",true)) return model(files.resolve(object.package,collisionModel,"Engine.Model"));
    Reader probe=r; bool spans=true;
    try { for (int i=0;i<2;++i) { const auto end=probe.word(); count(probe); if (end<probe.offset || end>probe.end) throw std::runtime_error("span"); probe.offset=end; } } catch (const std::exception&) { spans=false; }
    size_t boundary=r.end; if (spans) { boundary=r.word(); if (boundary<r.offset || boundary>r.end) throw std::runtime_error("Invalid native collision span"); }
    const auto faces=count(r,68,2000000); std::vector<Triangle> result;
    for (size_t i=0;i<faces;++i) {
        const auto faceNormal=point(r); r.skip(52); std::array<int,3> indices; for (auto& index:indices) index=r.compact(); const auto slot=r.compact();
        for (const auto index:indices) if (index<0 || size_t(index)>=vertices.size()) throw std::runtime_error("Invalid native collision vertex");
        if (slot<0 || size_t(slot)>=collides.size()) throw std::runtime_error("Native collision material outside slots");
        if (!collides[slot]) continue;
        auto a=vertices[indices[0]],b=vertices[indices[1]],c=vertices[indices[2]]; const auto norm=normal(a,b,c);
        if (norm.x*faceNormal.x+norm.y*faceNormal.y+norm.z*faceNormal.z<0) std::swap(b,c);
        triangle(result,a,b,c);
    }
    if (spans && r.offset!=boundary) throw std::runtime_error("Native collision triangle boundary mismatch");
    return result;
}
bool closedMesh(const std::vector<Triangle>& faces) {
    using Key=std::array<double,3>;
    std::map<std::pair<Key,Key>,std::pair<unsigned,int>> edges;
    double volume=0;
    for (const auto& t:faces) {
        const std::array<Key,3> p{{{t.a.x,t.a.y,t.a.z},{t.b.x,t.b.y,t.b.z},{t.c.x,t.c.y,t.c.z}}};
        for (size_t i=0;i<3;++i) {
            auto a=p[i],b=p[(i+1)%3]; if (a==b) return false;
            const int direction=a<b ? 1 : -1; if (b<a) std::swap(a,b);
            auto& edge=edges[{a,b}]; if (++edge.first>2) return false; edge.second+=direction;
        }
        volume+=t.a.x*(t.b.y*t.c.z-t.b.z*t.c.y)+t.a.y*(t.b.z*t.c.x-t.b.x*t.c.z)+t.a.z*(t.b.x*t.c.y-t.b.y*t.c.x);
    }
    return volume>1e-6 && !edges.empty() && std::all_of(edges.begin(),edges.end(),[](const auto& edge) { return edge.second.first==2 && edge.second.second==0; });
}

std::vector<uint32_t> bitmap(const Properties& ps,const std::string& name,size_t bits,uint32_t fallback) {
    const auto p=prop(ps,name); if (!p) return std::vector<uint32_t>((bits+31)/32,fallback);
    if (p->kind!=9) throw std::runtime_error("Invalid terrain bitmap: "+name);
    Reader r{p->value,0,p->value.size()}; const auto n=count(r,4);
    if (n<(bits+31)/32) throw std::runtime_error("Truncated terrain bitmap: "+name);
    std::vector<uint32_t> out; for (size_t i=0;i<n;++i) out.push_back(r.word());
    if (r.offset!=r.end) throw std::runtime_error("Unexpected terrain bitmap trailer"); return out;
}
void text(Reader& r) {
    const auto n=int64_t(r.compact()); const auto characters=size_t(n<0 ? -n : n);
    if (characters>1048576) throw std::runtime_error("Material string exceeds limits"); r.skip(characters*(n<0 ? 2 : 1));
}
int worldModel(const Package& p,std::set<size_t>& actors) {
    int modelRef=0; unsigned levels=0;
    for (size_t i=0;i<p.exports.size();++i) if (p.className(i)=="Engine.Level") {
        ++levels; auto r=native(p,i);
        const int databases=p.licensee>=23 ? 2 : 1;
        for (int database=0;database<databases;++database) {
            const auto n=r.word(),capacity=r.word();
            if (n>1000000 || capacity<n) throw std::runtime_error("Invalid native level actor database");
            for (unsigned j=0;j<n;++j) {
                const auto ref=r.compact();
                if (database==databases-1 && ref) {
                    if (ref<0 || size_t(ref)>p.exports.size()) throw std::runtime_error("Invalid native actor database reference");
                    actors.insert(size_t(ref-1));
                }
            }
        }
        for (int j=0;j<4;++j) text(r);
        const auto options=count(r,1,65536); for (size_t j=0;j<options;++j) text(r);
        r.skip(8); modelRef=r.compact();
    }
    if (levels!=1 || modelRef<=0 || p.className(size_t(modelRef-1))!="Engine.Model") throw std::runtime_error("Missing or ambiguous native level BSP model");
    return modelRef;
}
void materialPrefix(const Package& p,Reader& r) {
    if (p.licensee<16) return;
    if (p.licensee<37) {
        r.skip(4);
        if (p.licensee>=30) {
            r.skip(38+8*(p.licensee<36 ? 128 : 127)+(p.licensee>=33 && p.licensee<36 ? 1 : 0));
            for (int i=0;i<17;++i) text(r);
        }
        if (p.licensee>=31) r.skip(4);
    } else {
        r.skip(p.version<129 ? 36 : 92); const auto stages=count(r,1,4096);
        for (size_t i=0;i<stages;++i) { text(r); const auto refs=count(r,1,4096); for (size_t j=0;j<refs;++j) text(r); }
        text(r); r.skip(4);
    }
}
// TerrainInfo heightmap. G16 (format 10) or P8 scaled by 256, ToWorld axes,
// QuadVisibilityBitmap and EdgeTurnBitmap. Invisible quads are omitted. Inverted flips winding.
std::vector<Triangle> terrainGrid(unsigned width,unsigned height,Point origin,Point xAxis,Point yAxis,Point zAxis,const std::vector<uint16_t>& heights,const std::vector<uint32_t>& visible,const std::vector<uint32_t>& turned,bool inverted,unsigned quadsX=0,unsigned quadsY=0) {
    if (heights.size()!=size_t(width)*height) throw std::runtime_error("Terrain sample count mismatch");
    if (!quadsX) quadsX=width-1;
    if (!quadsY) quadsY=height-1;
    if (quadsX>width || quadsY>height) throw std::runtime_error("Terrain quad extent exceeds height texture");
    const unsigned vertexWidth=quadsX+1,vertexHeight=quadsY+1;
    std::vector<Point> vertices; vertices.reserve(size_t(vertexWidth)*vertexHeight);
    for (unsigned y=0;y<vertexHeight;++y) for (unsigned x=0;x<vertexWidth;++x) {
        const double z=heights[size_t(std::min(y,height-1))*width+std::min(x,width-1)];
        const Point q{double(x)-origin.x,double(y)-origin.y,z-origin.z};
        const Point world{float(q.x*xAxis.x+q.y*xAxis.y+q.z*xAxis.z),float(q.x*yAxis.x+q.y*yAxis.y+q.z*yAxis.z),float(q.x*zAxis.x+q.y*zAxis.y+q.z*zAxis.z)};
        for (const double n:{world.x,world.y,world.z}) if (!std::isfinite(n) || std::abs(n)>16777216) throw std::runtime_error("Terrain world coordinate outside bounds");
        vertices.push_back(world);
    }
    std::vector<Triangle> out;
    auto emit=[&](size_t a,size_t b,size_t c) { if (inverted) std::swap(b,c); triangle(out,vertices[a],vertices[b],vertices[c]); };
    for (unsigned y=0;y<quadsY;++y) for (unsigned x=0;x<quadsX;++x) {
        const auto a=size_t(y)*vertexWidth+x,b=a+1,d=a+vertexWidth,c=d+1;
        const auto bit=size_t(y)*width+x;
        if (!(visible[bit/32]&(1u<<(bit%32)))) continue;
        if (turned[bit/32]&(1u<<(bit%32))) { emit(a,b,d); emit(b,c,d); }
        else { emit(a,b,c); emit(a,c,d); }
    }
    return out;
}
struct TerrainData {
    unsigned width=0,height=0,quadsX=0,quadsY=0;
    Point origin,xAxis,yAxis,zAxis;
    std::vector<uint16_t> heights;
    std::vector<uint32_t> visible,turned;
    bool inverted=false;
    Point world(unsigned x,unsigned y) const {
        if(heights.empty()) {
            const Point result{float((double(x)-origin.x)*xAxis.x+(double(y)-origin.y)*xAxis.y),float((double(x)-origin.x)*yAxis.x+(double(y)-origin.y)*yAxis.y),0};
            for(double value:{result.x,result.y})if(!std::isfinite(value) || std::abs(value)>16777216)throw std::runtime_error("Terrain footprint outside world bounds");
            return result;
        }
        const double z=heights.at(size_t(std::min(y,height-1))*width+std::min(x,width-1));
        const Point q{double(x)-origin.x,double(y)-origin.y,z-origin.z};
        const Point result{float(q.x*xAxis.x+q.y*xAxis.y+q.z*xAxis.z),float(q.x*yAxis.x+q.y*yAxis.y+q.z*yAxis.z),float(q.x*zAxis.x+q.y*zAxis.y+q.z*zAxis.z)};
        for(double value:{result.x,result.y,result.z})if(!std::isfinite(value) || std::abs(value)>16777216)throw std::runtime_error("Terrain world coordinate outside bounds");
        return result;
    }
    bool sample(double wx,double wy,Point& out) const {
        if(heights.empty())return false;
        if (std::abs(xAxis.z)>1e-9 || std::abs(yAxis.z)>1e-9) return false;
        const double determinant=xAxis.x*yAxis.y-xAxis.y*yAxis.x;
        if (std::abs(determinant)<1e-9) return false;
        const auto base=world(0,0);
        const double dx=wx-base.x,dy=wy-base.y;
        const double x=(dx*yAxis.y-dy*xAxis.y)/determinant,y=(dy*xAxis.x-dx*yAxis.x)/determinant;
        const double ix=std::round(x),iy=std::round(y);
        if (ix<0 || iy<0 || ix>=width || iy>=height || std::abs(x-ix)>1e-4 || std::abs(y-iy)>1e-4) return false;
        out=world(unsigned(ix),unsigned(iy));
        return std::abs(out.x-wx)<0.02 && std::abs(out.y-wy)<0.02;
    }
    Bounds bounds() const {
        const std::array<Point,4> corners{world(0,0),world(quadsX,0),world(0,quadsY),world(quadsX,quadsY)};
        Bounds result{corners[0].x,corners[0].y,corners[0].x,corners[0].y};
        for (const auto& v:corners) { result.minX=std::min(result.minX,v.x); result.minY=std::min(result.minY,v.y); result.maxX=std::max(result.maxX,v.x); result.maxY=std::max(result.maxY,v.y); }
        return result;
    }
};
TerrainData terrainData(Files& files,const Object& object) {
    const auto& p=*object.package; const auto ps=p.properties(object.index); auto r=native(p,object.index);
    const auto sectors=count(r); for (size_t i=0;i<sectors;++i) r.compact();
    if (p.licensee<18) { const auto n=count(r,12); r.skip(n*12); }
    const auto sectorsX=r.word(),sectorsY=r.word();
    if (p.licensee<18) { const auto n=count(r,24); r.skip(n*24); }
    const auto origin=point(r),xAxis=point(r),yAxis=point(r),zAxis=point(r); r.skip(48);
    const auto width=r.word(),height=r.word();
    if (width<2 || height<2 || width>4096 || height>4096) throw std::runtime_error("Invalid native terrain dimensions");
    const size_t samples=size_t(width)*height;
    const auto colors=count(r,4); if (colors!=samples) throw std::runtime_error("Native terrain dimension/color mismatch"); r.skip(colors*4);
    unsigned quadsX=width-1,quadsY=height-1;
    const int sectorSize=integer(ps,"TerrainSectorSize",16);
    if (sectorSize<1 || sectorSize>256) throw std::runtime_error("Invalid terrain sector size");
    if (uint64_t(sectorsX)*sectorsY==sectors && uint64_t(sectorsX)*sectorSize==width && uint64_t(sectorsY)*sectorSize==height) { quadsX=width; quadsY=height; }
    const size_t quadBits=size_t(quadsY-1)*width+quadsX;
    const auto visible=bitmap(ps,"QuadVisibilityBitmap",quadBits,0xffffffff),turned=bitmap(ps,"EdgeTurnBitmap",quadBits,0);
    if(!reference(ps,"TerrainMap"))return {width,height,quadsX,quadsY,origin,xAxis,yAxis,zAxis,{},visible,turned,flag(ps,"Inverted",false)};
    const auto texture=files.resolve(object.package,reference(ps,"TerrainMap"),"Engine.Texture");
    const auto textureProps=texture.package->properties(texture.index);
    const int format=integer(textureProps,"Format"); if (format!=0 && format!=10) throw std::runtime_error("Unsupported native terrain height texture format");
    auto pixels=native(*texture.package,texture.index); materialPrefix(*texture.package,pixels); const auto mips=count(pixels,1,32);
    if (!mips) throw std::runtime_error("Terrain heightmap has no native mip");
    const auto end=pixels.word(); const auto bytes=count(pixels); const auto start=pixels.offset;
    if (end!=start+bytes || end>pixels.end || bytes!=samples*(format==10 ? 2 : 1)) throw std::runtime_error("Terrain heightmap mip boundary/size mismatch");
    pixels.skip(bytes); if (pixels.word()!=width || pixels.word()!=height) throw std::runtime_error("Terrain heightmap dimensions mismatch"); pixels.skip(2);
    std::vector<uint16_t> heights; heights.reserve(samples); Reader pixelsReader{texture.package->data,start,end};
    for (size_t i=0;i<samples;++i) heights.push_back(uint16_t(format==10 ? pixelsReader.word(2) : pixelsReader.byte()*256));
    return {width,height,quadsX,quadsY,origin,xAxis,yAxis,zAxis,std::move(heights),visible,turned,flag(ps,"Inverted",false)};
}
std::vector<Triangle> terrainFaces(const TerrainData& data,const std::vector<TerrainData>& neighbours,std::array<size_t,3>& fallback,std::array<size_t,3>& stitched) {
    if(data.heights.empty())return {};
    const auto stride=data.quadsX+1;
    std::vector<Point> vertices; vertices.reserve(size_t(stride)*(data.quadsY+1));
    for (unsigned y=0;y<=data.quadsY;++y) for (unsigned x=0;x<=data.quadsX;++x) {
        auto v=data.world(x,y);
        if (x>=data.width || y>=data.height) {
            const size_t edge=x>=data.width ? (y>=data.height ? 2 : 0) : 1;
            bool found=false;
            for (const auto& other:neighbours) {
                Point sample;
                if (!other.sample(v.x,v.y,sample)) continue;
                if (found && std::abs(v.z-sample.z)>0.02) throw std::runtime_error("Conflicting neighbouring terrain samples");
                v.z=sample.z; found=true;
            }
            if (found) ++stitched[edge]; else ++fallback[edge];
        }
        vertices.push_back(v);
    }
    std::vector<Triangle> out;
    auto emit=[&](size_t a,size_t b,size_t c) { if(data.inverted)std::swap(b,c); triangle(out,vertices[a],vertices[b],vertices[c]); };
    for(unsigned y=0;y<data.quadsY;++y)for(unsigned x=0;x<data.quadsX;++x) {
        const auto bit=size_t(y)*data.width+x;
        if (!(data.visible.at(bit/32)&(1u<<(bit%32)))) continue;
        const auto a=size_t(y)*stride+x,b=a+1,d=a+stride,c=d+1;
        if(data.turned.at(bit/32)&(1u<<(bit%32))) {emit(a,b,d);emit(b,c,d);} else {emit(a,b,c);emit(a,c,d);}
    }
    return out;
}
bool contains(const Bounds& b,Point v) { return v.x>=b.minX && v.y>=b.minY && v.x<b.maxX && v.y<b.maxY; }
std::vector<Triangle> clipTerrain(const Triangle& t,const std::vector<Bounds>& domains) {
    std::vector<Triangle> result;
    for(const auto& b:domains) {
        if(std::max({t.a.x,t.b.x,t.c.x})<b.minX || std::min({t.a.x,t.b.x,t.c.x})>=b.maxX || std::max({t.a.y,t.b.y,t.c.y})<b.minY || std::min({t.a.y,t.b.y,t.c.y})>=b.maxY)continue;
        if(contains(b,t.a) && contains(b,t.b) && contains(b,t.c)) {result.push_back(t);continue;}
        std::vector<Point> polygon{t.a,t.b,t.c};
        for(unsigned plane=0;plane<4 && !polygon.empty();++plane) {
            const bool axis=plane>=2,lower=(plane%2)==0;
            const double limit=axis ? (lower ? b.minY : b.maxY) : (lower ? b.minX : b.maxX);
            auto coord=[&](Point v){return axis ? v.y : v.x;};
            auto inside=[&](Point v){return lower ? coord(v)>=limit : coord(v)<=limit;};
            std::vector<Point> next; auto previous=polygon.back(); bool wasInside=inside(previous);
            for(auto v:polygon) {
                const bool isInside=inside(v);
                if(wasInside!=isInside) {
                    const double f=(limit-coord(previous))/(coord(v)-coord(previous));
                    next.push_back({previous.x+(v.x-previous.x)*f,previous.y+(v.y-previous.y)*f,previous.z+(v.z-previous.z)*f});
                }
                if(isInside)next.push_back(v); previous=v;wasInside=isInside;
            }
            polygon=std::move(next);
        }
        for(size_t i=1;i+1<polygon.size();++i) {
            const auto before=result.size(); triangle(result,polygon[0],polygon[i],polygon[i+1]);
            if(result.size()!=before){result.back().kind=t.kind;result.back().object=t.object;result.back().closed=t.closed;}
        }
    }
    return result;
}
std::vector<TerrainData> mapTerrains(Files& files,const std::shared_ptr<Package>& p) {
    std::set<size_t> actors;worldModel(*p,actors);std::vector<TerrainData> result;
    for(auto i:actors)if(p->className(i)=="Engine.TerrainInfo") {
        const auto ps=p->properties(i);
        if(!flag(ps,"bDeleteMe",false) && flag(ps,"bCollideActors",true) && flag(ps,"bBlockPlayers",true))result.push_back(terrainData(files,{p,i}));
    }
    return result;
}
struct TerrainCache {
    struct Entry { std::vector<TerrainData> data;size_t bytes=0;uint64_t used=0; };
    std::map<std::filesystem::path,Entry> entries;
    size_t bytes=0,hits=0;
    uint64_t clock=0;
    static constexpr size_t limit=64ull*1024*1024;
};
std::vector<TerrainData> cachedTerrains(Files& files,const std::filesystem::path& path,const std::shared_ptr<Package>& p,TerrainCache* cache) {
    if(!cache)return mapTerrains(files,p ? p : files.open(path));
    const auto key=std::filesystem::absolute(path).lexically_normal();
    if(auto it=cache->entries.find(key);it!=cache->entries.end()){it->second.used=++cache->clock;++cache->hits;return it->second.data;}
    auto data=mapTerrains(files,p ? p : files.open(path));size_t cost=sizeof(TerrainData)*data.capacity();
    for(const auto& t:data)cost+=t.heights.capacity()*sizeof(uint16_t)+(t.visible.capacity()+t.turned.capacity())*sizeof(uint32_t);
    if(cost<=cache->limit) {
        while(!cache->entries.empty() && cache->bytes>cache->limit-cost) {
            const auto oldest=std::min_element(cache->entries.begin(),cache->entries.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            cache->bytes-=oldest->second.bytes;cache->entries.erase(oldest);
        }
        cache->entries.emplace(key,TerrainCache::Entry{data,cost,++cache->clock});cache->bytes+=cost;
    }
    return data;
}
std::vector<TerrainData> adjacentTerrains(Files& files,const std::filesystem::path& path,Scene& scene,TerrainCache* cache=nullptr) {
    std::vector<TerrainData> result;
    if(!regionMap(path))return result;
    const auto stem=path.stem().wstring();const int x=std::stoi(stem.substr(0,2)),y=std::stoi(stem.substr(3,2));
    std::map<std::wstring,std::filesystem::path> available;
    for(const auto& entry:std::filesystem::directory_iterator(path.parent_path()))if(!entry.is_symlink() && entry.is_regular_file() && folded(entry.path().extension().wstring())==L".unr")available.emplace(folded(entry.path().stem().wstring()),entry.path());
    auto digits=[](int n){return (n<10 ? L"0" : L"")+std::to_wstring(n);};
    for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy) {
        if((!dx && !dy) || x+dx<0 || y+dy<0)continue;
        const auto key=digits(x+dx)+L"_"+digits(y+dy)+(classicMap(path) ? L"_classic" : L"");
        const auto it=available.find(key);if(it==available.end())continue;
        try {
            auto terrains=cachedTerrains(files,it->second,{},cache);
            if(terrains.empty())throw std::runtime_error("No active TerrainInfo");
            result.insert(result.end(),std::make_move_iterator(terrains.begin()),std::make_move_iterator(terrains.end()));
        }catch(const std::exception& e){scene.warnings.push_back("Cannot use neighbouring terrain "+output::utf8(it->second.filename())+": "+e.what()+". Missing edge samples will use flat fallback.");}
    }
    return result;
}

}
bool cylinderHits(const Cylinder& cylinder,const Bounds& area) {
    if(cylinder.radius<=0 || cylinder.halfHeight<=0)return false;
    const double minX=std::max(area.minX,cylinder.bounds.minX),minY=std::max(area.minY,cylinder.bounds.minY),maxX=std::min(area.maxX,cylinder.bounds.maxX),maxY=std::min(area.maxY,cylinder.bounds.maxY);
    if(minX>=maxX || minY>=maxY)return false;
    const double dx=cylinder.location.x-std::clamp(cylinder.location.x,minX,maxX),dy=cylinder.location.y-std::clamp(cylinder.location.y,minY,maxY);
    return dx*dx+dy*dy<cylinder.radius*cylinder.radius;
}
std::vector<size_t> liveActors(const Package& package) {
    std::set<size_t> actors; worldModel(package,actors); return {actors.begin(),actors.end()};
}
int inspect(const std::filesystem::path& path) {
    Files files(path); auto p=files.open(path);
    for (size_t i=0;i<p->exports.size();++i) if (p->className(i)=="Engine.StaticMeshActor") {
        const auto ps=p->properties(i); const auto ref=reference(ps,"StaticMesh"); if (!ref) continue;
        auto object=files.resolve(p,ref,"Engine.StaticMesh"); auto r=native(*object.package,object.index);
        std::cout << "MESH " << p->objectPath(ref) << " native=" << r.offset << " end=" << r.end << '\n';
        for (int j=0;j<100;++j) std::cout << int(r.byte()) << ' '; std::cout << '\n'; break;
    }
    for (size_t i=0;i<p->exports.size();++i) {
        const auto type=p->className(i);
        if (type!="Engine.TerrainInfo" && type!="Engine.Model" && type!="Engine.Level" && type!="Engine.StaticMesh") continue;
        std::cout<<i+1<<' '<<p->objectPath(int(i+1))<<' '<<type<<" bytes="<<p->exports[i].size<<'\n';
        auto ps=p->properties(i); for (const auto& v:ps) std::cout<<"  "<<v.name<<" kind="<<int(v.kind)<<" bytes="<<v.value.size()<<'\n';
    }
    return 0;
}
// Live Level actors only. World BSP is kind 3, terrain kind 1, blocking volumes kind 2, static meshes kind 0.
// An unrecognized actor that still blocks players fails the tile instead of being skipped.
Scene readScene(Files& files,const std::filesystem::path& path,TerrainCache* cache=nullptr) {
    auto p=files.open(path); Scene scene;
    std::set<size_t> actors; const auto world=worldModel(*p,actors);
    std::vector<TerrainData> terrains;
    try {
        terrains=cachedTerrains(files,path,p,cache);
        if(terrains.empty())throw std::runtime_error("No active TerrainInfo; cannot establish extraction bounds");
        for(const auto& terrain:terrains) {
            if(std::abs(terrain.xAxis.z)>1e-9 || std::abs(terrain.yAxis.z)>1e-9 || std::abs(terrain.xAxis.y)>1e-9 || std::abs(terrain.yAxis.x)>1e-9)throw std::runtime_error("Unsupported non-axis-aligned TerrainInfo footprint");
            const auto bounds=terrain.bounds();
            if(bounds.maxX<=bounds.minX || bounds.maxY<=bounds.minY)throw std::runtime_error("Empty TerrainInfo footprint");
            scene.bounds.push_back(bounds);
        }
    }catch(const std::exception& e){scene.errors.push_back(e.what());return scene;}
    auto neighbours=adjacentTerrains(files,path,scene,cache);
    neighbours.insert(neighbours.end(),terrains.begin(),terrains.end());
    std::map<std::string,std::vector<Triangle>> meshCache;
    auto append=[&](const std::vector<Triangle>& triangles,const Transform* matrix,unsigned char kind,uint32_t object=0) {
        for (const auto& t:triangles) {
            auto face=t;face.kind=kind;face.object=object;
            if (matrix) {
                face.a=matrix->apply(t.a);face.b=matrix->apply(t.b);face.c=matrix->apply(t.c);
                if (matrix->uniform*matrix->scale.x*matrix->scale.y*matrix->scale.z<0) std::swap(face.b,face.c);
                face.normal=normal(face.a,face.b,face.c);
            }
            auto clipped=clipTerrain(face,scene.bounds);
            scene.triangles.insert(scene.triangles.end(),clipped.begin(),clipped.end());
            if(scene.triangles.size()>4000000)throw std::runtime_error("Native collision triangle budget exceeded");
        }
    };
    for(const auto& terrain:terrains) {
        try {
            if(terrain.heights.empty())scene.notes.push_back("TerrainInfo has no heightmap; native XY footprint retained without terrain collision.");
            std::array<size_t,3> fallback{},stitched{};
            auto faces=terrainFaces(terrain,neighbours,fallback,stitched);append(faces,nullptr,1);
            const std::array<std::string,3> edgeNames{terrain.xAxis.x>=0 ? "maximum-X edge" : "minimum-X edge",terrain.yAxis.y>=0 ? "maximum-Y edge" : "minimum-Y edge","outer X/Y corner"};
            for(size_t edge=0;edge<3;++edge)if(fallback[edge])scene.notes.push_back(std::string("Terrain ")+edgeNames[edge]+": "+std::to_string(fallback[edge])+" neighbouring height samples unavailable; flat fallback used.");
        }catch(const std::exception& e){scene.errors.push_back(std::string("Terrain stitching: ")+e.what());}
    }
    for (size_t i=0;i<p->exports.size();++i) {
        const auto type=p->className(i);
        try {
            if (type=="Engine.Model" && int(i+1)==world) {
                const auto faces=model({p,i}); append(faces,nullptr,3); scene.bsp+=faces.size(); continue;
            }
            if (!actors.contains(i)) continue;
            if (type=="Engine.PlayerStart") {
                const auto ps=p->properties(i); if (flag(ps,"bDeleteMe",false) || !prop(ps,"Location")) continue;
                const auto location=vector(ps,"Location");
                if(std::any_of(scene.bounds.begin(),scene.bounds.end(),[&](const auto& bounds){return contains(bounds,location);}))scene.seeds.push_back(location); continue;
            }
            if (type=="Engine.TerrainInfo") continue;
            if (type!="Engine.StaticMeshActor" && type!="Engine.SkyMeshActor" && type!="Engine.BlockingVolume" && type!="Engine.L2BlockingVolume") {
                if (type=="Engine.Mover" || type=="Engine.L2NMover" || type=="Engine.MovableStaticMeshActor" || type=="Engine.L2MovableStaticMeshActor") { ++scene.dynamic; continue; }
                const auto props=p->properties(i);
                if (flag(props,"bDeleteMe",false) || collapsed(props)) continue;
                if ((type=="Engine.Light" || type=="Engine.Sunlight" || type=="Engine.Emitter") && !flag(props,"bCollideActors",false)) continue;
                if (reference(props,"StaticMesh") && flag(props,"bCollideActors",true) && flag(props,"bBlockPlayers",true)) {
                    const Transform matrix(props);
                    const auto faces=mesh(files,files.resolve(p,reference(props,"StaticMesh"),"Engine.StaticMesh"));
                    bool overlaps=false;
                    for(auto face:faces) {
                        face.a=matrix.apply(face.a);face.b=matrix.apply(face.b);face.c=matrix.apply(face.c);
                        if(!clipTerrain(face,scene.bounds).empty()){overlaps=true;break;}
                    }
                    if(overlaps)throw std::runtime_error("Unsupported potentially blocking static-mesh actor class inside terrain footprint");
                }
                if (reference(props,"TerrainMap")) throw std::runtime_error("Unsupported terrain actor subclass");
                continue;
            }
            const auto ps=p->properties(i);
            if (flag(ps,"bDeleteMe",false) || !flag(ps,"bCollideActors",true) || !flag(ps,"bBlockPlayers",true)) continue;
            if ((type=="Engine.StaticMeshActor" || type=="Engine.SkyMeshActor") && !flag(ps,"bStatic",type!="Engine.SkyMeshActor")) { ++scene.dynamic; continue; }
            if (collapsed(ps)) continue;
            if ((type=="Engine.StaticMeshActor" || type=="Engine.SkyMeshActor") && flag(ps,"bUseCylinderCollision",false)) {
                auto shape=cylinder(ps,uint32_t(i+1));
                for(const auto& bounds:scene.bounds)if(cylinderHits(shape,bounds)) {
                    auto clipped=shape;clipped.bounds=bounds;scene.cylinders.push_back(clipped);
                    if(scene.cylinders.size()>1000000)throw std::runtime_error("Native cylinder budget exceeded");
                }
                continue;
            }
            const Transform matrix(ps);
            if (type!="Engine.StaticMeshActor" && type!="Engine.SkyMeshActor") {
                const auto faces=model(files.resolve(p,reference(ps,"Brush"),"Engine.Model")); append(faces,&matrix,2); scene.volumes+=faces.size(); continue;
            }
            const int ref=reference(ps,"StaticMesh"); if (!ref) continue;
            const auto key=p->objectPath(ref);
            auto found=meshCache.find(key);
            if (found==meshCache.end()) {
                const auto object=files.resolve(p,ref,"Engine.StaticMesh");
                auto faces=mesh(files,object);
                const bool closed=closedMesh(faces); for (auto& face:faces) face.closed=closed;
                found=meshCache.emplace(key,std::move(faces)).first;
            }
            append(found->second,&matrix,0,uint32_t(i+1)); scene.meshes+=found->second.size();
        } catch(const DegenerateCollisionTransform& error) {
            scene.warnings.push_back("Ignored zero-volume actor "+p->exports[i].name+" ("+type+"): "+error.what()+". This actor contributes no collision.");
        } catch (const MissingMeshAsset& error) {
            scene.warnings.push_back("Ignored mesh "+p->exports[i].name+" ("+type+"): "+error.what()+". This actor contributes no collision.");
        } catch (const std::exception& error) { scene.errors.push_back(p->exports[i].name+" ("+type+"): "+error.what()); }
    }
    scene.terrain=scene.bsp=scene.meshes=scene.volumes=0;
    for(const auto& face:scene.triangles) {
        if(face.kind==1)++scene.terrain;else if(face.kind==2)++scene.volumes;else if(face.kind==3)++scene.bsp;else ++scene.meshes;
    }
    return scene;
}
Scene read(const std::filesystem::path& path) { Files files(path);return readScene(files,path); }
struct Cache::Impl {
    struct Entry { std::shared_ptr<const Scene> scene;size_t bytes;uint64_t used; };
    bool enabled;
    size_t budget,bytes=0,peak=0,hits=0,misses=0,evictions=0;
    uint64_t clock=0;
    std::map<std::filesystem::path,Entry> entries;
    std::filesystem::path folder;
    std::unique_ptr<Files> files;
    TerrainCache terrains;
    Impl(bool active,size_t limit):enabled(active),budget(limit){}
};
Cache::Cache(bool enabled,size_t sceneBudget) {
    if(!sceneBudget) {
        MEMORYSTATUSEX memory{sizeof(memory)};
        sceneBudget=GlobalMemoryStatusEx(&memory) ? size_t(std::clamp<unsigned long long>(memory.ullAvailPhys/4,64ull*1024*1024,2048ull*1024*1024)) : 512ull*1024*1024;
    }
    impl=std::make_unique<Impl>(enabled,sceneBudget);
}
Cache::~Cache()=default;
std::shared_ptr<const Scene> Cache::read(const std::filesystem::path& path) {
    auto& state=*impl;
    if(!state.enabled)return std::make_shared<const Scene>(collision::read(path));
    const auto key=std::filesystem::canonical(path);
    if(auto it=state.entries.find(key);it!=state.entries.end()){it->second.used=++state.clock;++state.hits;return it->second.scene;}
    ++state.misses;
    if(!state.files || state.folder!=key.parent_path()) {
        state.files=std::make_unique<Files>(key,std::clamp<size_t>(state.budget/2,16ull*1024*1024,512ull*1024*1024));state.terrains=TerrainCache{};state.folder=key.parent_path();
    }
    auto scene=std::make_shared<const Scene>(readScene(*state.files,key,&state.terrains));
    const size_t cost=sizeof(Scene)+scene->triangles.capacity()*sizeof(Triangle)+scene->cylinders.capacity()*sizeof(Cylinder)+scene->seeds.capacity()*sizeof(Point)+scene->bounds.capacity()*sizeof(Bounds);
    if(scene->errors.empty() && cost<=state.budget) {
        while(!state.entries.empty() && state.bytes>state.budget-cost) {
            const auto oldest=std::min_element(state.entries.begin(),state.entries.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            state.bytes-=oldest->second.bytes;state.entries.erase(oldest);++state.evictions;
        }
        state.entries.emplace(key,Impl::Entry{scene,cost,++state.clock});state.bytes+=cost;state.peak=std::max(state.peak,state.bytes);
    }
    return scene;
}
std::string Cache::summary() const {
    const auto& s=*impl;
    if(!s.enabled)return "disabled";
    return std::to_string(s.hits)+" collision hits, "+std::to_string(s.misses)+" collision reads, "+std::to_string(s.evictions)+" evictions, "+std::to_string(s.terrains.hits)+" terrain hits, "+std::to_string(s.files ? s.files->hits : 0)+" package hits, peak retained collision "+std::to_string(s.peak/(1024*1024))+" MiB";
}

int verify(const std::filesystem::path& path) {
    auto need=[](bool ok) { if (!ok) throw std::runtime_error("Native collision fixture failed"); };
    auto rejects=[&](auto work) { bool failed=false; try { work(); } catch (const std::exception&) { failed=true; } need(failed); };
    auto word=[](std::vector<uint8_t>& bytes,uint32_t n) { for (int i=0;i<4;++i) bytes.push_back(uint8_t(n>>(i*8))); };
    auto vertex=[&](std::vector<uint8_t>& bytes,float x,float y,float z) { for (float n:{x,y,z}) word(bytes,std::bit_cast<uint32_t>(n)); };
    auto package=std::make_shared<Package>(); package->version=123; package->licensee=0;
    package->names={"None","Materials","EnableCollision"};
    package->imports={{0,0,"Engine"},{0,-1,"Model"},{0,-1,"StaticMesh"}};
    auto modelBytes=[&](unsigned flags) {
        std::vector<uint8_t> bytes(42,0); bytes.push_back(0); bytes.push_back(4);
        vertex(bytes,0,0,0); vertex(bytes,128,0,0); vertex(bytes,128,128,0); vertex(bytes,0,128,0);
        bytes.push_back(1); vertex(bytes,0,0,1); bytes.resize(bytes.size()+13,0); bytes.push_back(0); bytes.push_back(0);
        for (int i=0;i<5;++i) bytes.push_back(0x81); bytes.resize(bytes.size()+34,0); bytes.push_back(4); bytes.resize(bytes.size()+20,0);
        bytes.push_back(1); bytes.push_back(0); word(bytes,flags); bytes.resize(bytes.size()+6+20,0);
        bytes.push_back(4); for (int i=0;i<4;++i) { bytes.push_back(uint8_t(i)); bytes.push_back(0); }
        return bytes;
    };
    package->data=modelBytes(0); package->exports={{-2,0,"World",0,0,package->data.size()}};
    auto faces=model({package,0}); need(faces.size()==2 && faces[0].normal.z>0);
    package->data=modelBytes(8); need(model({package,0}).empty());
    package->data=modelBytes(0); package->data.back()=0x40; rejects([&] { model({package,0}); });
    Files files(path);
    auto meshBytes=[&](bool enabled,int collisionRef) {
        std::vector<uint8_t> bytes{1,0x59,uint8_t(enabled ? 4 : 2),1};
        if (enabled) { bytes.push_back(2); bytes.push_back(0x83); }
        bytes.push_back(0); bytes.push_back(0); bytes.resize(bytes.size()+41,0); bytes.push_back(0); bytes.resize(bytes.size()+25,0);
        bytes.push_back(3); vertex(bytes,0,0,0); vertex(bytes,0,0,1); vertex(bytes,128,0,0); vertex(bytes,0,0,1); vertex(bytes,0,128,0); vertex(bytes,0,0,1); word(bytes,0);
        for (int i=0;i<2;++i) { bytes.push_back(0); word(bytes,0); } bytes.push_back(0);
        for (int i=0;i<2;++i) { bytes.push_back(0); word(bytes,0); }
        bytes.push_back(uint8_t(collisionRef)); bytes.push_back(1); vertex(bytes,0,0,1); bytes.resize(bytes.size()+52,0);
        bytes.push_back(0); bytes.push_back(1); bytes.push_back(2); bytes.push_back(0); bytes.push_back(0);
        return bytes;
    };
    package->data=meshBytes(true,0); package->exports={{-3,0,"Mesh",0,0,package->data.size()}};
    need(mesh(files,{package,0}).size()==1);
    package->data=meshBytes(false,0); package->exports[0].size=package->data.size(); need(mesh(files,{package,0}).empty());
    package->data=meshBytes(true,2); const auto offset=package->data.size(); const auto modelData=modelBytes(0);
    package->data.insert(package->data.end(),modelData.begin(),modelData.end()); package->exports[0].size=offset;
    package->exports.push_back({-2,0,"SimpleCollision",0,offset,modelData.size()}); need(mesh(files,{package,0}).size()==2);
    package->exports[0].size=43; rejects([&] { mesh(files,{package,0}); });
    Properties ps; Property visible; visible.name="QuadVisibilityBitmap"; visible.kind=9; visible.value={1,4,0,0,0}; ps.push_back(visible);
    const auto bits=bitmap(ps,"QuadVisibilityBitmap",32,0); need(bits.size()==1 && bits[0]==4);
    ps[0].value.pop_back(); rejects([&] { bitmap(ps,"QuadVisibilityBitmap",32,0); });
    Property scale; scale.name="DrawScale3D"; scale.kind=10; scale.structure="Vector"; vertex(scale.value,2,3,4);
    const auto transformed=Transform({scale}).apply({1,2,3}); need(transformed.x==2 && transformed.y==6 && transformed.z==12);
    Property zero; zero.name="DrawScale"; zero.kind=4; word(zero.value,0);
    need(collapsed({zero})); need(!collapsed({}));
    scale.value.clear(); vertex(scale.value,0,0,0); need(collapsed({scale}));
    scale.value.clear(); vertex(scale.value,0,1,1); need(!collapsed({scale}));
    rejects([&] { Transform test({scale}); });
    std::cout << "Native collision fixtures passed: BSP flags, stored collision faces, material flags, simple-model precedence, holes, transforms and bounds\n";
    return 0;
}
}
