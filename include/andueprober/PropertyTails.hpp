#pragma once
#include "Properties.hpp"

namespace andueprober {
enum class PropertyTailKind { Unknown, Enum, Array, Set, Map, Object, Bool, Struct, Byte, Class, Interface };
struct PropertyTailProfile {
    std::string identity;
    PropertyProbeProfile property;
    std::uint32_t extent = 0, propertyBaseExtent = 0;
    PropertyTailKind kind = PropertyTailKind::Unknown;
};
struct PropertyTailSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uintptr_t expectedFirst = 0;
    std::string firstIdentity;
    std::optional<std::uintptr_t> expectedSecond;
    std::string secondIdentity;
};

// Observes only the following kind-specific opaque pointer fields:
// Enum: FEnumProperty::UnderlyingType / Enum; Array: FArrayProperty::Inner;
// Set: FSetProperty::ElementProp; Map: FMapProperty::KeyProp / ValueProp;
// Object: FObjectPropertyBase::PropertyClass; Struct: FStructProperty::Struct;
// Byte: FByteProperty::Enum; Class: FObjectPropertyBase::PropertyClass /
// FClassProperty::MetaClass; Interface: FInterfaceProperty::InterfaceClass.
// The slash separates first/second.
// Single-field kinds require absent expectedSecond and empty secondIdentity.
// Dual-field kinds require expectedSecond, including explicitly declared zero.
// Each pointer column requires two distinct non-null expectations among 3-16
// independently named, non-overlapping metadata objects. Named targets are only
// compared; no target pointer is followed, type-checked, masked or invoked.
// Bool and Unknown return Unsupported; no bool mask or base-size inference occurs.
//
// property preserves the preceding scalar profile. propertyBaseExtent separately
// declares the occupied FProperty prefix, not an inferred sizeof or candidate.
// All nine FField/FProperty ranges fit and remain disjoint inside this prefix.
// Name, Owner and scalar evidence must match their complete observation identities.
// Tail fields start after the prefix, scan independently, remain disjoint and pass
// final readback before atomic publication. Required dependencies and tentative
// outputs pass bounded iterative evidence validation. Matching User overrides
// preserve ownership and require current validated evidence and prerequisites.
// The caller owns immutable input and synchronizes target metadata. Byte budgets,
// deadlines and cancellation remain shared with the supplied ReadBudget; input
// and retained report metadata are each bounded by 4 MiB. This operation does not
// establish complete container layouts, element ownership or complete Phase 5.
Status probePropertyTails(MemoryReader&, const PropertyTailProfile&, std::span<const PropertyTailSample>,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, Snapshot&);
}
