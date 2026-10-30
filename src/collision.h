#pragma once
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <cstddef>
namespace extractor { struct Package; }
namespace extractor::collision {
struct Point { double x=0,y=0,z=0; Point operator-(Point b) const { return {x-b.x,y-b.y,z-b.z}; } };
// kind: 0 static mesh, 1 terrain, 2 blocking volume, 3 world BSP.
struct Triangle { Point a,b,c,normal; unsigned char kind=0; uint32_t object=0; bool closed=false; };
struct Bounds { double minX=0,minY=0,maxX=0,maxY=0; };
struct Cylinder { Point location;double radius=0,halfHeight=0;Bounds bounds;uint32_t object=0; };
bool cylinderHits(const Cylinder& cylinder,const Bounds& area);
struct Scene {
    std::vector<Cylinder> cylinders;
    std::vector<Bounds> bounds;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
    std::vector<Triangle> triangles;
    std::vector<Point> seeds;
    size_t terrain=0,bsp=0,meshes=0,volumes=0,dynamic=0;
    std::vector<std::string> errors;
};
class Cache {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Cache(bool enabled=true,size_t sceneBudget=0);
    ~Cache();
    std::shared_ptr<const Scene> read(const std::filesystem::path& map);
    std::string summary() const;
};
Scene read(const std::filesystem::path& map);
std::vector<size_t> liveActors(const Package& package);
int inspect(const std::filesystem::path& map);
int verify(const std::filesystem::path& map);
}
