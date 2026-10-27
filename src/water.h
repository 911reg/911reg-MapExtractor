#pragma once
#include <filesystem>
namespace extractor {
int dumpWater(const std::filesystem::path& map);
int exportWater(const std::filesystem::path& client, const std::filesystem::path& output, unsigned workers,bool classic = false);
int verifyWaterCore();
}
