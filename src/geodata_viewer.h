#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>
#include <string>
namespace extractor {
struct GeodataRegion {std::filesystem::path path;std::wstring name;std::filesystem::path map;};
void openGeodataViewer(HWND owner,const std::filesystem::path& path={});
void openGeodataRegions(HWND owner,const std::vector<GeodataRegion>& regions);
bool handleGeodataViewerMessage(MSG& message);
void closeGeodataViewer();
int runGeodataViewer(const std::filesystem::path& path={});
int verifyGeodataViewport(const std::filesystem::path& path);
}
