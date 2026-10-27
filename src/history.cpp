#include "history.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace extractor {
ClientHistory::ClientHistory(std::filesystem::path settings) : file(std::move(settings)) {}
std::vector<std::wstring> ClientHistory::load() const {
    std::vector<std::wstring> result;
    for (unsigned i = 0; i < 10; ++i) {
        wchar_t path[32768]{};
        GetPrivateProfileStringW(L"RecentClients",std::to_wstring(i).c_str(),L"",path,32768,file.c_str());
        if (!*path) continue;
        if (std::none_of(result.begin(),result.end(),[&](const auto& other) { return _wcsicmp(other.c_str(),path) == 0; }))
            result.emplace_back(path);
    }
    return result;
}

bool ClientHistory::remember(const std::filesystem::path& client) const {
    auto folder = std::filesystem::weakly_canonical(client);
    if (!std::filesystem::is_directory(folder)) return false;
    if ((_wcsicmp(folder.filename().c_str(),L"Maps") == 0 || _wcsicmp(folder.filename().c_str(),L"System") == 0)
        && std::filesystem::is_directory(folder.parent_path() / "Maps")) folder = folder.parent_path();
    const auto path = folder.wstring();
    if (path.find_first_of(L"\r\n") != std::wstring::npos) return false;
    auto recent = load();
    std::erase_if(recent,[&](const auto& other) { return _wcsicmp(other.c_str(),path.c_str()) == 0; });
    recent.insert(recent.begin(),path);
    if (recent.size() > 10) recent.resize(10);

    // Windows profile APIs need UTF-16 to preserve paths outside the system code page.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,file.c_str());
    std::ifstream existing(file,std::ios::binary);
    if (!existing && std::filesystem::exists(file)) return false;
    std::string bytes{std::istreambuf_iterator<char>(existing),{}}; existing.close();
    if (bytes.size() < 2 || uint8_t(bytes[0]) != 0xff || uint8_t(bytes[1]) != 0xfe) {
        const bool utf8 = bytes.starts_with("\xef\xbb\xbf");
        const auto content = utf8 ? bytes.substr(3) : bytes;
        const UINT encoding = utf8 ? CP_UTF8 : CP_ACP;
        const auto count = MultiByteToWideChar(encoding,0,content.data(),int(content.size()),nullptr,0);
        std::wstring text(count,L'\0');
        MultiByteToWideChar(encoding,0,content.data(),int(content.size()),text.data(),count);
        auto temporary = file; temporary += L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
        std::ofstream converted(temporary,std::ios::binary);
        converted.write("\xff\xfe",2);
        converted.write(reinterpret_cast<const char*>(text.data()),std::streamsize(text.size()*sizeof(wchar_t)));
        converted.close();
        if (!converted || !MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); return false;
        }
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,file.c_str());
    }
    std::wstring section;
    for (size_t i = 0; i < recent.size(); ++i) {
        section += std::to_wstring(i)+L"="+recent[i]; section += L'\0';
    }
    section += L'\0';
    return WritePrivateProfileSectionW(L"RecentClients",section.c_str(),file.c_str()) != FALSE;
}

int verifyHistory(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    if (fs::exists(directory)) throw std::runtime_error("Choose a new history test directory");
    fs::create_directories(directory);
    const auto settings = fs::absolute(directory / "settings.ini");
    { std::ofstream file(settings); file << "[UI]\nLanguage=es\n"; }
    const auto unicode = directory / L"Client \u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac";
    fs::create_directories(unicode / "Maps"); fs::create_directories(unicode / "System");
    ClientHistory history(settings);
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Client history check failed"); };
    require(history.remember(unicode / "Maps"));
    require(history.remember(unicode / "System"));
    require(history.load().size() == 1 && history.load()[0] == fs::weakly_canonical(unicode).wstring());
    for (unsigned i = 0; i < 12; ++i) {
        const auto client = directory / (L"Client"+std::to_wstring(i)); fs::create_directory(client);
        require(history.remember(client));
    }
    require(history.load().size() == 10);
    require(history.remember(unicode));
    const auto persisted = ClientHistory(settings).load();
    require(persisted.size() == 10 && persisted.front() == fs::weakly_canonical(unicode).wstring());
    wchar_t language[32]{}; GetPrivateProfileStringW(L"UI",L"Language",L"",language,32,settings.c_str());
    require(std::wstring(language) == L"es");
    require(!history.remember(directory / "missing"));
    return 0;
}
}
