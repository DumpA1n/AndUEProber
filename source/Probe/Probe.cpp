#include "andueprober/Probe.hpp"
#include <limits>
#include <set>
#include <algorithm>

namespace andueprober {
Status probeUInt32Field(MemoryReader& reader, std::span<const FieldSample> samples,
    std::uint32_t extent, ReadBudget& budget, std::vector<Offset>& candidates) {
    candidates.clear();
    if (samples.empty() || extent < sizeof(std::uint32_t) || extent > 1024 * 1024)
        return {Error::InvalidArgument, "Field probing requires samples and a bounded object extent"};
    std::set<std::uintptr_t> distinct;
    for (const auto& sample : samples)
        if (!distinct.insert(sample.object).second)
            return {Error::InvalidArgument, "Field samples require distinct objects"};
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uint32_t); offset += alignof(std::uint32_t)) {
        bool matched = true;
        for (const auto& sample : samples) {
            if (sample.object > std::numeric_limits<std::uintptr_t>::max() - offset)
                return {Error::Overflow, "Sample field address overflow"};
            std::uint32_t value = 0;
            auto status = readExact(reader, sample.object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
            if (!status) return status;
            if (value != sample.expected) { matched = false; break; }
        }
        if (matched) {
            Offset candidate;
            candidate.value = offset;
            Evidence evidence{"uint32 matches every supplied sample", true, samples.size(), {offset}};
            evidence.source = "memory-generation:" + std::to_string(budget.generation);
            for (std::size_t index = 0; index < samples.size(); ++index)
                evidence.sampleIdentities.push_back((samples[index].identity.empty() ?
                    "sample:" + std::to_string(index) : samples[index].identity) +
                    ";expected-uint32:" + std::to_string(samples[index].expected));
            candidate.evidence.push_back(std::move(evidence));
            candidates.push_back(std::move(candidate));
        }
    }
    if (candidates.size() == 1 && samples.size() >= 2) candidates.front().validation = Validation::Validated;
    return {};
}
namespace {
struct ArrayHeader {
    std::uintptr_t objects = 0;
    std::int32_t count = 0, capacity = 0, chunks = 0, chunkCapacity = 0;
    bool operator==(const ArrayHeader&) const = default;
};
Status fieldAddress(std::uintptr_t base, std::uint64_t relative, std::uintptr_t& address) {
    if (relative > std::numeric_limits<std::uintptr_t>::max() - base)
        return {Error::Overflow, "Object array address overflow"};
    address = base + relative;
    return {};
}
template<class T> Status readField(MemoryReader& reader, std::uintptr_t base,
    std::uint64_t offset, ReadBudget& budget, T& value) {
    std::uintptr_t address = 0;
    if (auto status = fieldAddress(base, offset, address); !status) return status;
    return readExact(reader, address, std::as_writable_bytes(std::span(&value, 1)), budget);
}
Status header(MemoryReader& reader, std::uintptr_t array, const ObjectArrayProfile& profile,
    ReadBudget& budget, ArrayHeader& out) {
    if (!array || profile.identity.empty() || !profile.objects || !profile.count ||
        !profile.capacity || !profile.itemStride || !profile.itemObject ||
        *profile.itemStride < sizeof(std::uintptr_t) || *profile.itemStride > 4096 ||
        *profile.itemObject > *profile.itemStride - sizeof(std::uintptr_t) ||
        !profile.maximumObjects || profile.maximumObjects > 16 * 1024 * 1024 ||
        !profile.maximumExamined || profile.maximumExamined > profile.maximumObjects ||
        profile.sampleLimit < 2 || profile.sampleLimit > 64 ||
        profile.elementsPerChunk > profile.maximumObjects ||
        (profile.elementsPerChunk && (!profile.chunkCount || !profile.chunkCapacity)))
        return {Error::InvalidArgument, "Incomplete or out-of-range object array profile"};
    for (auto offset : {profile.objects, profile.count, profile.capacity, profile.chunkCount, profile.chunkCapacity})
        if (offset && *offset > 4096) return {Error::InvalidArgument, "Object array header exceeds profile bounds"};
    if (auto status = readField(reader, array, *profile.objects, budget, out.objects); !status) return status;
    if (auto status = readField(reader, array, *profile.count, budget, out.count); !status) return status;
    if (auto status = readField(reader, array, *profile.capacity, budget, out.capacity); !status) return status;
    if (out.count < 0 || out.capacity < out.count ||
        static_cast<std::uint32_t>(out.capacity) > profile.maximumObjects || (out.count && !out.objects))
        return {Error::InvalidEvidence, "Object array count, capacity or storage is corrupt"};
    if (profile.elementsPerChunk) {
        if (auto status = readField(reader, array, *profile.chunkCount, budget, out.chunks); !status) return status;
        if (auto status = readField(reader, array, *profile.chunkCapacity, budget, out.chunkCapacity); !status) return status;
        const auto needed = (static_cast<std::uint64_t>(out.count) + profile.elementsPerChunk - 1) / profile.elementsPerChunk;
        const auto maximum = (static_cast<std::uint64_t>(profile.maximumObjects) + profile.elementsPerChunk - 1) / profile.elementsPerChunk;
        if (out.chunks < 0 || out.chunkCapacity < out.chunks ||
            static_cast<std::uint64_t>(out.chunks) < needed || static_cast<std::uint64_t>(out.chunkCapacity) > maximum)
            return {Error::InvalidEvidence, "Object array chunk counts are corrupt"};
    }
    return {};
}
Status objectAt(MemoryReader& reader, const ArrayHeader& data, const ObjectArrayProfile& profile,
    std::uint32_t index, ReadBudget& budget, std::uintptr_t& object) {
    object = 0;
    if (index >= static_cast<std::uint32_t>(data.count))
        return {Error::InvalidArgument, "Object index is outside the array"};
    auto storage = data.objects;
    auto within = index;
    if (profile.elementsPerChunk) {
        const auto chunk = index / profile.elementsPerChunk;
        within = index % profile.elementsPerChunk;
        if (chunk >= static_cast<std::uint32_t>(data.chunks))
            return {Error::InvalidEvidence, "Object chunk is outside the array"};
        if (auto status = readField(reader, storage, static_cast<std::uint64_t>(chunk) * sizeof(std::uintptr_t), budget, storage); !status) return status;
        if (!storage) return {Error::InvalidEvidence, "Object array chunk is null"};
    }
    return readField(reader, storage, static_cast<std::uint64_t>(within) * *profile.itemStride + *profile.itemObject, budget, object);
}
}
std::string objectArrayLayoutIdentity(const ObjectArrayProfile& profile) {
    const auto field = [](const auto& value) { return value ? std::to_string(*value) : "absent"; };
    return "object-array-v1;profile:" + profile.identity + ";pointer-bytes:" + std::to_string(sizeof(std::uintptr_t)) +
        ";objects:" + field(profile.objects) + ";count:" + field(profile.count) + ";capacity:" + field(profile.capacity) +
        ";item-stride:" + field(profile.itemStride) + ";item-object:" + field(profile.itemObject) +
        ";chunk-count:" + field(profile.chunkCount) + ";chunk-capacity:" + field(profile.chunkCapacity) +
        ";elements-per-chunk:" + std::to_string(profile.elementsPerChunk);
}
Status readObjectCount(MemoryReader& reader, std::uintptr_t array, const ObjectArrayProfile& profile,
    ReadBudget& budget, std::uint32_t& count) {
    count = 0;
    ArrayHeader data;
    if (auto status = header(reader, array, profile, budget, data); !status) return status;
    count = static_cast<std::uint32_t>(data.count);
    return {};
}
Status readObjectAt(MemoryReader& reader, std::uintptr_t array, const ObjectArrayProfile& profile,
    std::uint32_t index, ReadBudget& budget, std::uintptr_t& object) {
    object = 0;
    ArrayHeader data;
    if (auto status = header(reader, array, profile, budget, data); !status) return status;
    return objectAt(reader, data, profile, index, budget, object);
}
Status probeObjectArrayIndices(MemoryReader& reader, std::uintptr_t array,
    const ObjectArrayProfile& profile, std::uint32_t extent, ReadBudget& budget,
    std::vector<Offset>& candidates) {
    candidates.clear();
    ArrayHeader initial;
    if (auto status = header(reader, array, profile, budget, initial); !status) return status;
    std::vector<FieldSample> samples;
    std::set<std::uintptr_t> unique;
    const auto limit = std::min(static_cast<std::uint32_t>(initial.count), profile.maximumExamined);
    for (std::uint32_t index = 0; index < limit && samples.size() < profile.sampleLimit; ++index) {
        std::uintptr_t object = 0;
        if (auto status = objectAt(reader, initial, profile, index, budget, object); !status) return status;
        if (!object) continue;
        if (!unique.insert(object).second)
            return {Error::InvalidEvidence, "Duplicate object pointers cannot establish independent index samples"};
        samples.push_back({object, index, "object-array-index:" + std::to_string(index)});
    }
    if (samples.size() < 2)
        return {Error::InvalidEvidence, "Fewer than two independent object samples within the scan limit"};
    std::vector<Offset> observed;
    if (auto status = probeUInt32Field(reader, samples, extent, budget, observed); !status) return status;
    ArrayHeader final;
    if (auto status = header(reader, array, profile, budget, final); !status) return status;
    if (!(initial == final)) return {Error::InvalidEvidence, "Object array header changed while probing"};
    for (const auto& sample : samples) {
        std::uintptr_t current = 0;
        if (auto status = objectAt(reader, final, profile, sample.expected, budget, current); !status) return status;
        if (current != sample.object) return {Error::InvalidEvidence, "Object array sample changed while probing"};
        for (const auto& offset : observed) {
            std::uint32_t value = 0;
            if (auto status = readField(reader, current, *offset.value, budget, value); !status) return status;
            if (value != sample.expected) return {Error::InvalidEvidence, "Object index changed while probing"};
        }
    }
    for (auto& offset : observed) {
        Evidence evidence{"object array bounds and sampled entries stable before and after probing", true, samples.size(), {}};
        evidence.source = profile.identity;
        for (const auto& sample : samples) evidence.sampleIdentities.push_back(sample.identity);
        offset.evidence.push_back(std::move(evidence));
    }
    candidates = std::move(observed);
    return {};
}
}
