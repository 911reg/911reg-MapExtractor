#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>

namespace extractor {

// Bounded UE2 tables and tagged properties, without editor or rendering dependencies.
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t offset = 0;
    size_t end = std::numeric_limits<size_t>::max();
    uint8_t byte() {
        if (offset >= bytes.size() || offset >= end) throw std::runtime_error("Truncated package data");
        return bytes[offset++];
    }
    uint32_t word(unsigned width = 4) {
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint32_t(byte()) << (8*i);
        return value;
    }
    int32_t compact() {
        const auto first = byte();
        uint64_t value = first & 63;
        bool more = (first & 64) != 0;
        for (unsigned shift = 6; more && shift <= 27; shift += 7) {
            const auto part = byte();
            value |= uint64_t(part & 127) << shift;
            more = (part & 128) != 0;
        }
        const bool negative = (first & 128) != 0;
        if (more || value > uint64_t(INT32_MAX) + unsigned(negative))
            throw std::runtime_error("Invalid compact integer");
        return static_cast<int32_t>(negative ? -int64_t(value) : int64_t(value));
    }
    void skip(size_t count) {
        if (offset > bytes.size() || count > bytes.size()-offset || offset > end || count > end-offset)
            throw std::runtime_error("Property exceeds object bounds");
        offset += count;
    }
};

struct Object { int32_t type = 0, outer = 0; std::string name; uint32_t flags = 0; size_t offset = 0, size = 0; };
struct Property {
    std::string name, structure;
    uint8_t kind = 0;
    uint32_t element = 0;
    bool boolean = false;
    std::vector<uint8_t> value;
};
struct Package {
    uint16_t version = 0, licensee = 0;
    std::string envelope = "plain";
    std::vector<Object> imports, exports;
    std::vector<uint8_t> data;
    std::vector<std::string> names;

    const std::string& name(int32_t index) const {
        if (index < 0 || size_t(index) >= names.size()) throw std::runtime_error("Invalid property name");
        return names[size_t(index)];
    }

    std::vector<Property> tags(Reader& reader, unsigned depth = 0) const {
        if (depth > 32) throw std::runtime_error("Property nesting exceeds limit");
        std::vector<Property> result;
        for (;;) {
            Property property; property.name = name(reader.compact());
            if (property.name == "None") return result;
            const auto info = reader.byte();
            property.kind = info & 15;
            if (!property.kind) throw std::runtime_error("Invalid property type");
            property.boolean = (info & 128) != 0;
            if (property.kind == 10) property.structure = name(reader.compact());
            constexpr size_t widths[] = {1,2,4,12,16};
            const auto mode = (info >> 4) & 7;
            const size_t size = mode < 5 ? widths[mode] : mode == 5 ? reader.byte() : mode == 6 ? reader.word(2) : reader.word();
            if (property.boolean && property.kind != 3) {
                const auto first = reader.byte();
                if (first < 128) property.element = first;
                else if (first < 192) property.element = ((first & 63) << 8) | reader.byte();
                else {
                    const auto a = reader.byte(), b = reader.byte(), c = reader.byte();
                    property.element = ((first & 63) << 24) | (uint32_t(a) << 16) | (uint32_t(b) << 8) | c;
                }
            }
            const auto start = reader.offset;
            // Some UE2 struct sizes describe their native layout, not their tagged serialization.
            if (property.kind == 10 && property.structure != "Vector" && property.structure != "Rotator" && property.structure != "Color")
                tags(reader,depth+1);
            else if (property.kind == 9 && property.name == "ZoneRenderState") {
                const auto count = reader.compact();
                if (count < 0 || count > 100000) throw std::runtime_error("Invalid zone property array");
                for (int32_t i = 0; i < count; ++i) tags(reader,depth+1);
            } else reader.skip(property.kind == 3 ? 0 : size);
            property.value.assign(reader.bytes.begin()+start,reader.bytes.begin()+reader.offset);
            result.push_back(std::move(property));
            if (result.size() > 100000) throw std::runtime_error("Excessive property count");
        }
    }

    std::vector<Property> properties(size_t index, size_t* nativeOffset = nullptr) const {
        const auto& object = exports.at(index);
        Reader reader{data,object.offset,object.offset+object.size};
        if (object.flags & 0x02000000) {
            const auto node = reader.compact(); reader.compact(); reader.skip(12);
            if (node) reader.compact();
        }
        auto result = tags(reader);
        if (nativeOffset) *nativeOffset = reader.offset;
        return result;
    }

    std::string objectPath(int32_t reference, unsigned depth = 0) const {
        if (!reference) return {};
        if (depth > 64) throw std::runtime_error("Cyclic or excessive object nesting");
        const auto& table = reference < 0 ? imports : exports;
        const auto index = reference < 0 ? -int64_t(reference)-1 : int64_t(reference)-1;
        if (uint64_t(index) >= table.size()) throw std::runtime_error("Invalid object reference");
        const auto& object = table[size_t(index)];
        return (object.outer ? objectPath(object.outer, depth+1)+"." : "") + object.name;
    }
    std::string className(size_t index) const {
        const auto type = exports.at(index).type;
        return type ? objectPath(type) : "Core.Class";
    }

    static Package open(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot open map");
        const auto length = input.tellg();
        if (length < 36 || length > 512ll*1024*1024) throw std::runtime_error("Map outside 36-byte to 512-MiB bounds");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) throw std::runtime_error("Map read failed");
        Package package;
        Reader reader{bytes};
        if (reader.word() != 0x9e2a83c1) {
            std::string header;
            for (size_t i = 0; i < 28; i += 2) {
                if (bytes[i+1]) throw std::runtime_error("Unknown map envelope");
                header += char(bytes[i]);
            }
            if (header != "Lineage2Ver111" && header != "Lineage2Ver120" && header != "Lineage2Ver121")
                throw std::runtime_error("Unsupported map envelope");
            package.envelope = header;
            const auto key = bytes[28] ^ 0xc1;
            bytes.erase(bytes.begin(), bytes.begin()+28);
            for (auto& value : bytes) value ^= key;
        }
        reader.offset = 0;
        if (reader.word() != 0x9e2a83c1) throw std::runtime_error("Invalid UE2 signature");
        package.version = uint16_t(reader.word(2)); package.licensee = uint16_t(reader.word(2));
        if (package.version < 118 || package.version > 133 || package.licensee > 40)
            throw std::runtime_error("Unsupported UE2 table profile");
        reader.word();
        // All table ranges are checked before any table entries are allocated.
        auto bounded = [&](uint32_t value, size_t maximum) -> size_t {
            if (value > maximum) throw std::runtime_error("Invalid table count or offset");
            return value;
        };
        const auto namesCount = bounded(reader.word(), 1000000), namesOffset = bounded(reader.word(), bytes.size());
        const auto exportsCount = bounded(reader.word(), 1000000), exportsOffset = bounded(reader.word(), bytes.size());
        const auto importsCount = bounded(reader.word(), 1000000), importsOffset = bounded(reader.word(), bytes.size());
        std::vector<std::string> names;
        reader.offset = namesOffset;
        for (size_t i = 0; i < namesCount; ++i) {
            const int64_t size = reader.compact();
            const auto count = size < 0 ? -size : size;
            if (count < 1 || count > 65536) throw std::runtime_error("Invalid name length");
            std::string name;
            for (int64_t j = 0; j < count; ++j) {
                const auto c = reader.word(size < 0 ? 2 : 1);
                if (j == count-1) { if (c) throw std::runtime_error("Unterminated name"); }
                else if (c < 128) name += char(c);
                else if (c < 2048) { name += char(192|(c>>6)); name += char(128|(c&63)); }
                else { name += char(224|(c>>12)); name += char(128|((c>>6)&63)); name += char(128|(c&63)); }
            }
            names.push_back(std::move(name)); reader.word();
        }
        auto name = [&](int32_t index) -> std::string {
            if (index < 0 || size_t(index) >= names.size()) throw std::runtime_error("Invalid name reference");
            return names[size_t(index)];
        };
        reader.offset = importsOffset;
        for (size_t i = 0; i < importsCount; ++i) {
            name(reader.compact()); name(reader.compact());
            Object object; object.outer = int32_t(reader.word()); object.name = name(reader.compact());
            package.imports.push_back(std::move(object));
        }
        reader.offset = exportsOffset;
        for (size_t i = 0; i < exportsCount; ++i) {
            Object object; object.type = reader.compact(); reader.compact();
            object.outer = int32_t(reader.word()); object.name = name(reader.compact()); object.flags = reader.word();
            const auto size = bounded(uint32_t(reader.compact()), bytes.size());
            const auto offset = size ? bounded(uint32_t(reader.compact()), bytes.size()) : 0;
            if (size > bytes.size()-offset) throw std::runtime_error("Export payload outside map");
            object.offset = offset; object.size = size;
            package.exports.push_back(std::move(object));
        }
        package.names = std::move(names); package.data = std::move(bytes);
        return package;
    }
};
}
