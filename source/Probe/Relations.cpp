#include "andueprober/Relations.hpp"
#include <limits>
#include <set>

namespace andueprober {
namespace {
Status add(std::uintptr_t object, std::uint32_t offset, std::uintptr_t& address) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "Object relation address overflow"};
    address = object + offset;
    return {};
}
Status pointerAt(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset,
    ReadBudget& budget, std::uintptr_t& pointer) {
    std::uintptr_t address;
    if (auto status = add(object, offset, address); !status) return status;
    return readExact(reader, address, std::as_writable_bytes(std::span(&pointer, 1)), budget);
}
template<class T> Status samplesValid(std::span<const T> samples, std::uint32_t extent) {
    if (samples.empty() || samples.size() > 64 || extent < sizeof(std::uintptr_t) || extent > 4096)
        return {Error::InvalidArgument, "Relations require bounded samples and object extents"};
    std::set<std::uintptr_t> distinct;
    for (const auto& sample : samples)
        if (sample.identity.empty() || sample.identity.size() > 1024 || !distinct.insert(sample.object).second)
            return {Error::InvalidArgument, "Relation samples require distinct objects and bounded identities"};
    return {};
}
Status className(MemoryReader& reader, std::uintptr_t pointer, std::uint32_t nameOffset,
    const NameLayout& layout, std::uintptr_t pool, const NamePoolProfile& profile,
    ReadBudget& budget, std::string& name) {
    if (!pointer) return {Error::InvalidEvidence, "Class pointer is null"};
    std::uintptr_t address;
    if (auto status = add(pointer, nameOffset, address); !status) return status;
    return readFName(reader, address, layout, pool, profile, budget, name);
}
bool candidateError(Error error) {
    return error == Error::Unmapped || error == Error::InvalidEvidence || error == Error::Overflow || error == Error::Unsupported;
}
}
Status probePointerField(MemoryReader& reader, std::span<const PointerSample> samples,
    std::uint32_t extent, const std::string& profileIdentity, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    if (auto status = samplesValid(samples, extent); !status) return status;
    if (profileIdentity.empty()) return {Error::InvalidArgument, "A relation profile identity is required"};
    for (const auto& sample : samples)
        if (sample.expectedIdentity.empty() || sample.expectedIdentity.size() > 1024)
            return {Error::InvalidArgument, "An expected pointer identity is required, including null relationships"};
    std::vector<Offset> candidates;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uintptr_t); offset += alignof(std::uintptr_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            std::uintptr_t pointer = 0;
            if (auto status = pointerAt(reader, sample.object, offset, budget, pointer); !status) return status;
            if (pointer != sample.expected) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, "Pointer differs from the declared relationship"});
                matched = false; break;
            }
        }
        if (matched) { Offset candidate; candidate.value = offset; candidates.push_back(std::move(candidate)); }
    }
    for (auto& candidate : candidates) {
        Evidence evidence{"pointer relationships match distinct supplied anchors and a final recheck", true, samples.size(), {*candidate.value}};
        evidence.source = profileIdentity + ";pointer-bytes:" + std::to_string(sizeof(std::uintptr_t)) +
            ";memory-generation:" + std::to_string(budget.generation);
        for (const auto& sample : samples) {
            std::uintptr_t pointer = 0;
            if (auto status = pointerAt(reader, sample.object, *candidate.value, budget, pointer); !status) return status;
            if (pointer != sample.expected) return {Error::InvalidEvidence, "Pointer relationship changed during probing"};
            evidence.sampleIdentities.push_back(sample.identity + ";expected-pointer:" + sample.expectedIdentity);
        }
        candidate.evidence.push_back(std::move(evidence));
        if (candidates.size() == 1 && samples.size() >= 2) candidate.validation = Validation::Validated;
    }
    report.candidates = std::move(candidates);
    return {};
}
Status probeClassField(MemoryReader& reader, std::span<const NameSample> samples,
    std::uint32_t extent, std::optional<std::uint32_t> nameOffset, const NameLayout& layout,
    std::uintptr_t pool, const NamePoolProfile& profile, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    if (auto status = samplesValid(samples, extent); !status) return status;
    if (auto status = validateNameLayout(layout); !status) return status;
    if (auto status = validateNamePoolProfile(profile); !status) return status;
    if (!nameOffset || *nameOffset > 4096)
        return {Error::InvalidArgument, "Class probing requires a bounded name field offset"};
    for (const auto& sample : samples)
        if (sample.expected.empty() || sample.expected.size() > 4096)
            return {Error::InvalidArgument, "Class samples require bounded expected names"};
    std::vector<Offset> candidates;
    std::vector<std::vector<std::uintptr_t>> pointers;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uintptr_t); offset += alignof(std::uintptr_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        std::vector<std::uintptr_t> observed;
        for (const auto& sample : samples) {
            std::uintptr_t pointer = 0;
            if (auto status = pointerAt(reader, sample.object, offset, budget, pointer); !status) return status;
            std::string name;
            auto status = className(reader, pointer, *nameOffset, layout, pool, profile, budget, name);
            if (!status && !candidateError(status.code)) return status;
            if (!status || name != sample.expected) {
                report.rejected.push_back({offset, status ? Error::InvalidEvidence : status.code, sample.identity,
                    status ? "Class name differs from the declared relationship" : status.message});
                matched = false; break;
            }
            observed.push_back(pointer);
        }
        if (matched) {
            Offset candidate; candidate.value = offset; candidates.push_back(std::move(candidate));
            pointers.push_back(std::move(observed));
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto& candidate = candidates[index];
        Evidence evidence{"class pointers and exact names match distinct anchors and a final recheck", true, samples.size(), {*candidate.value}};
        evidence.source = nameObservationIdentity(layout, profile, pool, budget.generation) +
            ";name-offset:" + std::to_string(*nameOffset);
        for (std::size_t item = 0; item < samples.size(); ++item) {
            const auto& sample = samples[item];
            std::uintptr_t pointer = 0;
            if (auto status = pointerAt(reader, sample.object, *candidate.value, budget, pointer); !status) return status;
            if (pointer != pointers[index][item]) return {Error::InvalidEvidence, "Class pointer changed during probing"};
            std::string name;
            if (auto status = className(reader, pointer, *nameOffset, layout, pool, profile, budget, name); !status) return status;
            if (name != sample.expected) return {Error::InvalidEvidence, "Class name changed during probing"};
            evidence.sampleIdentities.push_back(sample.identity + ";expected-class:" + sample.expected);
        }
        candidate.evidence.push_back(std::move(evidence));
        if (candidates.size() == 1 && samples.size() >= 2) candidate.validation = Validation::Validated;
    }
    report.candidates = std::move(candidates);
    return {};
}
Status beginFieldProbe(Snapshot& snapshot, const std::string& field) {
    if (field.empty()) return {Error::InvalidArgument, "A field name is required"};
    const auto current = snapshot.offsets.find(field);
    if (current == snapshot.offsets.end()) { snapshot.fieldReports.erase(field); return {}; }
    if (current->second.origin == Origin::User)
        return {Error::InvalidEvidence, "An automatic phase cannot replace an explicit user override"};
    snapshot.fieldReports.erase(field);
    auto stale = current->second;
    stale.validation = Validation::Stale;
    return publishOffset(snapshot, field, std::move(stale));
}
Status publishFieldProbe(Snapshot& snapshot, const std::string& field, const FieldProbeReport& report,
    std::span<const std::string> dependencies) {
    if (report.generation != snapshot.generation)
        return {Error::StaleIdentity, "Field observations belong to a different snapshot generation"};
    if (field.empty()) return {Error::InvalidArgument, "A field name is required"};
    snapshot.fieldReports[field] = report;
    if (report.candidates.size() != 1)
        return {Error::InvalidEvidence, "A field phase requires one unambiguous candidate"};
    auto candidate = report.candidates.front();
    if (!candidate.value || candidate.evidence.empty() ||
        (candidate.validation != Validation::Candidate && candidate.validation != Validation::Validated))
        return {Error::InvalidEvidence, "The field candidate lacks usable observation evidence"};
    for (const auto& dependency : dependencies) {
        const auto prior = snapshot.offsets.find(dependency);
        if (prior == snapshot.offsets.end() || !prior->second.value || !prior->second.version ||
            prior->second.validation == Validation::Rejected || prior->second.validation == Validation::Stale)
            return {Error::InvalidEvidence, "A field phase requires current upstream offset evidence"};
        candidate.dependencies[dependency] = prior->second.version;
        if (prior->second.validation != Validation::Validated) candidate.validation = Validation::Candidate;
    }
    return publishOffset(snapshot, field, std::move(candidate));
}
}
