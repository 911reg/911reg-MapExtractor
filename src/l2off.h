#pragma once
#include "export_utils.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include <windows.h>

namespace extractor::off {
inline std::string identifier(const std::string& value) {
    std::string result;
    for (unsigned char c:value) result+=((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_') ? char(c) : '_';
    return result.empty() ? "911reg" : result;
}
inline void save(const std::filesystem::path& path,const std::string& source) {
    std::string text;
    for (size_t i=0;i<source.size();++i) {
        if (source[i]=='\n' && (!i || source[i-1]!='\r')) text+='\r';
        text+=source[i];
    }
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!size && !text.empty()) throw std::runtime_error("Invalid L2OFF profile UTF-8");
    std::wstring wide(size,L'\0');
    if (size) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),size);
    std::string bytes="\xff\xfe";
    for (wchar_t c:wide) { bytes+=char(c&255); bytes+=char((c>>8)&255); }
    output::save(path,bytes);
}
struct Box { int minX=0,minY=0,maxX=0,maxY=0; };
inline std::vector<Box> boxes(const std::vector<std::array<int,2>>& polygon) {
    if (polygon.size()<4 || polygon.size()>512) throw std::runtime_error("L2OFF water footprint exceeds rectangular decomposition limits");
    std::vector<int> ys; int64_t twiceArea=0;
    for (size_t i=0;i<polygon.size();++i) {
        const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        if ((a[0]!=b[0] && a[1]!=b[1]) || a==b) throw std::runtime_error("L2OFF water requires axis-aligned edges; rotated/slanted footprints are not boxed");
        twiceArea+=int64_t(a[0])*b[1]-int64_t(b[0])*a[1]; ys.push_back(a[1]);
    }
    std::sort(ys.begin(),ys.end()); ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
    std::vector<Box> result; std::map<std::pair<int,int>,size_t> active;
    for (size_t strip=1;strip<ys.size();++strip) {
        std::vector<int> xs;
        for (size_t i=0;i<polygon.size();++i) {
            const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
            if (a[0]==b[0] && std::min(a[1],b[1])<=ys[strip-1] && std::max(a[1],b[1])>=ys[strip]) xs.push_back(a[0]);
        }
        std::sort(xs.begin(),xs.end());
        if (xs.size()%2) throw std::runtime_error("Invalid L2OFF water strip intersections");
        std::map<std::pair<int,int>,size_t> next;
        for (size_t i=0;i<xs.size();i+=2) {
            if (xs[i]>=xs[i+1]) throw std::runtime_error("Degenerate L2OFF water strip");
            const auto range=std::pair{xs[i],xs[i+1]}; const auto previous=active.find(range);
            if (previous!=active.end() && result[previous->second].maxY==ys[strip-1]) {
                result[previous->second].maxY=ys[strip]; next[range]=previous->second;
            } else {
                if (result.size()>=4096) throw std::runtime_error("L2OFF water box budget exceeded");
                next[range]=result.size(); result.push_back({xs[i],ys[strip-1],xs[i+1],ys[strip]});
            }
        }
        active=std::move(next);
    }
    int64_t covered=0;
    for (const auto& b:result) covered+=int64_t(b.maxX-b.minX)*(b.maxY-b.minY);
    if (!covered || covered*2!=std::abs(twiceArea)) throw std::runtime_error("L2OFF water decomposition changed footprint area");
    return result;
}
}
