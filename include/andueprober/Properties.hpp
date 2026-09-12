#pragma once
#include "Fields.hpp"

namespace andueprober {
struct PropertyProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0, fieldBaseExtent = 0;
    std::string fieldBaseProfileIdentity;
    FieldOwnerLayout ownerLayout;
};
struct PropertySample {
    std::uintptr_t object = 0;
    std::string identity;
    std::int32_t expectedArrayDim = 0, expectedElementSize = 0;
    std::uint64_t expectedPropertyFlags = 0;
    std::int32_t expectedOffsetInternal = 0;
    std::uint32_t containingValueSize = 0;
    std::string containingValueIdentity;
};

// Binds scalar observations to their module/generation, independently declared
// inherited prefix, FField metadata profile, owner layout and canonical names.
// The scan extent is not part of this layout observation identity.
std::string propertyObservationIdentity(const PropertyProbeProfile&, const NameLayout&,
    std::uintptr_t pool, const NamePoolProfile&);

// Observes only FProperty ArrayDim, ElementSize, PropertyFlags and Offset_Internal.
// Three to sixteen distinct named anchors supply independent metadata, with at
// least two distinct expectations per field. ArrayDim/ElementSize are positive
// int32 values; Offset_Internal is nonnegative, including zero. PropertyFlags is
// read as a complete uint64 value. The declared containing value extent (up to
// 16 MiB) is independent of the metadata-object scan extent. Checked multiplication
// and addition require the entire declared property value to fit that extent.
// No containing-value memory, property pointers or engine functions are accessed.
// fieldBaseExtent independently declares the occupied inherited prefix; it must
// not be derived from a scan extent or a candidate property offset. It is not a
// universal sizeof(FField): compiled derived layouts may reuse base tail padding.
//
// Only FField layouts are supported. Current validated Phase 1/2 and all five
// FField base fields supply dependency versions. All four unique disjoint field
// candidates follow the complete inherited prefix and pass final readback before
// atomic publication. Name and owner layouts must match their preceding evidence
// sources, including the physical pool and generation. All five base field ranges
// must fit the declared prefix. Strict matching-user override validation preserves
// value, origin, version and validation and rejects stale or incomplete evidence.
// Failed attempts preserve reports and stale automatic
// results. The caller owns input storage and synchronizes target metadata.
// Required dependencies and tentative outputs pass bounded iterative transitive
// validation (4096 nodes, 16384 edges, 4 MiB evidence) with the read deadline and
// cancellation. Unrelated stale results do not prevent this operation.
// Container, enum, property subclass metadata and complete Phase 5 are separate.
Status probePropertyFields(MemoryReader&, const PropertyProbeProfile&,
    std::span<const PropertySample>, const NameLayout&, std::uintptr_t pool,
    const NamePoolProfile&, ReadBudget&, Snapshot&);

enum class BoolTailRepresentation { Unknown, NativeBool };
struct PropertyBaseProbeProfile {
    std::string identity, moduleIdentity, propertyObservationIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0;
    BoolTailRepresentation boolRepresentation = BoolTailRepresentation::Unknown;
};
struct PropertyPointerTailSample {
    std::uintptr_t object = 0, expectedPointer = 0;
    std::string identity;
};
struct PropertyBoolTailSample {
    std::uintptr_t object = 0;
    std::string identity;
};

// Discovers both the aligned FProperty base size and the first known pointer in
// derived-property storage. Distinct pointer anchors come from independent struct/object
// property relationships. NativeBool anchors locate the supported 01 00 01 ff
// metadata tuple and establish the preceding eight-byte-aligned base boundary.
// The two results can differ when derived properties insert leading metadata.
// Pointers are compared as opaque values; pointed-to storage is never accessed.
Status probePropertyBases(MemoryReader&, const PropertyBaseProbeProfile&,
    std::span<const PropertyPointerTailSample>, std::span<const PropertyBoolTailSample>,
    ReadBudget&, Snapshot&);
}
