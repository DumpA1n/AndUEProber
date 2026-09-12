#pragma once
#include "Relations.hpp"

namespace andueprober {
struct ObjectFlagProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0;
};
struct ObjectFlagSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uint32_t expectedFlags = 0;
};

// Observes a uint32 ObjectFlags field using 3-16 independent named anchors with
// at least two distinct declared values. Zero and every high bit remain valid;
// no inferred valid-bit mask or engine call is used. Sample extents are disjoint.
// Phase 1 InternalIndex/NamePrivate/ClassPrivate/OuterPrivate must have current
// validated evidence. The exact NameLayout, pool profile, physical pool address
// and generation must match Phase 1 name evidence. Their
// storage ranges must fit the extent and cannot overlap the flags candidate.
// The unique candidate must pass a complete final readback.
// A matching override retains its value/origin/version/validation only when its
// evidence and all required dependency versions are current. Failed attempts
// retain available reports and invalidate prior automatic results. The caller
// owns the immutable inputs and synchronizes target storage throughout the call.
// The required evidence closure is bounded to 4096 offsets, 16384 dependencies
// and 4 MiB of entry/text metadata. Unrelated stale results do not block this field.
Status probeObjectFlags(MemoryReader&, const ObjectFlagProbeProfile&,
    std::span<const ObjectFlagSample>, const NameLayout&, std::uintptr_t pool,
    const NamePoolProfile&, ReadBudget&, Snapshot&);
}
