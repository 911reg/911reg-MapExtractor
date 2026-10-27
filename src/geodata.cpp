#include "geodata.h"
#include "export_utils.h"
#include "collision.h"
#include "map_selection.h"
#include "export_readme.h"
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
// 2 blocking volume, 3 world BSP. Column tops stay, including roofs. Lower
// floors stay only when a PlayerStart or an open direction can reach them.

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
    bool steep = false, terrain = false, reach = false;
};
using Cell = std::vector<Layer>;
using Block = std::array<Cell, 64>;

struct Geo {
    double ox = 0, oy = 0;
    std::vector<Triangle> triangles;
    std::vector<V> seeds;
    std::array<std::vector<uint32_t>, 65536> index;
    std::array<std::unique_ptr<Block>, 65536> blocks;
    // Cell index inside a block is (x % 8) * 8 + (y % 8). Blocks are stored as blockX * 256 + blockY.
    const Cell& cell(int x, int y) const {
        static const Cell empty;
        if (x < 0 || y < 0 || x >= 2048 || y >= 2048) return empty;
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
constexpr float terrainOffset = 26.296875f, meshOffset = 26.5f, modelOffset = 26.875f, modelTopOffset = 29.375f;
constexpr float steepClimb = 3.125f, steepSlopeLimit = 0.3f;
constexpr int headroom = 46, mergeGap = 3, voidHeight = -16352;

float walkUp() {
    static const float value = std::cos(45.573f * 3.14159265f / 180.f);
    return value;
}

int trunc8(float z) {
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
    bool steep = false;
};

float sampleHeight(const Sample& sample, bool top) {
    if (!std::isfinite(sample.z)) return -std::numeric_limits<float>::infinity();
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
    float z = hi;
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
        if (miss) {
            z = 0;
            for (int i = 0; i < count; ++i) z += first[i * 3 + 2];
            z /= float(count);
        }
    }
    const int smin = int(std::floor(lo));
    const int smax = std::max(smin + 1, int(std::ceil(hi)));
    samples.push_back({smin, smax, z, triangle.kind, steep});
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
                for (const auto& sample : samples) {
                    if (sample.smin < spans[i].smin - 1 || sample.smax > spans[i].smax + 1) continue;
                    const auto sampleZ = sampleHeight(sample, topSpan);
                    const bool terrain = sample.kind == 1;
                    if (sampleZ > height + 0.01f || (top && sampleZ >= height - 0.01f && !terrain && top->steep && !sample.steep)) top = &sample;
                    height = std::max(height, sampleZ);
                }
                if (!top || !std::isfinite(height)) {
                    height = float(spans[i].smax) + meshOffset;
                    top = nullptr;
                }
                Layer layer;
                layer.surface = height;
                layer.z = trunc8(height);
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

// A stored rise of 24 or less is open, including steep ground. Anything taller needs a gentle terrain slope.
bool canStep(const Geo& geo, const Layer& from, const Layer& to, int x, int y, int nx, int ny) {
    if (to.z - from.z <= 24) return true;
    const float rise = to.surface - from.surface;
    if (rise > steepClimb || !from.terrain || !to.terrain) return false;
    return terrainSlope(geo, x, y, nx, ny) <= steepSlopeLimit;
}

// Link to the neighbor layer whose height is closest, among layers that share headroom. The first span is often the roof.
int linkedIndex(const Cell& neighbor, const Layer& layer) {
    int best = -1, bestDelta = 0;
    for (int i = 0; i < int(neighbor.size()); ++i) {
        const auto& other = neighbor[i];
        if (std::min(layer.ceiling, other.ceiling) - std::max(layer.floor, other.floor) < headroom) continue;
        const int delta = std::abs(other.z - layer.z);
        if (best < 0 || delta < bestDelta) {
            best = i;
            bestDelta = delta;
        }
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
            for (auto& layer : (*geo.blocks[blockIndex])[size_t(x) * 8 + y]) for (unsigned direction = 0; direction < 4; ++direction) {
                const int nextX = cellX + dx[direction], nextY = cellY + dy[direction];
                if (nextX < 0 || nextY < 0 || nextX >= 2048 || nextY >= 2048) {
                    layer.flags |= flags[direction];
                    continue;
                }
                const auto& neighbor = geo.cell(nextX, nextY);
                const int index = linkedIndex(neighbor, layer);
                if (index >= 0 && canStep(geo, layer, neighbor[size_t(index)], cellX, cellY, nextX, nextY)) layer.flags |= flags[direction];
            }
        }
    }, "movement links");
}

// Seeds are the top span of every column, plus the highest span at or below each PlayerStart.
// Flood only follows open direction bits. Unreached lower spans are removed. Column tops remain.
void keepReached(Geo& geo) {
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

// A world-BSP plane at the L2J height limit that covers the whole region is an enclosure, not a floor.
void dropEnclosure(std::vector<Triangle>& triangles, double ox, double oy) {
    auto plane = [](const Triangle& triangle, double z) {
        return triangle.kind == 3 && std::abs(triangle.a.z - z) < 0.001 && std::abs(triangle.b.z - z) < 0.001 && std::abs(triangle.c.z - z) < 0.001;
    };
    auto covers = [&](double z) {
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        unsigned count = 0;
        for (const auto& triangle : triangles) if (plane(triangle, z)) {
            ++count;
            for (const auto* point : {&triangle.a, &triangle.b, &triangle.c}) {
                minX = std::min(minX, point->x);
                maxX = std::max(maxX, point->x);
                minY = std::min(minY, point->y);
                maxY = std::max(maxY, point->y);
            }
        }
        return count >= 2 && minX <= ox && minY <= oy && maxX >= ox + 32768 && maxY >= oy + 32768;
    };
    const bool low = covers(-16384), high = covers(16384);
    if (low || high) std::erase_if(triangles, [&](const Triangle& triangle) { return (low && plane(triangle, -16384)) || (high && plane(triangle, 16384)); });
}

void word(std::ostream& out, unsigned value) {
    out.put(char(value & 255));
    out.put(char((value >> 8) & 255));
}
void dword(std::ostream& out, unsigned value) { word(out, value); word(out, value >> 16); }

// L2J word: height is stored as (z << 1) with the low four bits holding NSWE. z is already a multiple of 8.
unsigned packed(Layer layer) { return (unsigned(uint16_t(layer.z * 2)) & 0xfff0) | layer.flags; }

enum class Format { L2J, L2OFF };

// An open 8x8 may collapse to one height when every cell differs by at most 24, or the span is exactly 32.
bool flatHeight(const int* heights, int& result) {
    int lo = heights[0], hi = heights[0], deviation = 0;
    for (int i = 1; i < 64; ++i) {
        lo = std::min(lo, heights[i]);
        hi = std::max(hi, heights[i]);
        deviation = std::max(deviation, std::abs(heights[i] - heights[0]));
    }
    result = heights[0];
    if (lo == hi) return true;
    if (deviation > 24 && hi - lo != 32) return false;
    result = deviation <= 24 ? (hi | 8) : (hi + lo) / 2;
    if (std::max(std::abs(result - lo), std::abs(result - hi)) > 24) result = lo + ((hi - lo) / 16) * 8;
    return result >= -16384 && result <= 16376 && result % 8 == 0 && std::max(std::abs(result - lo), std::abs(result - hi)) <= 24;
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
static int generateTile(const std::filesystem::path& input, const std::filesystem::path& output, unsigned workers, const std::wstring& stem, bool fixture) {
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
    std::cerr << "STAGE movement links | headroom=46\n";
    connect(*geo, workers);
    std::cerr << "STAGE reachable floors | column tops and player starts\n";
    keepReached(*geo);
    size_t count = 0;
    for (const auto& block : geo->blocks) if (block) for (const auto& cell : *block) count += cell.size();
    if (!count) throw std::runtime_error("No walkable cells in the selected tile; no geodata published");
    fs::create_directories(output);
    const std::array<std::pair<Format, std::wstring>, 2> formats{{{Format::L2J, L"L2J"}, {Format::L2OFF, L"L2OFF"}}};
    for (const auto& [format, name] : formats) {
        const auto folder = output / name;
        fs::create_directories(folder);
        const auto filename = stem + (format == Format::L2OFF ? L"_conv.dat" : L".l2j");
        const auto temporary = folder / (filename + L".partial");
        std::cerr << "STAGE serialize | " << output::utf8(fs::path(name)) << '\n';
        writeGeo(*geo, temporary, format, std::stoi(tile[1]), std::stoi(tile[2]));
    }
    for (const auto& [format, name] : formats) {
        const auto filename = stem + (format == Format::L2OFF ? L"_conv.dat" : L".l2j");
        fs::rename(output / name / (filename + L".partial"), output / name / filename);
    }
    if (debugFiles) output::save(output / "reports" / (stem + L".txt"),
        "CPU geodata\nSource: " + output::utf8(source.filename()) + "\nLayers: " + std::to_string(count) + "\n" + facts
        + "Native UE2 client collision; dynamic doors are exported separately. Test movement before replacing a live server.\n"
        + "Settings: cell 16, height unit 8, terrain +26.296875, mesh +26.5, model +26.875 (top span +29.375), truncate toward zero, headroom 46, stored step 24, steep climb 3.125, walk angle 45.573.\n"
        + "Column tops and PlayerStarts are kept; every other floor must be reachable. A stored step of 24 is open on every floor. Open 8x8 blocks may move by at most 24. Empty cells are closed at -16352.\n"
        + "Formats: L2J/*.l2j and L2OFF/*_conv.dat (PTS). Both store four cardinal directions. No live client/server files modified.\n");
    std::cerr << "Geodata complete: " << count << " layers; L2J, L2OFF.\n";
    return 0;
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
    for (const auto& map : maps) {
        try { generateTile(map, allVariants && classicMap(map) ? output / "Classic" : output, workers, mapOutput(map, root).stem().wstring(), false); }
        catch (const std::exception& error) {
            ++failures;
            summary += output::utf8(map.filename()) + ": " + error.what() + '\n';
            std::cerr << "SKIPPED: " << output::utf8(map.filename()) << ": " << error.what() << '\n';
        }
        std::cerr << "Checked " << ++done << " of " << maps.size() << " maps\n";
    }
    summary += "Maps: " + std::to_string(done) + "\nFailed maps: " + std::to_string(failures) + "\n";
    if (debugFiles) output::save(output / "911reg-geodata-report.txt", summary);
    writeExportReadme(output, "GEODATA");
    std::cerr << "RESULT geodata " << done - failures << ' ' << failures << " 0 0 0\n";
    return failures ? 2 : 0;
}

// Fixed triangles: stacked floors, a wall, a 32-unit ramp and a steep face. Workers must agree with VERIFY-GEODATA.
int verifyGeodata() {
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
    keepReached(*geo);
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Geodata geometry check failed"); };
    require(geo->cell(2, 2).size() == 1 && geo->cell(2, 2)[0].z == 152 && (geo->cell(2, 2)[0].flags & 1) != 0);
    require(geo->cell(5, 2).size() == 2 && geo->cell(5, 2)[0].z == 152 && geo->cell(5, 2)[1].z == 24 && (geo->cell(5, 2)[1].flags & 2) == 0);
    require(geo->cell(6, 2).size() == 2 && (geo->cell(6, 2)[1].flags & 2) != 0);
    require(geo->cell(16, 2).size() == 1 && geo->cell(16, 2)[0].z == 56);
    require(geo->cell(32, 2).size() == 1 && geo->cell(32, 2)[0].steep && geo->cell(32, 2)[0].z == 56);
    require(geo->cell(0, 0).size() == 1 && (geo->cell(0, 0)[0].flags & 2) != 0);
    require(packed({-16, 15}) == 65519 && packed({voidHeight, 0}) == 0x8040);
    require(boxHit(geo->triangles[4], {60, 20, 4}, {68, 28, 64}) && !boxHit(geo->triangles[4], {10, 20, 4}, {18, 28, 64}));
    std::cout << "Geodata checks passed: offsets, sealed floors, headroom, steep slopes, walls, NSWE, boundary and signed heights\n";
    return 0;
}
}
