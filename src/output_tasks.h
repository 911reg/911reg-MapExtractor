#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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
    case 4: name=L"Pathnodes"; break;
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
// A run from the window keeps this hidden marker next to its output until it finishes.
// The marker's only line is the temporary map list of that run, or nothing.
inline std::filesystem::path incompleteMarker(const std::filesystem::path& root,unsigned mode) {
    const auto target=taskOutput(root,mode);
    return target.parent_path()/(L"."+target.filename().wstring()+L".incomplete");
}
inline void markIncomplete(const std::filesystem::path& root,unsigned mode,const std::filesystem::path& selection) {
    const auto marker=incompleteMarker(root,mode);
    { std::ofstream file(marker,std::ios::binary|std::ios::trunc); const auto text=selection.u8string(); file.write(reinterpret_cast<const char*>(text.data()),std::streamsize(text.size())); if (!file) throw std::runtime_error("Cannot mark the run"); }
    SetFileAttributesW(marker.c_str(),FILE_ATTRIBUTE_HIDDEN);
}
inline void markComplete(const std::filesystem::path& root,unsigned mode) {
    std::error_code error; std::filesystem::remove(incompleteMarker(root,mode),error);
}
// Removes what an interrupted run left behind: its half-written output and its temporary
// map list. Returns false when no run was interrupted.
inline bool cleanupIncomplete(const std::filesystem::path& root,unsigned mode) {
    namespace fs=std::filesystem;
    const auto marker=incompleteMarker(root,mode);
    if (!fs::is_regular_file(marker)) return false;
    std::string line; { std::ifstream file(marker,std::ios::binary); std::getline(file,line); }
    if (!line.empty()) {
        const fs::path selection(std::u8string(line.begin(),line.end()));
        wchar_t temporary[MAX_PATH+1]{}; GetTempPathW(MAX_PATH+1,temporary);
        std::error_code error;
        const auto relative=fs::weakly_canonical(selection,error).lexically_relative(fs::weakly_canonical(temporary,error));
        // Only a map list this program created in the temporary folder is removed.
        if (!relative.empty() && *relative.begin()!=L".." && _wcsnicmp(selection.filename().c_str(),L"mxe",3)==0) fs::remove(selection,error);
    }
    clearTaskOutput(root,mode);
    SetFileAttributesW(marker.c_str(),FILE_ATTRIBUTE_NORMAL); fs::remove(marker);
    return true;
}
}
