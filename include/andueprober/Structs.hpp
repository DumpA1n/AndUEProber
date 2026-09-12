#pragma once
#include "Relations.hpp"

namespace andueprober {
struct StructProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0, fieldExtent = 0;
};
struct StructSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uintptr_t expectedSuper = 0;
    std::string superIdentity;
    std::uintptr_t expectedFirstChild = 0;
    std::string firstChildIdentity;
    std::uint32_t expectedPropertiesSize = 0;
    std::optional<std::uintptr_t> expectedChildProperties;
    std::string childPropertiesIdentity;
    std::optional<std::uint32_t> expectedMinAlignment;
};
struct StructFieldSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uintptr_t expectedNext = 0;
    std::string nextIdentity;
};

// Expected values and links must come from independent named metadata, such as an
// owned engine's compiled types. Reading an unknown offset to manufacture its own
// expected value is unsupported. A profile supplies 2-4 struct samples and 3-64
// distinct field samples. Cycles among declared Super/Next anchors are rejected.
// Phase 1 Index/Name/Class/Outer evidence must already be validated. The caller
// owns the mutable snapshot and sample containers for the entire operation.
//
// The operation invalidates prior automatic Phase 2 fields, gathers reports and
// rechecks observations before publishing all required offsets together. User
// overrides remain intact; they require existing validated evidence and current
// Phase 1 dependency versions. Matching an unvalidated or stale override and
// contradictory observations both fail the phase. FField
// requires explicit ChildProperties expectations, including a present zero.
// MinAlignment requires at least two independent scalar expectations; missing
// alignment metadata never derives a neighboring offset. Read failures preserve
// reports and leave prior automatic offsets stale. No engine functions are called.
Status probeStructFields(MemoryReader&, const StructProbeProfile&,
    std::span<const StructSample>, std::span<const StructFieldSample>, ReadBudget&, Snapshot&);
}
