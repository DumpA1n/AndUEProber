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
// Recognized addresses are candidates, not verified callable signatures. Scans have
// no cache and require a unique match within readable PT_LOAD ranges of this image.
Status findAdrpReference(MemoryReader&, const ModuleImage&, std::uintptr_t target,
    ReadBudget&, DiscoveryValue&);
Status findObjectArrayCandidate(MemoryReader&, const ModuleImage&, ReadBudget&, DiscoveryValue&);
Status findNameToStringCandidate(MemoryReader&, const ModuleImage&, ReadBudget&, DiscoveryValue&);
}
