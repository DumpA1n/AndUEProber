#pragma once
#include "Relations.hpp"

namespace andueprober {
struct EnumArrayLayout {
    std::optional<std::uint32_t> data, count, capacity;
    std::uint32_t size = 0;
};
struct EnumEntryLayout {
    std::optional<std::uint32_t> name, value;
    std::uint32_t stride = 0;
};
struct EnumProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0, fieldBaseExtent = 0;
    EnumArrayLayout array;
    EnumEntryLayout entry;
    std::uint32_t maximumValues = 4096, maximumCapacity = 65536;
};
struct EnumValueSample {
    std::string expectedName, identity;
    std::int64_t expectedValue = 0;
};
struct EnumSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::vector<EnumValueSample> values;
};

// Observes only UEnum::Names from 3-16 independently declared enum objects.
// Array entries contain the declared FName representation and an int64 value.
// Signed boundaries, negative values and aliases are supported without a value
// mask or magnitude heuristic. Every declared entry is read, and the unique
// candidate requires complete final header/data readback across all anchors.
// Each anchor and its nonempty array storage is disjoint from the other anchors.
//
// fieldBaseExtent is an independently declared reserved UField prefix, not an
// inferred sizeof or a scan result. Validated Phase 1 fields and UField::Next
// must fit within that prefix; array candidates begin after it. The exact name
// layout, pool profile/address and generation match Phase 1 evidence. Overrides
// require matching validated evidence and current dependency versions. Failed
// attempts retain reports and invalidate earlier automatic observations.
//
// At most 16384 independently named entries and 4 MiB of sample metadata are
// admitted. The caller owns immutable inputs and synchronizes target storage;
// reads share the supplied byte budget, cancellation and deadline. This operation
// neither invokes ProcessEvent nor establishes a callable engine signature.
Status probeEnumNames(MemoryReader&, const EnumProbeProfile&, std::span<const EnumSample>,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, Snapshot&);
}
