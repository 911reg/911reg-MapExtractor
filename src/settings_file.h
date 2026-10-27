#pragma once
#include <filesystem>

namespace extractor {
inline std::filesystem::path settingsFile(const std::filesystem::path& folder) {
    const auto current=folder/L"settings.ini";
    const auto legacy=folder/L"911reg.ini";
    if (!std::filesystem::exists(current) && std::filesystem::exists(legacy)) std::filesystem::rename(legacy,current);
    return current;
}
}
