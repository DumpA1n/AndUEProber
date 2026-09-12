#pragma once
#include "OwnedStructs.hpp"
#include "andueprober/Enums.hpp"

struct OwnedEnumEntry {
    std::uint32_t name = 0, number = 0;
    std::int64_t value = 0;
};
struct OwnedValueFirstEnumEntry {
    std::int64_t value = 0;
    std::uint32_t name = 0, number = 0;
};
struct OwnedEnumArray {
    void* data = nullptr;
    std::int32_t count = 0, capacity = 0;
};
struct OwnedCountFirstEnumArray {
    std::int32_t count = 0, capacity = 0;
    void* data = nullptr;
};
struct OwnedNativeEnum {
    OwnedNativeField field;
    std::uint64_t independentPadding = UINT64_MAX;
    OwnedEnumArray names, duplicate;
};
struct OwnedCountFirstEnum {
    OwnedNativeField field;
    std::uint64_t independentPadding = UINT64_MAX;
    OwnedCountFirstEnumArray names, duplicate;
};

struct OwnedEnums final : andueprober::MemoryReader {
    OwnedStructs phase2;
    std::array<OwnedNativeEnum, 3> enums;
    std::array<OwnedCountFirstEnum, 3> countFirstEnums;
    std::array<std::array<OwnedEnumEntry, 3>, 3> entries;
    std::array<std::array<OwnedValueFirstEnumEntry, 3>, 3> valueFirstEntries;
    bool countFirst = false, valueFirst = false;
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::function<void(OwnedEnums&, std::uintptr_t, std::size_t)> beforeRead;
    static constexpr std::array<std::array<std::int64_t, 3>, 3> values{{
        {{-7, 1000000, 9}}, {{INT64_MIN, INT64_MAX, 10}}, {{0, 0, -1}}}};

    explicit OwnedEnums(bool counterFirst = false, bool scalarFirst = false)
        : countFirst(counterFirst), valueFirst(scalarFirst) {
        for (std::size_t i = 0; i < 3; ++i) {
            enums[i].field = phase2.fields[i]; countFirstEnums[i].field = phase2.fields[i];
            for (std::size_t j = 0; j < 3; ++j) {
                const auto id = static_cast<std::uint32_t>(64 + (i * 3 + j) * 16);
                phase2.phase1.entry(id, entryName(i, j));
                entries[i][j] = {id, 0, values[i][j]}; valueFirstEntries[i][j] = {values[i][j], id, 0};
            }
            const auto data = valueFirst ? static_cast<void*>(valueFirstEntries[i].data()) : static_cast<void*>(entries[i].data());
            enums[i].names = {data, static_cast<std::int32_t>(i + 1), 3};
            countFirstEnums[i].names = {static_cast<std::int32_t>(i + 1), 3, data};
        }
    }
    static std::string entryName(std::size_t i, std::size_t j) {
        return "Owned" + std::to_string(i) + "::Value" + std::to_string(j);
    }
    std::uintptr_t object(std::size_t i) const {
        return countFirst ? reinterpret_cast<std::uintptr_t>(&countFirstEnums[i]) : reinterpret_cast<std::uintptr_t>(&enums[i]);
    }
    std::uintptr_t entryAddress(std::size_t i, std::size_t j) const {
        return valueFirst ? reinterpret_cast<std::uintptr_t>(&valueFirstEntries[i][j]) : reinterpret_cast<std::uintptr_t>(&entries[i][j]);
    }
    std::uint32_t valueOffset() const { return valueFirst ? offsetof(OwnedValueFirstEnumEntry, value) : offsetof(OwnedEnumEntry, value); }
    std::uint32_t namesOffset() const { return countFirst ? offsetof(OwnedCountFirstEnum, names) : offsetof(OwnedNativeEnum, names); }
    std::uint32_t duplicateOffset() const { return countFirst ? offsetof(OwnedCountFirstEnum, duplicate) : offsetof(OwnedNativeEnum, duplicate); }
    andueprober::EnumProbeProfile profile(andueprober::Layout layout = andueprober::Layout::FField) const {
        andueprober::EnumProbeProfile result;
        result.identity = "owned-enum-metadata-v1"; result.moduleIdentity = "owned-structs-image";
        result.generation = epoch; result.layout = layout;
        result.extent = countFirst ? sizeof(OwnedCountFirstEnum) : sizeof(OwnedNativeEnum);
        static_assert(sizeof(OwnedNativeField) <= offsetof(OwnedNativeEnum, names));
        static_assert(sizeof(OwnedNativeField) <= offsetof(OwnedCountFirstEnum, names));
        result.fieldBaseExtent = sizeof(OwnedNativeField);
        result.array = countFirst ? andueprober::EnumArrayLayout{
            offsetof(OwnedCountFirstEnumArray, data), offsetof(OwnedCountFirstEnumArray, count),
            offsetof(OwnedCountFirstEnumArray, capacity), sizeof(OwnedCountFirstEnumArray)} : andueprober::EnumArrayLayout{
            offsetof(OwnedEnumArray, data), offsetof(OwnedEnumArray, count), offsetof(OwnedEnumArray, capacity), sizeof(OwnedEnumArray)};
        result.entry = valueFirst ? andueprober::EnumEntryLayout{offsetof(OwnedValueFirstEnumEntry, name),
            offsetof(OwnedValueFirstEnumEntry, value), sizeof(OwnedValueFirstEnumEntry)} : andueprober::EnumEntryLayout{
            offsetof(OwnedEnumEntry, name), offsetof(OwnedEnumEntry, value), sizeof(OwnedEnumEntry)};
        return result;
    }
    std::array<andueprober::EnumSample, 3> samples() const {
        std::array<andueprober::EnumSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = {object(i), "owned-enum:" + std::to_string(i), {}};
            for (std::size_t j = 0; j <= i; ++j)
                result[i].values.push_back({entryName(i, j), "owned-enum:" + std::to_string(i) + ":entry:" + std::to_string(j), values[i][j]});
        }
        return result;
    }
    andueprober::Snapshot initial(andueprober::Layout layout = andueprober::Layout::FField) {
        auto snapshot = phase2.initial(layout);
        const auto structures = phase2.structSamples(); const auto fields = phase2.fieldSamples();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probeStructFields(*this, phase2.profile(layout), structures, fields, budget, snapshot)) std::abort();
        reads = 0; return snapshot;
    }
    void setValue(std::size_t i, std::size_t j, std::int64_t value) {
        if (valueFirst) valueFirstEntries[i][j].value = value; else entries[i][j].value = value;
    }
    void setName(std::size_t i, std::size_t j, std::uint32_t value) {
        if (valueFirst) valueFirstEntries[i][j].name = value; else entries[i][j].name = value;
    }
    void setHeader(std::size_t i, void* data, std::int32_t count, std::int32_t capacity) {
        if (countFirst) countFirstEnums[i].names = {count, capacity, data}; else enums[i].names = {data, count, capacity};
    }
    void duplicate() {
        for (std::size_t i = 0; i < 3; ++i)
            if (countFirst) countFirstEnums[i].duplicate = countFirstEnums[i].names; else enums[i].duplicate = enums[i].names;
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        const auto copy = [&](const auto& storage) {
            const auto start = reinterpret_cast<std::uintptr_t>(&storage);
            if (address < start || address - start > sizeof(storage) || destination.size() > sizeof(storage) - (address - start)) return false;
            std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&storage) + address - start, destination.size());
            return true;
        };
        bool copied = false;
        for (std::size_t i = 0; i < 3 && !copied; ++i)
            copied = copy(enums[i]) || copy(countFirstEnums[i]) || copy(entries[i]) || copy(valueFirstEntries[i]);
        if (!copied) {
            const auto result = phase2.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
