#pragma once
#include "Structs.hpp"

namespace andueprober {
enum class FieldOwnerRepresentation { Unknown, SeparateBoolean, Tagged };
struct FieldOwnerLayout {
    FieldOwnerRepresentation representation = FieldOwnerRepresentation::Unknown;
    std::uint32_t pointerOffset = 0, kindOffset = 8, size = 16;
};
struct FieldProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0;
    FieldOwnerLayout ownerLayout;
};
struct FieldBaseSample {
    std::uintptr_t object = 0;
    std::string identity, expectedName;
    std::uintptr_t expectedOwner = 0;
    std::string ownerIdentity;
    bool ownerIsUObject = false;
    std::uintptr_t expectedNext = 0;
    std::string nextIdentity;
    std::uintptr_t expectedClass = 0;
    std::string classIdentity;
    std::uint32_t expectedFlags = 0;
};
// Names the module, generation, metadata profile, owner representation and
// canonical name-pool observation used by FField base evidence. The scan extent
// does not identify the field layout or its occupied inherited prefix.
std::string fieldObservationIdentity(const FieldProbeProfile&, const NameLayout&,
    std::uintptr_t pool, const NamePoolProfile&);

// Observes only FField NamePrivate, Owner, Next, ClassPrivate and FlagsPrivate.
// Property/container metadata is outside this operation. Three to sixteen distinct
// named anchors declare their relationships independently of the scanned bytes.
// Owner supports only the explicit SeparateBoolean layout: a raw 64-bit pointer
// and a disjoint byte encoded as 0/1. Tagged/unknown representations are unsupported.
// Class and owner pointers are compared without dereferencing; Next cycles among
// supplied anchors reject. No pointer masking or engine invocation is performed.
//
// The caller reuses the same canonical NameLayout, pool address and NamePoolProfile
// as Phase 1. The layout, physical pool address and generation must match validated
// Phase 1 name evidence. Callers retain the actual pool's lifetime.
// Phase 1 Index/Name/Class/Outer and Phase 2 Next/SuperStruct/Children/PropertiesSize/
// ChildProperties require validated current evidence. All five results record
// those versions, occupy disjoint ranges and pass final readback before publication.
// Required dependencies and tentative outputs pass bounded iterative transitive
// validation (4096 nodes, 16384 edges, 4 MiB evidence) with the read deadline and
// cancellation. Unrelated stale results do not prevent this operation.
// Matching overrides require passing validated evidence and all current required
// dependencies. Their value/origin/version/validation remain unchanged. Failures
// preserve available reports and stale automatic results. Input/storage ownership
// and synchronization remain with the caller throughout the operation.
Status probeFieldFields(MemoryReader&, const FieldProbeProfile&, std::span<const FieldBaseSample>,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, Snapshot&);
}
