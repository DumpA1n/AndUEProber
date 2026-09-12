#pragma once
#include "OwnedProperties.hpp"
#include "andueprober/PropertyValues.hpp"

struct OwnedBoolMetadata {
    OwnedNativeProperty property;
    std::uint8_t padding0, fieldMask, padding1, byteOffset, padding2, fieldSize, padding3, byteMask;
    std::array<std::uint8_t, 4> duplicate;
};
struct OwnedPathMetadata {
    OwnedNativeProperty property;
    std::uint64_t gap;
    OwnedFieldName propertyClass;
    std::uint64_t independentGap;
    OwnedFieldName duplicate;
};
struct OwnedPropertyValues final : andueprober::MemoryReader {
    OwnedProperties phase5;
    std::array<OwnedBoolMetadata, 3> booleans;
    std::array<OwnedPathMetadata, 3> paths;
    std::uint64_t epoch = 1;
    std::size_t reads = 0;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::function<void(OwnedPropertyValues&, std::uintptr_t, std::size_t)> beforeRead;
    static constexpr std::array<std::array<std::uint8_t, 4>, 3> expected{{{1, 0, 1, 255}, {4, 2, 8, 8}, {8, 3, 128, 128}}};
    const std::array<std::string, 3> names{"FieldPathAlpha", "FieldPathBeta", "FieldPathGamma_2"};
    static constexpr std::array<std::uint32_t, 3> nameIds{112, 144, 176};
    OwnedPropertyValues() {
        for (std::size_t i = 0; i < booleans.size(); ++i) {
            // Deliberate nonsemantic bytes prevent padding from creating success.
            std::memset(&booleans[i], 0xCC, sizeof(booleans[i])); booleans[i].property = phase5.properties[i];
            setBool(i, expected[i]);
            std::memset(&paths[i], 0xDD, sizeof(paths[i])); paths[i].property = phase5.properties[i];
            phase5.phase5.nameMemory().entry(nameIds[i], i == 2 ? "FieldPathGamma" : names[i]);
            paths[i].propertyClass = {nameIds[i], i == 2 ? 3u : 0u};
        }
    }
    void setBool(std::size_t i, std::array<std::uint8_t, 4> value) {
        booleans[i].fieldSize = value[0]; booleans[i].byteOffset = value[1]; booleans[i].byteMask = value[2]; booleans[i].fieldMask = value[3];
    }
    static constexpr std::array<std::uint32_t, 4> boolOffsets{offsetof(OwnedBoolMetadata, fieldSize), offsetof(OwnedBoolMetadata, byteOffset),
        offsetof(OwnedBoolMetadata, byteMask), offsetof(OwnedBoolMetadata, fieldMask)};
    andueprober::BoolPropertyProfile boolProfile() const {
        return {"owned-compiled-bool-v1", phase5.profile(), sizeof(OwnedBoolMetadata), sizeof(OwnedNativeProperty)};
    }
    andueprober::FieldPathPropertyProfile pathProfile() const {
        return {"owned-compiled-inline-fieldpath-v1", phase5.profile(), sizeof(OwnedPathMetadata), sizeof(OwnedNativeProperty), andueprober::FieldPathRepresentation::InlineFName};
    }
    std::array<andueprober::BoolPropertySample, 3> boolSamples() const {
        std::array<andueprober::BoolPropertySample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = {reinterpret_cast<std::uintptr_t>(&booleans[i]), "owned-bool:" + std::to_string(i),
            i == 0 ? andueprober::BoolEncoding::NativeByte : andueprober::BoolEncoding::SingleBit,
            expected[i][0], expected[i][1], expected[i][2], expected[i][3], expected[i][0], "owned-storage:" + std::to_string(i)};
        return result;
    }
    std::array<andueprober::FieldPathPropertySample, 3> pathSamples() const {
        std::array<andueprober::FieldPathPropertySample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = {reinterpret_cast<std::uintptr_t>(&paths[i]), "owned-fieldpath:" + std::to_string(i), names[i]};
        return result;
    }
    andueprober::Snapshot initial() {
        auto result = phase5.initial(); const auto& source = phase5.phase5.nameMemory(); andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probePropertyFields(*this, phase5.profile(), phase5.samples(), source.nameLayout, source.address(64), source.pool, budget, result)) std::abort();
        reads = 0; return result;
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads; if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        const auto copy = [&](const auto& object) {
            const auto start = reinterpret_cast<std::uintptr_t>(&object);
            if (address < start || address - start > sizeof(object) || destination.size() > sizeof(object) - (address - start)) return false;
            std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&object) + address - start, destination.size()); return true;
        };
        bool copied = false;
        for (std::size_t i = 0; i < booleans.size() && !copied; ++i) copied = copy(booleans[i]) || copy(paths[i]);
        if (!copied) {
            const auto result = phase5.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
