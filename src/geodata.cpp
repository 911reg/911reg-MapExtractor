#include "geodata.h"
#include "export_utils.h"
#include "collision.h"
#include "map_selection.h"
#include "export_readme.h"
#include "water.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <limits>
#include <set>
#include <sstream>
#include <thread>

// One region is 2048 x 2048 cells of 16 world units, grouped into 256 x 256
// blocks of 8 x 8. L2J and L2OFF are the same cells: a height truncated onto
// a multiple of 8, plus four direction bits. East is 1 (+X), West is 2 (-X),
// South is 4 (+Y), North is 8 (-Y). A set bit is walkable. The server does
// not measure the step again.
//
// Triangle::kind comes from the collision reader: 0 static mesh, 1 terrain,

namespace extractor {
namespace {
using V = collision::Point;
using Triangle = collision::Triangle;

V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Layer {
    int z = 0;
    unsigned flags = 0;
    float surface = 0;
    int floor = 0, ceiling = 65535;
    bool steep = false, terrain = false, reach = false, wet = false;
};
using Cell = std::vector<Layer>;
using Block = std::array<Cell, 64>;

struct Geo {
    double ox = 0, oy = 0;
    std::vector<Triangle> triangles;
    std::vector<V> seeds;
    bool boundaryOnly=false;
    std::map<std::pair<int,int>,Cell> halo;
    std::array<std::vector<uint32_t>, 65536> index;
    std::array<std::unique_ptr<Block>, 65536> blocks;
    // Cell index inside a block is (x % 8) * 8 + (y % 8). Blocks are stored as blockX * 256 + blockY.
    const Cell& cell(int x, int y) const {
        static const Cell empty;
        if (x < 0 || y < 0 || x >= 2048 || y >= 2048) { const auto found=halo.find({x,y}); return found==halo.end() ? empty : found->second; }
        const auto& block = blocks[size_t(x / 8) * 256 + y / 8];
        return block ? (*block)[size_t(x % 8) * 8 + y % 8] : empty;
    }
};

// Separating-axis test used by the geometry self-check. Generation uses the polygon clip instead.
bool boxHit(const Triangle& triangle, V low, V high) {
    const V center{(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
    const V half{(high.x - low.x) / 2, (high.y - low.y) / 2, (high.z - low.z) / 2};
    const std::array<V, 3> points{triangle.a - center, triangle.b - center, triangle.c - center};
    auto separated = [&](V axis) {
        const double radius = std::abs(axis.x) * half.x + std::abs(axis.y) * half.y + std::abs(axis.z) * half.z;
        const double a = dot(points[0], axis), b = dot(points[1], axis), c = dot(points[2], axis);
        return std::min({a, b, c}) > radius + 0.000001 || std::max({a, b, c}) < -radius - 0.000001;
    };
    const std::array<V, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (auto axis : axes) if (separated(axis)) return false;
    if (separated(triangle.normal)) return false;
    for (size_t i = 0; i < 3; ++i) for (auto axis : axes) if (separated(cross(points[(i + 1) % 3] - points[i], axis))) return false;
    return true;
}

// OBJ is only the geodata-fixture harness. The window and the geodata command read UNR packages.
std::vector<Triangle> loadObj(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 512ull * 1024 * 1024) throw std::runtime_error("Collision OBJ exceeds 512 MiB");
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read collision OBJ");
    std::vector<V> vertices;
    std::vector<Triangle> triangles;
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() > 65536) throw std::runtime_error("OBJ line exceeds limit");
        std::istringstream fields(line);
        std::string type;
        fields >> type;
        if (type == "v") {
            V vertex;
            if (!(fields >> vertex.x >> vertex.y >> vertex.z)) throw std::runtime_error("Invalid OBJ vertex");
            for (double coordinate : {vertex.x, vertex.y, vertex.z}) if (!std::isfinite(coordinate) || std::abs(coordinate) > 16777216) throw std::runtime_error("OBJ coordinate outside bounds");
            vertices.push_back(vertex);
            if (vertices.size() > 2000000) throw std::runtime_error("OBJ vertex budget exceeded");
        } else if (type == "f") {
            std::array<V, 3> points;
            for (auto& point : points) {
                std::string token;
                if (!(fields >> token)) throw std::runtime_error("OBJ requires triangle faces");
                token = token.substr(0, token.find('/'));
                size_t used = 0;
                const auto index = std::stoll(token, &used);
                if (used != token.size() || !index) throw std::runtime_error("Invalid OBJ face index");
                const auto actual = index < 0 ? int64_t(vertices.size()) + index : index - 1;
                if (actual < 0 || uint64_t(actual) >= vertices.size()) throw std::runtime_error("OBJ face index outside vertices");
                point = vertices[size_t(actual)];
            }
            std::string extra;
            if (fields >> extra && !extra.starts_with('#')) throw std::runtime_error("Triangulate OBJ faces before importing");
            const auto normal = cross(points[1] - points[0], points[2] - points[0]);
            if (dot(normal, normal) < 0.00000001) throw std::runtime_error("Degenerate collision triangle");
            triangles.push_back({points[0], points[1], points[2], normal});
            if (triangles.size() > 4000000) throw std::runtime_error("OBJ triangle budget exceeded");
        } else if (!type.empty() && type[0] != '#' && type != "vt" && type != "vn" && type != "o" && type != "g" && type != "s" && type != "usemtl" && type != "mtllib")
            throw std::runtime_error("Unsupported OBJ record: " + type);
    }
    if (triangles.empty()) throw std::runtime_error("Collision OBJ contains no triangles");
    return triangles;
}

// 128-unit buckets. A triangle is listed in every bucket its bounds touch, then clipped per 16-unit cell.
void buildIndex(Geo& geo) {
    size_t assignments = 0;
    for (size_t i = 0; i < geo.triangles.size(); ++i) {
        const auto& triangle = geo.triangles[i];
        const double minX = std::min({triangle.a.x, triangle.b.x, triangle.c.x}) - geo.ox;
        const double maxX = std::max({triangle.a.x, triangle.b.x, triangle.c.x}) - geo.ox;
        const double minY = std::min({triangle.a.y, triangle.b.y, triangle.c.y}) - geo.oy;
        const double maxY = std::max({triangle.a.y, triangle.b.y, triangle.c.y}) - geo.oy;
        if (maxX < 0 || maxY < 0 || minX >= 32768 || minY >= 32768) continue;
        for (int x = std::clamp(int(std::floor(minX / 128)), 0, 255); x <= std::clamp(int(std::floor(maxX / 128)), 0, 255); ++x)
            for (int y = std::clamp(int(std::floor(minY / 128)), 0, 255); y <= std::clamp(int(std::floor(maxY / 128)), 0, 255); ++y) {
                if (geo.boundaryOnly && x!=0 && y!=0 && x!=255 && y!=255) continue;
                if (++assignments > 16000000) throw std::runtime_error("Collision spatial-index budget exceeded");
                geo.index[size_t(x) * 256 + y].push_back(uint32_t(i));
            }
    }
}

template<class Work>
void parallel(unsigned workers, Work work, const char* stage = nullptr) {
    std::atomic<size_t> next{0}, completed{0};
    std::exception_ptr failure;
    std::mutex guard;
    std::vector<std::jthread> pool;
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back([&] {
        try {
            for (;;) {
                const auto block = next.fetch_add(1);
                if (block >= 65536) break;
                work(block);
                const auto done = completed.fetch_add(1) + 1;
                if (stage && done % 8192 == 0) {
                    std::lock_guard lock(guard);
                    std::cerr << "STAGE " << stage << " | blocks " << done << "/65536\n";
                }
            }
        } catch (...) {
            std::lock_guard lock(guard);
            if (!failure) failure = std::current_exception();
            next = 65536;
        }
    });
    pool.clear();
    if (failure) std::rethrow_exception(failure);
}

// Retail surface offsets, applied before truncation toward zero onto a multiple of 8.
constexpr float terrainOffset = 26.0f, meshOffset = 26.5f, modelOffset = 26.875f, modelTopOffset = 29.375f;
constexpr float steepClimb = 3.125f, steepSlopeLimit = 0.3f;
constexpr int headroom = 46, mergeGap = 3, voidHeight = -16352;

float walkUp() {
    static const float value = std::cos(45.573f * 3.14159265f / 180.f);
    return value;
}

int quantize8(float z) {
    if (!std::isfinite(z) || z < -16384.f || z >= 16384.f) throw std::runtime_error("Walkable height outside L2J range (" + std::to_string(z) + ")");
    return static_cast<int>(z) / 8 * 8;
}

// Sutherland-Hodgman clip of one half-plane. sign selects the kept side of `value` on `axis`.
int clipPoly(const float* in, int count, float* out, int axis, float value, float sign) {
    int written = 0;
    for (int i = 0, previous = count - 1; i < count; previous = i++) {
        const float from = sign * (in[previous * 3 + axis] - value);
        const float to = sign * (in[i * 3 + axis] - value);
        if ((from >= 0.f) != (to >= 0.f)) {
            const float t = from / (from - to);
            for (int component = 0; component < 3; ++component) out[written * 3 + component] = in[previous * 3 + component] + (in[i * 3 + component] - in[previous * 3 + component]) * t;
            ++written;
        }
        if (to >= 0.f) {
            out[written * 3] = in[i * 3];
            out[written * 3 + 1] = in[i * 3 + 1];
            out[written * 3 + 2] = in[i * 3 + 2];
            ++written;
        }
    }
    return written;
}

struct Sample {
    int smin = 0, smax = 0;
    float z = 0;
    unsigned char kind = 0;
    bool steep = false,center=true,surface=true;
    uint32_t object=0; bool closed=false,spanOnly=false; float ray=0; int direction=0;
};

float sampleHeight(const Sample& sample, bool top) {
    if (!std::isfinite(sample.z)) return -std::numeric_limits<float>::infinity();
    if (!retailHeightOffsets) return sample.z;
    if (sample.kind == 1) return sample.z + terrainOffset;
    if (sample.kind == 2 || sample.kind == 3) return sample.z + (top ? modelTopOffset : modelOffset);
    return sample.z + meshOffset;
}

// Clip the triangle to the 16x16 cell. A flat clip that lies on one cell edge is a boundary leak and is dropped.
// A thin diagonal sliver is kept: that is what fills the open terrain cells.
void addSample(std::vector<Sample>& samples, const Triangle& triangle, float x0, float y0) {
    float first[24], second[24];
    const V* points[3]{&triangle.a, &triangle.b, &triangle.c};
    for (int i = 0; i < 3; ++i) {
        first[i * 3] = float(points[i]->x);
        first[i * 3 + 1] = float(points[i]->y);
        first[i * 3 + 2] = float(points[i]->z);
    }
    int count = clipPoly(first, 3, second, 0, x0, 1.f);
    count = clipPoly(second, count, first, 0, x0 + 16.f, -1.f);
    count = clipPoly(first, count, second, 1, y0, 1.f);
    count = clipPoly(second, count, first, 1, y0 + 16.f, -1.f);
    if (count < 3) return;
    float lo = first[2], hi = first[2], minX = first[0], maxX = first[0], minY = first[1], maxY = first[1];
    for (int i = 0; i < count; ++i) {
        lo = std::min(lo, first[i * 3 + 2]);
        hi = std::max(hi, first[i * 3 + 2]);
        minX = std::min(minX, first[i * 3]);
        maxX = std::max(maxX, first[i * 3]);
        minY = std::min(minY, first[i * 3 + 1]);
        maxY = std::max(maxY, first[i * 3 + 1]);
    }
    if (hi - lo < 0.05f && (maxX - minX < 0.05f || maxY - minY < 0.05f)) return;
    const double length = std::sqrt(dot(triangle.normal, triangle.normal));
    const bool steep = length <= 0 || std::abs(triangle.normal.z) / length < walkUp();
    float z = hi; bool center=true;
    if (triangle.kind == 1) {
        // Terrain uses the height at the cell center. If the center misses the triangle, use the clip's average Z.
        const double denominator = (triangle.b.y - triangle.c.y) * (triangle.a.x - triangle.c.x) + (triangle.c.x - triangle.b.x) * (triangle.a.y - triangle.c.y);
        const double centerX = double(x0) + 8.0, centerY = double(y0) + 8.0;
        bool miss = std::abs(denominator) < 0.00000001;
        if (!miss) {
            const double l0 = ((triangle.b.y - triangle.c.y) * (centerX - triangle.c.x) + (triangle.c.x - triangle.b.x) * (centerY - triangle.c.y)) / denominator;
            const double l1 = ((triangle.c.y - triangle.a.y) * (centerX - triangle.c.x) + (triangle.a.x - triangle.c.x) * (centerY - triangle.c.y)) / denominator;
            const double l2 = 1.0 - l0 - l1;
            miss = l0 < -0.0001 || l1 < -0.0001 || l2 < -0.0001;
            if (!miss) z = float(l0 * triangle.a.z + l1 * triangle.b.z + l2 * triangle.c.z);
        }
        center=!miss;
        if (miss) {
            z = 0;
            for (int i = 0; i < count; ++i) z += first[i * 3 + 2];
            z /= float(count);
        }
    }
    const int smin = int(std::floor(lo));
    const int smax = std::max(smin + 1, int(std::ceil(hi)));
    Sample sample{smin,smax,z,triangle.kind,steep,center,triangle.normal.z>=-1e-8};
    sample.object=triangle.object; sample.closed=triangle.closed;
    sample.direction=triangle.normal.z>1e-8 ? 1 : triangle.normal.z<-1e-8 ? -1 : 0;
    if (sample.closed && sample.direction) {
        const double x=double(x0)+8,y=double(y0)+8;
        const double denominator=(triangle.b.y-triangle.c.y)*(triangle.a.x-triangle.c.x)+(triangle.c.x-triangle.b.x)*(triangle.a.y-triangle.c.y);
        const double a=((triangle.b.y-triangle.c.y)*(x-triangle.c.x)+(triangle.c.x-triangle.b.x)*(y-triangle.c.y))/denominator;
        const double b=((triangle.c.y-triangle.a.y)*(x-triangle.c.x)+(triangle.a.x-triangle.c.x)*(y-triangle.c.y))/denominator;
        sample.center=a>=-1e-7 && b>=-1e-7 && a+b<=1.0000001;
        sample.ray=float(a*triangle.a.z+b*triangle.b.z+(1-a-b)*triangle.c.z);
    }
    samples.push_back(sample);
}

void rasterize(Geo& geo, unsigned workers) {
    parallel(workers, [&](size_t blockIndex) {
        if (geo.index[blockIndex].empty()) return;
        auto block = std::make_unique<Block>();
        bool used = false;
        for (int x = 0; x < 8; ++x) for (int y = 0; y < 8; ++y) {
            const float x0 = float(geo.ox + (double(blockIndex / 256) * 8 + x) * 16);
            const float y0 = float(geo.oy + (double(blockIndex % 256) * 8 + y) * 16);
            std::vector<Sample> samples;
            for (auto ref : geo.index[blockIndex]) {
                addSample(samples, geo.triangles[ref], x0, y0);
                if (samples.size() > 4096) throw std::runtime_error("Cell collision complexity exceeds budget");
            }
            if (samples.empty()) continue;
            std::map<uint32_t,std::vector<std::pair<float,int>>> objects;
            for (const auto& sample:samples) if (sample.closed && sample.center && sample.direction) objects[sample.object].push_back({sample.ray,sample.direction});
            for (auto& [object,hits]:objects) {
                std::sort(hits.begin(),hits.end()); int winding=0; float low=0;
                for (size_t k=0;k<hits.size();) {
                    const float z=hits[k].first; int direction=0;
                    do { direction+=hits[k++].second; } while (k<hits.size() && std::abs(hits[k].first-z)<1e-4);
                    if (direction<0) { if (!winding) low=z; ++winding; }
                    else if (direction>0 && winding>0 && !--winding && z>low) {
                        Sample solid; solid.smin=int(std::floor(low)); solid.smax=int(std::ceil(z)); solid.spanOnly=true; samples.push_back(solid);
                    }
                }
            }
            std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
                return a.smin < b.smin || (a.smin == b.smin && a.smax < b.smax);
            });
            // Solid extents merge when the next one starts within 3 units. A gap under 46 units of headroom drops the lower span.
            struct Span { int smin, smax; };
            std::vector<Span> spans;
            for (const auto& sample : samples) {
                if (!spans.empty() && sample.smin <= spans.back().smax + mergeGap) spans.back().smax = std::max(spans.back().smax, sample.smax);
                else spans.push_back({sample.smin, sample.smax});
            }
            auto& cell = (*block)[size_t(x) * 8 + y];
            for (size_t i = 0; i < spans.size(); ++i) {
                const int ceiling = i + 1 < spans.size() ? spans[i + 1].smin : 65535;
                if (i + 1 < spans.size() && spans[i + 1].smin - spans[i].smax < headroom) continue;
                const bool topSpan = i + 1 == spans.size();
                const Sample* top = nullptr;
                auto height = -std::numeric_limits<float>::infinity();
                const bool centered=std::any_of(samples.begin(),samples.end(),[&](const Sample& sample) { return sample.kind==1 && sample.center && sample.smin>=spans[i].smin-1 && sample.smax<=spans[i].smax+1; });
                for (const auto& sample : samples) {
                    if (sample.spanOnly) continue;
                    if (centered && sample.kind==1 && !sample.center) continue;
                    if (sample.smin < spans[i].smin - 1 || sample.smax > spans[i].smax + 1) continue;
                    const auto sampleZ = sampleHeight(sample, topSpan);
                    const bool terrain = sample.kind == 1;
                    if (sampleZ > height + 0.01f || (top && sampleZ >= height - 0.01f && !terrain && top->steep && !sample.steep)) top = &sample;
                    height = std::max(height, sampleZ);
                }
                if (!top || !std::isfinite(height)) {
                    height = float(spans[i].smax) + (retailHeightOffsets ? meshOffset : 0.f);
                    top = nullptr;
                }

                if (top && top->steep && (top->kind==2 || top->kind==3) && (height>=16384 || height<-16384)) continue;
                Layer layer;
                layer.surface = height;
                layer.z = quantize8(height);
                layer.floor = spans[i].smax;
                layer.ceiling = ceiling;
                layer.steep = top && top->steep;
                layer.terrain = top && top->kind == 1;
                cell.push_back(layer);
            }
            if (cell.size() > 125) throw std::runtime_error("Cell exceeds 125 L2J layers");
            used |= !cell.empty();
        }
        if (used) geo.blocks[blockIndex] = std::move(block);
    }, "rasterize floors");
}

// Steep terrain may still be walked when the slope along this step is at most 0.3.
float terrainSlope(const Geo& geo, int x, int y, int nx, int ny) {
    const float x0 = float(geo.ox + x * 16 + 8), y0 = float(geo.oy + y * 16 + 8);
    const float x1 = float(geo.ox + nx * 16 + 8), y1 = float(geo.oy + ny * 16 + 8);
    const float dx = x1 - x0, dy = y1 - y0, length = std::sqrt(dx * dx + dy * dy);
    float slope = -std::numeric_limits<float>::infinity();
    const std::array<size_t, 2> blocks{size_t(x / 8) * 256 + size_t(y / 8), size_t(nx / 8) * 256 + size_t(ny / 8)};
    const int used = blocks[0] == blocks[1] ? 1 : 2;
    for (int index = 0; index < used; ++index) for (auto ref : geo.index[blocks[index]]) {
        const auto& triangle = geo.triangles[ref];
        if (triangle.kind != 1) continue;
        const double normalLength = std::sqrt(dot(triangle.normal, triangle.normal));
        if (normalLength <= 0 || std::abs(triangle.normal.z) / normalLength >= walkUp() || std::abs(triangle.normal.z) < 0.00000001) continue;
        const V* points[3]{&triangle.a, &triangle.b, &triangle.c};
        float lo = 0, hi = 1;
        for (int i = 0; i < 3 && lo < hi; ++i) {
            const auto& a = *points[i];
            const auto& b = *points[(i + 1) % 3];
            const auto& c = *points[(i + 2) % 3];
            auto side = [&](float px, float py) { return float((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)); };
            const float inside = side(float(c.x), float(c.y)) > 0.f ? 1.f : -1.f;
            const float start = inside * side(x0, y0), end = inside * side(x1, y1);
            if (start < 0.f && end < 0.f) hi = lo;
            else if (start < 0.f) lo = std::max(lo, start / (start - end));
            else if (end < 0.f) hi = std::min(hi, start / (start - end));
        }
        if (hi - lo > 0.0001f) slope = std::max(slope, float(-(triangle.normal.x * dx + triangle.normal.y * dy) / (triangle.normal.z * double(length))));
    }
    return slope;
}

bool canStep(const Geo& geo,const Layer& from,const Layer& to,int x,int y,int nx,int ny) {
    if (from.wet && to.wet) return true;
    const float rise=to.surface-from.surface;
    if (!to.steep) return rise<=16.32f;
    if (rise>steepClimb || !from.terrain || !to.terrain) return rise<=steepClimb;
    return terrainSlope(geo,x,y,nx,ny)<=steepSlopeLimit;
}

int linkedIndex(const Cell& neighbor,const Layer& layer) {
    int best=-1;
    for(int i=0;i<int(neighbor.size());++i) {
        const auto& other=neighbor[i];
        if(std::min(layer.ceiling,other.ceiling)-std::max(layer.floor,other.floor)<headroom) continue;
        if(best<0 || other.floor<neighbor[size_t(best)].floor) best=i;
    }
    return best;
}

void connect(Geo& geo, unsigned workers) {
    constexpr int dx[]{1, -1, 0, 0}, dy[]{0, 0, 1, -1};
    constexpr unsigned flags[]{1, 2, 4, 8};
    parallel(workers, [&](size_t blockIndex) {
        if (!geo.blocks[blockIndex]) return;
        for (int x = 0; x < 8; ++x) for (int y = 0; y < 8; ++y) {
            const int cellX = int(blockIndex / 256) * 8 + x, cellY = int(blockIndex % 256) * 8 + y;
            for (auto& layer : (*geo.blocks[blockIndex])[size_t(x) * 8 + y]) layer.flags=0;
            for (auto& layer : (*geo.blocks[blockIndex])[size_t(x) * 8 + y]) for (unsigned direction = 0; direction < 4; ++direction) {
                const int nextX = cellX + dx[direction], nextY = cellY + dy[direction];
                const auto& neighbor = geo.cell(nextX, nextY);
                const int index = linkedIndex(neighbor, layer);
                if (index >= 0 && (nextX>=0 && nextY>=0 && nextX<2048 && nextY<2048 ? canStep(geo, layer, neighbor[size_t(index)], cellX, cellY, nextX, nextY) : neighbor[size_t(index)].surface-layer.surface<=16.32f)) layer.flags |= flags[direction];
            }
        }
    }, "movement links");
}

void preserveFloors(Geo& geo) {
    for (auto& block:geo.blocks) if (block) for (auto& cell:*block)
        std::sort(cell.begin(),cell.end(),[](const Layer& a,const Layer& b) { return a.z>b.z; });
}

void keepReached(Geo& geo) {
    for (auto& block:geo.blocks) if (block) for (auto& cell:*block) {
        std::sort(cell.begin(),cell.end(),[](const Layer& a,const Layer& b) { return a.floor<b.floor; });
        for (auto& layer:cell) layer.reach=false;
    }
    struct Node { int x, y, index; };
    std::vector<Node> stack;
    auto at = [&](int x, int y) -> Cell* {
        if (x < 0 || y < 0 || x >= 2048 || y >= 2048) return nullptr;
        auto& block = geo.blocks[size_t(x / 8) * 256 + size_t(y / 8)];
        return block ? &(*block)[size_t(x % 8) * 8 + size_t(y % 8)] : nullptr;
    };
    auto visit = [&](int x, int y, int index) {
        auto* cell = at(x, y);
        if (!cell || index < 0 || index >= int(cell->size()) || (*cell)[index].reach) return;
        (*cell)[index].reach = true;
        stack.push_back({x, y, index});
    };
    for (int x = 0; x < 2048; ++x) for (int y = 0; y < 2048; ++y) {
        auto* cell = at(x, y);
        if (cell && !cell->empty()) visit(x, y, int(cell->size()) - 1);
    }
    for (const auto& seed : geo.seeds) {
        const int x = int(std::floor((seed.x - geo.ox) / 16.0));
        const int y = int(std::floor((seed.y - geo.oy) / 16.0));
        auto* cell = at(x, y);
        if (!cell) continue;
        int found = -1;
        const int height = static_cast<int>(seed.z);
        for (int i = 0; i < int(cell->size()); ++i) if ((*cell)[i].floor <= height) found = i;
        if (found >= 0) visit(x, y, found);
    }
    constexpr int dx[]{1, -1, 0, 0}, dy[]{0, 0, 1, -1};
    constexpr unsigned flags[]{1, 2, 4, 8};
    while (!stack.empty()) {
        const auto node = stack.back();
        stack.pop_back();
        const auto& layer = (*at(node.x, node.y))[node.index];
        for (int direction = 0; direction < 4; ++direction) {
            if (!(layer.flags & flags[direction])) continue;
            const int nextX = node.x + dx[direction], nextY = node.y + dy[direction];
            auto* neighbor = at(nextX, nextY);
            if (!neighbor) continue;
            const int index = linkedIndex(*neighbor, layer);
            if (index >= 0) visit(nextX, nextY, index);
        }
    }
    for (auto& block : geo.blocks) if (block) {
        bool used = false;
        for (auto& cell : *block) {
            std::erase_if(cell, [](const Layer& layer) { return !layer.reach; });
            std::sort(cell.begin(), cell.end(), [](const Layer& a, const Layer& b) { return a.z > b.z; });
            used |= !cell.empty();
        }
        if (!used) block.reset();
    }
}

size_t compactFloors(Geo& geo) {
    size_t changed=0;
    for(auto& block:geo.blocks) if(block) {
        bool eligible=true;int low=16376,high=-16384;
        for(const auto& cell:*block) {
            if(cell.size()!=1 || cell[0].flags!=15) {eligible=false;break;}
            low=std::min(low,cell[0].z);high=std::max(high,cell[0].z);
        }
        if(!eligible || high-low>32 || low==high) continue;
        const int z=((low+(high-low)/2)/8)*8;
        if(std::max(std::abs(z-low),std::abs(z-high))>24) continue;
        for(auto& cell:*block) {changed+=cell[0].z!=z;cell[0].z=z;}
    }
    return changed;
}

void dropEnclosure(std::vector<Triangle>& triangles,double,double) {
    const auto before=triangles.size();
    std::erase_if(triangles,[](const Triangle& t) {
        if (t.kind!=3) return false;
        for (const double z:{-16384.0,16384.0}) if (std::abs(t.a.z-z)<0.001 && std::abs(t.b.z-z)<0.001 && std::abs(t.c.z-z)<0.001) return true;
        return false;
    });
    if (before!=triangles.size()) std::cerr<<"STAGE world bounds | removed "<<before-triangles.size()<<" BSP cap triangles at engine Z limits\n";
}

void applyWater(Geo& geo,const std::vector<WaterGeometry>& water) {
    size_t touched=0;
    for (int x=0;x<2048;++x) for (int y=0;y<2048;++y) {
        auto& block=geo.blocks[size_t(x/8)*256+y/8]; if (!block) continue;
        auto& cell=(*block)[size_t(x%8)*8+y%8];
        for (auto& layer:cell) for (const auto& zone:water) {
            const double wx=geo.ox+x*16+8,wy=geo.oy+y*16+8;
            if (layer.surface<zone.low || layer.surface>zone.high || !waterContains(zone,wx,wy)) continue;
            layer.wet=true; ++touched; break;
        }
    }
    if (!water.empty()) std::cerr<<"STAGE water collision | "<<water.size()<<" zones, "<<touched<<" submerged floors\n";
}

using BoundaryCache=std::map<std::filesystem::path,std::shared_ptr<Geo>>;
void addHalo(Geo& geo,const std::filesystem::path& source,unsigned workers,BoundaryCache& cache) {
    const int tx=int(geo.ox/32768)+20,ty=int(geo.oy/32768)+18;
    const bool classic=classicMap(source);
    std::map<std::wstring,std::filesystem::path> files;
    for (const auto& entry:std::filesystem::directory_iterator(source.parent_path()))
        if (!entry.is_symlink() && entry.is_regular_file() && folded(entry.path().extension().wstring())==L".unr") files.emplace(folded(entry.path().stem().wstring()),entry.path());
    for (const auto [dx,dy]:std::array<std::pair<int,int>,4>{{{1,0},{-1,0},{0,1},{0,-1}}}) {
        auto digits=[](int n) { return (n>=0 && n<10 ? L"0" : L"")+std::to_wstring(n); };
        auto name=digits(tx+dx)+L"_"+digits(ty+dy)+(classic ? L"_classic" : L"");
        const auto found=files.find(name); if (found==files.end()) continue;
        auto cached=cache.find(found->second);
        if (cached==cache.end()) {
            std::cerr<<"MAP "<<output::utf8(found->second.filename())<<" | neighbouring boundary collision\n";
            auto edge=std::make_shared<Geo>(); edge->boundaryOnly=true; edge->ox=geo.ox+dx*32768; edge->oy=geo.oy+dy*32768;
            auto scene=collision::read(found->second);
            if (!scene.errors.empty()) throw std::runtime_error("Cannot validate neighbouring map "+output::utf8(found->second.filename())+": "+scene.errors.front());
            edge->triangles=std::move(scene.triangles); dropEnclosure(edge->triangles,edge->ox,edge->oy);
            buildIndex(*edge); rasterize(*edge,workers);
            applyWater(*edge,readWaterGeometry(found->second));
            connect(*edge,workers);
            for (auto& block:edge->blocks) if (block) for (auto& cell:*block) std::sort(cell.begin(),cell.end(),[](const Layer& a,const Layer& b) { return a.z>b.z; });
            edge->triangles.clear(); edge->triangles.shrink_to_fit(); for (auto& bucket:edge->index) { std::vector<uint32_t>().swap(bucket); }
            if (cache.size()>=8) cache.erase(cache.begin());
            cached=cache.emplace(found->second,std::move(edge)).first;
        }
        const auto& other=*cached->second;
        for (int across=0;across<2048;++across) for (int depth=0;depth<8;++depth) {
            const int x=dx==1 ? 2048+depth : dx==-1 ? -8+depth : across;
            const int y=dy==1 ? 2048+depth : dy==-1 ? -8+depth : across;
            const auto& cell=other.cell(x-dx*2048,y-dy*2048);
            if (!cell.empty()) geo.halo.emplace(std::pair{x,y},cell);
        }
    }
}

int nodeLayerIndex(const Cell& source,size_t sourceIndex,const Cell& target) {
    const auto& from=source[sourceIndex];
    const int sourceCeiling=sourceIndex ? source[sourceIndex-1].z : 65535;
    int best=-1,delta=0;
    for (size_t i=0;i<target.size();++i) {
        const int targetCeiling=i ? target[i-1].z : 65535;
        if (std::min(sourceCeiling,targetCeiling)-std::max(from.z,target[i].z)<headroom) continue;
        const int distance=std::abs(target[i].z-from.z);
        if (best<0 || distance<delta) { best=int(i); delta=distance; }
    }
    return best;
}

unsigned pathLink(const Geo& geo,int x,int y,int index,int dx,int dy) {
    for (int step=0;step<8;++step) {
        const auto& from=geo.cell(x,y); if (index<0 || size_t(index)>=from.size()) return 0;
        const auto& layer=from[size_t(index)];
        const unsigned fx=dx>0 ? 1 : 2,fy=dy>0 ? 4 : 8;
        auto linked=[&](int nx,int ny,unsigned flag) {
            if (!(layer.flags&flag)) return -1;
            return nodeLayerIndex(from,size_t(index),geo.cell(nx,ny));
        };
        int next=-1;
        if (!dx) next=linked(x,y+dy,fy);
        else if (!dy) next=linked(x+dx,y,fx);
        else {
            const int ix=linked(x+dx,y,fx),iy=linked(x,y+dy,fy);
            if (ix<0 || iy<0) return 0;
            const auto& a=geo.cell(x+dx,y)[size_t(ix)]; const auto& b=geo.cell(x,y+dy)[size_t(iy)];
            if (!(a.flags&fy) || !(b.flags&fx)) return 0;
            next=nodeLayerIndex(geo.cell(x+dx,y),size_t(ix),geo.cell(x+dx,y+dy));
            if (next<0 || next!=nodeLayerIndex(geo.cell(x,y+dy),size_t(iy),geo.cell(x+dx,y+dy))) return 0;
        }
        if (next<0) return 0;
        x+=dx; y+=dy; index=next;
    }
    return unsigned(index+1);
}
void writePathnodes(const Geo& geo,const std::filesystem::path& file) {
    std::ofstream out(file,std::ios::binary); if (!out) throw std::runtime_error("Cannot create pathnodes");
    const std::array<std::pair<int,int>,8> directions{{{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}}};
    size_t nodes=0;
    for (int y=0;y<256;++y) for (int x=0;x<256;++x) {
        const auto& cell=geo.cell(x*8+3,y*8+3); if (cell.size()>255) throw std::runtime_error("Too many pathnode layers");
        out.put(char(cell.size()));
        for (size_t i=0;i<cell.size();++i) {
            const auto z=uint16_t(cell[i].z); out.put(char(z>>8)); out.put(char(z&255));
            for (auto [dx,dy]:directions) out.put(char(pathLink(geo,x*8+3,y*8+3,int(i),dx,dy)));
            ++nodes;
        }
    }
    out.close(); if (!out) throw std::runtime_error("Pathnode write failed");
    std::cerr<<"STAGE pathnodes | "<<nodes<<" nodes\n";
}

void word(std::ostream& out, unsigned value) {
    out.put(char(value & 255));
    out.put(char((value >> 8) & 255));
}
void dword(std::ostream& out, unsigned value) { word(out, value); word(out, value >> 16); }

// L2J word: height is stored as (z << 1) with the low four bits holding NSWE. z is already a multiple of 8.
unsigned packed(Layer layer) { return (unsigned(uint16_t(layer.z * 2)) & 0xfff0) | layer.flags; }

enum class Format { L2J, L2OFF };

bool flatHeight(const int* heights, int& result) {
    result=heights[0];
    return std::all_of(heights,heights+64,[&](int z) { return z==result; });
}

// L2J block tags: 0 flat, 1 complex (one layer, not fully open), 2 multilayer.
// L2OFF word tags: 0 flat with two raw heights, 64 complex, or the layer count for a multilayer block.
// An empty cell is one closed layer at -16352 (packed 0x8040). Flat blocks are fully walkable.
size_t writeGeo(const Geo& geo, const std::filesystem::path& file, Format format, int tileX, int tileY) {
    std::ofstream out(file, std::ios::binary);
    size_t layers = 0;
    if (!out) throw std::runtime_error("Cannot create geodata");
    const bool off = format == Format::L2OFF;
    unsigned totalCells = 0, simpleBlocks = 0, flatBlocks = 0;
    if (off) for (int i = 0; i < 18; ++i) out.put(0);
    for (size_t blockIndex = 0; blockIndex < 65536; ++blockIndex) {
        const auto* block = geo.blocks[blockIndex].get();
        bool flat = block != nullptr;
        bool multi = false;
        int heights[64]{}, height = 0;
        if (block) {
            for (size_t i = 0; i < 64; ++i) {
                const auto& cell = (*block)[i];
                flat &= cell.size() == 1 && cell[0].flags == 15;
                if (cell.size() == 1) heights[i] = cell[0].z;
                multi |= cell.size() > 1;
                layers += cell.size();
            }
        }
        if (flat && flatHeight(heights, height)) {
            if (off) {
                word(out, 0);
                word(out, uint16_t(height));
                ++simpleBlocks;
                ++flatBlocks;
            } else out.put(0);
            word(out, uint16_t(height));
            continue;
        }
        unsigned blockLayers = 0;
        for (size_t i = 0; i < 64; ++i) blockLayers += block ? unsigned(std::max<size_t>(1, (*block)[i].size())) : 1;
        if (off) {
            word(out, multi ? blockLayers : 64);
            totalCells += blockLayers;
            if (!multi) ++simpleBlocks;
        } else out.put(char(multi ? 2 : 1));
        for (size_t i = 0; i < 64; ++i) {
            const size_t count = block ? (*block)[i].size() : 0;
            if (multi) {
                if (off) word(out, unsigned(count ? count : 1));
                else out.put(char(count ? count : 1));
            }
            if (!count) word(out, packed({voidHeight, 0}));
            else for (auto layer : (*block)[i]) word(out, packed(layer));
        }
    }
    if (off) {
        out.seekp(0);
        out.put(char(tileX));
        out.put(char(tileY));
        word(out, 128);
        word(out, 16);
        dword(out, totalCells);
        dword(out, simpleBlocks);
        dword(out, flatBlocks);
    }
    out.close();
    if (!out) throw std::runtime_error("Geodata write failed");
    return layers;
}
}

// Region origin is ((mapX - 20) * 32768, (mapY - 18) * 32768). Files are written as .partial and renamed together.
static int generateTile(const std::filesystem::path& input, const std::filesystem::path& output, unsigned workers, const std::wstring& stem, bool fixture, BoundaryCache* cache=nullptr) {
    namespace fs = std::filesystem;
    const auto source = fs::canonical(input);
    std::wsmatch tile;
    const std::wregex pattern(LR"(^([0-9]{1,2})_([0-9]{1,2})$)");
    if (!std::regex_match(stem, tile, pattern)) throw std::runtime_error("Geodata requires a numeric XX_YY region name");
    auto geo = std::make_unique<Geo>();
    geo->ox = (std::stoi(tile[1]) - 20) * 32768.;
    geo->oy = (std::stoi(tile[2]) - 18) * 32768.;
    std::string facts;
    if (fixture) geo->triangles = loadObj(source);
    else {
        std::cerr << "MAP " << output::utf8(source.filename()) << " | native client collision\n";
        auto scene = collision::read(source);
        facts = "Terrain triangles: " + std::to_string(scene.terrain) + "\nBSP triangles: " + std::to_string(scene.bsp)
            + "\nStatic-mesh triangles: " + std::to_string(scene.meshes) + "\nBlocking-volume triangles: " + std::to_string(scene.volumes)
            + "\nDynamic actors excluded: " + std::to_string(scene.dynamic) + "\nPlayer starts: " + std::to_string(scene.seeds.size()) + "\n";
        if (!scene.errors.empty()) {
            std::string errors;
            for (const auto& error : scene.errors) errors += error + '\n';
            if (debugFiles) output::save(output / "reports" / (stem + L".txt"), facts + "Native collision import incomplete; no tile published.\n" + errors);
            for (const auto& error : scene.errors) std::cerr << "SKIPPED: " << output::utf8(source.filename()) << " / " << error << '\n';
            throw std::runtime_error("Client collision data could not be read completely (" + std::to_string(scene.errors.size()) + " issues)");
        }
        geo->triangles = std::move(scene.triangles);
        geo->seeds = std::move(scene.seeds);
    }
    dropEnclosure(geo->triangles, geo->ox, geo->oy);
    std::cerr << "STAGE spatial index | triangles=" << geo->triangles.size() << '\n';
    buildIndex(*geo);
    std::cerr << "STAGE rasterize floors | 16-unit spans\n";
    rasterize(*geo, workers);
    if (!fixture) { applyWater(*geo,readWaterGeometry(source)); if (cache) addHalo(*geo,source,workers,*cache); }
    std::cerr << "STAGE movement links | headroom=46\n";
    connect(*geo, workers);
    std::cerr << "STAGE reachable floors | exposed surfaces and player starts\n";
    keepReached(*geo);
    connect(*geo, workers);
    if(compactGeodata) std::cerr<<"STAGE compact open blocks | "<<compactFloors(*geo)<<" adjusted heights, maximum 24 units\n";
    size_t count = 0;
    for (const auto& block : geo->blocks) if (block) for (const auto& cell : *block) count += cell.size();
    if (!count) throw std::runtime_error("No walkable cells in the selected tile; no geodata published");
    fs::create_directories(output);
    std::vector<std::pair<Format, std::wstring>> formats;
    if (!pathnodesOnly && geodataFormat != "l2off") formats.push_back({Format::L2J, L"L2J"});
    if (!pathnodesOnly && geodataFormat != "l2j") formats.push_back({Format::L2OFF, L"L2OFF"});
    for (const auto& [format, name] : formats) {
        const auto folder = output / name;
        fs::create_directories(folder);
        const auto filename = stem + (format == Format::L2OFF ? L"_conv.dat" : L".l2j");
        const auto temporary = folder / (filename + L".partial");
        std::cerr << "STAGE serialize | " << output::utf8(fs::path(name)) << '\n';
        writeGeo(*geo, temporary, format, std::stoi(tile[1]), std::stoi(tile[2]));
    }
    if (emitPathnodes || pathnodesOnly) {
        const auto folder=pathnodesOnly ? output : output/"Pathnodes";
        fs::create_directories(folder);
        writePathnodes(*geo,folder/(stem+L".pn.partial"));
    }
    for (const auto& [format, name] : formats) {
        const auto filename = stem + (format == Format::L2OFF ? L"_conv.dat" : L".l2j");
        fs::rename(output / name / (filename + L".partial"), output / name / filename);
    }
    if (emitPathnodes || pathnodesOnly) fs::rename((pathnodesOnly ? output : output/"Pathnodes")/(stem+L".pn.partial"),(pathnodesOnly ? output : output/"Pathnodes")/(stem+L".pn"));
    if (debugFiles) output::save(output / "reports" / (stem + L".txt"),
        "CPU geodata\nSource: " + output::utf8(source.filename()) + "\nLayers: " + std::to_string(count) + "\n" + facts
        + "Native UE2 client collision; dynamic doors are exported separately. Test movement before replacing a live server.\n"
        + "Settings: cell 16, height unit 8, client surface offsets by default (raw surfaces only with --world-heights), truncate toward zero, headroom 46, mesh surface step 16.32, steep climb 3.125, walk angle 45.573.\n"
        + "Lower floors require a movement route from an exposed surface or PlayerStart. Movement uses unquantized surface rise and the lowest compatible neighbour span. Adjacent region collision validates seams; unavailable neighbours stay closed. Fully open single-floor blocks may be compacted with height changes bounded to 24 units; --exact-heights disables this. Empty cells are closed at -16352.\n"
        + "Formats: " + (geodataFormat == "l2j" ? "L2J/*.l2j" : geodataFormat == "l2off" ? "L2OFF/*_conv.dat" : "L2J/*.l2j and L2OFF/*_conv.dat") + ". Four cardinal directions. No live client/server files modified.\n");
    std::cerr << (pathnodesOnly ? "Pathnodes complete: " : "Geodata complete: ") << count << " layers; " << (geodataFormat == "l2j" ? "L2J" : geodataFormat == "l2off" ? "L2OFF" : "L2J, L2OFF") << ".\n";
    return 0;
}

static std::shared_ptr<Geo> loadL2j(const std::filesystem::path& file) {
    if (std::filesystem::file_size(file)>512ull*1024*1024) throw std::runtime_error("Geodata input exceeds 512 MiB");
    std::ifstream input(file,std::ios::binary);
    auto byte=[&]() { const int value=input.get(); if (value<0) throw std::runtime_error("Truncated L2J geodata"); return unsigned(value); };
    auto word=[&]() { const auto low=byte(); return low|(byte()<<8); };
    auto geo=std::make_shared<Geo>();
    for (size_t b=0;b<65536;++b) {
        const auto type=byte(); if (type>2) throw std::runtime_error("Unsupported L2J block type");
        auto block=std::make_unique<Block>(); bool used=false;
        if (type==0) {
            const int z=int16_t(word());
            for (auto& cell:*block) { Layer layer; layer.z=z; layer.surface=float(z); layer.floor=z; layer.flags=15; cell.push_back(layer); } used=true;
        } else for (auto& cell:*block) {
            const unsigned count=type==2 ? byte() : 1;
            if (!count || count>125) throw std::runtime_error("Invalid L2J layer count");
            for (unsigned i=0;i<count;++i) {
                const unsigned packed=word(); Layer layer; layer.z=int16_t(packed&0xfff0)/2; layer.flags=packed&15;
                layer.surface=float(layer.z); layer.floor=layer.z;
                if (layer.z!=voidHeight || layer.flags!=0 || count!=1) cell.push_back(layer);
            }
            std::sort(cell.begin(),cell.end(),[](const Layer& a,const Layer& b) { return a.z>b.z; });
            for (size_t i=1;i<cell.size();++i) cell[i].ceiling=cell[i-1].z;
            used|=!cell.empty();
        }
        if (used) geo->blocks[b]=std::move(block);
    }
    if (input.get()!=EOF) throw std::runtime_error("Unexpected L2J geodata trailer");
    return geo;
}
int generatePathnodes(const std::filesystem::path& input,const std::filesystem::path& output) {
    namespace fs=std::filesystem;
    if (fs::exists(output)) throw std::runtime_error("Pathnode output already exists");
    const auto root=fs::is_regular_file(input) ? fs::canonical(input).parent_path() : fs::canonical(input);
    std::vector<fs::path> maps;
    if (fs::is_regular_file(input)) maps.push_back(fs::canonical(input));
    else for (const auto& entry:fs::recursive_directory_iterator(root)) if (!entry.is_symlink() && entry.is_regular_file() && folded(entry.path().extension().wstring())==L".l2j" && regionMap(entry.path())) maps.push_back(entry.path());
    if (maps.empty()) throw std::runtime_error("No numeric L2J regions found");
    std::sort(maps.begin(),maps.end()); fs::create_directories(output);
    size_t failures=0;
    for (const auto& map:maps) try {
        if (!regionMap(map)) throw std::runtime_error("Pathnodes require numeric XX_XX.l2j input");
        std::cerr<<"MAP "<<output::utf8(map.filename())<<" | pathnode links\n";
        auto geo=loadL2j(map);
        const auto stem=map.stem().wstring(); const auto separator=stem.find(L'_');
        const int tx=std::stoi(stem.substr(0,separator)),ty=std::stoi(stem.substr(separator+1));
        for (const auto [dx,dy]:std::array<std::pair<int,int>,4>{{{1,0},{-1,0},{0,1},{0,-1}}}) {
            auto digits=[](int n) { return (n>=0 && n<10 ? L"0" : L"")+std::to_wstring(n); };
            const auto neighbour=map.parent_path()/(digits(tx+dx)+L"_"+digits(ty+dy)+(classicMap(map) ? L"_Classic" : L"")+L".l2j");
            if (!fs::exists(neighbour)) continue;
            const auto other=loadL2j(neighbour);
            for (int across=0;across<2048;++across) for (int depth=0;depth<8;++depth) {
                const int x=dx==1 ? 2048+depth : dx==-1 ? -8+depth : across;
                const int y=dy==1 ? 2048+depth : dy==-1 ? -8+depth : across;
                const auto& cell=other->cell(x-dx*2048,y-dy*2048); if (!cell.empty()) geo->halo.emplace(std::pair{x,y},cell);
            }
        }
        const auto relative=fs::relative(map,root); auto folder=output/relative.parent_path(); auto outputStem=stem;
        if (classicMap(map)) { outputStem.resize(outputStem.size()-8); folder/="Classic"; }
        fs::create_directories(folder);
        writePathnodes(*geo,folder/(outputStem+L".pn.partial")); fs::rename(folder/(outputStem+L".pn.partial"),folder/(outputStem+L".pn"));
    } catch (const std::exception& error) { ++failures; std::cerr<<"SKIPPED: "<<output::utf8(map.filename())<<": "<<error.what()<<'\n'; }
    writeExportReadme(output,"PATHNODES"); std::cerr<<"RESULT pathnodes "<<maps.size()-failures<<' '<<failures<<" 0 0 0\n";
    return failures ? 2 : 0;
}

int generateGeodata(const std::filesystem::path& input, const std::filesystem::path& output, unsigned workers, bool classic, bool fixture) {
    namespace fs = std::filesystem;
    if (fs::exists(output)) throw std::runtime_error("Geodata output already exists; choose a new folder");
    if (fixture) return generateTile(input, output, workers, input.stem().wstring(), true);
    const auto maps = selectMaps(input, classic);
    const auto root = fs::is_regular_file(input) ? fs::canonical(input).parent_path() : fs::canonical(input);
    std::set<std::wstring> regions;
    for (const auto& map : maps) if (!regions.insert((allVariants && classicMap(map) ? L"Classic/" : L"") + folded(mapOutput(map, root).stem().wstring())).second)
        throw std::runtime_error("Multiple selected maps resolve to the same geodata region");
    fs::create_directories(output);
    if (debugFiles) output::save(output / "911reg-map-sources.tsv", mapSources(maps, root));
    size_t failures = 0, done = 0;
    std::string summary = "Native client collision geodata\n";
    BoundaryCache boundaryCache;
    for (const auto& map : maps) {
        try { generateTile(map, allVariants && classicMap(map) ? output / "Classic" : output, workers, mapOutput(map, root).stem().wstring(), false, &boundaryCache); }
        catch (const std::exception& error) {
            const auto tileRoot=allVariants && classicMap(map) ? output/"Classic" : output;
            const auto stem=mapOutput(map,root).stem().wstring();
            const std::array<std::filesystem::path,3> artifacts{tileRoot/"L2J"/(stem+L".l2j"),tileRoot/"L2OFF"/(stem+L"_conv.dat"),(pathnodesOnly ? tileRoot : tileRoot/"Pathnodes")/(stem+L".pn")};
            for (const auto& file:artifacts) { std::error_code ignored; fs::remove(file,ignored); fs::remove(file.wstring()+L".partial",ignored); }
            ++failures;
            summary += output::utf8(map.filename()) + ": " + error.what() + '\n';
            std::cerr << "SKIPPED: " << output::utf8(map.filename()) << ": " << error.what() << '\n';
        }
        std::cerr << "Checked " << ++done << " of " << maps.size() << " maps\n";
    }
    summary += "Maps: " + std::to_string(done) + "\nFailed maps: " + std::to_string(failures) + "\n";
    if (debugFiles) output::save(output / "911reg-geodata-report.txt", summary);
    if (!pathnodesOnly && emitPathnodes) for (const auto& folder:{output/"Pathnodes",output/"Classic"/"Pathnodes"}) if (fs::is_directory(folder)) writeExportReadme(folder,"PATHNODES");
    writeExportReadme(output, pathnodesOnly ? "PATHNODES" : "GEODATA");
    std::cerr << "RESULT " << (pathnodesOnly ? "pathnodes " : "geodata ") << done - failures << ' ' << failures << " 0 0 0\n";
    return failures ? 2 : 0;
}

// Fixed triangles: stacked floors, a wall, a 32-unit ramp and a steep face. Workers must agree with VERIFY-GEODATA.
int verifyGeodata() {
    retailHeightOffsets=false;
    auto geo = std::make_unique<Geo>();
    auto triangle = [&](V a, V b, V c) { geo->triangles.push_back({a, b, c, cross(b - a, c - a)}); };
    triangle({0, 0, 0}, {128, 0, 0}, {128, 128, 0});
    triangle({0, 0, 0}, {128, 128, 0}, {0, 128, 0});
    triangle({0, 0, 128}, {128, 0, 128}, {128, 128, 128});
    triangle({0, 0, 128}, {128, 128, 128}, {0, 128, 128});
    triangle({64, 0, 0}, {64, 128, 0}, {64, 128, 96});
    triangle({64, 0, 0}, {64, 128, 96}, {64, 0, 96});
    triangle({128, 0, 0}, {256, 0, 0}, {256, 128, 0});
    triangle({128, 0, 0}, {256, 128, 0}, {128, 128, 0});
    triangle({256, 0, 0}, {384, 0, 0}, {384, 128, 0});
    triangle({256, 0, 0}, {384, 128, 0}, {256, 128, 0});
    triangle({256, 0, 32}, {384, 128, 32}, {384, 0, 32});
    triangle({256, 0, 32}, {256, 128, 32}, {384, 128, 32});
    triangle({512, 0, 0}, {640, 0, 256}, {640, 128, 256});
    triangle({512, 0, 0}, {640, 128, 256}, {512, 128, 0});
    buildIndex(*geo);
    rasterize(*geo, 2);
    connect(*geo, 2);
    preserveFloors(*geo);
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Geodata geometry check failed"); };
    require(geo->cell(2, 2).size() == 2 && geo->cell(2, 2)[0].z == 128 && (geo->cell(2, 2)[0].flags & 1) != 0);
    require(geo->cell(5, 2).size() == 2 && geo->cell(5, 2)[0].z == 128 && geo->cell(5, 2)[1].z == 0 && (geo->cell(5, 2)[1].flags & 2) == 0);
    require(geo->cell(6, 2).size() == 2 && (geo->cell(6, 2)[1].flags & 2) != 0);
    require(geo->cell(16, 2).size()==1 && geo->cell(16, 2)[0].z==32);
    require(geo->cell(32, 2).size() == 1 && geo->cell(32, 2)[0].steep && geo->cell(32, 2)[0].z == 32);
    require(geo->cell(0, 0).size() == 2 && (geo->cell(0, 0)[0].flags & 2) == 0);
    require(packed({-16, 15}) == 65519 && packed({voidHeight, 0}) == 0x8040);
    require(boxHit(geo->triangles[4], {60, 20, 4}, {68, 28, 64}) && !boxHit(geo->triangles[4], {10, 20, 4}, {18, 28, 64}));
    std::cout << "Geodata checks passed: world heights, preserved floors, headroom, steep slopes, walls, NSWE, closed boundary and signed heights\n";
    return 0;
}
}
