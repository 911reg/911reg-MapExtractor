#include "localization.h"
#include "settings_file.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace i18n {
static std::vector<Language> catalog;
static std::map<std::string,std::vector<std::wstring>> strings;
static size_t current = 0;
static std::filesystem::path settings;
static std::wstring decode(const std::string& value) {
    const auto count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if (!count && !value.empty()) throw std::runtime_error("Translation catalog is not UTF-8");
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),count);
    size_t offset = 0;
    while ((offset = result.find(L"\\n",offset)) != std::wstring::npos) result.replace(offset,2,L"\n");
    return result;
}
static std::vector<std::string> columns(const std::string& line) {
    std::vector<std::string> result;
    size_t begin = 0;
    for (;;) {
        const auto end = line.find('\t',begin);
        result.push_back(line.substr(begin,end == std::string::npos ? end : end-begin));
        if (end == std::string::npos) break;
        begin = end+1;
    }
    return result;
}
void load() {
    if (!catalog.empty()) return;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    const auto folder = std::filesystem::path(executable).parent_path();
    settings = extractor::settingsFile(folder);
    std::ifstream file(folder / "Languages" / "strings.tsv",std::ios::binary);
    if (!file) throw std::runtime_error("Languages/strings.tsv is missing. Keep the application folder together.");
    std::string line;
    std::getline(file,line); if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("\xef\xbb\xbf")) line.erase(0,3);
    const auto header = columns(line);
    if (header.size() < 2 || header[0] != "id" || header[1] != "en") throw std::runtime_error("Invalid language header");
    for (size_t i = 1; i < header.size(); ++i) catalog.push_back({header[i],decode(header[i])});
    while (std::getline(file,line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto cells = columns(line);
        if (cells.size() != header.size() || strings.contains(cells[0])) throw std::runtime_error("Invalid translation row");
        auto& values = strings[cells[0]];
        for (size_t i = 1; i < cells.size(); ++i) values.push_back(decode(cells[i].empty() ? cells[1] : cells[i]));
    }
    const auto names = strings.find("language_name");
    if (names == strings.end()) throw std::runtime_error("Language names missing");
    for (size_t i = 0; i < catalog.size(); ++i) catalog[i].name = names->second[i];
    wchar_t preferred[32]{};
    GetPrivateProfileStringW(L"UI",L"Language",L"en",preferred,32,settings.c_str());
    for (size_t i = 0; i < catalog.size(); ++i) if (decode(catalog[i].code) == preferred) current = i;
}
const std::vector<Language>& languages() { return catalog; }
size_t selected() { return current; }
void select(size_t index) {
    if (index >= catalog.size()) return;
    current = index;
    WritePrivateProfileStringW(L"UI",L"Language",decode(catalog[index].code).c_str(),settings.c_str());
}
std::wstring tr(const std::string& key) {
    const auto found = strings.find(key);
    if (found == strings.end()) throw std::runtime_error("Missing translation key: " + key);
    return found->second[current];
}
std::wstring format(const std::string& key, size_t first, size_t second) {
    auto result = tr(key);
    for (auto [token,value] : {std::pair{L"{0}",first},std::pair{L"{1}",second}}) {
        size_t offset = 0;
        while ((offset = result.find(token,offset)) != std::wstring::npos) {
            const auto text = std::to_wstring(value); result.replace(offset,3,text); offset += text.size();
        }
    }
    return result;
}
}
