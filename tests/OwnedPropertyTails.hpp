#pragma once
#include "OwnedProperties.hpp"
#include "andueprober/PropertyTails.hpp"

struct OwnedTailTarget { std::uint64_t identity; };
struct OwnedEnumPropertyTail {
    OwnedNativeProperty property;
    std::uint64_t padding;
    OwnedTailTarget* enumeration;
    std::uint64_t independentGap;
    OwnedTailTarget* underlying;
    OwnedTailTarget* duplicateUnderlying;
    OwnedTailTarget* duplicateEnum;
};
struct OwnedArrayPropertyTail {
    OwnedNativeProperty property;
    std::array<std::uint64_t, 2> padding;
    OwnedTailTarget* inner;
    OwnedTailTarget* duplicateInner;
};
struct OwnedSetPropertyTail {
    OwnedNativeProperty property;
    OwnedTailTarget* element;
    std::uint64_t independentGap;
    OwnedTailTarget* duplicateElement;
};
struct OwnedMapPropertyTail {
    OwnedNativeProperty property;
    std::uint64_t padding;
    OwnedTailTarget* value;
    std::uint64_t independentGap;
    OwnedTailTarget* key;
    OwnedTailTarget* duplicateKey;
    OwnedTailTarget* duplicateValue;
};
struct OwnedObjectPropertyTail {
    OwnedNativeProperty property;
    std::uint64_t padding;
    OwnedTailTarget* propertyClass;
    OwnedTailTarget* duplicateClass;
};
struct OwnedStructPropertyTail {
    OwnedNativeProperty property;
    OwnedTailTarget* structure;
    std::uint64_t padding;
    OwnedTailTarget* duplicateStruct;
};
struct OwnedBytePropertyTail {
    OwnedNativeProperty property;
    std::array<std::uint64_t, 3> padding;
    OwnedTailTarget* enumeration;
    OwnedTailTarget* duplicateEnum;
};
struct OwnedClassPropertyTail {
    OwnedObjectPropertyTail base;
    std::uint64_t independentGap;
    OwnedTailTarget* metaClass;
    OwnedTailTarget* duplicateMetaClass;
};
struct OwnedInterfacePropertyTail {
    OwnedNativeProperty property;
    std::uint64_t padding;
    OwnedTailTarget* interfaceClass;
    OwnedTailTarget* duplicateInterface;
};
struct OwnedPropertyTails final : andueprober::MemoryReader {
    OwnedProperties phase5;
    std::array<OwnedEnumPropertyTail, 3> enumerations;
    std::array<OwnedArrayPropertyTail, 3> arrays;
    std::array<OwnedSetPropertyTail, 3> sets;
    std::array<OwnedMapPropertyTail, 3> maps;
    std::array<OwnedObjectPropertyTail, 3> objects;
    std::array<OwnedStructPropertyTail, 3> structures;
    std::array<OwnedBytePropertyTail, 3> bytes;
    std::array<OwnedClassPropertyTail, 3> classes;
    std::array<OwnedInterfacePropertyTail, 3> interfaces;
    std::array<OwnedTailTarget, 40> targets{};
    std::uint64_t epoch = 1;
    andueprober::Error failure = andueprober::Error::None;
    bool shortRead = false;
    std::size_t reads = 0, targetReads = 0;
    std::function<void(OwnedPropertyTails&, std::uintptr_t, std::size_t)> beforeRead;
    static constexpr std::array<andueprober::PropertyTailKind, 9> kinds{
        andueprober::PropertyTailKind::Enum, andueprober::PropertyTailKind::Array, andueprober::PropertyTailKind::Set,
        andueprober::PropertyTailKind::Map, andueprober::PropertyTailKind::Object, andueprober::PropertyTailKind::Struct,
        andueprober::PropertyTailKind::Byte, andueprober::PropertyTailKind::Class, andueprober::PropertyTailKind::Interface};
    OwnedPropertyTails() {
        const auto initialize = [&](auto& collection) {
            for (std::size_t i = 0; i < collection.size(); ++i) {
                static_assert(std::is_trivially_copyable_v<std::remove_reference_t<decltype(collection[i])>>);
                // The tail scan starts after the complete member; its padding is deterministic.
                std::memset(&collection[i], 0, sizeof(collection[i]));
                collection[i].property = phase5.properties[i];
            }
        };
        initialize(enumerations); initialize(arrays); initialize(sets); initialize(maps); initialize(objects);
        initialize(structures); initialize(bytes); initialize(interfaces);
        for (std::size_t i = 0; i < classes.size(); ++i) {
            std::memset(&classes[i], 0, sizeof(classes[i]));
            classes[i].base.property = phase5.properties[i];
        }
        for (std::size_t i = 0; i < targets.size(); ++i) targets[i].identity = 0x1000 + i;
        for (const auto kind : kinds) for (std::size_t i = 0; i < 3; ++i) {
            setPointer(kind, i, 0, target(kind, i, 0));
            if (dual(kind)) setPointer(kind, i, 1, target(kind, i, 1));
        }
    }
    static bool dual(andueprober::PropertyTailKind kind) {
        return kind == andueprober::PropertyTailKind::Enum || kind == andueprober::PropertyTailKind::Map || kind == andueprober::PropertyTailKind::Class;
    }
    std::uintptr_t target(andueprober::PropertyTailKind kind, std::size_t sample, std::size_t column) const {
        return reinterpret_cast<std::uintptr_t>(&targets[(static_cast<unsigned>(kind) - 1) * 4 + column * 2 + sample % 2]);
    }
    std::uintptr_t address(andueprober::PropertyTailKind kind, std::size_t index) const {
        switch (kind) {
        case andueprober::PropertyTailKind::Enum: return reinterpret_cast<std::uintptr_t>(&enumerations[index]);
        case andueprober::PropertyTailKind::Array: return reinterpret_cast<std::uintptr_t>(&arrays[index]);
        case andueprober::PropertyTailKind::Set: return reinterpret_cast<std::uintptr_t>(&sets[index]);
        case andueprober::PropertyTailKind::Map: return reinterpret_cast<std::uintptr_t>(&maps[index]);
        case andueprober::PropertyTailKind::Struct: return reinterpret_cast<std::uintptr_t>(&structures[index]);
        case andueprober::PropertyTailKind::Byte: return reinterpret_cast<std::uintptr_t>(&bytes[index]);
        case andueprober::PropertyTailKind::Class: return reinterpret_cast<std::uintptr_t>(&classes[index]);
        case andueprober::PropertyTailKind::Interface: return reinterpret_cast<std::uintptr_t>(&interfaces[index]);
        default: return reinterpret_cast<std::uintptr_t>(&objects[index]);
        }
    }
    std::uint32_t extent(andueprober::PropertyTailKind kind) const {
        switch (kind) {
        case andueprober::PropertyTailKind::Enum: return sizeof(OwnedEnumPropertyTail);
        case andueprober::PropertyTailKind::Array: return sizeof(OwnedArrayPropertyTail);
        case andueprober::PropertyTailKind::Set: return sizeof(OwnedSetPropertyTail);
        case andueprober::PropertyTailKind::Map: return sizeof(OwnedMapPropertyTail);
        case andueprober::PropertyTailKind::Struct: return sizeof(OwnedStructPropertyTail);
        case andueprober::PropertyTailKind::Byte: return sizeof(OwnedBytePropertyTail);
        case andueprober::PropertyTailKind::Class: return sizeof(OwnedClassPropertyTail);
        case andueprober::PropertyTailKind::Interface: return sizeof(OwnedInterfacePropertyTail);
        default: return sizeof(OwnedObjectPropertyTail);
        }
    }
    static std::uint32_t offset(andueprober::PropertyTailKind kind, std::size_t column) {
        switch (kind) {
        case andueprober::PropertyTailKind::Enum: return column ? offsetof(OwnedEnumPropertyTail, enumeration) : offsetof(OwnedEnumPropertyTail, underlying);
        case andueprober::PropertyTailKind::Array: return offsetof(OwnedArrayPropertyTail, inner);
        case andueprober::PropertyTailKind::Set: return offsetof(OwnedSetPropertyTail, element);
        case andueprober::PropertyTailKind::Map: return column ? offsetof(OwnedMapPropertyTail, value) : offsetof(OwnedMapPropertyTail, key);
        case andueprober::PropertyTailKind::Struct: return offsetof(OwnedStructPropertyTail, structure);
        case andueprober::PropertyTailKind::Byte: return offsetof(OwnedBytePropertyTail, enumeration);
        case andueprober::PropertyTailKind::Class: return column ? offsetof(OwnedClassPropertyTail, metaClass) : offsetof(OwnedObjectPropertyTail, propertyClass);
        case andueprober::PropertyTailKind::Interface: return offsetof(OwnedInterfacePropertyTail, interfaceClass);
        default: return offsetof(OwnedObjectPropertyTail, propertyClass);
        }
    }
    andueprober::PropertyTailProfile profile(andueprober::PropertyTailKind kind) const {
        return {"owned-property-tail:" + std::to_string(static_cast<unsigned>(kind)), phase5.profile(), extent(kind), sizeof(OwnedNativeProperty), kind};
    }
    std::array<andueprober::PropertyTailSample, 3> samples(andueprober::PropertyTailKind kind) const {
        std::array<andueprober::PropertyTailSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = {address(kind, i), "owned-tail:" + std::to_string(i), target(kind, i, 0),
                "owned-target:" + std::to_string(static_cast<unsigned>(kind)) + ":first:" + std::to_string(i % 2), {}, {}};
            if (dual(kind)) {
                result[i].expectedSecond = target(kind, i, 1);
                result[i].secondIdentity = "owned-target:" + std::to_string(static_cast<unsigned>(kind)) + ":second:" + std::to_string(i % 2);
            }
        }
        return result;
    }
    andueprober::Snapshot initial() {
        auto result = phase5.initial(); const auto& source = phase5.phase5.nameMemory();
        andueprober::ReadBudget budget; budget.generation = epoch;
        if (!andueprober::probePropertyFields(*this, phase5.profile(), phase5.samples(), source.nameLayout,
            source.address(64), source.pool, budget, result)) std::abort();
        reads = 0; targetReads = 0; return result;
    }
    void setPointer(andueprober::PropertyTailKind kind, std::size_t i, std::size_t column, std::uintptr_t value, bool duplicate = false) {
        auto* pointer = reinterpret_cast<OwnedTailTarget*>(value);
        switch (kind) {
        case andueprober::PropertyTailKind::Enum:
            if (column) { if (duplicate) enumerations[i].duplicateEnum = pointer; else enumerations[i].enumeration = pointer; }
            else { if (duplicate) enumerations[i].duplicateUnderlying = pointer; else enumerations[i].underlying = pointer; } break;
        case andueprober::PropertyTailKind::Array: if (duplicate) arrays[i].duplicateInner = pointer; else arrays[i].inner = pointer; break;
        case andueprober::PropertyTailKind::Set: if (duplicate) sets[i].duplicateElement = pointer; else sets[i].element = pointer; break;
        case andueprober::PropertyTailKind::Map:
            if (column) { if (duplicate) maps[i].duplicateValue = pointer; else maps[i].value = pointer; }
            else { if (duplicate) maps[i].duplicateKey = pointer; else maps[i].key = pointer; } break;
        case andueprober::PropertyTailKind::Struct: if (duplicate) structures[i].duplicateStruct = pointer; else structures[i].structure = pointer; break;
        case andueprober::PropertyTailKind::Byte: if (duplicate) bytes[i].duplicateEnum = pointer; else bytes[i].enumeration = pointer; break;
        case andueprober::PropertyTailKind::Class:
            if (column) { if (duplicate) classes[i].duplicateMetaClass = pointer; else classes[i].metaClass = pointer; }
            else { if (duplicate) classes[i].base.duplicateClass = pointer; else classes[i].base.propertyClass = pointer; } break;
        case andueprober::PropertyTailKind::Interface: if (duplicate) interfaces[i].duplicateInterface = pointer; else interfaces[i].interfaceClass = pointer; break;
        default: if (duplicate) objects[i].duplicateClass = pointer; else objects[i].propertyClass = pointer; break;
        }
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads; if (beforeRead) beforeRead(*this, address, destination.size());
        if (failure != andueprober::Error::None) return {0, failure};
        const auto targetBegin = reinterpret_cast<std::uintptr_t>(targets.data());
        if (address >= targetBegin && address - targetBegin < sizeof(targets)) { ++targetReads; return {0, andueprober::Error::Unmapped}; }
        const auto copy = [&](const auto& object) {
            const auto start = reinterpret_cast<std::uintptr_t>(&object);
            if (address < start || address - start > sizeof(object) || destination.size() > sizeof(object) - (address - start)) return false;
            std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&object) + address - start, destination.size()); return true;
        };
        bool copied = false;
        for (std::size_t i = 0; i < 3 && !copied; ++i)
            copied = copy(enumerations[i]) || copy(arrays[i]) || copy(sets[i]) || copy(maps[i]) || copy(objects[i]) ||
                copy(structures[i]) || copy(bytes[i]) || copy(classes[i]) || copy(interfaces[i]);
        if (!copied) {
            const auto result = phase5.read(address, destination);
            if (result.error != andueprober::Error::None) return result;
        }
        return {shortRead && !destination.empty() ? destination.size() - 1 : destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};
