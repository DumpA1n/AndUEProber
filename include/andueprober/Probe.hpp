#pragma once
#include "Core.hpp"

namespace andueprober {
struct FieldSample {
    std::uintptr_t object;
    std::uint32_t expected;
    std::string identity;
};
// Matches a uint32 field against distinct objects; ambiguous candidates remain unvalidated.
Status probeUInt32Field(MemoryReader&, std::span<const FieldSample>, std::uint32_t extent,
    ReadBudget&, std::vector<Offset>& candidates);

// Offsets are relative to the selected TUObjectArray, including zero-valued fields.
// This describes memory supplied by a profile; it is not an engine ABI contract.
struct ObjectArrayProfile {
    std::string identity;
    std::optional<std::uint32_t> objects, count, capacity, itemStride, itemObject;
    std::optional<std::uint32_t> chunkCount, chunkCapacity;
    std::uint32_t elementsPerChunk = 0;
    std::uint32_t maximumObjects = 1024 * 1024;
    std::uint32_t maximumExamined = 1024;
    std::uint32_t sampleLimit = 8;
};
std::string objectArrayLayoutIdentity(const ObjectArrayProfile&);
Status readObjectCount(MemoryReader&, std::uintptr_t array, const ObjectArrayProfile&,
    ReadBudget&, std::uint32_t& count);
Status readObjectAt(MemoryReader&, std::uintptr_t array, const ObjectArrayProfile&,
    std::uint32_t index, ReadBudget&, std::uintptr_t& object);
// Validates bounds, samples distinct array entries and rechecks the array after probing.
// A unique result carries profile/sample provenance into the immutable session snapshot.
Status probeObjectArrayIndices(MemoryReader&, std::uintptr_t array, const ObjectArrayProfile&,
    std::uint32_t objectExtent, ReadBudget&, std::vector<Offset>& candidates);
}
