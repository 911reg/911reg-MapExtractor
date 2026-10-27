// Door geometry from live Engine.Mover actors. XML profiles and L2OFF fragments
// carry the client footprint. HP, defenses and opening rules stay in the server's own data.
#include "doors.h"
#include "package.h"
#include "map_selection.h"
#include "export_readme.h"
#include "l2off.h"
#include "collision.h"
#include "client_defaults.h"
#include <bit>
#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <windows.h>
#include <iomanip>
#include <set>

namespace extractor {
static std::vector<Property> moverDefaults() {
    auto encode=[](std::vector<uint8_t>& bytes,float value) {
        const auto bits=std::bit_cast<uint32_t>(value);
        for (unsigned shift=0;shift<32;shift+=8) bytes.push_back(uint8_t(bits>>shift));
    };
    std::vector<Property> result;
    Property height; height.name="RangeHeight"; height.kind=4; encode(height.value,defaults::moverHeight); result.push_back(std::move(height));
    for (size_t i=0;i<defaults::moverRange.size();++i) {
        Property range; range.name="RangeDelta"; range.kind=10; range.structure="Vector"; range.element=uint32_t(i);
        for (const auto value:defaults::moverRange[i]) encode(range.value,value);
        result.push_back(std::move(range));
    }
    return result;
}
int dumpDoors(const std::filesystem::path& map) {
    std::cout << std::setprecision(17);
    if (folded(map.extension().wstring())!=L".unr") throw std::runtime_error("Door inspection requires a UNR map");
    const auto package = Package::open(map);
    const auto live=collision::liveActors(package);
    for (size_t i = 0; i < package.exports.size(); ++i) {
        const auto type = package.className(i);
        if (type != "Engine.Mover" && type != "Engine.L2NMover") continue;
        std::cout << package.exports[i].name << " class=" << type << " live=" << std::binary_search(live.begin(),live.end(),i) << '\n';
        for (const auto& property : package.properties(i)) {
            std::cout << "  " << property.name << '[' << property.element << "] kind=" << unsigned(property.kind)
                << " struct=" << property.structure << " size=" << property.value.size();
            Reader reader{property.value};
            if (property.kind == 2 && property.value.size() == 4) std::cout << " int=" << int32_t(reader.word());
            if (property.kind == 1 && property.value.size() == 1) std::cout << " byte=" << unsigned(reader.byte());
            if (property.kind == 4 && property.value.size() == 4) std::cout << " float=" << std::bit_cast<float>(reader.word());
            if (property.kind == 5) std::cout << " ref=" << package.objectPath(reader.compact());
            if (property.kind == 3) std::cout << " bool=" << property.boolean;
            if (property.structure == "Vector" && property.value.size() == 12)
                for (unsigned j = 0; j < 3; ++j) std::cout << ' ' << std::bit_cast<float>(reader.word());
            std::cout << '\n';
        }
    }
    return 0;
}

struct Point { double x = 0, y = 0, z = 0; };
struct Door {
    int32_t id = 0, localId = 0;
    std::string actor, name, meshName;
    Point location;
    std::array<Point,4> nodes;
    std::array<Point,4> deltas;
    std::array<Point,4> exportNodes;
    bool reordered=false,roundedOutward=false;
    int minZ = 0, maxZ = 0;
    double rangeHeight = 0;
    bool inherited = false;
};
static double turn(const Point& a,const Point& b,const Point& c) {
    return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
static bool convex(const std::array<Point,4>& points) {
    double sign=0;
    for (size_t i=0;i<4;++i) {
        for (size_t j=i+1;j<4;++j) if (points[i].x==points[j].x && points[i].y==points[j].y) return false;
        const auto cross=turn(points[i],points[(i+1)%4],points[(i+2)%4]);
        if (!cross) continue;
        if (sign && (sign>0)!=(cross>0)) return false;
        sign=cross;
    }
    return sign!=0;
}
static void normalizeRange(Door& door) {
    auto points=door.nodes;
    if (!convex(points)) {
        std::sort(points.begin(),points.end(),[](const Point& a,const Point& b) { return a.x==b.x ? a.y<b.y : a.x<b.x; });
        std::vector<Point> hull;
        for (const auto& p:points) {
            while (hull.size()>1 && turn(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back();
            hull.push_back(p);
        }
        const auto lower=hull.size();
        for (int i=2;i>=0;--i) {
            while (hull.size()>lower && turn(hull[hull.size()-2],hull.back(),points[i])<=0) hull.pop_back();
            hull.push_back(points[i]);
        }
        hull.pop_back();
        if (hull.size()!=4) throw std::runtime_error("RangeDelta does not define four usable XY corners");
        const auto first=std::find_if(hull.begin(),hull.end(),[&](const Point& p) { return p.x==door.nodes[0].x && p.y==door.nodes[0].y; });
        std::rotate(hull.begin(),first,hull.end()); std::copy(hull.begin(),hull.end(),points.begin()); door.reordered=true;
    }
    door.exportNodes=points;
    for (auto& p:door.exportNodes) { p.x=std::round(p.x); p.y=std::round(p.y); }
    if (convex(door.exportNodes)) return;
    double bestArea=std::numeric_limits<double>::infinity(),bestDistance=bestArea;
    for (unsigned mask=0;mask<256;++mask) {
        auto candidate=points; double distance=0;
        for (size_t i=0;i<4;++i) {
            candidate[i].x=mask&(1u<<(i*2)) ? std::ceil(points[i].x) : std::floor(points[i].x);
            candidate[i].y=mask&(2u<<(i*2)) ? std::ceil(points[i].y) : std::floor(points[i].y);
            distance+=std::pow(candidate[i].x-points[i].x,2)+std::pow(candidate[i].y-points[i].y,2);
        }
        if (!convex(candidate)) continue;
        bool contains=true;
        for (const auto& p:points) {
            double side=0;
            for (size_t i=0;i<4;++i) {
                const auto cross=turn(candidate[i],candidate[(i+1)%4],p);
                if (std::abs(cross)<0.000001) continue;
                if (side && (side>0)!=(cross>0)) { contains=false; break; }
                side=cross;
            }
            if (!contains) break;
        }
        if (!contains) continue;
        double area=0; for (size_t i=0;i<4;++i) area+=turn(candidate[0],candidate[i],candidate[(i+1)%4]);
        area=std::abs(area);
        if (area<bestArea || (area==bestArea && distance<bestDistance)) { door.exportNodes=candidate; bestArea=area; bestDistance=distance; }
    }
    if (!std::isfinite(bestArea)) throw std::runtime_error("RangeDelta cannot form a conservative integer XY footprint");
    door.roundedOutward=true;
}
static bool samePlacementShape(const Door& a,const Door& b) {
    if (a.meshName!=b.meshName || a.localId!=b.localId || a.location.x!=b.location.x || a.location.y!=b.location.y || std::abs(a.rangeHeight-b.rangeHeight)>0.001) return false;
    for (const auto& p:a.nodes) {
        if (std::none_of(b.nodes.begin(),b.nodes.end(),[&](const Point& q) { return std::abs(p.x-q.x)<0.001 && std::abs(p.y-q.y)<0.001 && std::abs((p.z-a.location.z)-(q.z-b.location.z))<0.001; })) return false;
    }
    return true;
}

static std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string(); return {value.begin(),value.end()};
}
static std::string xml(const std::string& text) {
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
static Point point(const Property& property) {
    if (property.structure != "Vector" || property.value.size() != 12) throw std::runtime_error("Invalid door vector");
    Reader reader{property.value};
    Point value{std::bit_cast<float>(reader.word()),std::bit_cast<float>(reader.word()),std::bit_cast<float>(reader.word())};
    for (double c : {value.x,value.y,value.z})
        if (!std::isfinite(c) || std::abs(c) > 16777216) throw std::runtime_error("Door coordinate outside bounds");
    return value;
}
static std::string stringValue(const Property& property) {
    Reader reader{property.value};
    const int64_t length = reader.compact();
    const auto count = length < 0 ? -length : length;
    if (count < 1 || count > 65536) throw std::runtime_error("Invalid door name length");
    std::wstring text;
    for (int64_t i = 0; i < count; ++i) {
        const auto c = reader.word(length < 0 ? 2 : 1);
        if (i == count-1) { if (c) throw std::runtime_error("Unterminated door name"); }
        else text += wchar_t(c);
    }
    const int bytes = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if (!bytes && !text.empty()) throw std::runtime_error("Invalid door name encoding");
    std::string result(bytes,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),bytes,nullptr,nullptr);
    return result;
}
static const Property* find(const std::vector<Property>& values, const std::string& name, unsigned element = 0) {
    const Property* found = nullptr;
    for (const auto& value : values) if (value.name == name && value.element == element) {
        if (found) throw std::runtime_error("Duplicate door property: " + name);
        found = &value;
    }
    return found;
}
static int32_t integer(const Property& property) {
    if (property.kind != 2 || property.value.size() != 4) throw std::runtime_error("Invalid door ID");
    Reader reader{property.value}; return std::bit_cast<int32_t>(reader.word());
}
static Door decodeDoor(const Package& package, size_t index, const std::vector<Property>& properties,
    const std::vector<Property>& defaults) {
    Door door; door.actor = package.exports[index].name; door.name = door.actor;
    const auto id = find(properties,"L2ServerObjectRealID");
    if (!id || (door.id = integer(*id)) <= 0) throw std::runtime_error("Missing server door ID; no ID invented");
    if (const auto local = find(properties,"L2ServerObjectID")) door.localId = integer(*local);
    const auto location = find(properties,"Location");
    if (!location) throw std::runtime_error("Missing door location");
    door.location = point(*location);
    if (const auto mesh = find(properties,"StaticMesh")) {
        Reader reader{mesh->value}; const auto ref = reader.compact();
        if (ref) {
            const auto path = package.objectPath(ref);
            const auto separator = path.find_last_of('.');
            door.name = separator == std::string::npos ? path : path.substr(separator+1);
            door.meshName = door.name;
        }
    }
    if (const auto name = find(properties,"MoverName")) {
        if (name->kind != 13) throw std::runtime_error("Unexpected door name type");
        const auto value = stringValue(*name); if (!value.empty()) door.name = value;
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto range = find(properties,"RangeDelta",i);
        if (!range) { range = find(defaults,"RangeDelta",i); door.inherited = true; }
        if (!range) throw std::runtime_error("Missing door range and client defaults");
        const auto delta = point(*range);
        door.deltas[i] = delta;
        door.nodes[i] = {float(float(door.location.x)+float(delta.x)),float(float(door.location.y)+float(delta.y)),float(float(door.location.z)+float(delta.z))};
    }
    auto height = find(properties,"RangeHeight");
    if (!height) { height = find(defaults,"RangeHeight"); door.inherited = true; }
    if (!height || height->kind != 4 || height->value.size() != 4) throw std::runtime_error("Missing door height and client defaults");
    Reader reader{height->value}; const double h = std::bit_cast<float>(reader.word());
    door.rangeHeight = h;
    if (!std::isfinite(h) || h <= 0 || h > 16777216) throw std::runtime_error("Door height unusable; mesh fallback not implemented");
    double low = door.nodes[0].z, high = low;
    for (size_t i = 0; i < 4; ++i) {
        const auto& a = door.nodes[i];
        low = std::min(low,a.z); high = std::max(high,a.z);
        for (double c : {a.x,a.y,a.z}) if (!std::isfinite(c) || std::abs(c) > 16777216) throw std::runtime_error("Door bounds exceed coordinate limits");
    }
    normalizeRange(door);
    door.minZ = int(std::floor(low)); door.maxZ = int(std::ceil(high+h));
    if (door.maxZ > 16777216 || door.maxZ <= door.minZ) throw std::runtime_error("Door vertical bounds unusable");
    return door;
}
static std::string loadText(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Missing profile template: " + utf8(path.filename()));
    return {std::istreambuf_iterator<char>(file),{}};
}
static std::string render(std::string text, const std::map<std::string,std::string>& values) {
    // Replacement scans the template once; actor names are never interpreted as template syntax.
    std::string result;
    size_t cursor = 0;
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
static std::map<std::string,std::string> fields(const Door& door) {
    auto number = [](double value) { return std::to_string(int(std::round(value))); };
    std::map<std::string,std::string> result{
        {"id",std::to_string(door.id)},{"name",xml(door.name)},
        {"x",number(door.location.x)},{"y",number(door.location.y)},{"z",number(door.location.z)},
        {"minZ",std::to_string(door.minZ)},{"maxZ",std::to_string(door.maxZ)},
        {"height",std::to_string(door.maxZ-door.minZ)}};
    for (size_t i = 0; i < 4; ++i) {
        const auto prefix = std::string("n")+std::to_string(i+1);
        result[prefix+"x"] = number(door.exportNodes[i].x); result[prefix+"y"] = number(door.exportNodes[i].y);
        result[prefix+"z"] = number(door.exportNodes[i].z);
    }
    return result;
}
static void save(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary); file << text; file.close();
    if (!file) throw std::runtime_error("Could not save door output");
}

int verifyDoorCore() {
    auto require = [](bool condition,const char* reason) { if (!condition) throw std::runtime_error(reason); };
    auto rejects = [&](auto operation,const char* reason) {
        bool rejected = false;
        try { operation(); } catch (const std::exception&) { rejected = true; }
        require(rejected,reason);
    };
    auto append = [](std::vector<uint8_t>& bytes,uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (i*8)));
    };
    auto vector = [&](const std::string& name,unsigned element,float x,float y,float z) {
        Property p; p.name = name; p.element = element; p.kind = 10; p.structure = "Vector";
        for (float value : {x,y,z}) append(p.value,std::bit_cast<uint32_t>(value));
        return p;
    };
    Package package; package.exports.push_back({0,0,"Probe"});
    std::vector<Property> properties;
    Property id; id.name = "L2ServerObjectRealID"; id.kind = 2; append(id.value,12345678); properties.push_back(id);
    properties.push_back(vector("Location",0,100,-200,-10.25f));
    properties.push_back(vector("RangeDelta",0,-4,-8,-0.5f));
    properties.push_back(vector("RangeDelta",1,4,-8,-0.5f));
    properties.push_back(vector("RangeDelta",2,4,8,-0.5f));
    properties.push_back(vector("RangeDelta",3,-4,8,-0.5f));
    Property height; height.name = "RangeHeight"; height.kind = 4; append(height.value,std::bit_cast<uint32_t>(150.f));
    const std::vector<Property> defaults{height};
    const auto door = decodeDoor(package,0,properties,defaults);
    const auto bundled=moverDefaults();
    const auto inheritedDoor=decodeDoor(package,0,{properties[0],properties[1]},bundled);
    require(inheritedDoor.rangeHeight==150 && inheritedDoor.deltas[0].x==-80 && inheritedDoor.deltas[2].y==80 && inheritedDoor.inherited,"Bundled Mover defaults regression");
    auto overrideProperties=properties; Property explicitHeight=height; explicitHeight.value.clear(); append(explicitHeight.value,std::bit_cast<uint32_t>(7.5f)); overrideProperties.push_back(explicitHeight);
    require(decodeDoor(package,0,overrideProperties,bundled).rangeHeight==7.5,"Serialized height did not override bundled defaults");
    require(door.id == 12345678 && door.nodes[0].x == 96 && door.nodes[0].y == -208 && door.nodes[0].z == -10.75,
        "World-axis door geometry regression");
    require(door.minZ == -11 && door.maxZ == 140 && door.inherited,"Conservative Z/default regression");
    require(fields(door).at("height") == "151","Exported height regression");
    auto crossed=properties; std::swap(crossed[3].value,crossed[4].value);
    const auto reordered=decodeDoor(package,0,crossed,defaults);
    require(reordered.reordered && convex(reordered.exportNodes),"Crossed RangeDelta corners were not repaired");
    auto thin=properties;
    thin[2]=vector("RangeDelta",0,-0.1f,-8,0); thin[3]=vector("RangeDelta",1,0.1f,-8,0);
    thin[4]=vector("RangeDelta",2,0.1f,8,0); thin[5]=vector("RangeDelta",3,-0.1f,8,0);
    const auto widened=decodeDoor(package,0,thin,defaults);
    require(widened.roundedOutward && convex(widened.exportNodes) && widened.nodes[0].x!=widened.exportNodes[0].x,"Thin RangeDelta footprint collapsed or raw data changed");
    auto lower=door; lower.location.z-=3072; for (auto& p:lower.nodes) p.z-=3072;
    require(samePlacementShape(door,lower),"Stacked duplicate placement was not recognized");
    lower.location.x+=1; require(!samePlacementShape(door,lower),"Unrelated duplicate placement accepted as stacked");
    package.imports = {{0,0,"Embryo_Temple_S"},{0,-1,"Outside"},{0,-2,"Temple_Out_door_01"}};
    Property mesh; mesh.name = "StaticMesh"; mesh.kind = 5; mesh.value = {0x83};
    auto named = properties; named.push_back(mesh);
    require(decodeDoor(package,0,named,defaults).name == "Temple_Out_door_01","Door name retained mesh package prefix");
    Property label; label.name="MoverName"; label.kind=13;
    const std::string overrideName="DisplayOverride";
    label.value.push_back(uint8_t(overrideName.size()+1)); label.value.insert(label.value.end(),overrideName.begin(),overrideName.end()); label.value.push_back(0); named.push_back(label);
    const auto renamed=decodeDoor(package,0,named,defaults);
    require(renamed.name==overrideName && renamed.meshName=="Temple_Out_door_01","Staticobject mesh name was replaced by MoverName");
    auto missingId = properties; missingId.erase(missingId.begin());
    rejects([&] { decodeDoor(package,0,missingId,defaults); },"Missing ID accepted");
    rejects([&] { decodeDoor(package,0,properties,{}); },"Missing client defaults accepted");
    auto invalid = properties;
    invalid[1] = vector("Location",0,std::numeric_limits<float>::quiet_NaN(),0,0);
    rejects([&] { decodeDoor(package,0,invalid,defaults); },"NaN coordinate accepted");
    for (size_t i = 2; i < properties.size(); ++i) properties[i] = vector("RangeDelta",unsigned(i-2),0,0,0);
    rejects([&] { decodeDoor(package,0,properties,defaults); },"Degenerate polygon accepted");
    require(xml("<&\"'>") == "&lt;&amp;&quot;&apos;&gt;","XML escaping regression");
    require(render("{{name}}",{{"name","{{id}}"}}) == "{{id}}","Template values reinterpreted");
    rejects([&] { render("{{unknown}}",{}); },"Unknown template field accepted");
    package.names = {"None","Location","Vector"};
    package.data = {1,0x3a,2};
    for (float value : {100.f,-200.f,-10.25f}) append(package.data,std::bit_cast<uint32_t>(value));
    package.data.push_back(0); package.exports[0].size = package.data.size();
    require(point(package.properties(0).at(0)).y == -200,"Tagged Vector regression");
    package.exports[0].size = 2;
    rejects([&] { package.properties(0); },"Property escaped export boundary");
    std::cout << "Door core checks passed: geometry, Z bounds, defaults, invalid inputs, XML and tag bounds\n";
    return 0;
}

int exportDoors(const std::filesystem::path& client, const std::filesystem::path& output, unsigned workers,bool classic) {
    namespace fs = std::filesystem;
    auto root = fs::canonical(client);
    fs::path singleMap;
    if (fs::is_regular_file(root) && _wcsicmp(root.extension().c_str(),L".unr") == 0) {
        singleMap = root; root = root.parent_path();
    }
    if (fs::is_directory(root / "Maps")) root /= "Maps";
    else if (root.filename() == L"System" && fs::is_directory(root.parent_path() / "Maps")) root = root.parent_path() / "Maps";
    if (fs::exists(output)) throw std::runtime_error("Door output folder already exists; choose a new one");
    const auto defaults=moverDefaults();
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    const auto resources = fs::path(executable).parent_path() / "profiles";
    const std::array<std::string,4> profiles{"l2jserver","acis","l2scripts","mobius"};
    std::array<std::string,4> documents, entries;
    for (size_t i = 0; i < profiles.size(); ++i) {
        documents[i] = loadText(resources / profiles[i] / "doors.xml.tpl");
        entries[i] = loadText(resources / profiles[i] / "door.xml.tpl");
    }
    const auto offDocument=loadText(resources/"l2off"/"doors.txt.tpl"),offEntry=loadText(resources/"l2off"/"door.txt.tpl");
    std::cerr << "STAGE selecting maps | matching selected files\n";
    auto maps = selectMaps(singleMap.empty() ? root : singleMap,classic);
    if (maps.empty()) throw std::runtime_error("No game maps found");
    fs::create_directories(output);
    if (debugFiles) save(output / "911reg-map-sources.tsv",mapSources(maps,root));
    std::atomic<size_t> next{0}, total{0}, rejected{0}, inherited{0}, badMaps{0};
    std::vector<std::string> notes(maps.size());
    std::vector<std::array<std::string,4>> combined(maps.size());
    std::vector<std::vector<int32_t>> combinedIds(maps.size());
    std::vector<std::map<int32_t,std::string>> clientRows(maps.size());
    std::vector<std::set<int32_t>> duplicateIds(maps.size());
    std::atomic<size_t> clientRejected{0};
    std::atomic<size_t> warnings{0},repaired{0};
    std::mutex progress; size_t completed = 0;
    std::cerr << "Found " << maps.size() << " maps. Extracting doors...\n";
    {
        std::vector<std::jthread> pool;
        for (unsigned thread = 0; thread < std::min<size_t>(workers,maps.size()); ++thread) pool.emplace_back([&] {
            for (;;) {
                const size_t mapIndex = next.fetch_add(1); if (mapIndex >= maps.size()) break;
                { std::lock_guard lock(progress); std::cerr << "MAP " << utf8(maps[mapIndex].filename()) << " | door properties\n"; }
                std::ostringstream report; std::vector<Door> doors;
                try {
                    const auto package = Package::open(maps[mapIndex]);
                    const auto live=collision::liveActors(package);
                    std::map<int32_t,size_t> ids;
                    std::map<int32_t,bool> blocked;
                    std::ostringstream alternatives;
                    alternatives << "id\tkeptActor\tkeptZ\tdiscardedActor\tdiscardedX\tdiscardedY\tdiscardedZ\tnode\tdeltaX\tdeltaY\tdeltaZ\n" << std::setprecision(17);
                    bool duplicates=false;
                    for (size_t i = 0; i < package.exports.size(); ++i) {
                        if (!std::binary_search(live.begin(),live.end(),i)) continue;
                        const auto type = package.className(i);
                        if (type != "Engine.Mover" && type != "Engine.L2NMover") continue;
                        try {
                            const auto props = package.properties(i);
                            const auto kind = find(props,"L2ServerObjectType");
                            if (!kind || kind->kind != 1 || kind->value.size() != 1 || kind->value[0] != 1) continue;
                            if (type != "Engine.Mover") throw std::runtime_error("Door actor class is not a supported Mover");
                            if (const auto deleted = find(props,"bDeleteMe"); deleted && deleted->boolean) continue;
                            auto door = decodeDoor(package,i,props,defaults);
                            if (door.reordered || door.roundedOutward) {
                                ++repaired; report << "NOTICE: " << utf8(maps[mapIndex].filename()) << " / " << door.actor << ": repaired RangeDelta" << (door.reordered ? " corner order" : "") << (door.roundedOutward ? " outward integer rounding" : "") << "; source coordinates preserved in raw data\n";
                            }
                            if (blocked.contains(door.id)) throw std::runtime_error("Conflicting server door ID remains unresolved: "+std::to_string(door.id));
                            if (ids.contains(door.id)) {
                                duplicateIds[mapIndex].insert(door.id);
                                auto& kept=doors[ids.at(door.id)];
                                if (!samePlacementShape(kept,door)) { blocked[door.id]=true; ++rejected; throw std::runtime_error("Conflicting door ID has different XY/mesh/shape; upper-placement policy cannot resolve: "+std::to_string(door.id)); }
                                auto discarded=door;
                                if (door.location.z>kept.location.z || (door.location.z==kept.location.z && door.actor<kept.actor)) { discarded=kept; kept=std::move(door); }
                                ++warnings; duplicates=true;
                                report << "WARNING: " << utf8(maps[mapIndex].filename()) << " / ID " << kept.id << ": kept " << kept.actor << " at Z=" << kept.location.z << ", discarded " << discarded.actor << " at Z=" << discarded.location.z << " (upper-placement policy)\n";
                                for (size_t node=0;node<4;++node) alternatives << kept.id << '\t' << kept.actor << '\t' << kept.location.z << '\t' << discarded.actor << '\t' << discarded.location.x << '\t' << discarded.location.y << '\t' << discarded.location.z << '\t' << node << '\t' << discarded.deltas[node].x << '\t' << discarded.deltas[node].y << '\t' << discarded.deltas[node].z << '\n';
                                continue;
                            }
                            ids[door.id] = doors.size();
                            doors.push_back(std::move(door));
                        } catch (const std::exception& error) {
                            ++rejected; report << utf8(maps[mapIndex].filename()) << " / " << package.exports[i].name << ": " << error.what() << '\n';
                        }
                    }
                    std::erase_if(doors,[&](const Door& door) { return blocked.contains(door.id); });
                    if (debugFiles && duplicates) { auto path=mapOutput(maps[mapIndex],root); path.replace_extension(L".duplicates.tsv"); save(output/"raw"/path,alternatives.str()); }
                    std::sort(doors.begin(),doors.end(),[](const Door& a,const Door& b) { return a.id < b.id; });
                    if (!doors.empty()) {
                        auto relative = mapOutput(maps[mapIndex],root); relative.replace_extension(L".xml");
                        std::ostringstream raw;
                        raw << "id\tactor\tnode\tlocationX\tlocationY\tlocationZ\tdeltaX\tdeltaY\tdeltaZ\tworldX\tworldY\tworldZ\trangeHeight\tminZ\tmaxZ\tclientDefaults\texportX\texportY\texportZ\n" << std::setprecision(17);
                        for (const auto& door : doors) for (size_t node = 0; node < 4; ++node) {
                            const auto& p = door.nodes[node];
                            raw << door.id << '\t' << door.actor << '\t' << node << '\t'
                                << door.location.x << '\t' << door.location.y << '\t' << door.location.z << '\t'
                                << door.deltas[node].x << '\t' << door.deltas[node].y << '\t' << door.deltas[node].z << '\t'
                                << p.x << '\t' << p.y << '\t' << p.z << '\t' << door.rangeHeight << '\t'
                                << door.minZ << '\t' << door.maxZ << '\t' << door.inherited << '\t' << door.exportNodes[node].x << '\t' << door.exportNodes[node].y << '\t' << door.exportNodes[node].z << '\n';
                        }
                        auto rawPath = relative; rawPath.replace_extension(L".tsv");
                        if (debugFiles) save(output / "raw" / rawPath,raw.str());
                        for (size_t profile = 0; profile < profiles.size(); ++profile) {
                            if (!profileEnabled(profiles[profile])) continue;
                            std::string body;
                            for (const auto& door : doors) body += render(entries[profile],fields(door));
                            if (combineXml) combined[mapIndex][profile]=std::move(body);
                            else save(profileFolder(output,profiles[profile]) / relative,render(documents[profile],{{"doors",body}}));
                        }
                        for (const auto& door:doors) {
                            combinedIds[mapIndex].push_back(door.id);
                            if (door.meshName.empty() || door.meshName.find_first_of("[]\t\r\n")!=std::string::npos) {
                                ++clientRejected; report << utf8(maps[mapIndex].filename()) << " / " << door.actor << ": Staticobject companion omitted: missing or unrepresentable StaticMesh name\n";
                            } else clientRows[mapIndex][door.id]=door.meshName;
                        }
                        if (profileEnabled("l2off")) {
                        std::string offBody;
                        for (const auto& door:doors) {
                            auto values=fields(door);
                            values["scriptName"]=off::identifier(door.name)+"_"+std::to_string(door.id);
                            values["rangeHeight"]=std::to_string(int(std::ceil(door.rangeHeight)));
                            offBody+=render(offEntry,values);
                        }
                        auto offPath=relative; offPath.replace_extension(L".txt");
                        off::save(profileFolder(output,"l2off")/offPath,render(offDocument,{{"doors",offBody}}));
                        }
                        total += doors.size();
                        for (const auto& door : doors) if (door.inherited) ++inherited;
                    }
                } catch (const std::exception& error) { ++badMaps; report << utf8(maps[mapIndex].filename()) << ": " << error.what() << '\n'; }
                notes[mapIndex] = report.str();
                std::lock_guard lock(progress);
                std::cerr << "Checked " << ++completed << " of " << maps.size() << " maps\n";
            }
        });
    }
    if (combineXml) {
        for (int variant=0;variant<2;++variant) {
            std::map<int32_t,bool> ids;
            std::array<std::string,4> bodies;
            bool present=false;
            for (size_t i=0;i<maps.size();++i) {
                if (int(allVariants && classicMap(maps[i]))!=variant) continue;
                present=true;
                for (const auto id:combinedIds[i]) if (!ids.emplace(id,true).second) throw std::runtime_error("Duplicate door ID across maps prevents combined XML: "+std::to_string(id));
                for (size_t p=0;p<profiles.size();++p) bodies[p]+=combined[i][p];
            }
            if (present) for (size_t p=0;p<profiles.size();++p) if (profileEnabled(profiles[p])) save(profileFolder(output,profiles[p])/(variant ? "Classic/doors.xml" : "doors.xml"),render(documents[p],{{"doors",bodies[p]}}));
        }
    }
    for (int variant=0;variant<2;++variant) {
        std::map<int32_t,std::string> rows;
        std::map<int32_t,bool> conflicts;
        bool present=false;
        for (size_t i=0;i<maps.size();++i) {
            if (int(allVariants && classicMap(maps[i]))!=variant) continue;
            present=true;
            for (const auto& [id,name]:clientRows[i]) {
                const auto found=rows.find(id);
                if (found!=rows.end() && found->second!=name) conflicts[id]=true;
                else rows[id]=name;
            }
        }
        for (const auto& [id,unused]:conflicts) { rows.erase(id); ++clientRejected; std::cerr<<"SKIPPED: Client name for door ID "<<id<<" has conflicting static-mesh names\n"; }
        if (!present) continue;
        std::string clientDat,fileEdit;
        for (const auto& [id,name]:rows) {
            clientDat+="staticobject_begin\tid="+std::to_string(id)+"\tname=["+name+"]\tstaticobject_end\r\n";
            fileEdit+=std::to_string(id)+"\t"+name+"\r\n";
        }
        const auto folder=variant ? output/"Classic" : output;
        save(folder/"saticobject-e_ClientDat.txt",clientDat);
        save(folder/"staticobject-e_FileEdit.txt",fileEdit);
    }
    std::ostringstream report;
    report << "911reg MapExtractor - door extraction\n911reg was here. These doors work for a living.\n\n"
        << "Doors exported: " << total << "\nDoors using bundled standard Mover defaults: " << inherited
        << "\nRejected actors: " << rejected << "\nUnreadable/failed maps: " << badMaps << "\nDuplicate placement warnings: " << warnings << "\nRangeDelta footprints repaired: " << repaired << "\nStaticobject companion rows rejected: " << clientRejected << "\n\n"
        << "Geometry comes from client Location + RangeDelta and RangeHeight.\n"
        << "HP/PDef/MDef and gameplay flags are editable template defaults, NOT extracted server stats.\n"
        << "L2OFF: UTF-16LE script fragments; merge into doordata.txt by editor_id. Names have ID suffixes; reconcile NPC/script references.\n"
        << "L2OFF height is ceil(client RangeHeight), nodes/position round to nearest. Review normal_type/by_npc and stats defaults.\n"
        << "Review these geometry templates before merging with server data. No live files modified.\n"
        << "Classic selection uses normal region output names; see 911reg-map-sources.tsv for provenance.\n"
        << "Mesh fallback and custom Mover subclasses are not supported yet.\n";
    for (const auto& note : notes) report << note;
    std::ostringstream duplicateSummary;
    if (std::any_of(duplicateIds.begin(),duplicateIds.end(),[](const auto& ids) { return !ids.empty(); })) {
        duplicateSummary << "\nDuplicate door IDs mean that multiple doors in the same map share an ID. Their coordinates may differ.\nFor stacked duplicates, only the upper placement is exported; the other placements have no separate server collision entry.\nDuplicate door IDs:\n";
        for (size_t i=0;i<maps.size();++i) {
            const auto& ids=duplicateIds[i]; if (ids.empty()) continue;
            duplicateSummary << utf8(maps[i].stem()) << ": " << ids.size() << " duplicate door ID" << (ids.size()==1 ? "" : "s") << ". ID" << (ids.size()==1 ? "" : "s") << ": ";
            bool first=true; for (const auto id:ids) { if (!first) duplicateSummary << ", "; duplicateSummary << id; first=false; }
            duplicateSummary << ".\n";
        }
    }
    report << duplicateSummary.str();
    if (debugFiles) save(output / "911reg-door-report.txt",report.str());
    writeExportReadme(output,"DOORS");
    for (const auto& note:notes) {
        std::istringstream lines(note); std::string line;
        while (std::getline(lines,line)) if (!line.empty() && !line.starts_with("NOTICE:") && !line.starts_with("WARNING:")) std::cerr<<"SKIPPED: "<<line<<'\n';
    }
    std::cerr << "Doors exported: " << total << ". Need attention: " << rejected+badMaps+clientRejected << ".\n";
    std::cerr << duplicateSummary.str();
    std::cerr<<"RESULT doors "<<total<<' '<<rejected<<' '<<badMaps<<' '<<clientRejected<<' '<<warnings<<'\n';
    return rejected || badMaps || clientRejected ? 2 : 0;
}
}
