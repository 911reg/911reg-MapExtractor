#pragma once
#include <filesystem>
#include "collision.h"
#include <optional>
namespace extractor {
struct WaterPlane {collision::Point normal;double distance=0;};
struct WaterNode {WaterPlane plane;int32_t children[2]{-1,-1};bool solid=false;};
struct WaterGeometry {
    std::vector<collision::Point> polygon;double low=0,high=0;
    std::vector<WaterNode> nodes;
    bool rootOutside=true;
    collision::Bounds bounds;
    std::optional<int> priority;
    bool wet=true;
};
bool waterContains(const WaterGeometry& zone,double x,double y);
bool waterAt(const std::vector<WaterGeometry>& zones,double x,double y,double z);
std::vector<WaterGeometry> readWaterGeometry(const std::filesystem::path& map);
int dumpWater(const std::filesystem::path& map);
int exportWater(const std::filesystem::path& client, const std::filesystem::path& output, unsigned workers,bool classic = false);
int verifyWaterCore();
}
