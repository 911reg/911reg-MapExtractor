#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace extractor {
class ClientHistory {
    std::filesystem::path file;
public:
    explicit ClientHistory(std::filesystem::path settings);
    std::vector<std::wstring> load() const;
    bool remember(const std::filesystem::path& client) const;
};
int verifyHistory(const std::filesystem::path& directory);
}
