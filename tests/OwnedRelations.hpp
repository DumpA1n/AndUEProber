#pragma once
#include "andueprober/Probe.hpp"
#include "andueprober/Relations.hpp"
#include <array>
#include <cstring>
#include <functional>

struct OwnedRelations final : andueprober::MemoryReader {
    alignas(8) std::array<std::byte, 4096> bytes;
    andueprober::ObjectArrayProfile array;
    andueprober::NamePoolProfile pool;
    andueprober::NameLayout nameLayout{0, {}, 4, 8};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    std::function<void(OwnedRelations&, std::size_t)> beforeRead;
    bool shortRead = false;
    OwnedRelations() {
        bytes.fill(std::byte{0xff});
        array.identity = "owned-object-relations-v1"; array.objects = 0; array.count = 8; array.capacity = 12;
        array.itemStride = 8; array.itemObject = 0; array.maximumObjects = 4; array.maximumExamined = 4; array.sampleLimit = 4;
        put<std::uintptr_t>(32, address(128)); put<std::int32_t>(40, 4); put<std::int32_t>(44, 4);
        pool.identity = "owned-relation-names-v1"; pool.blocks = 0; pool.header = 0; pool.string = 2;
        pool.blockBits = 8; pool.maximumBlocks = 1;
        put<std::uintptr_t>(64, address(512));
        entry(0, "Object"); entry(16, "/Script/CoreUObject"); entry(32, "Class"); entry(48, "Package");
        for (std::uint32_t index = 0; index < 4; ++index) {
            const auto offset = 2048 + index * 64;
            put<std::uintptr_t>(128 + index * 8, address(offset));
            put<std::uint32_t>(offset, index); put<std::uint32_t>(offset + 4, index ? 0x11 : 1);
            put<std::uintptr_t>(offset + 8, object(index == 0 ? 3 : 2));
            put<std::uint32_t>(offset + 16, index == 0 ? 16 : index == 1 ? 0 : index == 2 ? 32 : 48);
            put<std::uint32_t>(offset + 20, 0);
            put<std::uintptr_t>(offset + 24, index ? object(0) : 0);
        }
    }
    std::uintptr_t address(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(bytes.data()) + offset; }
    std::uintptr_t object(std::size_t index) const { return address(2048 + index * 64); }
    template<class T> void put(std::size_t offset, T value) {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) std::abort();
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    void entry(std::uint32_t id, const std::string& name) {
        put<std::uint16_t>(512 + id * 2, static_cast<std::uint16_t>(name.size() << 6));
        std::memcpy(bytes.data() + 514 + id * 2, name.data(), name.size());
    }
    std::array<andueprober::NameSample, 2> names() const {
        return {{{object(1), "Object", "owned-object:1"}, {object(0), "/Script/CoreUObject", "owned-object:0"}}};
    }
    std::array<andueprober::NameSample, 2> classes() const {
        return {{{object(1), "Class", "owned-object:1"}, {object(0), "Package", "owned-object:0"}}};
    }
    std::array<andueprober::PointerSample, 2> outers() const {
        return {{{object(1), object(0), "owned-object:1", "owned-object:0"}, {object(0), 0, "owned-object:0", "null"}}};
    }
    andueprober::ReadResult read(std::uintptr_t pointer, std::span<std::byte> destination) override {
        const auto start = address(0);
        if (pointer < start || pointer - start > bytes.size() || destination.size() > bytes.size() - (pointer - start))
            return {0, andueprober::Error::Unmapped};
        if (beforeRead) beforeRead(*this, pointer - start);
        if (failure != andueprober::Error::None) return {0, failure};
        std::memcpy(destination.data(), bytes.data() + pointer - start, destination.size());
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
// Runs the same production phase and publication functions used by the inspector.
inline andueprober::Status observeOwnedRelations(andueprober::MemoryReader& reader, const OwnedRelations& fixture,
    andueprober::ReadBudget& budget, andueprober::Snapshot& snapshot) {
    using namespace andueprober;
    snapshot.layoutIdentity = objectArrayLayoutIdentity(fixture.array) + "|" + nameLayoutIdentity(fixture.nameLayout, fixture.pool);
    FieldProbeReport report;
    report.generation = budget.generation;
    if (auto status = beginFieldProbe(snapshot, "UObject::InternalIndex"); !status) return status;
    if (auto status = probeObjectArrayIndices(reader, fixture.address(32), fixture.array, 32, budget, report.candidates); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::InternalIndex", report, {}); !status) return status;
    if (auto status = beginFieldProbe(snapshot, "UObject::NamePrivate"); !status) return status;
    const auto names = fixture.names();
    if (auto status = probeNameField(reader, names, 32, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report); !status) return status;
    const std::array<std::string, 1> indexDependency{"UObject::InternalIndex"};
    if (auto status = publishFieldProbe(snapshot, "UObject::NamePrivate", report, indexDependency); !status) return status;
    const std::array<std::string, 2> dependencies{"UObject::InternalIndex", "UObject::NamePrivate"};
    if (auto status = beginFieldProbe(snapshot, "UObject::ClassPrivate"); !status) return status;
    const auto classes = fixture.classes();
    if (auto status = probeClassField(reader, classes, 32, snapshot.offsets.at("UObject::NamePrivate").value,
        fixture.nameLayout, fixture.address(64), fixture.pool, budget, report); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::ClassPrivate", report, dependencies); !status) return status;
    if (auto status = beginFieldProbe(snapshot, "UObject::OuterPrivate"); !status) return status;
    const auto outers = fixture.outers();
    if (auto status = probePointerField(reader, outers, 32, fixture.array.identity, budget, report); !status) return status;
    return publishFieldProbe(snapshot, "UObject::OuterPrivate", report, dependencies);
}
