#pragma once
#include "OwnedProperties.hpp"

struct OwnedPropertyBases final : andueprober::MemoryReader {
    OwnedProperties phase;
    static constexpr std::uint32_t scalarEnd = offsetof(OwnedNativeProperty, offsetInternal) + sizeof(std::int32_t);
    static constexpr std::uint32_t size = (scalarEnd + 15u) & ~7u;
    static constexpr std::uint32_t subPropertyBase = size + 8;
    static constexpr std::uint32_t extent = subPropertyBase + 24;
    std::array<std::array<std::byte, extent>, 4> tails{};
    std::array<std::uintptr_t, 2> expected{0x1020304050607080ULL, 0x8070605040302010ULL};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    std::size_t reads = 0;
    std::function<void(OwnedPropertyBases&, std::uintptr_t, std::size_t)> beforeRead;
    OwnedPropertyBases() {
        for (std::size_t index = 0; index < 2; ++index)
            std::memcpy(tails[index].data() + subPropertyBase, &expected[index], sizeof(expected[index]));
        constexpr std::array<std::uint8_t, 4> nativeBool{1, 0, 1, 0xff};
        for (std::size_t index = 2; index < 4; ++index)
            std::memcpy(tails[index].data() + size + 3, nativeBool.data(), nativeBool.size());
    }
    andueprober::Snapshot initial() {
        auto snapshot = phase.initial();
        auto profile = phase.profile();
        auto samples = phase.samples();
        auto& names = phase.phase5.nameMemory();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probePropertyFields(*this, profile, samples, names.nameLayout,
            names.address(64), names.pool, budget, snapshot)) std::abort();
        reads = 0;
        return snapshot;
    }
    andueprober::PropertyBaseProbeProfile profile() const {
        const auto scalar = phase.profile();
        const auto& names = phase.phase5.nameMemory();
        return {"owned-property-bases-v1", scalar.moduleIdentity,
            andueprober::propertyObservationIdentity(scalar, names.nameLayout, names.address(64), names.pool),
            epoch, andueprober::Layout::FField, extent, andueprober::BoolTailRepresentation::NativeBool};
    }
    std::array<andueprober::PropertyPointerTailSample, 2> pointerSamples() const {
        return {{{reinterpret_cast<std::uintptr_t>(&tails[0]), expected[0], "owned-struct-property"},
            {reinterpret_cast<std::uintptr_t>(&tails[1]), expected[1], "owned-object-property"}}};
    }
    std::array<andueprober::PropertyBoolTailSample, 2> boolSamples() const {
        return {{{reinterpret_cast<std::uintptr_t>(&tails[2]), "owned-bool-property-a"},
            {reinterpret_cast<std::uintptr_t>(&tails[3]), "owned-bool-property-b"}}};
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        for (const auto& tail : tails) {
            const auto begin = reinterpret_cast<std::uintptr_t>(&tail);
            if (address >= begin && address - begin <= tail.size() && destination.size() <= tail.size() - (address - begin)) {
                std::memcpy(destination.data(), tail.data() + address - begin, destination.size());
                return {destination.size()};
            }
        }
        return phase.read(address, destination);
    }
    std::uint64_t generation() const override { return epoch; }
};
