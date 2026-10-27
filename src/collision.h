#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace extractor { struct Package; }
namespace extractor::collision {
struct Point { double x=0,y=0,z=0; Point operator-(Point b) const { return {x-b.x,y-b.y,z-b.z}; } };
// kind: 0 static mesh, 1 terrain, 2 blocking volume, 3 world BSP.
struct Triangle { Point a,b,c,normal; unsigned char kind=0; };
struct Scene {
    std::vector<Triangle> triangles;
    std::vector<Point> seeds;
    size_t terrain=0,bsp=0,meshes=0,volumes=0,dynamic=0;
    std::vector<std::string> errors;
};
Scene read(const std::filesystem::path& map);
std::vector<size_t> liveActors(const Package& package);
int inspect(const std::filesystem::path& map);
int verify(const std::filesystem::path& map);
}
