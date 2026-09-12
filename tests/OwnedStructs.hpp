#pragma once
#include "OwnedRelations.hpp"
#include "andueprober/Structs.hpp"
#include <algorithm>
#include <limits>

struct OwnedObjectPrefix {
    std::uint32_t index = 0, flags = 1;
    std::uintptr_t klass = 0;
    std::uint32_t name = 0, number = 0;
    std::uintptr_t outer = 0;
};
struct OwnedNativeField {
    OwnedObjectPrefix object;
    std::uintptr_t independentPadding = 0xABABABAB;
    OwnedNativeField* next = nullptr;
    OwnedNativeField* duplicateNext = nullptr;
};
struct OwnedNativeStruct {
    OwnedNativeField field;
    OwnedNativeStruct* super = nullptr;
    OwnedNativeField* children = nullptr;
    void* childProperties = nullptr;
    std::uint32_t propertiesSize = 0;
    std::uint32_t unrelatedScalar = 0xFEFEFEFE;
    std::uint32_t minAlignment = 0;
    std::uint32_t duplicateSize = 0xCDCDCDCD;
    OwnedNativeStruct* duplicateSuper = nullptr;
};
struct alignas(8) OwnedRootPayload { std::byte bytes[24]; };
struct alignas(16) OwnedBranchPayload { std::byte bytes[48]; };
struct alignas(32) OwnedLeafPayload { std::byte bytes[96]; };

struct OwnedStructs final : andueprober::MemoryReader {
    OwnedRelations phase1;
    std::array<OwnedNativeField, 3> fields;
    std::array<OwnedNativeStruct, 3> structs;
    std::array<std::uint64_t, 3> properties{11, 22, 33};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::function<void(OwnedStructs&, std::uintptr_t, std::size_t)> beforeRead;
    static constexpr std::array<std::uint32_t, 3> sizes{
        sizeof(OwnedRootPayload), sizeof(OwnedBranchPayload), sizeof(OwnedLeafPayload)};
    static constexpr std::array<std::uint32_t, 3> alignments{
        alignof(OwnedRootPayload), alignof(OwnedBranchPayload), alignof(OwnedLeafPayload)};
    OwnedStructs() {
        for (std::size_t index = 0; index < structs.size(); ++index) {
            fields[index].object.index = static_cast<std::uint32_t>(index + 4);
            fields[index].object.klass = phase1.object(2);
            fields[index].object.outer = reinterpret_cast<std::uintptr_t>(&structs[index]);
            fields[index].next = index + 1 < fields.size() ? &fields[index + 1] : nullptr;
            structs[index].field.object.index = static_cast<std::uint32_t>(index + 7);
            structs[index].field.object.klass = phase1.object(2);
            structs[index].field.object.outer = phase1.object(0);
            structs[index].super = index ? &structs[index - 1] : nullptr;
            structs[index].children = &fields[index];
            structs[index].childProperties = &properties[index];
            structs[index].propertiesSize = sizes[index];
            structs[index].minAlignment = alignments[index];
        }
    }
    andueprober::StructProbeProfile profile(andueprober::Layout layout = andueprober::Layout::FField) const {
        return {"owned-compiled-struct-metadata-v1", "owned-structs-image", epoch, layout,
            sizeof(OwnedNativeStruct), sizeof(OwnedNativeField)};
    }
    std::array<andueprober::StructSample, 3> structSamples() const {
        const std::array<std::string, 3> names{"OwnedRootPayload", "OwnedBranchPayload", "OwnedLeafPayload"};
        std::array<andueprober::StructSample, 3> result;
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = {reinterpret_cast<std::uintptr_t>(&structs[index]), names[index],
                index ? reinterpret_cast<std::uintptr_t>(&structs[index - 1]) : 0, index ? names[index - 1] : "null",
                reinterpret_cast<std::uintptr_t>(&fields[index]), "owned-field:" + std::to_string(index), sizes[index],
                reinterpret_cast<std::uintptr_t>(&properties[index]), "owned-property:" + std::to_string(index), alignments[index]};
        return result;
    }
    std::array<andueprober::StructFieldSample, 3> fieldSamples() const {
        std::array<andueprober::StructFieldSample, 3> result;
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = {reinterpret_cast<std::uintptr_t>(&fields[index]), "owned-field:" + std::to_string(index),
                index + 1 < fields.size() ? reinterpret_cast<std::uintptr_t>(&fields[index + 1]) : 0,
                index + 1 < fields.size() ? "owned-field:" + std::to_string(index + 1) : "null"};
        return result;
    }
    andueprober::Snapshot initial(andueprober::Layout layout = andueprober::Layout::FField) {
        andueprober::Snapshot snapshot;
        snapshot.sessionId = "owned-structs"; snapshot.moduleIdentity = "owned-structs-image";
        snapshot.generation = epoch; snapshot.layout = layout;
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!observeOwnedRelations(*this, phase1, budget, snapshot)) std::abort();
        reads = 0;
        return snapshot;
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        const auto copy = [&](const auto& value) {
            const auto begin = reinterpret_cast<std::uintptr_t>(&value);
            if (address < begin || address - begin > sizeof(value) || destination.size() > sizeof(value) - (address - begin))
                return false;
            std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&value) + address - begin, destination.size());
            return true;
        };
        bool copied = false;
        for (const auto& field : fields) if (copy(field)) { copied = true; break; }
        if (!copied) for (const auto& value : structs) if (copy(value)) { copied = true; break; }
        if (!copied) {
            auto result = phase1.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
