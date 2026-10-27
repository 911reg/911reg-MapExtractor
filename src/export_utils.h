#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace extractor::output {
inline std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string(); return {value.begin(),value.end()};
}
inline std::string xml(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        if (c == '&') result += "&amp;";
        else if (c == '<') result += "&lt;";
        else if (c == '>') result += "&gt;";
        else if (c == '"') result += "&quot;";
        else if (c == '\'') result += "&apos;";
        else if (c >= 32) result += char(c);
    }
    return result;
}
inline std::string loadText(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Missing profile template: " + utf8(path.filename()));
    return {std::istreambuf_iterator<char>(file),{}};
}
inline std::string render(const std::string& text, const std::map<std::string,std::string>& values) {
    std::string result; size_t cursor = 0;
    for (;;) {
        const auto begin = text.find("{{",cursor);
        if (begin == std::string::npos) { result += text.substr(cursor); break; }
        result += text.substr(cursor,begin-cursor);
        const auto end = text.find("}}",begin+2);
        if (end == std::string::npos) throw std::runtime_error("Invalid profile placeholder");
        const auto key = text.substr(begin+2,end-begin-2);
        const auto found = values.find(key);
        if (found == values.end()) throw std::runtime_error("Unknown profile field: " + key);
        result += found->second; cursor = end+2;
    }
    return result;
}
inline void save(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary); file << text; file.close();
    if (!file) throw std::runtime_error("Could not save extraction output");
}
}
