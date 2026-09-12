#pragma once
#include "OwnedClasses.hpp"
#include "andueprober/Functions.hpp"
#include <type_traits>

inline std::atomic<unsigned> ownedNativeFunctionCalls{0};
[[gnu::noinline]] inline std::uint32_t ownedNativeFunctionA(std::uint32_t value) {
    ++ownedNativeFunctionCalls; return value + 1;
}
[[gnu::noinline]] inline std::uint32_t ownedNativeFunctionB(std::uint32_t value) {
    ++ownedNativeFunctionCalls; return value + 2;
}
struct OwnedRootReturnParms { std::uint64_t result; };
struct OwnedBranchReturnParms { std::uint32_t input, padding; std::uint64_t result; };
struct OwnedLeafVoidParms { std::uint64_t first, second, third; };
struct OwnedNativeFunction {
    OwnedNativeStruct structure;
    std::uint32_t flags = 0;
    std::uint8_t numParms = 0;
    std::uint8_t independentPadding = 0xA5;
    std::uint16_t parmsSize = 0;
    std::uint16_t returnOffset = 0xffff;
    std::uint16_t independentScalar = 0x5555;
    std::uintptr_t nativeFunction = 0;
    std::uint32_t duplicateFlags = 0xEEEEEEEE;
    std::uint8_t duplicateNumParms = 0xB0;
    std::uint8_t duplicatePadding = 0xB1;
    std::uint16_t duplicateParmsSize = 0xB2B2;
    std::uint16_t duplicateReturnOffset = 0xB3B3;
    std::uintptr_t duplicateNativeFunction = 0;
};
struct OwnedCompactFunction {
    std::uint32_t flags;
    std::uint8_t numParms;
    std::uint8_t padding = 0xA5;
    std::uint16_t parmsSize;
    std::uint16_t returnOffset;
    std::uintptr_t nativeFunction;
};
struct OwnedFunctions final : andueprober::MemoryReader {
    OwnedClasses phase3;
    std::array<OwnedNativeFunction, 3> functions;
    std::array<OwnedCompactFunction, 3> compact;
    static constexpr std::array<std::uint32_t, 3> flags{0x80000410, 0x00000820, 0x00001040};
    static constexpr std::array<std::uint8_t, 3> counts{1, 2, 3};
    static constexpr std::array<std::uint16_t, 3> sizes{
        sizeof(OwnedRootReturnParms), sizeof(OwnedBranchReturnParms), sizeof(OwnedLeafVoidParms)};
    static constexpr std::array<std::uint16_t, 3> returns{
        offsetof(OwnedRootReturnParms, result), offsetof(OwnedBranchReturnParms, result), 0xffff};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::function<void(OwnedFunctions&, std::uintptr_t, std::size_t)> beforeRead;
    static std::uintptr_t nativeAddress(std::size_t index) {
        return index == 0 ? reinterpret_cast<std::uintptr_t>(&ownedNativeFunctionA) :
            index == 1 ? reinterpret_cast<std::uintptr_t>(&ownedNativeFunctionB) : 0;
    }
    OwnedFunctions() {
        static_assert(std::is_trivially_copyable_v<OwnedNativeFunction> && std::is_trivially_copyable_v<OwnedCompactFunction>);
        for (std::size_t i = 0; i < functions.size(); ++i) {
            // Scans include padding; its bytes are independent deterministic
            // fixture data rather than indeterminate stack contents.
            std::memset(&functions[i], 0, sizeof(functions[i]));
            const auto& source = phase3.phase2.structs[i];
            auto& structure = functions[i].structure;
            structure.field.object.index = source.field.object.index;
            structure.field.object.flags = source.field.object.flags;
            structure.field.object.klass = source.field.object.klass;
            structure.field.object.name = source.field.object.name;
            structure.field.object.number = source.field.object.number;
            structure.field.object.outer = source.field.object.outer;
            structure.field.independentPadding = source.field.independentPadding;
            structure.field.next = source.field.next;
            structure.field.duplicateNext = source.field.duplicateNext;
            structure.super = source.super; structure.children = source.children;
            structure.childProperties = source.childProperties;
            structure.propertiesSize = source.propertiesSize; structure.unrelatedScalar = source.unrelatedScalar;
            structure.minAlignment = source.minAlignment; structure.duplicateSize = source.duplicateSize;
            structure.duplicateSuper = source.duplicateSuper;
            functions[i].flags = flags[i]; functions[i].numParms = counts[i];
            functions[i].parmsSize = sizes[i]; functions[i].returnOffset = returns[i];
            functions[i].nativeFunction = nativeAddress(i);
            functions[i].independentPadding = 0xA5; functions[i].independentScalar = 0x5555;
            functions[i].duplicateFlags = 0xEEEEEEEE; functions[i].duplicateNumParms = 0xB0;
            functions[i].duplicatePadding = 0xB1; functions[i].duplicateParmsSize = 0xB2B2;
            functions[i].duplicateReturnOffset = 0xB3B3;
            std::memset(&compact[i], 0, sizeof(compact[i]));
            compact[i].flags = flags[i]; compact[i].numParms = counts[i]; compact[i].padding = 0xA5;
            compact[i].parmsSize = sizes[i]; compact[i].returnOffset = returns[i]; compact[i].nativeFunction = nativeAddress(i);
        }
    }
    andueprober::FunctionProbeProfile profile(andueprober::Layout layout = andueprober::Layout::FField,
        bool compactRecords = false) const {
        return {"owned-compiled-function-metadata-v1", "owned-structs-image", epoch, layout,
            static_cast<std::uint32_t>(compactRecords ? sizeof(OwnedCompactFunction) : sizeof(OwnedNativeFunction))};
    }
    std::array<andueprober::FunctionSample, 3> samples(bool compactRecords = false) const {
        std::array<andueprober::FunctionSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = {compactRecords ? reinterpret_cast<std::uintptr_t>(&compact[i]) : reinterpret_cast<std::uintptr_t>(&functions[i]),
                "owned-function:" + std::to_string(i), flags[i], counts[i], sizes[i], returns[i], nativeAddress(i),
                i == 2 ? "null" : "owned-native-function:" + std::to_string(i)};
        return result;
    }
    andueprober::Snapshot initial(andueprober::Layout layout = andueprober::Layout::FField) {
        auto result = phase3.initial(layout); auto classes = phase3.samples();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probeClassFields(*this, phase3.profile(layout), classes, budget, result)) std::abort();
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
        for (const auto& object : functions) if (copy(object)) { copied = true; break; }
        if (!copied) for (const auto& object : compact) if (copy(object)) { copied = true; break; }
        if (!copied) {
            const auto result = phase3.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
