#pragma once
#include "Core.hpp"

namespace andueprober {
struct ModuleRange {
    std::uintptr_t start = 0;
    std::size_t size = 0;
    bool readable = false, executable = false;
};
struct ModuleImage {
    std::string identity;
    std::uint64_t generation = 0;
    std::uintptr_t loadBias = 0;
    std::vector<ModuleRange> ranges;
};
struct DiscoveryValue {
    std::optional<std::uintptr_t> address;
    Evidence evidence;
};
// Accepts ELF64 little-endian AArch64 ET_DYN images. Caller owns the module lease;
// loadBias is the linker relocation base, independent of the OS page size.
Status readModuleImage(MemoryReader&, std::uintptr_t elfAddress, std::uintptr_t loadBias,
    std::string identity, ReadBudget&, ModuleImage&);
// Uses immutable file metadata when an admitted target has erased its in-memory
// ELF header. PT_LOAD addresses still refer to the runtime reader and are
// validated against its generation before discovery scans begin.
Status readModuleImageFromFile(MemoryReader&, const std::string& modulePath,
    std::uintptr_t loadBias, std::string identity, ReadBudget&, ModuleImage&);
// Recognized addresses are candidates, not verified callable signatures. Scans have
// no cache and require a unique match within readable PT_LOAD ranges of this image.
Status findAdrpReference(MemoryReader&, const ModuleImage&, std::uintptr_t target,
    ReadBudget&, DiscoveryValue&);
// Matches a bounded byte pattern in executable AArch64 PT_LOAD ranges, then
// decodes an ADRP plus ADD/LDR address sequence at match + instructionOffset.
// The byte pattern may start between instruction boundaries; match +
// instructionOffset must identify an aligned ADRP. Mask bytes are 0x00 for
// wildcard and 0xff for exact comparison. The decoded
// address is a candidate within a readable range; the function does not read or
// invoke it. This supports profile-specific data-address discovery without
// admitting an unbounded scanner or a callable-signature assumption.
Status findAddressFromAarch64Pattern(MemoryReader&, const ModuleImage&,
    std::span<const std::byte> pattern, std::span<const std::byte> mask,
    std::int32_t instructionOffset, ReadBudget&, DiscoveryValue&);
Status findObjectArrayCandidate(MemoryReader&, const ModuleImage&, ReadBudget&, DiscoveryValue&);
// Shared UE NamePoolData instruction forms are matched in one executable-range
// pass. A unique ADRP plus ADD/LDR data address is required.
Status findNamePoolCandidate(MemoryReader&, const ModuleImage&, ReadBudget&, DiscoveryValue&);
Status findNameToStringCandidate(MemoryReader&, const ModuleImage&, ReadBudget&, DiscoveryValue&);
}
