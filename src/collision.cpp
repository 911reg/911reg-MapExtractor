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
Point normal(Point a,Point b,Point c) { const auto u=b-a,v=c-a; return {u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x}; }
void triangle(std::vector<Triangle>& out,Point a,Point b,Point c) {
    const auto n=normal(a,b,c);
    if (n.x*n.x+n.y*n.y+n.z*n.z<1e-10) return;
    if (out.size()>=4000000) throw std::runtime_error("Native collision triangle budget exceeded"); out.push_back({a,b,c,n});
}
Reader native(const Package& p,size_t i) { size_t offset=0; p.properties(i,&offset); return {p.data,offset,p.exports.at(i).offset+p.exports.at(i).size}; }
struct Object { std::shared_ptr<Package> package; size_t index=0; };
class Files {
    std::filesystem::path root;
    std::map<std::wstring,std::vector<std::filesystem::path>> paths;
    std::map<std::filesystem::path,std::shared_ptr<Package>> cache;
public:
    explicit Files(const std::filesystem::path& map) {
        root=std::filesystem::canonical(map).parent_path();
        while (root.has_parent_path() && root!=root.parent_path() && !std::filesystem::is_directory(root/"System")) root=root.parent_path();
        if (!std::filesystem::is_directory(root/"System")) throw std::runtime_error("Cannot find this map's client root (System folder required)");
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
        if (auto found=cache.find(path); found!=cache.end()) return found->second;
        size_t used=0; for (const auto& [key,p]:cache) used+=p->data.size();
        if (used>256ull*1024*1024) cache.clear();
        auto p=std::make_shared<Package>(Package::open(path)); cache[path]=p; return p;
    }
    Object resolve(const std::shared_ptr<Package>& p,int ref,const std::string& expected) {
        if (!ref) throw std::runtime_error("Missing "+expected+" reference");
        if (ref>0) {
            const auto i=size_t(ref-1); if (p->className(i)!=expected) throw std::runtime_error("Collision reference class mismatch"); return {p,i};
        }
        const auto path=p->objectPath(ref); const auto dot=path.find('.');
        if (dot==std::string::npos) throw std::runtime_error("Invalid external collision reference: "+path);
        const auto key=folded(std::filesystem::path(path.substr(0,dot)).wstring());
        const auto found=paths.find(key); if (found==paths.end()) throw std::runtime_error("Missing client package: "+path.substr(0,dot));
        const auto target=folded(std::filesystem::path(path.substr(dot+1)).wstring());
        Object result; unsigned matches=0;
        for (const auto& file:found->second) {
            const auto package=open(file);
            for (size_t i=0;i<package->exports.size();++i) if (package->className(i)==expected && folded(std::filesystem::path(package->objectPath(int(i+1))).wstring())==target) { result={package,i}; ++matches; }
        }
        if (matches!=1) throw std::runtime_error("Missing or ambiguous client object: "+path); return result;
    }
};
struct Transform {
    Point location,pivot,scale,rotation; double uniform=1;
    explicit Transform(const Properties& ps):location(vector(ps,"Location")),pivot(vector(ps,"PrePivot")),scale(vector(ps,"DrawScale3D",{1,1,1})) {
        if (auto p=prop(ps,"DrawScale")) { Reader r{p->value}; if (p->kind!=4 || p->value.size()!=4) throw std::runtime_error("Invalid DrawScale"); uniform=scalar(r); }
        if (auto p=prop(ps,"Rotation")) { if (p->structure!="Rotator" || p->value.size()!=12) throw std::runtime_error("Invalid Rotation"); Reader r{p->value}; rotation={double(int32_t(r.word())),double(int32_t(r.word())),double(int32_t(r.word()))}; }
        if (!uniform || !scale.x || !scale.y || !scale.z) throw std::runtime_error("Singular collision transform");
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
std::vector<Triangle> terrain(Files& files,const Object& object) {
    const auto& p=*object.package; const auto ps=p.properties(object.index); auto r=native(p,object.index);
    const auto sectors=count(r); for (size_t i=0;i<sectors;++i) r.compact();
    if (p.licensee<18) { const auto n=count(r,12); r.skip(n*12); }
    r.skip(8);
    if (p.licensee<18) { const auto n=count(r,24); r.skip(n*24); }
    const auto origin=point(r),xAxis=point(r),yAxis=point(r),zAxis=point(r); r.skip(48);
    const auto width=r.word(),height=r.word();
    if (width<2 || height<2 || width>4096 || height>4096) throw std::runtime_error("Invalid native terrain dimensions");
    const size_t samples=size_t(width)*height;
    const auto colors=count(r,4); if (colors!=samples) throw std::runtime_error("Native terrain dimension/color mismatch"); r.skip(colors*4);
    const auto texture=files.resolve(object.package,reference(ps,"TerrainMap"),"Engine.Texture");
    const auto textureProps=texture.package->properties(texture.index);
    const int format=integer(textureProps,"Format"); if (format!=0 && format!=10) throw std::runtime_error("Unsupported native terrain height texture format");
    auto pixels=native(*texture.package,texture.index); materialPrefix(*texture.package,pixels); const auto mips=count(pixels,1,32);
    if (!mips) throw std::runtime_error("Terrain heightmap has no native mip");
    const auto end=pixels.word(); const auto bytes=count(pixels); const auto start=pixels.offset;
    if (end!=start+bytes || end>pixels.end || bytes!=samples*(format==10 ? 2 : 1)) throw std::runtime_error("Terrain heightmap mip boundary/size mismatch");
    pixels.skip(bytes); if (pixels.word()!=width || pixels.word()!=height) throw std::runtime_error("Terrain heightmap dimensions mismatch"); pixels.skip(2);
    const size_t quadBits=size_t(height-1)*width-1;
    const auto visible=bitmap(ps,"QuadVisibilityBitmap",quadBits,0xffffffff),turned=bitmap(ps,"EdgeTurnBitmap",quadBits,0);
    std::vector<Point> vertices; vertices.reserve(samples); Reader heights{texture.package->data,start,end};
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const double z=format==10 ? heights.word(2) : heights.byte()*256;
        const Point q{double(x)-origin.x,double(y)-origin.y,z-origin.z};
        vertices.push_back({float(q.x*xAxis.x+q.y*xAxis.y+q.z*xAxis.z),float(q.x*yAxis.x+q.y*yAxis.y+q.z*yAxis.z),float(q.x*zAxis.x+q.y*zAxis.y+q.z*zAxis.z)});
    }
    const bool inverted=flag(ps,"Inverted",false); std::vector<Triangle> out;
    auto emit=[&](size_t a,size_t b,size_t c) { if (inverted) std::swap(b,c); triangle(out,vertices[a],vertices[b],vertices[c]); };
    for (unsigned y=0;y+1<height;++y) for (unsigned x=0;x+1<width;++x) {
        const auto a=size_t(y)*width+x,b=a+1,d=a+width,c=d+1;
        if (!(visible[a/32]&(1u<<(a%32)))) continue;
        if (turned[a/32]&(1u<<(a%32))) { emit(a,b,d); emit(b,c,d); }
        else { emit(a,b,c); emit(a,c,d); }
    }
    return out;
}
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
Scene read(const std::filesystem::path& path) {
    Files files(path); auto p=files.open(path); Scene scene;
    std::set<size_t> actors; const auto world=worldModel(*p,actors);
    std::map<std::string,std::vector<Triangle>> meshCache;
    auto append=[&](const std::vector<Triangle>& triangles,const Transform* matrix,unsigned char kind) {
        const auto start=scene.triangles.size();
        for (const auto& t:triangles) {
            if (matrix) {
                auto a=matrix->apply(t.a),b=matrix->apply(t.b),c=matrix->apply(t.c);
                if (matrix->uniform*matrix->scale.x*matrix->scale.y*matrix->scale.z<0) std::swap(b,c);
                triangle(scene.triangles,a,b,c);
            } else triangle(scene.triangles,t.a,t.b,t.c);
        }
        for (auto i=start;i<scene.triangles.size();++i) scene.triangles[i].kind=kind;
    };
    for (size_t i=0;i<p->exports.size();++i) {
        const auto type=p->className(i);
        try {
            if (type=="Engine.Model" && int(i+1)==world) {
                const auto faces=model({p,i}); append(faces,nullptr,3); scene.bsp+=faces.size(); continue;
            }
            if (!actors.contains(i)) continue;
            if (type=="Engine.PlayerStart") {
                const auto ps=p->properties(i); if (flag(ps,"bDeleteMe",false) || !prop(ps,"Location")) continue;
                scene.seeds.push_back(vector(ps,"Location")); continue;
            }
            if (type=="Engine.TerrainInfo") {
                const auto ps=p->properties(i); if (flag(ps,"bDeleteMe",false) || !flag(ps,"bCollideActors",true) || !flag(ps,"bBlockPlayers",true)) continue;
                const auto faces=terrain(files,{p,i}); append(faces,nullptr,1); scene.terrain+=faces.size(); continue;
            }
            if (type!="Engine.StaticMeshActor" && type!="Engine.BlockingVolume" && type!="Engine.L2BlockingVolume") {
                if (type=="Engine.Mover" || type=="Engine.L2NMover" || type=="Engine.MovableStaticMeshActor" || type=="Engine.L2MovableStaticMeshActor") { ++scene.dynamic; continue; }
                const auto props=p->properties(i);
                if ((type=="Engine.Light" || type=="Engine.Sunlight" || type=="Engine.Emitter") && !flag(props,"bCollideActors",false)) continue;
                if (reference(props,"StaticMesh") && flag(props,"bCollideActors",true) && flag(props,"bBlockPlayers",true)) throw std::runtime_error("Unsupported potentially blocking static-mesh actor class");
                if (reference(props,"TerrainMap")) throw std::runtime_error("Unsupported terrain actor subclass");
                continue;
            }
            const auto ps=p->properties(i);
            if (flag(ps,"bDeleteMe",false) || !flag(ps,"bCollideActors",true) || !flag(ps,"bBlockPlayers",true)) continue;
            if (type=="Engine.StaticMeshActor" && !flag(ps,"bStatic",true)) { ++scene.dynamic; continue; }
            const Transform matrix(ps);
            if (type!="Engine.StaticMeshActor") {
                const auto faces=model(files.resolve(p,reference(ps,"Brush"),"Engine.Model")); append(faces,&matrix,2); scene.volumes+=faces.size(); continue;
            }
            if (flag(ps,"bUseCylinderCollision",false)) throw std::runtime_error("Cylinder override on static mesh is not supported");
            const int ref=reference(ps,"StaticMesh"); if (!ref) continue;
            const auto key=p->objectPath(ref);
            auto found=meshCache.find(key);
            if (found==meshCache.end()) {
                const auto object=files.resolve(p,ref,"Engine.StaticMesh");
                auto faces=mesh(files,object);
                found=meshCache.emplace(key,std::move(faces)).first;
            }
            append(found->second,&matrix,0); scene.meshes+=found->second.size();
        } catch (const std::exception& error) { scene.errors.push_back(p->exports[i].name+" ("+type+"): "+error.what()); }
    }
    return scene;
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
    std::cout << "Native collision fixtures passed: BSP flags, stored collision faces, material flags, simple-model precedence, holes, transforms and bounds\n";
    return 0;
}
}
