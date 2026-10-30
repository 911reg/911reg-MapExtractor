#pragma once
#include <filesystem>
#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <regex>
#include <stdexcept>
#include <vector>
#include "export_utils.h"

namespace extractor {
inline std::vector<std::filesystem::path> requestedMaps;
inline bool allVariants=false, combineXml=false;
inline bool debugFiles=true;
inline bool useCollisionCache=true;
inline bool retailHeightOffsets=true,emitPathnodes=true,pathnodesOnly=false,compactGeodata=true;
inline std::string geodataFormat="l2j";
inline std::string targetProfile="all";
inline bool profileEnabled(const std::string& profile) { return targetProfile=="all" || targetProfile==profile; }
inline std::filesystem::path profileFolder(const std::filesystem::path& root,const std::string& profile) { return !debugFiles && targetProfile!="all" ? root : root/profile; }
inline bool classicMap(const std::filesystem::path& source) {
    static const std::wregex pattern(LR"(^[0-9]{1,2}_[0-9]{1,2}_Classic$)",std::regex::icase);
    return std::regex_match(source.stem().wstring(),pattern);
}
inline bool regionMap(const std::filesystem::path& source) {
    static const std::wregex pattern(LR"(^[0-9]{2}_[0-9]{2}(_Classic)?$)",std::regex::icase);
    return std::regex_match(source.stem().wstring(),pattern);
}
inline void preferMapVariant(std::vector<std::filesystem::path>& maps,bool classic) {
    std::map<std::wstring,bool> preferred;
    for (const auto& path:maps) if (classicMap(path)==classic) {
        auto stem=path.stem().wstring(); if (classic) stem.resize(stem.size()-8);
        auto key=(path.parent_path()/stem).wstring();
        std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c) { return wchar_t(std::towlower(c)); });
        preferred.emplace(key,true);
    }
    std::erase_if(maps,[&](const auto& path) {
        if (classicMap(path)==classic) return false;
        auto stem=path.stem().wstring(); if (!classic) stem.resize(stem.size()-8);
        auto key=(path.parent_path()/stem).wstring();
        std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c) { return wchar_t(std::towlower(c)); });
        return preferred.contains(key);
    });
}
inline std::wstring folded(std::wstring value) {
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c) { return wchar_t(std::towlower(c)); }); return value;
}
inline void filterRequestedMaps(std::vector<std::filesystem::path>& maps) {
    if (requestedMaps.empty()) return;
    std::map<std::wstring,std::filesystem::path> available;
    for (const auto& path:maps) available.emplace(folded(std::filesystem::canonical(path).wstring()),path);
    std::map<std::wstring,bool> seen;
    std::vector<std::filesystem::path> selected;
    for (const auto& path:requestedMaps) {
        const auto key=folded(path.lexically_normal().wstring());
        const auto found=available.find(key);
        if (found==available.end()) throw std::runtime_error("Selected map is unavailable for this client or Classic mode");
        if (seen.emplace(key,true).second) selected.push_back(found->second);
    }
    maps=std::move(selected);
}
inline std::filesystem::path mapOutput(const std::filesystem::path& source,const std::filesystem::path& root) {
    auto relative = std::filesystem::relative(source,root);
    static const std::wregex pattern(LR"(^([0-9]{1,2}_[0-9]{1,2})_Classic$)",std::regex::icase);
    std::wsmatch match; const auto stem = relative.stem().wstring();
    if (std::regex_match(stem,match,pattern)) {
        relative = relative.parent_path() / (match[1].str()+relative.extension().wstring());
        if (allVariants) relative = std::filesystem::path("Classic")/relative;
    }
    return relative;
}
inline std::vector<std::filesystem::path> selectMaps(const std::filesystem::path& input,bool classic,bool regionOnly=true) {
    namespace fs = std::filesystem;
    auto root = fs::canonical(input); const bool single = fs::is_regular_file(root);
    if (single && regionOnly && !regionMap(root)) throw std::runtime_error("Choose a region map named XX_XX.unr or XX_XX_Classic.unr");
    const auto requested = single ? root : fs::path{}; if (single) root = root.parent_path();
    if (allVariants) {
        std::vector<fs::path> result;
        if (single) result.push_back(requested);
        else for (const auto& entry:fs::recursive_directory_iterator(root)) {
            if (!entry.is_symlink() && entry.is_regular_file() && folded(entry.path().extension().wstring())==L".unr") if (!regionOnly || regionMap(entry.path())) result.push_back(entry.path());
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return folded(a.wstring())<folded(b.wstring()); });
        filterRequestedMaps(result);
        if (result.empty()) throw std::runtime_error("No UNR maps found");
        std::map<std::wstring,bool> outputs;
        for (const auto& path:result) if (!outputs.emplace(folded(mapOutput(path,root).wstring()),true).second) throw std::runtime_error("Selected maps have conflicting output paths");
        return result;
    }
    std::map<std::wstring,std::array<fs::path,2>> groups;
    static const std::wregex suffix(LR"(^[0-9]{1,2}_[0-9]{1,2}_Classic$)",std::regex::icase);
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink() || !entry.is_regular_file() || folded(entry.path().extension().wstring()) != L".unr") continue;
        if (regionOnly && !regionMap(entry.path())) continue;
        const auto output = mapOutput(entry.path(),root);
        const auto key = folded(output.wstring()); const bool variant = std::regex_match(entry.path().stem().wstring(),suffix);
        if (single && key != folded(mapOutput(requested,root).wstring())) continue;
        auto& slot = groups[key][variant ? 1 : 0];
        if (!slot.empty()) throw std::runtime_error("Multiple maps resolve to the same output; resolve the naming conflict");
        slot = entry.path();
    }
    std::vector<fs::path> result;
    for (const auto& [key,pair] : groups) {
        const auto& selected = classic && !pair[1].empty() ? pair[1] : pair[0];
        if (!selected.empty()) result.push_back(selected);
    }
    filterRequestedMaps(result);
    if (result.empty()) throw std::runtime_error("No selected maps; enable Classic mode when selecting a Classic-only map");
    return result;
}
inline std::string mapSources(const std::vector<std::filesystem::path>& maps,const std::filesystem::path& root) {
    std::string result = "source\toutput\n";
    for (const auto& source : maps) result += output::utf8(std::filesystem::relative(source,root))+"\t"+output::utf8(mapOutput(source,root))+"\n";
    return result;
}
}
