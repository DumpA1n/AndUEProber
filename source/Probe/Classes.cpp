#include "andueprober/Classes.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 2> fields{"UClass::CastFlags", "UClass::ClassDefaultObject"};
bool nameValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
template<class T>
Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset, ReadBudget& budget, T& value) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "Class field address overflow"};
    return readExact(reader, object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
}
Status samplesValid(const ClassProbeProfile& profile, std::span<const ClassSample> samples) {
    if (samples.size() < 3 || samples.size() > 16)
        return {Error::InvalidArgument, "Class probing requires 3-16 independent named anchors"};
    std::set<std::uintptr_t> objects, defaults;
    std::set<std::string> identities;
    std::set<std::uint64_t> flags;
    std::map<std::string, std::uintptr_t> defaultIdentities;
    for (const auto& sample : samples) {
        if (!sample.object || !nameValid(sample.identity) || !nameValid(sample.defaultObjectIdentity) ||
            !objects.insert(sample.object).second || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "Class anchors require distinct objects and bounded metadata identities"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Class sample extent overflows the address space"};
        if (sample.expectedDefaultObject == sample.object)
            return {Error::InvalidEvidence, "A class cannot be its own default object"};
        const auto [target, inserted] = defaultIdentities.emplace(sample.defaultObjectIdentity, sample.expectedDefaultObject);
        if (!inserted && target->second != sample.expectedDefaultObject)
            return {Error::InvalidEvidence, "A default-object identity denotes contradictory addresses"};
        flags.insert(sample.expectedCastFlags);
        if (sample.expectedDefaultObject) defaults.insert(sample.expectedDefaultObject);
    }
    if (flags.size() < 2 || defaults.size() < 2)
        return {Error::InvalidEvidence, "Class anchors require distinct flags and at least two distinct non-null default objects"};
    return {};
}
Status castFlags(MemoryReader& reader, std::span<const ClassSample> samples, std::uint32_t extent,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uint64_t); offset += sizeof(std::uint64_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            std::uint64_t observed = 0;
            if (auto status = at(reader, sample.object, offset, budget, observed); !status) return status;
            if (observed != sample.expectedCastFlags) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity,
                    "Cast flags differ from independently declared uint64 metadata"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"uint64 matches independent named class metadata", true, samples.size(), {offset}};
            evidence.source = provenance;
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";declared-uint64:" + std::to_string(sample.expectedCastFlags));
            candidate.evidence.push_back(std::move(evidence));
            report.candidates.push_back(std::move(candidate));
        }
    }
    if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    return {};
}
template<class T, class Expected>
Status recheck(MemoryReader& reader, std::span<const ClassSample> samples, Expected expected,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    for (auto& candidate : report.candidates) {
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, *candidate.value, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity,
                    "Class observation changed before phase publication"});
                report.candidates.clear();
                return {Error::InvalidEvidence, "Class observations changed before phase publication"};
            }
        }
        Evidence evidence{"complete class phase final recheck matches declared anchors", true, samples.size(), {*candidate.value}};
        evidence.source = provenance;
        for (const auto& sample : samples) evidence.sampleIdentities.push_back(sample.identity);
        candidate.evidence.push_back(std::move(evidence));
    }
    return {};
}
}

Status probeClassFields(MemoryReader& reader, const ClassProbeProfile& profile,
    std::span<const ClassSample> samples, ReadBudget& budget, Snapshot& snapshot) {
    if (sizeof(std::uintptr_t) != sizeof(std::uint64_t))
        return {Error::Unsupported, "Class probing requires an explicit 64-bit pointer layout"};
    if (!nameValid(profile.identity) || !nameValid(profile.moduleIdentity) || !profile.generation ||
        (profile.layout != Layout::UProperty && profile.layout != Layout::FField) ||
        profile.extent < sizeof(std::uint64_t) || profile.extent > 4096)
        return {Error::InvalidArgument, "Class probing requires explicit bounded layout and module metadata"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "Class profile, reader and snapshot identities do not match"};
    for (const auto& field : fields) {
        const auto found = snapshot.offsets.find(field);
        if (found != snapshot.offsets.end() && found->second.origin == Origin::User) snapshot.fieldReports.erase(field);
        else if (auto status = beginFieldProbe(snapshot, field); !status) return status;
    }
    std::vector<std::string> dependencies{"UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate",
        "UObject::OuterPrivate", "UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize"};
    if (profile.layout == Layout::FField) dependencies.push_back("UStruct::ChildProperties");
    std::map<std::string, std::uint64_t> upstream;
    EvidenceLimits evidenceLimits;
    evidenceLimits.cancelled = budget.cancelled;
    evidenceLimits.deadline = budget.deadline;
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) {
        snapshot.fieldReports[fields.front()] = FieldProbeReport{budget.generation};
        return status;
    }
    for (const auto& dependency : dependencies)
        upstream.emplace(dependency, snapshot.offsets.at(dependency).version);
    if (auto status = samplesValid(profile, samples); !status) return status;
    const auto provenance = "class-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";layout:" + (profile.layout == Layout::FField ? "FField" : "UProperty") +
        ";memory-generation:" + std::to_string(profile.generation);
    std::vector<PointerSample> pointers;
    for (const auto& sample : samples)
        pointers.push_back({sample.object, sample.expectedDefaultObject, sample.identity, sample.defaultObjectIdentity});
    std::array<FieldProbeReport, 2> reports;
    const auto retain = [&] {
        for (std::size_t i = 0; i < fields.size(); ++i) snapshot.fieldReports[fields[i]] = reports[i];
    };
    auto status = castFlags(reader, samples, profile.extent, provenance, budget, reports[0]);
    if (status) status = probePointerField(reader, pointers, profile.extent, provenance, budget, reports[1]);
    if (status) status = recheck<std::uint64_t>(reader, samples, [](const auto& sample) { return sample.expectedCastFlags; },
        provenance, budget, reports[0]);
    if (status) status = recheck<std::uintptr_t>(reader, samples, [](const auto& sample) { return sample.expectedDefaultObject; },
        provenance, budget, reports[1]);
    retain();
    if (!status) return status;
    for (const auto& [name, version] : upstream) {
        const auto current = snapshot.offsets.find(name);
        if (current == snapshot.offsets.end() || current->second.version != version || current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Upstream evidence changed during class probing"};
    }
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) return status;
    auto published = snapshot;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (reports[i].candidates.size() != 1)
            return {Error::InvalidEvidence, "Class probing requires one unambiguous observation per field"};
        const auto existing = snapshot.offsets.find(fields[i]);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) {
            if (existing->second.value != reports[i].candidates.front().value)
                return {Error::InvalidEvidence, "A class observation contradicts an explicit user override"};
            const auto& retained = existing->second;
            for (const auto& [dependency, version] : upstream) {
                const auto prior = retained.dependencies.find(dependency);
                if (prior == retained.dependencies.end() || prior->second != version)
                    return {Error::InvalidEvidence, "A matching class override requires current Phase 1 and Phase 2 dependency versions"};
            }
        } else if (auto result = publishFieldProbe(published, fields[i], reports[i], dependencies); !result) return result;
    }
    if (auto status = validateEvidenceClosure(published, fields, evidenceLimits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Class phase cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Class phase deadline exceeded before publication"};
    if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
        snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
        return {Error::StaleIdentity, "Class identity changed before publication"};
    snapshot = std::move(published);
    return {};
}
}
