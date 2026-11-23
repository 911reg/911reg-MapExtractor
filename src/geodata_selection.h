#pragma once
#include "map_selection.h"

namespace extractor::viewer {
struct MapChoice { std::filesystem::path map,geodata;std::wstring name; };
inline bool hasVariantPairs(const std::vector<std::filesystem::path>& maps) {
    std::map<std::wstring,unsigned> groups;
    for(const auto& map:maps)if(regionMap(map)){auto stem=folded(map.stem().wstring());const bool classic=classicMap(map);if(classic)stem.resize(stem.size()-8);groups[stem]|=classic ? 2u:1u;}
    return std::any_of(groups.begin(),groups.end(),[](const auto& entry){return entry.second==3;});
}
inline std::vector<MapChoice> geodataChoices(std::vector<std::filesystem::path> maps,const std::filesystem::path& results,bool preferClassic) {
    std::map<std::wstring,std::array<std::filesystem::path,2>> groups;
    for(const auto& map:maps)if(regionMap(map)){auto stem=folded(map.stem().wstring());const bool classic=classicMap(map);if(classic)stem.resize(stem.size()-8);auto& slot=groups[stem][classic ? 1:0];if(!slot.empty() && slot!=map)throw std::runtime_error("Multiple selected maps have the same region and variant");slot=map;}
    std::vector<MapChoice> result;
    for(const auto& [key,pair]:groups){const auto& map=pair[preferClassic && !pair[1].empty() ? 1:!pair[0].empty() ? 0:1];const bool classic=classicMap(map);const auto stem=map.stem().wstring();const auto base=classic ? stem.substr(0,stem.size()-8):stem;const auto root=results/L"Geodata";
        const auto destination=(classic ? root/L"Classic":root)/L"L2J"/(base+L".l2j");
        std::vector<std::filesystem::path> candidates{destination};
        if(classic){candidates.push_back(root/L"Classic"/(base+L".l2j"));candidates.push_back(root/(stem+L".l2j"));candidates.push_back(root/L"L2J"/(stem+L".l2j"));}
        else candidates.push_back(root/(base+L".l2j"));
        auto path=destination;for(const auto& candidate:candidates)if(std::filesystem::is_regular_file(candidate)){path=candidate;break;}
        result.push_back({map,path,stem});
    }
    return result;
}
}
