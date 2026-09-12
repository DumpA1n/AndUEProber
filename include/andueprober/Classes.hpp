#pragma once
#include "Structs.hpp"

namespace andueprober {
struct ClassProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0;
};
struct ClassSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uint64_t expectedCastFlags = 0;
    std::uintptr_t expectedDefaultObject = 0;
    std::string defaultObjectIdentity;
};

// Independent compiled metadata supplies 3-16 distinct named class anchors,
// at least two flag values and two distinct non-null default objects. The caller
// owns the mutable snapshot and sample containers throughout this operation.
// No expected values, object sizes or sample order are inferred from unknown
// fields. Default objects are compared as pointers and are not dereferenced.
//
// Phase 1 Index/Name/Class/Outer and Phase 2 Next/SuperStruct/Children/PropertiesSize
// require validated evidence; FField also requires ChildProperties. Their versions
// become dependencies of both results. MinAlignment is independent and optional.
// Bounded scans include offset zero. Final rechecks precede publication of both
// fields together. Failures retain reports and stale prior automatic results;
// matching user overrides retain their origin/version. Overrides require existing
// validated evidence and current required dependency versions; matching an
// unvalidated or stale override fails without changing its validation. Conflicting
// overrides fail without replacement. This operation does not call engine functions.
Status probeClassFields(MemoryReader&, const ClassProbeProfile&,
    std::span<const ClassSample>, ReadBudget&, Snapshot&);
}
