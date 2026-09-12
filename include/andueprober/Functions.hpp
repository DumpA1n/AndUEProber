#pragma once
#include "Structs.hpp"

namespace andueprober {
struct FunctionProbeProfile {
    std::string identity, moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::uint32_t extent = 0;
};
struct FunctionSample {
    std::uintptr_t object = 0;
    std::string identity;
    std::uint32_t expectedFlags = 0;
    std::uint8_t expectedNumParms = 0;
    std::uint16_t expectedParmsSize = 0;
    std::uint16_t expectedReturnOffset = 0xffff;
    std::uintptr_t expectedNativeFunction = 0;
    std::string nativeFunctionIdentity;
};

// Independent metadata supplies 3-16 distinct named functions, at least two
// different expectations for each scalar and two distinct non-null native
// function addresses. An additional explicitly named null function is permitted.
// Return offset 0xffff denotes no return value; other offsets must be below the
// declared parameter size. Zero remains a valid count, size, flags or offset.
// Native addresses are compared without dereferencing or invoking them. This
// observation establishes neither executable ownership nor an engine signature.
//
// Bounded scans use the declared uint32/uint8/uint16/uint16/pointer widths. The
// five unique candidates must occupy non-overlapping byte ranges and pass a final
// readback before atomic publication. Phase 1 Index/Name/Class/Outer and Phase 2
// Next/SuperStruct/Children/PropertiesSize must have validated current evidence;
// FField additionally requires ChildProperties. These versions become dependencies.
// A matching user override requires existing passing validation and all current
// required dependencies; it retains its value, origin, version and validation.
// Failures retain reports and stale prior automatic observations. The caller
// owns the snapshot, samples and their synchronized storage during the operation.
Status probeFunctionFields(MemoryReader&, const FunctionProbeProfile&,
    std::span<const FunctionSample>, ReadBudget&, Snapshot&);
}
