#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <array>
#include <optional>
#include <stop_token>
namespace extractor::viewer {
struct CellKey { uint16_t x=0,y=0;uint8_t layer=0;bool operator==(const CellKey&) const=default; };
struct Cell { CellKey key;int16_t height=0;uint8_t nswe=0; };
class Document {
    std::vector<uint32_t> starts;
public:
    std::filesystem::path source;
    std::vector<Cell> cells;
    int originX=0,originY=0;
    bool worldCoordinates=false;
    float minHeight=0,maxHeight=0;
    static Document read(const std::filesystem::path& path,std::stop_token stop={});
    std::optional<uint32_t> find(CellKey key) const;
    std::optional<uint32_t> neighbor(uint32_t index,unsigned direction) const;
    uint8_t oneWay(uint32_t index) const;
    static bool isVoid(const Cell& cell);
};
int verifyDocument(const std::filesystem::path& folder);
}
