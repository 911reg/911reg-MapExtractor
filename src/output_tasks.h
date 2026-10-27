#pragma once
#include <filesystem>
#include <stdexcept>
#include <windows.h>

namespace extractor {
// mode: 0 Doors, 1 Water, 2 map-details.json (command-line scan only), 3 Geodata.
inline std::filesystem::path taskOutput(const std::filesystem::path& root,unsigned mode) {
    if (root.empty()) throw std::runtime_error("Choose an output folder");
    const wchar_t* name=nullptr;
    switch (mode) {
    case 0: name=L"Doors"; break;
    case 1: name=L"Water"; break;
    case 2: name=L"map-details.json"; break;
    case 3: name=L"Geodata"; break;
    default: throw std::runtime_error("Unknown output task");
    }
    auto base=std::filesystem::weakly_canonical(std::filesystem::absolute(root));
    if (base.filename().empty() && base!=base.root_path()) base=base.parent_path();
    const auto target=(base/name).lexically_normal();
    if (target.parent_path()!=base || target==base) throw std::runtime_error("Invalid task output path");
    return target;
}
inline void clearTaskOutput(const std::filesystem::path& root,unsigned mode) {
    namespace fs=std::filesystem;
    const auto target=taskOutput(root,mode);
    auto check=[](const fs::path& path) {
        const auto attributes=GetFileAttributesW(path.c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Cannot clear linked or inaccessible output");
    };
    const auto status=fs::symlink_status(target);
    if (status.type()==fs::file_type::not_found) return;
    check(target);
    if (fs::is_directory(status)) for (const auto& entry:fs::recursive_directory_iterator(target)) check(entry.path());
    fs::remove_all(target);
}
}
