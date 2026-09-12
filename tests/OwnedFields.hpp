#pragma once
#include "OwnedFunctions.hpp"
#include "andueprober/Fields.hpp"

struct OwnedFieldName { std::uint32_t index, number; };
struct OwnedFieldOwner { std::uintptr_t pointer; std::uint8_t isUObject; };
struct OwnedReverseFieldOwner { std::uint8_t isUObject; std::uintptr_t pointer; };
struct OwnedFieldClass { std::uint64_t identity; };
struct OwnedNativeFField {
    OwnedFieldName name;
    OwnedFieldOwner owner;
    OwnedNativeFField* next;
    OwnedFieldClass* klass;
    std::uint32_t flags;
    OwnedFieldName duplicateName;
    OwnedFieldOwner duplicateOwner;
    OwnedNativeFField* duplicateNext;
    OwnedFieldClass* duplicateClass;
    std::uint32_t duplicateFlags;
};
struct OwnedReverseFField {
    OwnedFieldName name;
    OwnedReverseFieldOwner owner;
    OwnedNativeFField* next;
    OwnedFieldClass* klass;
    std::uint32_t flags;
};
struct OwnedFields final : andueprober::MemoryReader {
    OwnedFunctions phase4;
    std::array<OwnedNativeFField, 3> fields;
    std::array<OwnedReverseFField, 3> reversed;
    std::array<OwnedFieldClass, 2> classes{{{0x41414141}, {0x42424242}}};
    static constexpr std::array<std::uint32_t, 3> flags{0x80000100, 0x40000200, 0x10000400};
    static constexpr std::array<std::uint32_t, 3> nameIds{64, 80, 96};
    const std::array<std::string, 3> names{"FieldAlpha", "FieldBeta", "FieldGamma"};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0;
    std::function<void(OwnedFields&, std::uintptr_t, std::size_t)> beforeRead;
    OwnedRelations& nameMemory() { return phase4.phase3.phase2.phase1; }
    const OwnedRelations& nameMemory() const { return phase4.phase3.phase2.phase1; }
    std::uintptr_t declaredOwner(std::size_t index) const {
        return index == 1 ? reinterpret_cast<std::uintptr_t>(&fields[0]) :
            reinterpret_cast<std::uintptr_t>(&phase4.phase3.phase2.structs[index]);
    }
    OwnedFields() {
        static_assert(std::is_trivially_copyable_v<OwnedNativeFField> && std::is_trivially_copyable_v<OwnedReverseFField>);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            nameMemory().entry(nameIds[i], names[i]);
            // Scanned padding is deterministic independent fixture data.
            std::memset(&fields[i], 0, sizeof(fields[i]));
            fields[i].name = {nameIds[i], 0};
            fields[i].owner.pointer = declaredOwner(i); fields[i].owner.isUObject = i != 1;
            fields[i].next = i + 1 < fields.size() ? &fields[i + 1] : nullptr;
            fields[i].klass = &classes[i % classes.size()]; fields[i].flags = flags[i];
            fields[i].duplicateName = {0xffffffff, 0xffffffff}; fields[i].duplicateFlags = 0xDDDDDDDD;
            std::memset(&reversed[i], 0, sizeof(reversed[i]));
            reversed[i].name = {nameIds[i], 0};
            reversed[i].owner.pointer = declaredOwner(i); reversed[i].owner.isUObject = i != 1;
            reversed[i].next = fields[i].next; reversed[i].klass = fields[i].klass; reversed[i].flags = flags[i];
        }
    }
    andueprober::FieldProbeProfile profile(bool reverse = false) const {
        return {"owned-compiled-ffield-metadata-v1", "owned-structs-image", epoch, andueprober::Layout::FField,
            static_cast<std::uint32_t>(reverse ? sizeof(OwnedReverseFField) : sizeof(OwnedNativeFField)),
            {andueprober::FieldOwnerRepresentation::SeparateBoolean,
                static_cast<std::uint32_t>(reverse ? offsetof(OwnedReverseFieldOwner, pointer) : offsetof(OwnedFieldOwner, pointer)),
                static_cast<std::uint32_t>(reverse ? offsetof(OwnedReverseFieldOwner, isUObject) : offsetof(OwnedFieldOwner, isUObject)),
                static_cast<std::uint32_t>(reverse ? sizeof(OwnedReverseFieldOwner) : sizeof(OwnedFieldOwner))}};
    }
    std::array<andueprober::FieldBaseSample, 3> samples(bool reverse = false) const {
        std::array<andueprober::FieldBaseSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = {reverse ? reinterpret_cast<std::uintptr_t>(&reversed[i]) : reinterpret_cast<std::uintptr_t>(&fields[i]),
                (reverse ? "owned-reverse-ffield:" : "owned-ffield:") + std::to_string(i), names[i], declaredOwner(i),
                i == 1 ? "owned-ffield:0" : "owned-struct:" + std::to_string(i), i != 1,
                i + 1 < fields.size() ? reinterpret_cast<std::uintptr_t>(&fields[i + 1]) : 0,
                i + 1 < fields.size() ? "owned-ffield:" + std::to_string(i + 1) : "null",
                reinterpret_cast<std::uintptr_t>(&classes[i % classes.size()]), "owned-field-class:" + std::to_string(i % classes.size()), flags[i]};
        return result;
    }
    andueprober::Snapshot initial() {
        auto result = phase4.initial(); auto functions = phase4.samples();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probeFunctionFields(*this, phase4.profile(), functions, budget, result)) std::abort();
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
        for (const auto& object : fields) if (copy(object)) { copied = true; break; }
        if (!copied) for (const auto& object : reversed) if (copy(object)) { copied = true; break; }
        // FFieldClass and native function addresses are deliberately unreadable.
        if (!copied) {
            const auto result = phase4.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
