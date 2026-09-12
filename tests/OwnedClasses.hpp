#pragma once
#include "OwnedStructs.hpp"
#include "andueprober/Classes.hpp"

struct OwnedNativeClass {
    OwnedNativeStruct structure;
    std::uint64_t independentPadding = 0x777788889999AAAA;
    std::uint64_t castFlags = 0;
    OwnedObjectPrefix* defaultObject = nullptr;
    std::uint64_t duplicateFlags = 0xAAAAAAAA55555555;
    OwnedObjectPrefix* duplicateDefault = nullptr;
};
// The compact records exercise zero-valued offsets independently of the native
// UObject-prefix fixture. Their explicit extent is their compiled record size.
struct OwnedCompactClass {
    std::uint64_t castFlags;
    OwnedObjectPrefix* defaultObject;
};
struct OwnedClasses final : andueprober::MemoryReader {
    OwnedStructs phase2;
    std::array<OwnedObjectPrefix, 3> defaultObjects;
    std::array<OwnedNativeClass, 3> classes;
    std::array<OwnedCompactClass, 3> compact;
    static constexpr std::array<std::uint64_t, 3> flags{0, 0x8000000000000010ULL, 0x0000000200000040ULL};
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::uint64_t epoch = 1;
    std::function<void(OwnedClasses&, std::uintptr_t, std::size_t)> beforeRead;
    OwnedClasses() {
        for (std::size_t i = 0; i < classes.size(); ++i) {
            classes[i].structure = phase2.structs[i];
            classes[i].castFlags = flags[i];
            classes[i].defaultObject = &defaultObjects[i];
            defaultObjects[i].klass = reinterpret_cast<std::uintptr_t>(&classes[i]);
            compact[i] = {flags[i], &defaultObjects[i]};
        }
    }
    andueprober::ClassProbeProfile profile(andueprober::Layout layout = andueprober::Layout::FField,
        bool compactRecords = false) const {
        return {"owned-compiled-class-metadata-v1", "owned-structs-image", epoch, layout,
            static_cast<std::uint32_t>(compactRecords ? sizeof(OwnedCompactClass) : sizeof(OwnedNativeClass))};
    }
    std::array<andueprober::ClassSample, 3> samples(bool compactRecords = false) const {
        std::array<andueprober::ClassSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = {compactRecords ? reinterpret_cast<std::uintptr_t>(&compact[i]) : reinterpret_cast<std::uintptr_t>(&classes[i]),
                "owned-class:" + std::to_string(i), flags[i], reinterpret_cast<std::uintptr_t>(&defaultObjects[i]),
                "owned-default-object:" + std::to_string(i)};
        return result;
    }
    andueprober::Snapshot initial(andueprober::Layout layout = andueprober::Layout::FField) {
        auto result = phase2.initial(layout);
        auto structs = phase2.structSamples(); auto fields = phase2.fieldSamples();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probeStructFields(*this, phase2.profile(layout), structs, fields, budget, result)) std::abort();
        reads = 0;
        return result;
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        const auto copy = [&](const auto& object) {
            const auto start = reinterpret_cast<std::uintptr_t>(&object);
            if (address < start || address - start > sizeof(object) || destination.size() > sizeof(object) - (address - start)) return false;
            std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&object) + address - start, destination.size());
            return true;
        };
        bool copied = false;
        for (const auto& object : classes) if (copy(object)) { copied = true; break; }
        if (!copied) for (const auto& object : compact) if (copy(object)) { copied = true; break; }
        // Default objects have declared addresses but are deliberately unreadable.
        if (!copied) {
            const auto result = phase2.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
