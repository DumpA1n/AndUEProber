#pragma once
#include "Core.hpp"

namespace andueprober {
enum class NarrowEncoding { Ascii, Utf8 };
// An explicit length-prefixed pool layout. Outline-number and encrypted entries require a different provider.
struct NamePoolProfile {
    std::string identity;
    std::optional<std::uint32_t> blocks, header, string;
    std::uint32_t blockBits = 16, stride = 2, maximumBlocks = 8192, maximumUnits = 1024;
    std::uint32_t lengthShift = 6;
    NarrowEncoding narrowEncoding = NarrowEncoding::Ascii;
    bool outlineNumbers = false;
};
struct NameLayout {
    // Inline numbers use the nonnegative int32 GetNumber contract; zero denotes no suffix.
    std::optional<std::uint32_t> comparison, display, number;
    std::uint32_t size = 0;
};
struct NameSample { std::uintptr_t object; std::string expected, identity; };
Status validateNamePoolProfile(const NamePoolProfile&);
Status validateNameLayout(const NameLayout&);
std::string nameLayoutIdentity(const NameLayout&, const NamePoolProfile&);
// Binds a concrete observation to its pool address and reader generation.
// Layout identity alone does not establish that two phases observed the same pool.
std::string nameObservationIdentity(const NameLayout&, const NamePoolProfile&,
    std::uintptr_t poolAddress, std::uint64_t generation);
Status readPoolName(MemoryReader&, std::uintptr_t pool, std::uint32_t id,
    const NamePoolProfile&, ReadBudget&, std::string&);
Status readFName(MemoryReader&, std::uintptr_t address, const NameLayout&,
    std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, std::string&);
// Exact names and distinct objects establish evidence; callers supply each anchor identity.
// Invalid/unmapped entry candidates and undeclared narrow encodings are recorded as rejected;
// invalid profile configuration, permission, short-read and operation-budget errors abort the phase.
Status probeNameField(MemoryReader&, std::span<const NameSample>, std::uint32_t extent,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, FieldProbeReport&);
}
