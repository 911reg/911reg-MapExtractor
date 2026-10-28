#pragma once
#include <filesystem>
#include "collision.h"
namespace extractor {
struct WaterGeometry { std::vector<collision::Point> polygon; double low=0,high=0; };
bool waterContains(const WaterGeometry& zone,double x,double y);
std::vector<WaterGeometry> readWaterGeometry(const std::filesystem::path& map);
int dumpWater(const std::filesystem::path& map);
int exportWater(const std::filesystem::path& client, const std::filesystem::path& output, unsigned workers,bool classic = false);
int verifyWaterCore();
}
