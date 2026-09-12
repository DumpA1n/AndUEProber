#pragma once
#include "Names.hpp"

namespace andueprober {
struct PointerSample {
    std::uintptr_t object, expected;
    std::string identity, expectedIdentity;
};
Status probePointerField(MemoryReader&, std::span<const PointerSample>, std::uint32_t extent,
    const std::string& profileIdentity, ReadBudget&, FieldProbeReport&);
// Null, unmapped, overflowing, nonmatching or undecodable candidates are recorded as rejected.
// Permission, short-read, budget, generation and cancellation failures abort the phase.
Status probeClassField(MemoryReader&, std::span<const NameSample>, std::uint32_t extent,
    std::optional<std::uint32_t> nameOffset, const NameLayout&, std::uintptr_t pool,
    const NamePoolProfile&, ReadBudget&, FieldProbeReport&);
// Invalidates an automatic field and downstream observations before a new attempt.
Status beginFieldProbe(Snapshot&, const std::string& field);
// Publishes only one unambiguous candidate, preserving user overrides and upstream versions.
// Candidate upstream evidence keeps the dependent result unvalidated.
Status publishFieldProbe(Snapshot&, const std::string& field, const FieldProbeReport&,
    std::span<const std::string> dependencies);
}
