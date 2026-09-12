#pragma once
#include "OwnedFields.hpp"
#include "andueprober/Properties.hpp"

struct OwnedScalarValue { std::uint32_t value; };
struct OwnedArrayValue { std::uint64_t prefix; std::array<std::uint64_t, 3> values; };
struct OwnedVectorElement { double x, y; };
struct OwnedVectorValue { std::array<std::byte, 16> prefix; std::array<OwnedVectorElement, 2> values; };
struct OwnedNativeProperty {
    OwnedNativeFField field;
    std::int32_t arrayDim, elementSize;
    std::uint64_t propertyFlags;
    std::int32_t offsetInternal, duplicateArrayDim, duplicateElementSize;
    std::uint64_t duplicatePropertyFlags;
    std::int32_t duplicateOffsetInternal;
};
struct OwnedProperties final : andueprober::MemoryReader {
    OwnedFields phase5;
    std::array<OwnedNativeProperty, 3> properties;
    static constexpr std::array<std::int32_t, 3> dimensions{1, std::tuple_size_v<decltype(OwnedArrayValue::values)>, std::tuple_size_v<decltype(OwnedVectorValue::values)>};
    static constexpr std::array<std::int32_t, 3> elementSizes{sizeof(std::uint32_t), sizeof(std::uint64_t), sizeof(OwnedVectorElement)};
    static constexpr std::array<std::uint64_t, 3> flags{0, 0x8000000100000000ULL, 0x1000000000000010ULL};
    static constexpr std::array<std::int32_t, 3> valueOffsets{offsetof(OwnedScalarValue, value), offsetof(OwnedArrayValue, values), offsetof(OwnedVectorValue, values)};
    static constexpr std::array<std::uint32_t, 3> containingSizes{sizeof(OwnedScalarValue), sizeof(OwnedArrayValue), sizeof(OwnedVectorValue)};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::function<void(OwnedProperties&, std::uintptr_t, std::size_t)> beforeRead;
    OwnedProperties() {
        static_assert(std::is_trivially_copyable_v<OwnedNativeProperty>);
        for (std::size_t i = 0; i < properties.size(); ++i) {
            // Every scanned padding byte belongs to deterministic fixture data.
            std::memset(&properties[i], 0, sizeof(properties[i]));
            const auto& source = phase5.fields[i]; auto& field = properties[i].field;
            field.name.index = source.name.index; field.name.number = source.name.number;
            field.owner.pointer = source.owner.pointer; field.owner.isUObject = source.owner.isUObject;
            field.next = source.next; field.klass = source.klass; field.flags = source.flags;
            field.duplicateName.index = source.duplicateName.index; field.duplicateName.number = source.duplicateName.number;
            field.duplicateOwner.pointer = source.duplicateOwner.pointer; field.duplicateOwner.isUObject = source.duplicateOwner.isUObject;
            field.duplicateNext = source.duplicateNext; field.duplicateClass = source.duplicateClass; field.duplicateFlags = source.duplicateFlags;
            properties[i].arrayDim = dimensions[i]; properties[i].elementSize = elementSizes[i];
            properties[i].propertyFlags = flags[i]; properties[i].offsetInternal = valueOffsets[i];
            properties[i].duplicateArrayDim = -1; properties[i].duplicateElementSize = -1;
            properties[i].duplicatePropertyFlags = 0xCCCCCCCCCCCCCCCCULL; properties[i].duplicateOffsetInternal = -1;

        }
    }
    andueprober::PropertyProbeProfile profile() const {
        return {"owned-compiled-property-metadata-v1", "owned-structs-image", epoch, andueprober::Layout::FField,
            sizeof(OwnedNativeProperty), sizeof(OwnedNativeFField), phase5.profile().identity, phase5.profile().ownerLayout};
    }
    std::array<andueprober::PropertySample, 3> samples() const {
        std::array<andueprober::PropertySample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = {reinterpret_cast<std::uintptr_t>(&properties[i]),
                "owned-property:" + std::to_string(i), dimensions[i], elementSizes[i], flags[i], valueOffsets[i], containingSizes[i],
                "owned-containing-value:" + std::to_string(i)};
        return result;
    }
    andueprober::Snapshot initial() {
        auto result = phase5.initial(); const auto fields = phase5.samples(); const auto& pool = phase5.nameMemory();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probeFieldFields(*this, phase5.profile(), fields, pool.nameLayout, pool.address(64), pool.pool, budget, result)) std::abort();
        reads = 0; return result;
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
        for (const auto& object : properties) if (copy(object)) { copied = true; break; }
        if (!copied) {
            const auto result = phase5.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
