#pragma once
#include "andueprober/Properties.hpp"
#include <array>

namespace andueprober::detail {
extern const std::array<std::string, 9> propertyDependencies;
struct PropertyContext {
    std::map<std::string, std::uint64_t> versions;
    std::string source;
};
bool propertyTextValid(const std::string&);
Status preparePropertyContext(MemoryReader&, const PropertyProbeProfile&, std::uint32_t occupiedPrefix,
    std::uint32_t extent, std::uint32_t minimumWidth, const NameLayout&, std::uintptr_t,
    const NamePoolProfile&, std::span<const std::string> outputs, ReadBudget&, Snapshot&, PropertyContext&);
Status publishPropertyContext(MemoryReader&, const PropertyProbeProfile&, const PropertyContext&,
    std::span<const std::string> outputs, ReadBudget&, Snapshot&);
}
