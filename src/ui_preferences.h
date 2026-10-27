#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <windows.h>

namespace extractor {
inline const std::array<std::string,6> serverProfiles{"l2jserver","acis","l2scripts","mobius","l2off","all"};
// server is a profile id. The window label L2jMabios is still saved as "mobius".
// debug is DebugFiles. The window no longer shows that switch; normal runs force it off.
// --verify-ui turns it back on so the existing checks still see reports.
struct UiPreferences {
    std::string server="l2jserver";
    bool combine=true,debug=false;
    std::string geodata="both";
    static UiPreferences load(const std::filesystem::path& path) {
        UiPreferences result; wchar_t value[128]{};
        GetPrivateProfileStringW(L"UI",L"ServerType",L"l2jserver",value,128,path.c_str());
        for (const auto& profile:serverProfiles) if (std::wstring(profile.begin(),profile.end())==value) result.server=profile;
        result.combine=GetPrivateProfileIntW(L"UI",L"CombineMaps",1,path.c_str())!=0;
        result.debug=GetPrivateProfileIntW(L"UI",L"DebugFiles",0,path.c_str())!=0;
        GetPrivateProfileStringW(L"UI",L"GeodataFormat",L"both",value,128,path.c_str());
        const std::wstring format=value;
        result.geodata=format==L"l2j" ? "l2j" : format==L"l2off" ? "l2off" : "both";
        return result;
    }
    bool save(const std::filesystem::path& path) const {
        const std::wstring value(server.begin(),server.end());
        const std::wstring format(geodata.begin(),geodata.end());
        return WritePrivateProfileStringW(L"UI",L"ServerType",value.c_str(),path.c_str()) && WritePrivateProfileStringW(L"UI",L"CombineMaps",combine ? L"1" : L"0",path.c_str()) && WritePrivateProfileStringW(L"UI",L"DebugFiles",debug ? L"1" : L"0",path.c_str()) && WritePrivateProfileStringW(L"UI",L"GeodataFormat",format.c_str(),path.c_str());
    }
};
}
