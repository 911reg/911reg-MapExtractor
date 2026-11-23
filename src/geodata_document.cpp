#include "geodata_document.h"
#include "geodata_selection.h"
#include <fstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <iostream>
namespace extractor::viewer {
namespace {
constexpr uint32_t limit=16000000;
uint8_t byte(std::istream& in) {const auto value=in.get();if(value==std::char_traits<char>::eof())throw std::runtime_error("Truncated L2J geodata");return uint8_t(value);}
int16_t word(std::istream& in) {const auto lo=byte(in),hi=byte(in);return int16_t(uint16_t(lo)|(uint16_t(hi)<<8));}
}
bool Document::isVoid(const Cell& cell) {return cell.height==-16352 && cell.nswe==0;}
Document Document::read(const std::filesystem::path& path,std::stop_token stop) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Cannot open geodata file");
    const auto length=in.tellg();if(length<196608 || length>1024ll*1024*1024)throw std::runtime_error("L2J file size is outside supported bounds");in.seekg(0);
    Document result;result.source=std::filesystem::absolute(path);result.starts.resize(4194305);result.cells.reserve(4194304);
    std::wsmatch region;const auto stem=path.stem().wstring();
    if(std::regex_match(stem,region,std::wregex(LR"(^([0-9]{1,2})_([0-9]{1,2})(?:_Classic)?$)",std::regex::icase))) {result.originX=(std::stoi(region[1])-20)*32768;result.originY=(std::stoi(region[2])-18)*32768;result.worldCoordinates=true;}
    bool first=true;
    for(uint32_t block=0;block<65536;++block) {
        if(stop.stop_requested())throw std::runtime_error("Geodata loading cancelled");
        const auto kind=byte(in);if(kind>2)throw std::runtime_error("Invalid L2J block type");const int16_t flat=kind==0 ? word(in) : 0;
        if(kind==0 && flat%8)throw std::runtime_error("Unaligned L2J flat-block height");
        for(uint32_t c=0;c<64;++c) {
            const auto n=kind==2 ? byte(in) : 1;if(!n || n>125)throw std::runtime_error("Invalid L2J layer count");
            if(result.cells.size()+n>limit)throw std::runtime_error("Geodata exceeds the 16-million-layer viewer budget");
            result.starts[block*64+c]=uint32_t(result.cells.size());
            for(unsigned layer=0;layer<unsigned(n);++layer) {
                const int16_t packed=kind==0 ? 0 : word(in);
                const auto height=kind==0 ? flat : int16_t((int(packed)&~15)/2);
                Cell cell{{uint16_t((block/256)*8+c/8),uint16_t((block%256)*8+c%8),uint8_t(layer)},height,uint8_t(kind==0 ? 15 : packed&15)};
                result.cells.push_back(cell);
                if(!isVoid(cell)){if(first){result.minHeight=result.maxHeight=height;first=false;}else{result.minHeight=std::min(result.minHeight,float(height));result.maxHeight=std::max(result.maxHeight,float(height));}}
            }
        }
    }
    result.starts.back()=uint32_t(result.cells.size());if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Unexpected trailing data after L2J region");
    if(first)throw std::runtime_error("Geodata contains only void cells");return result;
}
std::optional<uint32_t> Document::find(CellKey key) const {
    if(key.x>=2048 || key.y>=2048)return {};
    const uint32_t cell=(uint32_t(key.x/8)*256+key.y/8)*64+uint32_t(key.x%8)*8+key.y%8;
    if(starts.empty() || key.layer>=starts[cell+1]-starts[cell])return {};return starts[cell]+key.layer;
}
std::optional<uint32_t> Document::neighbor(uint32_t index,unsigned direction) const {
    if(index>=cells.size() || direction>=4)return {};
    constexpr int dx[]{1,-1,0,0},dy[]{0,0,1,-1};const auto& from=cells[index];const int x=int(from.key.x)+dx[direction],y=int(from.key.y)+dy[direction];if(x<0 || x>=2048 || y<0 || y>=2048)return {};
    const auto first=find({uint16_t(x),uint16_t(y),0});if(!first)return {};
    std::optional<uint32_t> best;int distance=129;
    for(uint32_t i=*first;i<cells.size() && cells[i].key.x==x && cells[i].key.y==y;++i)if(!isVoid(cells[i])) {const int delta=std::abs(int(cells[i].height)-from.height);if(delta<distance){distance=delta;best=i;}}
    return best;
}
uint8_t Document::oneWay(uint32_t index) const {
    constexpr unsigned flags[]{1,2,4,8},reverse[]{2,1,8,4};uint8_t result=0;
    for(unsigned d=0;d<4;++d)if(cells.at(index).nswe&flags[d]) {const auto next=neighbor(index,d);if(!next || !(cells[*next].nswe&reverse[d]))result|=uint8_t(flags[d]);}return result;
}
int verifyDocument(const std::filesystem::path& folder) {
    if(std::filesystem::exists(folder))throw std::runtime_error("Viewer fixture destination must be new");std::filesystem::create_directories(folder);
    const auto path=folder/"22_22_Classic.l2j";
    auto put=[](std::ostream& out,int n){out.put(char(n&255));out.put(char((n>>8)&255));};
    {std::ofstream out(path,std::ios::binary);out.put(2);out.put(2);put(out,(-16*2)|1);put(out,(64*2)|15);for(int c=1;c<64;++c){out.put(1);put(out,(-16*2)|(c==8 ? 0 : 15));}out.put(1);for(int c=0;c<64;++c)put(out,(24*2)|5);for(int b=2;b<65536;++b){out.put(0);put(out,-16);}}
    const auto doc=Document::read(path);auto need=[](bool ok){if(!ok)throw std::runtime_error("Geodata viewer document fixture failed");};
    need(doc.worldCoordinates && doc.originX==65536 && doc.originY==131072 && doc.cells.size()==4194305);
    const auto a=doc.find({0,0,0}),upper=doc.find({0,0,1});need(a && upper && doc.cells[*a].height==-16 && doc.cells[*upper].height==64 && !doc.find({0,0,2}));
    const auto single=doc.find({0,8,0});need(single && doc.cells[*single].height==24 && doc.cells[*single].nswe==5);
    need(doc.neighbor(*a,0)==doc.find({1,0,0}) && (doc.oneWay(*a)&1)!=0 && !doc.neighbor(*a,1));
    const auto invalid=folder/"trailing-data.l2j";std::filesystem::copy_file(path,invalid);{std::ofstream out(invalid,std::ios::binary|std::ios::app);out.put(0);}bool rejected=false;try{Document::read(invalid);}catch(const std::exception&){rejected=true;}need(rejected);
    std::stop_source cancelled;cancelled.request_stop();rejected=false;try{Document::read(path,cancelled.get_token());}catch(const std::exception&){rejected=true;}need(rejected);
    const std::vector<std::filesystem::path> maps{folder/"22_22.unr",folder/"22_22_Classic.unr",folder/"23_22.unr"};need(hasVariantPairs(maps));
    auto normal=geodataChoices(maps,folder,false),classic=geodataChoices(maps,folder,true);need(normal.size()==2 && classic.size()==2 && normal[0].name==L"22_22" && classic[0].name==L"22_22_Classic" && classic[1].name==L"23_22");
    need(normal[0].geodata==folder/"Geodata"/"L2J"/"22_22.l2j" && classic[0].geodata==folder/"Geodata"/"Classic"/"L2J"/"22_22.l2j");
    std::filesystem::create_directories(folder/"Geodata"/"L2J");{std::ofstream out(normal[0].geodata);out.put(0);}need(!std::filesystem::is_regular_file(geodataChoices(maps,folder,true)[0].geodata));
    std::filesystem::create_directories(folder/"Geodata"/"Classic");const auto legacyClassic=folder/"Geodata"/"Classic"/"22_22.l2j";{std::ofstream out(legacyClassic);out.put(0);}need(geodataChoices(maps,folder,true)[0].geodata==legacyClassic);
    const std::vector<std::filesystem::path> classicOnly{maps[1]};need(!hasVariantPairs(classicOnly) && geodataChoices(classicOnly,folder,false)[0].name==L"22_22_Classic");
    const auto adjacent=folder/"23_22.l2j";std::filesystem::copy_file(path,adjacent);const auto neighborDoc=Document::read(adjacent);need(neighborDoc.originX==doc.originX+32768 && neighborDoc.originY==doc.originY);
    std::cout<<"Geodata viewer fixtures passed: block layouts, signed heights, layers, origins, addressing, links, invalid input, cancellation, selected maps, variant choices and output discovery\n";return 0;
}
}
