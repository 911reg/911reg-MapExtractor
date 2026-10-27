#pragma once
#include <filesystem>
namespace extractor {
int dumpDoors(const std::filesystem::path& map);
int verifyDoorCore();
int exportDoors(const std::filesystem::path& client, const std::filesystem::path& output, unsigned workers,bool classic = false);
}
