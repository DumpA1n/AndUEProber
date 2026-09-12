#include "andueprober/Structs.hpp"
#include "andueprober/Evidence.hpp"
#include "andueprober/Probe.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 4> dependencies{
    "UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate", "UObject::OuterPrivate"};
const std::array<std::string, 6> fields{
    "UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize",
    "UStruct::ChildProperties", "UStruct::MinAlignment"};
bool nameValid(const std::string& name) { return !name.empty() && name.size() <= 1024; }
template<class T> Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset,
    ReadBudget& budget, T& result) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "Struct field address overflow"};
    return readExact(reader, object + offset, std::as_writable_bytes(std::span(&result, 1)), budget);
}
Status noCycles(const std::map<std::uintptr_t, std::uintptr_t>& edges, const char* relationship) {
    for (const auto& [start, unused] : edges) {
        (void)unused;
        std::set<std::uintptr_t> visited;
        auto current = start;
        while (current && edges.contains(current)) {
            if (!visited.insert(current).second)
                return {Error::InvalidEvidence, std::string("Declared ") + relationship + " anchors contain a cycle"};
            current = edges.at(current);
        }
    }
    return {};
}
Status samplesValid(const StructProbeProfile& profile, std::span<const StructSample> structs,
    std::span<const StructFieldSample> members) {
    if (structs.size() < 2 || structs.size() > 4 || members.size() < 3 || members.size() > 64)
        return {Error::InvalidArgument, "Struct probing requires 2-4 struct anchors and 3-64 field anchors"};
    std::set<std::uintptr_t> objects;
    std::set<std::string> identities;
    std::map<std::uintptr_t, std::uintptr_t> superEdges, nextEdges;
    std::size_t alignments = 0;
    for (const auto& sample : structs) {
        if (!sample.object || !nameValid(sample.identity) || !nameValid(sample.superIdentity) ||
            !nameValid(sample.firstChildIdentity) || !objects.insert(sample.object).second ||
            !identities.insert(sample.identity).second || sample.expectedPropertiesSize > 16 * 1024 * 1024)
            return {Error::InvalidArgument, "Struct anchors require distinct objects, names and bounded independent size metadata"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Struct sample extent overflows the address space"};
        if (profile.layout == Layout::FField && (!sample.expectedChildProperties || !nameValid(sample.childPropertiesIdentity)))
            return {Error::Unsupported, "FField probing requires explicit child-properties anchors"};
        if (sample.expectedFirstChild == sample.object ||
            (sample.expectedChildProperties && *sample.expectedChildProperties == sample.object))
            return {Error::InvalidEvidence, "A struct cannot be its own first child"};
        if (sample.expectedMinAlignment) {
            const auto alignment = *sample.expectedMinAlignment;
            if (!alignment || alignment > 4096 || (alignment & (alignment - 1)))
                return {Error::InvalidArgument, "Independent alignment metadata must be a bounded power of two"};
            ++alignments;
        }
        superEdges.emplace(sample.object, sample.expectedSuper);
    }
    if (alignments == 1)
        return {Error::InvalidEvidence, "Alignment requires at least two independent scalar anchors"};
    objects.clear(); identities.clear();
    for (const auto& sample : members) {
        if (!sample.object || !nameValid(sample.identity) || !nameValid(sample.nextIdentity) ||
            !objects.insert(sample.object).second || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "Field anchors require distinct objects and named Next relationships"};
        if (profile.fieldExtent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Field sample extent overflows the address space"};
        nextEdges.emplace(sample.object, sample.expectedNext);
    }
    if (auto status = noCycles(superEdges, "SuperStruct"); !status) return status;
    return noCycles(nextEdges, "Next");
}
using ScalarSamples = std::vector<FieldSample>;
Status scalars(MemoryReader& reader, std::span<const FieldSample> samples, std::uint32_t extent,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    std::vector<Offset> candidates;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uint32_t); offset += alignof(std::uint32_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            std::uint32_t value = 0;
            if (auto status = at(reader, sample.object, offset, budget, value); !status) return status;
            if (value != sample.expected) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity,
                    "Scalar differs from independently declared metadata"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"uint32 matches independent named struct metadata", true, samples.size(), {offset}};
            evidence.source = provenance;
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";declared-uint32:" + std::to_string(sample.expected));
            candidate.evidence.push_back(std::move(evidence));
            candidates.push_back(std::move(candidate));
        }
    }
    if (candidates.size() == 1) candidates.front().validation = Validation::Validated;
    report.candidates = std::move(candidates);
    return {};
}
template<class T, class Sample, class Expected>
Status recheck(MemoryReader& reader, std::span<const Sample> samples, Expected expected,
    ReadBudget& budget, FieldProbeReport& report) {
    for (auto& candidate : report.candidates) {
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, *candidate.value, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity,
                    "Declared struct relationship changed during the phase"});
                report.candidates.clear();
                return {Error::InvalidEvidence, "Struct observations changed before phase publication"};
            }
        }
        Evidence evidence{"complete phase final recheck matches declared anchors", true, samples.size(), {*candidate.value}};
        evidence.source = "memory-generation:" + std::to_string(budget.generation);
        for (const auto& sample : samples) evidence.sampleIdentities.push_back(sample.identity);
        candidate.evidence.push_back(std::move(evidence));
    }
    return {};
}
}

Status probeStructFields(MemoryReader& reader, const StructProbeProfile& profile,
    std::span<const StructSample> structs, std::span<const StructFieldSample> members,
    ReadBudget& budget, Snapshot& snapshot) {
    if (!nameValid(profile.identity) || !nameValid(profile.moduleIdentity) || !profile.generation ||
        (profile.layout != Layout::UProperty && profile.layout != Layout::FField) ||
        profile.extent < sizeof(std::uintptr_t) || profile.extent > 4096 ||
        profile.fieldExtent < sizeof(std::uintptr_t) || profile.fieldExtent > 4096)
        return {Error::InvalidArgument, "Struct probing requires explicit bounded layout and module metadata"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "Struct profile, reader and snapshot identities do not match"};
    std::map<std::string, std::uint64_t> upstream;
    for (const auto& field : fields) {
        const auto found = snapshot.offsets.find(field);
        if (found != snapshot.offsets.end() && found->second.origin == Origin::User) {
            snapshot.fieldReports.erase(field);
            continue;
        }
        if (auto status = beginFieldProbe(snapshot, field); !status) return status;
    }
    EvidenceLimits evidenceLimits;
    evidenceLimits.cancelled = budget.cancelled;
    evidenceLimits.deadline = budget.deadline;
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) {
        snapshot.fieldReports[fields.front()] = FieldProbeReport{budget.generation};
        return status;
    }
    for (const auto& dependency : dependencies)
        upstream.emplace(dependency, snapshot.offsets.at(dependency).version);
    if (auto status = samplesValid(profile, structs, members); !status) return status;
    const auto provenance = "struct-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";layout:" + (profile.layout == Layout::FField ? "FField" : "UProperty") +
        ";memory-generation:" + std::to_string(profile.generation);
    std::map<std::string, std::vector<PointerSample>> pointers;
    std::map<std::string, ScalarSamples> scalarSamples;
    for (const auto& sample : members)
        pointers[fields[0]].push_back({sample.object, sample.expectedNext, sample.identity, sample.nextIdentity});
    for (const auto& sample : structs) {
        pointers[fields[1]].push_back({sample.object, sample.expectedSuper, sample.identity, sample.superIdentity});
        pointers[fields[2]].push_back({sample.object, sample.expectedFirstChild, sample.identity, sample.firstChildIdentity});
        scalarSamples[fields[3]].push_back({sample.object, sample.expectedPropertiesSize, sample.identity});
        if (profile.layout == Layout::FField)
            pointers[fields[4]].push_back({sample.object, *sample.expectedChildProperties, sample.identity, sample.childPropertiesIdentity});
        if (sample.expectedMinAlignment)
            scalarSamples[fields[5]].push_back({sample.object, *sample.expectedMinAlignment, sample.identity});
    }
    std::map<std::string, FieldProbeReport> reports;
    const auto retain = [&] { for (const auto& [name, report] : reports) snapshot.fieldReports[name] = report; };
    for (const auto& field : fields) {
        Status status;
        if (const auto found = pointers.find(field); found != pointers.end())
            status = probePointerField(reader, found->second, field == fields[0] ? profile.fieldExtent : profile.extent,
                provenance, budget, reports[field]);
        else if (const auto found = scalarSamples.find(field); found != scalarSamples.end())
            status = scalars(reader, found->second, profile.extent, provenance, budget, reports[field]);
        else continue;
        if (!status) { retain(); return status; }
    }
    for (const auto& [field, samples] : pointers) {
        auto status = recheck<std::uintptr_t>(reader, std::span(samples), [](const auto& sample) { return sample.expected; },
            budget, reports[field]);
        if (!status) { retain(); return status; }
    }
    for (const auto& [field, samples] : scalarSamples) {
        auto status = recheck<std::uint32_t>(reader, std::span(samples), [](const auto& sample) { return sample.expected; },
            budget, reports[field]);
        if (!status) { retain(); return status; }
    }
    retain();
    for (const auto& [name, version] : upstream) {
        const auto current = snapshot.offsets.find(name);
        if (current == snapshot.offsets.end() || current->second.version != version || current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Phase 1 evidence changed during struct probing"};
    }
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) return status;
    auto published = snapshot;
    for (const auto& [field, report] : reports) {
        if (report.candidates.size() != 1)
            return {Error::InvalidEvidence, "Struct probing requires one unambiguous observation per requested field"};
        const auto existing = snapshot.offsets.find(field);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) {
            if (existing->second.value != report.candidates.front().value)
                return {Error::InvalidEvidence, "A struct observation contradicts an explicit user override"};
            const auto& retained = existing->second;
            for (const auto& [dependency, version] : upstream) {
                const auto prior = retained.dependencies.find(dependency);
                if (prior == retained.dependencies.end() || prior->second != version)
                    return {Error::InvalidEvidence, "A matching struct override requires current Phase 1 dependency versions"};
            }
            continue;
        }
        if (auto status = publishFieldProbe(published, field, report, dependencies); !status) return status;
    }
    std::vector<std::string> outputs;
    for (const auto& [field, report] : reports) { (void)report; outputs.push_back(field); }
    if (auto status = validateEvidenceClosure(published, outputs, evidenceLimits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Struct phase cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Struct phase deadline exceeded before publication"};
    if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
        snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
        return {Error::StaleIdentity, "Struct identity changed before publication"};
    snapshot = std::move(published);
    return {};
}
}
