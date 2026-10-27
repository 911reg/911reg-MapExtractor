// Water from Gameplay.WaterVolume and Engine.WaterVolume brushes. Other volumes are ignored.
// Sloped or incomplete brushes are reported. They are not replaced with a bounding box.
#include "water.h"
#include "l2off.h"
#include "package.h"
#include "export_utils.h"
#include "map_selection.h"
#include "export_readme.h"
#include "client_defaults.h"
#include <bit>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <windows.h>
#include <optional>

namespace extractor {
struct WaterPoint { double x = 0, y = 0, z = 0; };
using Face = std::vector<WaterPoint>;
static const Property* property(const std::vector<Property>& properties, const std::string& name) {
    const Property* found = nullptr;
    for (const auto& p : properties) if (p.name == name && !p.element) {
        if (found) throw std::runtime_error("Duplicate water property: " + name);
        found = &p;
    }
    return found;
}
static WaterPoint readPoint(Reader& reader) {
    WaterPoint p{std::bit_cast<float>(reader.word()),std::bit_cast<float>(reader.word()),std::bit_cast<float>(reader.word())};
    for (double c : {p.x,p.y,p.z}) if (!std::isfinite(c) || std::abs(c) > 16777216) throw std::runtime_error("Invalid water coordinate");
    return p;
}
static size_t count(Reader& reader,size_t maximum = 2000000) {
    const auto size = reader.compact();
    if (size < 0 || size_t(size) > maximum) throw std::runtime_error("Water model count exceeds limit");
    return size_t(size);
}
static std::vector<Face> modelFaces(const Package& package,int32_t reference) {
    if (reference <= 0 || size_t(reference) > package.exports.size() || package.className(size_t(reference-1)) != "Engine.Model")
        throw std::runtime_error("Water brush is not a local Model");
    const size_t index = size_t(reference-1); size_t offset = 0;
    package.properties(index,&offset); const auto& object = package.exports[index];
    Reader reader{package.data,offset,object.offset+object.size};
    reader.skip(41);
    auto size = count(reader); reader.skip(size*12);
    std::vector<WaterPoint> points; size = count(reader);
    for (size_t i = 0; i < size; ++i) points.push_back(readPoint(reader));
    struct Node { int32_t first; unsigned vertices; };
    std::vector<Node> nodes; size = count(reader,200000);
    for (size_t i = 0; i < size; ++i) {
        reader.skip(25); Node node{reader.compact(),0}; reader.compact();
        for (unsigned j = 0; j < 5; ++j) reader.compact();
        reader.skip(34); node.vertices = reader.byte(); reader.skip(20); nodes.push_back(node);
    }
    size = count(reader);
    for (size_t i = 0; i < size; ++i) {
        reader.compact(); reader.word();
        for (unsigned j = 0; j < 6; ++j) reader.compact();
        reader.skip(20 + (package.version >= 123 && package.licensee >= 23 ? 4 : 0));
    }
    std::vector<int32_t> pool; size = count(reader,4000000);
    for (size_t i = 0; i < size; ++i) { pool.push_back(reader.compact()); reader.compact(); }
    std::vector<Face> result;
    size_t vertexBudget = 0;
    for (const auto& node : nodes) {
        if (node.vertices < 3) continue;
        vertexBudget += node.vertices;
        if (vertexBudget > 2000000) throw std::runtime_error("Water brush exceeds geometry budget");
        if (node.first < 0 || size_t(node.first) > pool.size() || node.vertices > pool.size()-size_t(node.first))
            throw std::runtime_error("Water face outside vertex pool");
        Face face;
        for (unsigned i = 0; i < node.vertices; ++i) {
            const auto ref = pool[size_t(node.first)+i];
            if (ref < 0 || size_t(ref) >= points.size()) throw std::runtime_error("Water vertex outside point table");
            face.push_back(points[size_t(ref)]);
        }
        result.push_back(std::move(face));
    }
    if (result.empty()) throw std::runtime_error("Water brush has no collision faces");
    return result;
}

struct WaterTransform {
    WaterPoint location{}, pivot{}, scale{1,1,1}, rotation{};
    double uniform = 1;
    WaterPoint apply(WaterPoint p) const {
        p = {(p.x-pivot.x)*scale.x*uniform,(p.y-pivot.y)*scale.y*uniform,(p.z-pivot.z)*scale.z*uniform};
        auto angle = [](double value) { return double((uint32_t(int32_t(value)) >> 2) & 16383)*6.2831853071795864769/16384.; };
        const double pitch = angle(rotation.x), yaw = angle(rotation.y), roll = angle(rotation.z);
        const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw), cr = std::cos(roll), sr = std::sin(roll);
        WaterPoint result{float(location.x+p.x*cp*cy+p.y*(cy*sp*sr-cr*sy)-p.z*(cr*cy*sp+sr*sy)),
            float(location.y+p.x*cp*sy+p.y*(cr*cy+sp*sr*sy)+p.z*(cy*sr-cr*sp*sy)),
            float(location.z+p.x*sp-p.y*cp*sr+p.z*cp*cr)};
        for (double c : {result.x,result.y,result.z}) if (!std::isfinite(c) || std::abs(c) > 16777216) throw std::runtime_error("Water world coordinate outside limits");
        return result;
    }
};
static WaterTransform transform(const std::vector<Property>& properties) {
    WaterTransform result;
    for (auto [name,target] : {std::pair{"Location",&result.location},std::pair{"PrePivot",&result.pivot},std::pair{"DrawScale3D",&result.scale}}) {
        if (const auto p = property(properties,name)) {
            if (p->structure != "Vector" || p->value.size() != 12) throw std::runtime_error("Invalid water transform vector");
            Reader reader{p->value}; *target = readPoint(reader);
        }
    }
    if (const auto p = property(properties,"Rotation")) {
        if (p->structure != "Rotator" || p->value.size() != 12) throw std::runtime_error("Invalid water rotation");
        Reader reader{p->value}; result.rotation = {double(std::bit_cast<int32_t>(reader.word())),double(std::bit_cast<int32_t>(reader.word())),double(std::bit_cast<int32_t>(reader.word()))};
    }
    if (const auto p = property(properties,"DrawScale")) {
        if (p->kind != 4 || p->value.size() != 4) throw std::runtime_error("Invalid water scale");
        Reader reader{p->value}; result.uniform = std::bit_cast<float>(reader.word());
    }
    if (!std::isfinite(result.uniform) || result.uniform == 0 || result.scale.x == 0 || result.scale.y == 0 || result.scale.z == 0)
        throw std::runtime_error("Singular water transform");
    return result;
}

using Key = std::pair<int64_t,int64_t>;
using Edge = std::pair<Key,Key>;
static Key key(WaterPoint p) { return {int64_t(std::llround(p.x*1024)),int64_t(std::llround(p.y*1024))}; }
static Edge edge(Key a,Key b) { return a < b ? Edge{a,b} : Edge{b,a}; }
static double area(const Face& face) {
    double value = 0;
    for (size_t i = 0; i < face.size(); ++i) value += face[i].x*face[(i+1)%face.size()].y-face[(i+1)%face.size()].x*face[i].y;
    return value*0.5;
}
static std::map<Edge,unsigned> boundary(const std::vector<Face>& caps) {
    std::map<Edge,unsigned> counts;
    for (const auto& face : caps) for (size_t i = 0; i < face.size(); ++i) {
        const auto a = key(face[i]), b = key(face[(i+1)%face.size()]);
        if (a != b && ++counts[edge(a,b)] > 2) throw std::runtime_error("Non-manifold water cap");
    }
    std::erase_if(counts,[](const auto& item) { return item.second == 2; });
    return counts;
}
static bool contains(const Face& polygon, WaterPoint point) {
    bool inside = false;
    for (size_t i = 0,j = polygon.size()-1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i]; const auto& b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x) inside = !inside;
    }
    return inside;
}
struct WaterZone { std::string actor, name; Face polygon; double low = 0, high = 0; };

static std::vector<WaterZone> prism(const std::vector<Face>& faces,const std::string& actor,const std::string& name) {
    constexpr double tolerance = 0.01;
    double low = std::numeric_limits<double>::max(), high = -low;
    for (const auto& face : faces) for (const auto& p : face) { low = std::min(low,p.z); high = std::max(high,p.z); }
    if (!std::isfinite(low) || !std::isfinite(high) || high-low < tolerance) throw std::runtime_error("Water volume has no usable height");
    std::vector<Face> bottom, top, sides;
    for (const auto& face : faces) {
        if (face.size() < 3) throw std::runtime_error("Invalid water face");
        const bool below = std::all_of(face.begin(),face.end(),[&](const auto& p) { return std::abs(p.z-low) <= tolerance; });
        const bool above = std::all_of(face.begin(),face.end(),[&](const auto& p) { return std::abs(p.z-high) <= tolerance; });
        if (below) bottom.push_back(face); else if (above) top.push_back(face); else sides.push_back(face);
    }
    auto edges = boundary(bottom);
    if (edges.empty() || edges != boundary(top)) throw std::runtime_error("Water is not a vertical prism (sloped or mismatched caps)");
    std::map<Key,std::vector<Key>> neighbors; std::map<Key,WaterPoint> points;
    for (const auto& face : bottom) for (const auto& p : face) points[key(p)] = p;
    for (const auto& [e,n] : edges) { neighbors[e.first].push_back(e.second); neighbors[e.second].push_back(e.first); }
    for (const auto& [p,adjacent] : neighbors) if (adjacent.size() != 2) throw std::runtime_error("Water footprint is not a simple closed loop");
    std::set<Edge> visited; std::vector<WaterZone> result;
    for (const auto& [start,adjacent] : neighbors) {
        if (visited.contains(edge(start,adjacent[0]))) continue;
        Face polygon; Key previous = start, current = adjacent[0]; polygon.push_back(points.at(start));
        visited.insert(edge(start,current));
        while (current != start) {
            polygon.push_back(points.at(current));
            const auto& options = neighbors.at(current); const auto next = options[0] == previous ? options[1] : options[0];
            if (!visited.insert(edge(current,next)).second) throw std::runtime_error("Repeated edge in water footprint");
            previous = current; current = next;
            if (polygon.size() > neighbors.size()) throw std::runtime_error("Water footprint traversal overflow");
        }
        if (polygon.size() < 3 || std::abs(area(polygon)) < 0.01) throw std::runtime_error("Degenerate water footprint");
        if (area(polygon) < 0) std::reverse(polygon.begin(),polygon.end());
        result.push_back({actor,name+"_"+std::to_string(result.size()+1),std::move(polygon),low,high});
    }
    for (size_t i = 0; i < result.size(); ++i) for (size_t j = 0; j < result.size(); ++j)
        if (i != j && contains(result[i].polygon,result[j].polygon[0])) throw std::runtime_error("Water footprint contains holes or nested loops");
    double footprintArea = 0, bottomArea = 0, topArea = 0;
    for (const auto& z : result) footprintArea += area(z.polygon);
    for (const auto& f : bottom) bottomArea += std::abs(area(f));
    for (const auto& f : top) topArea += std::abs(area(f));
    if (std::abs(bottomArea-footprintArea) > std::max(0.1,footprintArea*0.000001) || std::abs(topArea-footprintArea) > std::max(0.1,footprintArea*0.000001))
        throw std::runtime_error("Water cap coverage is incomplete or overlapping");
    // Side faces must cover every boundary wall, not just share the same XY bounds.
    std::map<Edge,double> wallArea;
    for (const auto& face : sides) {
        bool matched = false;
        for (const auto& [e,n] : edges) {
            const auto a = points.at(e.first), b = points.at(e.second);
            const double dx = b.x-a.x, dy = b.y-a.y, length = std::hypot(dx,dy);
            auto project = [&](WaterPoint p) { return ((p.x-a.x)*dx+(p.y-a.y)*dy)/length; };
            if (!std::all_of(face.begin(),face.end(),[&](WaterPoint p) {
                const auto t = project(p); return std::abs((p.x-a.x)*dy-(p.y-a.y)*dx)/length <= tolerance && t >= -tolerance && t <= length+tolerance;
            })) continue;
            double sideArea = 0;
            for (size_t i = 0; i < face.size(); ++i) sideArea += project(face[i])*face[(i+1)%face.size()].z-project(face[(i+1)%face.size()])*face[i].z;
            wallArea[e] += std::abs(sideArea)*0.5; matched = true; break;
        }
        if (!matched) throw std::runtime_error("Sloped/internal water wall is not representable by server zones");
    }
    for (const auto& [e,n] : edges) {
        const auto a = points.at(e.first), b = points.at(e.second);
        const double expected = std::hypot(b.x-a.x,b.y-a.y)*(high-low);
        if (std::abs(wallArea[e]-expected) > std::max(0.1,expected*0.000001)) throw std::runtime_error("Water boundary wall coverage mismatch");
    }
    return result;
}

static bool isWater(const std::string& type,const std::vector<Property>& properties) {
    if (type != "Gameplay.WaterVolume" && type != "Engine.WaterVolume") return false;
    if (const auto deleted = property(properties,"bDeleteMe"); deleted && deleted->boolean) return false;
    if (const auto flag = property(properties,"bWaterVolume")) {
        if (flag->kind != 3) throw std::runtime_error("Invalid water-volume flag");
        return flag->boolean;
    }
    return defaults::waterVolume;
}
static std::vector<Face> waterFaces(const Package& package,const std::vector<Property>& properties) {
    const auto brush = property(properties,"Brush");
    if (!brush || brush->kind != 5) throw std::runtime_error("Water volume has no usable brush reference");
    Reader reference{brush->value}; auto faces = modelFaces(package,reference.compact());
    const auto matrix = transform(properties);
    for (auto& face : faces) for (auto& p : face) p = matrix.apply(p);
    return faces;
}
static Face integerPolygon(const Face& source) {
    Face result;
    for (const auto& p : source) {
        WaterPoint rounded{std::round(p.x),std::round(p.y),0};
        if (result.empty() || result.back().x != rounded.x || result.back().y != rounded.y) result.push_back(rounded);
    }
    if (result.size() > 1 && result.front().x == result.back().x && result.front().y == result.back().y) result.pop_back();
    if (result.size() < 3 || area(result) <= 0) throw std::runtime_error("Water footprint degenerates after integer rounding");
    auto orientation = [](WaterPoint a,WaterPoint b,WaterPoint c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); };
    auto on = [&](WaterPoint a,WaterPoint b,WaterPoint p) { return orientation(a,b,p) == 0 && p.x >= std::min(a.x,b.x) && p.x <= std::max(a.x,b.x) && p.y >= std::min(a.y,b.y) && p.y <= std::max(a.y,b.y); };
    for (size_t i = 0; i < result.size(); ++i) for (size_t j = i+1; j < result.size(); ++j) {
        if (j == i+1 || (i == 0 && j+1 == result.size())) continue;
        const auto a = result[i], b = result[(i+1)%result.size()], c = result[j], d = result[(j+1)%result.size()];
        const double o1 = orientation(a,b,c), o2 = orientation(a,b,d), o3 = orientation(c,d,a), o4 = orientation(c,d,b);
        if (((o1 < 0 && o2 > 0) || (o1 > 0 && o2 < 0)) && ((o3 < 0 && o4 > 0) || (o3 > 0 && o4 < 0)))
            throw std::runtime_error("Water footprint intersects itself after rounding");
        if (on(a,b,c) || on(a,b,d) || on(c,d,a) || on(c,d,b)) throw std::runtime_error("Water footprint touches itself after rounding");
    }
    return result;
}

struct VolumeBounds { WaterPoint low, high; };
static VolumeBounds bounds(const std::vector<Face>& faces) {
    const double maximum = std::numeric_limits<double>::max();
    VolumeBounds result{{maximum,maximum,maximum},{-maximum,-maximum,-maximum}};
    for (const auto& f : faces) for (const auto& p : f) {
        result.low = {std::min(result.low.x,p.x),std::min(result.low.y,p.y),std::min(result.low.z,p.z)};
        result.high = {std::max(result.high.x,p.x),std::max(result.high.y,p.y),std::max(result.high.z,p.z)};
    }
    return result;
}
static bool overlaps(const VolumeBounds& a,const VolumeBounds& b) {
    return std::min(a.high.x,b.high.x)-std::max(a.low.x,b.low.x) > 0.01
        && std::min(a.high.y,b.high.y)-std::max(a.low.y,b.low.y) > 0.01
        && std::min(a.high.z,b.high.z)-std::max(a.low.z,b.low.z) > 0.01;
}
static std::optional<int32_t> priority(const std::vector<Property>& properties,const std::string& type) {
    const auto p = property(properties,"Priority");
    if (!p) {
        if (type=="Gameplay.WaterVolume" || type=="Engine.WaterVolume" || type=="Engine.PhysicsVolume") return defaults::physicsVolumePriority;
        return {};
    }
    if (p->kind != 2 || p->value.size() != 4) throw std::runtime_error("Invalid physics-volume priority");
    Reader reader{p->value}; return std::bit_cast<int32_t>(reader.word());
}

int verifyWaterCore() {
    auto require = [](bool value,const char* why) { if (!value) throw std::runtime_error(why); };
    auto rejects = [&](auto operation,const char* why) {
        bool failed = false; try { operation(); } catch (const std::exception&) { failed = true; }
        require(failed,why);
    };
    auto solid = [](Face polygon,double low,double high) {
        std::vector<Face> faces; Face bottom = polygon, top = polygon;
        for (auto& p : bottom) p.z = low; for (auto& p : top) p.z = high;
        std::reverse(bottom.begin(),bottom.end()); faces.push_back(bottom); faces.push_back(top);
        for (size_t i = 0; i < polygon.size(); ++i) {
            auto a = polygon[i], b = polygon[(i+1)%polygon.size()]; a.z = low; b.z = low;
            faces.push_back({a,b,{b.x,b.y,high},{a.x,a.y,high}});
        }
        return faces;
    };
    const Face square{{0,0,0},{4,0,0},{4,4,0},{0,4,0}};
    const auto box = solid(square,-10,20); const auto zones = prism(box,"WaterVolume","Map_WaterVolume");
    require(zones.size() == 1 && area(zones[0].polygon) == 16 && zones[0].low == -10 && zones[0].high == 20,"Water box regression");
    const Face concave{{0,0,0},{4,0,0},{4,1,0},{1,1,0},{1,4,0},{0,4,0}};
    const auto lShape = prism(solid(concave,-10,20),"Concave","Map_Concave");
    require(lShape.size() == 1 && area(lShape[0].polygon) == 7 && !contains(lShape[0].polygon,{2,2,0}),"Concave water footprint was enlarged");
    const std::vector<std::array<int,2>> offPolygon{{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}};
    const auto offParts=off::boxes(offPolygon);
    require(offParts.size()==2,"L2OFF concave pool did not split into exact rectangles");
    for (int x=0;x<4;++x) for (int y=0;y<4;++y) {
        bool wet=false;
        for (const auto& b:offParts) wet|=x+0.5>b.minX && x+0.5<b.maxX && y+0.5>b.minY && y+0.5<b.maxY;
        require(wet==contains(concave,{x+0.5,y+0.5,0}),"L2OFF rectangular decomposition flooded dry space");
    }
    auto reverseOff=offPolygon; std::reverse(reverseOff.begin(),reverseOff.end());
    require(off::boxes(reverseOff).size()==2,"L2OFF reversed winding failed");
    rejects([&] { off::boxes({{0,0},{4,1},{3,5},{-1,4}}); },"L2OFF rotated pool silently enlarged");
    require(off::identifier("a]\r\n[bad") == "a____bad","L2OFF identifier injection accepted");
    auto missing = box; missing.pop_back();
    rejects([&] { prism(missing,"Missing","Missing"); },"Missing water wall accepted");
    auto sloped = box;
    for (auto& face : sloped) for (auto& p : face) if (p.z == -10) p.z += p.x;
    rejects([&] { prism(sloped,"Slope","Slope"); },"Sloped water silently boxed");
    auto nested = box; auto inner = solid({{1,1,0},{3,1,0},{3,3,0},{1,3,0}},-10,20);
    nested.insert(nested.end(),inner.begin(),inner.end());
    rejects([&] { prism(nested,"Hole","Hole"); },"Nested water loops accepted");
    auto disjoint = box; auto island = solid({{10,0,0},{14,0,0},{14,4,0},{10,4,0}},-10,20);
    disjoint.insert(disjoint.end(),island.begin(),island.end());
    require(prism(disjoint,"Parts","Parts").size() == 2,"Separate water components lost");
    WaterTransform matrix; matrix.location = {10,20,30}; matrix.pivot = {1,2,3}; matrix.scale = {2,3,4}; matrix.rotation.y = 16384;
    const auto p = matrix.apply({2,4,6});
    require(std::abs(p.x-4) < 0.0001 && std::abs(p.y-22) < 0.0001 && std::abs(p.z-42) < 0.0001,"Water transform regression");
    rejects([&] { integerPolygon({{0,0,0},{0.1,0,0},{0,0.1,0}}); },"Water rounding collapse accepted");
    require(isWater("Gameplay.WaterVolume",{}) && !isWater("Engine.PhysicsVolume",{}),"Water classification regression");
    require(priority({},"Gameplay.WaterVolume")==0 && priority({},"Engine.PhysicsVolume")==0 && !priority({},"Custom.PhysicsVolume"),"Bundled physics-volume priority regression");
    Property rank; rank.name="Priority"; rank.kind=2; rank.value={7,0,0,0};
    require(priority({rank},"Gameplay.WaterVolume")==7,"Serialized water priority did not override bundled defaults");
    Property dry; dry.name = "bWaterVolume"; dry.kind = 3; dry.boolean = false;
    require(!isWater("Gameplay.WaterVolume",{dry}),"Dry water override ignored");
    require(overlaps(bounds(box),bounds(inner)) && !overlaps(bounds(box),bounds(island)),"Dry overlap detection regression");
    std::vector<uint8_t> invalid{0x81}; Reader reader{invalid};
    rejects([&] { count(reader); },"Negative model count accepted");
    std::cout << "Water core checks passed: boxes, concavity, transforms, slopes, holes, walls, rounding, dry overlaps and exact L2OFF rectangles\n";
    return 0;
}

int exportWater(const std::filesystem::path& client,const std::filesystem::path& output,unsigned workers,bool classic) {
    namespace fs = std::filesystem;
    auto root = fs::canonical(client); fs::path singleMap;
    if (fs::is_regular_file(root) && _wcsicmp(root.extension().c_str(),L".unr") == 0) { singleMap = root; root = root.parent_path(); }
    if (fs::is_directory(root / "Maps")) root /= "Maps";
    else if (_wcsicmp(root.filename().c_str(),L"System") == 0 && fs::is_directory(root.parent_path() / "Maps")) root = root.parent_path() / "Maps";
    if (fs::exists(output)) throw std::runtime_error("Water output folder already exists; choose a new one");
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    const auto resources = fs::path(executable).parent_path() / "profiles";
    const std::array<std::string,4> profiles{"l2jserver","acis","l2scripts","mobius"};
    std::array<std::string,4> documents,entries,nodeTemplates;
    for (size_t i = 0; i < profiles.size(); ++i) {
        documents[i] = output::loadText(resources / profiles[i] / "water.xml.tpl");
        entries[i] = output::loadText(resources / profiles[i] / "zone.xml.tpl");
        nodeTemplates[i] = output::loadText(resources / profiles[i] / "water-node.xml.tpl");
    }
    const auto offDocument=output::loadText(resources/"l2off"/"water.txt.tpl"),offEntry=output::loadText(resources/"l2off"/"zone.txt.tpl");
    std::cerr << "STAGE selecting maps | matching selected files\n";
    auto maps = selectMaps(singleMap.empty() ? root : singleMap,classic);
    if (maps.empty()) throw std::runtime_error("No game maps found");
    fs::create_directories(output);
    if (debugFiles) output::save(output / "911reg-map-sources.tsv",mapSources(maps,root));
    std::atomic<size_t> next{0}, total{0}, rejected{0}, accepted{0}, badMaps{0};
    std::atomic<size_t> offRejected{0},offBoxes{0};
    std::vector<std::string> notes(maps.size());
    std::vector<std::array<std::string,4>> combined(maps.size()); std::mutex progress; size_t completed = 0;
    std::cerr << "Found " << maps.size() << " maps. Extracting water volumes...\n";
    {
        std::vector<std::jthread> pool;
        for (unsigned thread = 0; thread < std::min<size_t>(workers,maps.size()); ++thread) pool.emplace_back([&] {
            for (;;) {
                const auto mapIndex = next.fetch_add(1); if (mapIndex >= maps.size()) break;
                { std::lock_guard lock(progress); std::cerr << "MAP " << output::utf8(maps[mapIndex].filename()) << " | WaterVolume collision\n"; }
                std::ostringstream report, rawFaces, rawZones; std::vector<WaterZone> zones;
                rawFaces << "actor\tface\tvertex\tx\ty\tz\n" << std::setprecision(17);
                rawZones << "name\tactor\tnode\tx\ty\tminZ\tmaxZ\tsourceMinZ\tsourceMaxZ\n" << std::setprecision(17);
                size_t mapAccepted = 0, candidateCount = 0;
                try {
                    const auto package = Package::open(maps[mapIndex]);
                    struct DryVolume { std::string name; VolumeBounds shape; std::optional<int32_t> priority; };
                    std::vector<DryVolume> dryVolumes; bool unresolvedDry = false;
                    for (size_t i = 0; i < package.exports.size(); ++i) {
                        const auto type = package.className(i);
                        if (type.find("PhysicsVolume") == std::string::npos && type.find("AirVolume") == std::string::npos) continue;
                        try {
                            const auto properties = package.properties(i);
                            if (const auto deleted = property(properties,"bDeleteMe"); deleted && deleted->boolean) continue;
                            const auto wet = property(properties,"bWaterVolume");
                            if ((wet && wet->kind == 3 && wet->boolean) || !property(properties,"Brush")) continue;
                            dryVolumes.push_back({package.exports[i].name,bounds(waterFaces(package,properties)),priority(properties,type)});
                        } catch (const std::exception& error) {
                            unresolvedDry = true; ++rejected;
                            report << output::utf8(maps[mapIndex].filename()) << " / " << package.exports[i].name << ": Cannot resolve dry-volume geometry: " << error.what() << '\n';
                        }
                    }
                    for (size_t i = 0; i < package.exports.size(); ++i) {
                        const auto type = package.className(i);
                        if (type != "Gameplay.WaterVolume" && type != "Engine.WaterVolume") continue;
                        try {
                            const auto properties = package.properties(i);
                            if (!isWater(type,properties)) continue;
                            ++candidateCount;
                            const auto faces = waterFaces(package,properties);
                            for (size_t f = 0; f < faces.size(); ++f) for (size_t v = 0; v < faces[f].size(); ++v) {
                                const auto& p = faces[f][v]; rawFaces << package.exports[i].name << '\t' << f << '\t' << v << '\t' << p.x << '\t' << p.y << '\t' << p.z << '\n';
                            }
                            if (unresolvedDry) throw std::runtime_error("Unresolved dry volume prevents safe water export for this map");
                            const auto waterBounds = bounds(faces);
                            const auto waterPriority = priority(properties,type);
                            for (const auto& dry : dryVolumes) {
                                if (dry.priority && waterPriority && *dry.priority < *waterPriority) continue;
                                if (overlaps(waterBounds,dry.shape)) throw std::runtime_error("Potential dry-volume priority overlap requires subtraction: " + dry.name);
                            }
                            auto parts = prism(faces,package.exports[i].name,output::utf8(mapOutput(maps[mapIndex],root).stem())+"_"+package.exports[i].name);
                            for (auto& part : parts) {
                                part.polygon = integerPolygon(part.polygon);
                                if (std::floor(part.high) <= std::ceil(part.low)) throw std::runtime_error("Water height disappears at integer precision");
                            }
                            zones.insert(zones.end(),parts.begin(),parts.end()); ++mapAccepted;
                        } catch (const std::exception& error) {
                            ++rejected; report << output::utf8(maps[mapIndex].filename()) << " / " << package.exports[i].name << ": " << error.what() << '\n';
                        }
                    }
                    auto relative = mapOutput(maps[mapIndex],root); relative.replace_extension(L".xml");
                    if (candidateCount) {
                        auto rawPath = relative; rawPath.replace_extension(L".faces.tsv"); if (debugFiles) output::save(output / "raw" / rawPath,rawFaces.str());
                    }
                    if (!zones.empty()) {
                        for (const auto& zone : zones) for (size_t node = 0; node < zone.polygon.size(); ++node)
                            rawZones << zone.name << '\t' << zone.actor << '\t' << node << '\t' << zone.polygon[node].x << '\t' << zone.polygon[node].y << '\t'
                                << std::ceil(zone.low) << '\t' << std::floor(zone.high) << '\t' << zone.low << '\t' << zone.high << '\n';
                        auto rawPath = relative; rawPath.replace_extension(L".zones.tsv"); if (debugFiles) output::save(output / "raw" / rawPath,rawZones.str());
                        for (size_t profile = 0; profile < profiles.size(); ++profile) {
                            if (!profileEnabled(profiles[profile])) continue;
                            std::string body;
                            for (const auto& zone : zones) {
                                const auto minZ = std::to_string(int(std::ceil(zone.low))), maxZ = std::to_string(int(std::floor(zone.high)));
                                std::string nodes;
                                for (const auto& p : zone.polygon) nodes += output::render(nodeTemplates[profile],{
                                    {"x",std::to_string(int(p.x))},{"y",std::to_string(int(p.y))},{"minZ",minZ},{"maxZ",maxZ}});
                                body += output::render(entries[profile],{{"name",output::xml(zone.name)},{"minZ",minZ},{"maxZ",maxZ},{"nodes",nodes}});
                            }
                            if (combineXml) combined[mapIndex][profile]=std::move(body);
                            else output::save(profileFolder(output,profiles[profile]) / relative,output::render(documents[profile],{{"zones",body}}));
                        }
                        if (profileEnabled("l2off")) {
                        std::string offBody;
                        const auto stem=mapOutput(maps[mapIndex],root).stem().wstring(); std::wsmatch region;
                        const bool numeric=std::regex_match(stem,region,std::wregex(LR"(^([0-9]{1,2})_([0-9]{1,2})$)"));
                        for (const auto& zone:zones) {
                            try {
                                if (!numeric) throw std::runtime_error("L2OFF map_no requires a numeric XX_YY map name");
                                std::vector<std::array<int,2>> polygon;
                                for (const auto& p:zone.polygon) polygon.push_back({int(p.x),int(p.y)});
                                const auto boxes=off::boxes(polygon);
                                for (size_t i=0;i<boxes.size();++i) {
                                    const auto& b=boxes[i];
                                    offBody+=output::render(offEntry,{
                                        {"name",off::identifier(zone.name)+"_box_"+std::to_string(i+1)},
                                        {"mapX",std::to_string(std::stoi(region[1]))},{"mapY",std::to_string(std::stoi(region[2]))},
                                        {"minX",std::to_string(b.minX)},{"minY",std::to_string(b.minY)},
                                        {"maxX",std::to_string(b.maxX)},{"maxY",std::to_string(b.maxY)},
                                        {"minZ",std::to_string(int(std::ceil(zone.low)))},{"maxZ",std::to_string(int(std::floor(zone.high)))}});
                                }
                                offBoxes+=boxes.size();
                            } catch (const std::exception& error) {
                                ++offRejected; report << output::utf8(maps[mapIndex].filename()) << " / " << zone.actor << " / L2OFF only: " << error.what() << '\n';
                            }
                        }
                        if (!offBody.empty()) {
                            auto offPath=relative; offPath.replace_extension(L".txt");
                            off::save(profileFolder(output,"l2off")/offPath,output::render(offDocument,{{"zones",offBody}}));
                        }
                        }
                        total += zones.size(); accepted += mapAccepted;
                    }
                } catch (const std::exception& error) { ++badMaps; report << output::utf8(maps[mapIndex].filename()) << ": " << error.what() << '\n'; }
                notes[mapIndex] = report.str(); std::lock_guard lock(progress);
                std::cerr << "Checked " << ++completed << " of " << maps.size() << " maps\n";
            }
        });
    }
    if (combineXml) {
        for (int variant=0;variant<2;++variant) {
            std::array<std::string,4> bodies;
            bool present=false;
            for (size_t i=0;i<maps.size();++i) {
                if (int(allVariants && classicMap(maps[i]))!=variant) continue;
                present=true;
                for (size_t p=0;p<profiles.size();++p) bodies[p]+=combined[i][p];
            }
            if (present) for (size_t p=0;p<profiles.size();++p) if (profileEnabled(profiles[p])) output::save(profileFolder(output,profiles[p])/(variant ? "Classic/water.xml" : "water.xml"),output::render(documents[p],{{"zones",bodies[p]}}));
        }
    }
    std::ostringstream report;
    report << "911reg MapExtractor - water extraction\n911reg found the wet bits.\n\nWater zones exported: " << total
        << "\nVolumes accepted: " << accepted << "\nRejected candidates: " << rejected << "\nUnreadable/failed maps: " << badMaps
        << "\nL2OFF boxes: " << offBoxes << "\nL2OFF-only rejected zones: " << offRejected
        << "\n\nBoundaries come from transformed client collision brushes, not water textures.\n"
        << "Only closed vertical prisms are exported. Slopes, holes, invalid caps/walls and rounding failures are reported.\n"
        << "XY rounds to nearest integer; minZ=ceil(bottom), maxZ=floor(top). Original world faces are in raw/*.faces.tsv.\n"
        << "Potential dry-volume overrides at higher/unknown priority are rejected, not silently flooded.\n"
        << "General class inheritance, legacy water ZoneInfo geometry and dry-volume subtraction are not implemented.\n"
        << "Review server loader placement before merging per-map files; aCis water output belongs in its WaterZone loader file.\n"
        << "L2OFF UTF-16LE fragments merge into areadata.txt. Exact axis-aligned rectangle decomposition only; no bounding-box enlargement.\n"
        << "Slanted water footprints remain available in XML profiles but are reported as L2OFF-only omissions.\n"
        << "Classic selection uses normal region output names; see 911reg-map-sources.tsv. No live client/server files were changed.\n\n";
    for (const auto& note : notes) report << note;
    if (debugFiles) output::save(output / "911reg-water-report.txt",report.str());
    writeExportReadme(output,"WATER");
    for (const auto& note:notes) { std::istringstream lines(note); std::string line; while (std::getline(lines,line)) if (!line.empty()) std::cerr<<"SKIPPED: "<<line<<'\n'; }
    std::cerr<<"RESULT water "<<total<<' '<<rejected<<' '<<badMaps<<' '<<offRejected<<" 0\n";
    std::cerr << "Water zones exported: " << total << ". Need attention: " << rejected+badMaps+offRejected << ".\n";
    return rejected || badMaps || offRejected ? 2 : 0;
}
int dumpWater(const std::filesystem::path& map) {
    const auto package = Package::open(map);
    for (size_t i = 0; i < package.exports.size(); ++i) {
        const auto type = package.className(i);
        if (type != "Gameplay.WaterVolume" && type != "Engine.WaterVolume") continue;
        std::cout << package.exports[i].name << " class=" << type << '\n';
        for (const auto& p : package.properties(i)) {
            std::cout << "  " << p.name << " type=" << unsigned(p.kind) << " struct=" << p.structure << " size=" << p.value.size();
            Reader reader{p.value};
            if (p.kind == 3) std::cout << " bool=" << p.boolean;
            if (p.kind == 5) {
                const auto ref = reader.compact(); std::cout << " ref=" << ref << ' ' << package.objectPath(ref);
                if (p.name == "Brush" && ref > 0) for (size_t j = 0; j < package.exports.size(); ++j)
                    if (package.exports[j].outer == ref) std::cout << " child=" << j+1 << ':' << package.className(j);
                if (p.name == "Brush" && ref > 0) {
                    size_t offset = 0; package.properties(size_t(ref-1),&offset);
                    const auto& object = package.exports[size_t(ref-1)];
                    Reader native{package.data,offset,object.offset+object.size}; native.skip(41);
                    auto count = native.compact(); std::cout << " vectors=" << count; native.skip(size_t(count)*12);
                    count = native.compact(); std::cout << " points=" << count; native.skip(size_t(count)*12);
                    std::cout << " nodes=" << native.compact();
                }
            }
            if (p.structure == "Vector" && p.value.size() == 12)
                for (unsigned k = 0; k < 3; ++k) std::cout << ' ' << std::setprecision(17) << std::bit_cast<float>(reader.word());
            std::cout << '\n';
        }
    }
    return 0;
}
}
