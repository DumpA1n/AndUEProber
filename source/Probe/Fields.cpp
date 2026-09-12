#include "andueprober/Fields.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 5> fields{"FField::NamePrivate", "FField::Owner", "FField::Next", "FField::ClassPrivate", "FField::FlagsPrivate"};
const std::array<std::string, 9> dependencies{"UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate",
    "UObject::OuterPrivate", "UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize", "UStruct::ChildProperties"};
bool textValid(const std::string& value, std::size_t maximum = 1024) {
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos && validateUtf8(value);
}
Status addressOf(std::uintptr_t object, std::uint32_t offset, std::uintptr_t& address) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "FField address overflow"};
    address = object + offset; return {};
}
template<class T>
Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset, ReadBudget& budget, T& value) {
    std::uintptr_t address = 0;
    if (auto status = addressOf(object, offset, address); !status) return status;
    return readExact(reader, address, std::as_writable_bytes(std::span(&value, 1)), budget);
}
Status ownerValid(const FieldOwnerLayout& owner, std::uint32_t extent) {
    if (owner.representation != FieldOwnerRepresentation::SeparateBoolean)
        return {Error::Unsupported, "FField owner requires a declared SeparateBoolean representation"};
    if (owner.size < sizeof(std::uintptr_t) + 1 || owner.size > 64 || owner.size > extent ||
        owner.pointerOffset > owner.size - sizeof(std::uintptr_t) || owner.pointerOffset % sizeof(std::uintptr_t) ||
        owner.kindOffset >= owner.size ||
        (owner.kindOffset >= owner.pointerOffset && owner.kindOffset < owner.pointerOffset + sizeof(std::uintptr_t)))
        return {Error::InvalidArgument, "FField owner layout requires bounded disjoint pointer and boolean fields"};
    return {};
}
Status samplesValid(const FieldProbeProfile& profile, std::span<const FieldBaseSample> samples) {
    if (samples.size() < 3 || samples.size() > 16)
        return {Error::InvalidArgument, "FField probing requires 3-16 independent named anchors"};
    std::set<std::uintptr_t> objects;
    std::set<std::string> identities;
    std::map<std::uintptr_t, std::uintptr_t> next, owners;
    std::map<std::string, std::uintptr_t> declared;
    const auto identity = [&](const std::string& name, std::uintptr_t address) {
        const auto [entry, inserted] = declared.emplace(name, address);
        return inserted || entry->second == address;
    };
    for (const auto& sample : samples) {
        if (!sample.object || !textValid(sample.identity) || !textValid(sample.expectedName, 4096) ||
            !textValid(sample.ownerIdentity) || !textValid(sample.nextIdentity) || !textValid(sample.classIdentity) ||
            !objects.insert(sample.object).second || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "FField anchors require distinct objects and bounded names and identities"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "FField sample extent overflows the address space"};
        if (!sample.expectedClass || !identity(sample.identity, sample.object) ||
            !identity(sample.ownerIdentity, sample.expectedOwner) || !identity(sample.nextIdentity, sample.expectedNext) ||
            !identity(sample.classIdentity, sample.expectedClass))
            return {Error::InvalidEvidence, "FField class anchors must be non-null and named identities cannot contradict addresses"};
        next.emplace(sample.object, sample.expectedNext);
        if (!sample.ownerIsUObject) owners.emplace(sample.object, sample.expectedOwner);
    }
    for (const auto* edges : {&next, &owners}) {
        for (const auto& [start, unused] : *edges) {
            (void)unused;
            std::set<std::uintptr_t> seen;
            auto current = start;
            while (current && edges->contains(current)) {
                if (!seen.insert(current).second) return {Error::InvalidEvidence, "Declared FField relationships contain a cycle"};
                current = edges->at(current);
            }
        }
    }
    return {};
}
Status ownerAt(MemoryReader& reader, const FieldBaseSample& sample, std::uint32_t offset,
    const FieldOwnerLayout& layout, ReadBudget& budget, bool& matched) {
    std::uintptr_t pointer = 0; std::uint8_t kind = 0;
    if (auto status = at(reader, sample.object, offset + layout.pointerOffset, budget, pointer); !status) return status;
    if (auto status = at(reader, sample.object, offset + layout.kindOffset, budget, kind); !status) return status;
    matched = pointer == sample.expectedOwner && kind <= 1 && (kind == 1) == sample.ownerIsUObject;
    return {};
}
Status owners(MemoryReader& reader, const FieldProbeProfile& profile, std::span<const FieldBaseSample> samples,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    for (std::uint32_t offset = 0; offset <= profile.extent - profile.ownerLayout.size; offset += sizeof(std::uintptr_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            if (auto status = ownerAt(reader, sample, offset, profile.ownerLayout, budget, matched); !status) return status;
            if (!matched) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, "FField owner pointer or 0/1 kind differs from declared metadata"});
                break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"owner pointer and SeparateBoolean kind match independent metadata", true, samples.size(), {offset}};
            evidence.source = provenance;
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";owner:" + sample.ownerIdentity +
                    ";owner-is-uobject:" + (sample.ownerIsUObject ? "true" : "false"));
            candidate.evidence.push_back(std::move(evidence));
            report.candidates.push_back(std::move(candidate));
        }
    }
    if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    return {};
}
Status flags(MemoryReader& reader, std::span<const FieldBaseSample> samples, std::uint32_t extent,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(std::uint32_t); offset += sizeof(std::uint32_t)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            std::uint32_t value = 0;
            if (auto status = at(reader, sample.object, offset, budget, value); !status) return status;
            if (value != sample.expectedFlags) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, "FField flags differ from independent uint32 metadata"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"uint32 flags match independent field metadata", true, samples.size(), {offset}};
            evidence.source = provenance;
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";declared-uint32:" + std::to_string(sample.expectedFlags));
            candidate.evidence.push_back(std::move(evidence)); report.candidates.push_back(std::move(candidate));
        }
    }
    if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    return {};
}
template<class Observe>
Status recheck(std::span<const FieldBaseSample> samples, Observe observe, const std::string& provenance, FieldProbeReport& report) {
    for (auto& candidate : report.candidates) {
        for (const auto& sample : samples) {
            bool matched = false;
            if (auto status = observe(sample, *candidate.value, matched); !status) return status;
            if (!matched) {
                report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity, "FField observation changed before phase publication"});
                report.candidates.clear();
                return {Error::InvalidEvidence, "FField observations changed before phase publication"};
            }
        }
        Evidence evidence{"complete FField phase final recheck matches declared anchors", true, samples.size(), {*candidate.value}};
        evidence.source = provenance;
        for (const auto& sample : samples) evidence.sampleIdentities.push_back(sample.identity);
        candidate.evidence.push_back(std::move(evidence));
    }
    return {};
}
Status overrideValid(const Offset& retained, const std::map<std::string, std::uint64_t>& upstream) {
    for (const auto& [dependency, version] : upstream) {
        const auto prior = retained.dependencies.find(dependency);
        if (prior == retained.dependencies.end() || prior->second != version)
            return {Error::InvalidEvidence, "A matching FField override requires current upstream dependency versions"};
    }
    return {};
}
}

std::string fieldObservationIdentity(const FieldProbeProfile& profile, const NameLayout& names,
    std::uintptr_t pool, const NamePoolProfile& poolProfile) {
    const auto& owner = profile.ownerLayout;
    const auto representation = owner.representation == FieldOwnerRepresentation::SeparateBoolean ? "SeparateBoolean" :
        owner.representation == FieldOwnerRepresentation::Tagged ? "Tagged" : "Unknown";
    return "field-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";memory-generation:" + std::to_string(profile.generation) + ";owner:" + representation + ";owner-pointer:" +
        std::to_string(owner.pointerOffset) + ";owner-kind:" + std::to_string(owner.kindOffset) +
        ";owner-size:" + std::to_string(owner.size) + ";" + nameObservationIdentity(names, poolProfile, pool, profile.generation);
}

Status probeFieldFields(MemoryReader& reader, const FieldProbeProfile& profile, std::span<const FieldBaseSample> samples,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    if (profile.layout != Layout::FField || sizeof(std::uintptr_t) != sizeof(std::uint64_t))
        return {Error::Unsupported, "FField probing requires the explicit 64-bit FField layout"};
    if (auto status = ownerValid(profile.ownerLayout, profile.extent); !status) return status;
    if (!textValid(profile.identity) || !textValid(profile.moduleIdentity) || !profile.generation || !pool ||
        !textValid(poolProfile.identity) || profile.extent < sizeof(std::uintptr_t) || profile.extent > 4096)
        return {Error::InvalidArgument, "FField probing requires explicit bounded module and name-pool metadata"};
    if (auto status = validateNameLayout(names); !status) return status;
    if (auto status = validateNamePoolProfile(poolProfile); !status) return status;
    if (profile.extent < names.size) return {Error::InvalidArgument, "FField extent cannot contain the declared FName layout"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "FField profile, reader and snapshot identities do not match"};
    for (const auto& field : fields) {
        const auto found = snapshot.offsets.find(field);
        if (found != snapshot.offsets.end() && found->second.origin == Origin::User) snapshot.fieldReports.erase(field);
        else if (auto status = beginFieldProbe(snapshot, field); !status) return status;
    }
    EvidenceLimits evidenceLimits; evidenceLimits.deadline = budget.deadline; evidenceLimits.cancelled = budget.cancelled;
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) return status;
    std::map<std::string, std::uint64_t> upstream;
    for (const auto& dependency : dependencies) upstream.emplace(dependency, snapshot.offsets.at(dependency).version);

    const auto nameIdentity = nameObservationIdentity(names, poolProfile, pool, profile.generation);
    const auto& nameEvidence = snapshot.offsets.at("UObject::NamePrivate").evidence;
    if (std::none_of(nameEvidence.begin(), nameEvidence.end(), [&](const auto& evidence) { return evidence.source == nameIdentity; }))
        return {Error::InvalidEvidence, "FField names must use the validated Phase 1 canonical name layout"};
    if (auto status = samplesValid(profile, samples); !status) return status;
    const auto& owner = profile.ownerLayout;
    const auto provenance = fieldObservationIdentity(profile, names, pool, poolProfile);
    std::vector<NameSample> nameSamples;
    std::vector<PointerSample> nextSamples, classSamples;
    for (const auto& sample : samples) {
        nameSamples.push_back({sample.object, sample.expectedName, sample.identity});
        nextSamples.push_back({sample.object, sample.expectedNext, sample.identity, sample.nextIdentity});
        classSamples.push_back({sample.object, sample.expectedClass, sample.identity, sample.classIdentity});
    }
    std::array<FieldProbeReport, 5> reports;
    const auto retain = [&] {
        for (std::size_t i = 0; i < fields.size(); ++i) if (reports[i].generation) snapshot.fieldReports[fields[i]] = reports[i];
    };
    auto status = probeNameField(reader, nameSamples, profile.extent, names, pool, poolProfile, budget, reports[0]);
    if (status) status = owners(reader, profile, samples, provenance, budget, reports[1]);
    if (status) status = probePointerField(reader, nextSamples, profile.extent, provenance, budget, reports[2]);
    if (status) status = probePointerField(reader, classSamples, profile.extent, provenance, budget, reports[3]);
    if (status) status = flags(reader, samples, profile.extent, provenance, budget, reports[4]);
    if (status) status = recheck(samples, [&](const auto& sample, auto offset, bool& matched) {
        std::uintptr_t address = 0; std::string observed;
        if (auto result = addressOf(sample.object, offset, address); !result) return result;
        const auto result = readFName(reader, address, names, pool, poolProfile, budget, observed);
        matched = result && observed == sample.expectedName; return result;
    }, provenance, reports[0]);
    if (status) status = recheck(samples, [&](const auto& sample, auto offset, bool& matched) {
        return ownerAt(reader, sample, offset, owner, budget, matched);
    }, provenance, reports[1]);
    for (std::size_t i = 2; status && i < 4; ++i) status = recheck(samples, [&](const auto& sample, auto offset, bool& matched) {
        std::uintptr_t observed = 0;
        const auto result = at(reader, sample.object, offset, budget, observed);
        matched = result && observed == (i == 2 ? sample.expectedNext : sample.expectedClass); return result;
    }, provenance, reports[i]);
    if (status) status = recheck(samples, [&](const auto& sample, auto offset, bool& matched) {
        std::uint32_t observed = 0;
        const auto result = at(reader, sample.object, offset, budget, observed);
        matched = result && observed == sample.expectedFlags; return result;
    }, provenance, reports[4]);
    retain();
    if (!status) return status;
    for (const auto& [dependency, version] : upstream) {
        const auto current = snapshot.offsets.find(dependency);
        if (current == snapshot.offsets.end() || current->second.version != version || current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Upstream evidence changed during FField probing"};
    }
    for (const auto& report : reports) if (report.candidates.size() != 1)
        return {Error::InvalidEvidence, "FField probing requires one unambiguous observation per field"};
    const std::array<std::uint32_t, 5> widths{names.size, owner.size, sizeof(std::uintptr_t), sizeof(std::uintptr_t), sizeof(std::uint32_t)};
    for (std::size_t i = 0; i < fields.size(); ++i) for (std::size_t j = i + 1; j < fields.size(); ++j) {
        const auto begin = *reports[i].candidates.front().value, other = *reports[j].candidates.front().value;
        if (begin < other + widths[j] && other < begin + widths[i]) {
            reports[i].rejected.push_back({begin, Error::InvalidEvidence, fields[j], "FField candidate ranges overlap"});
            retain(); return {Error::InvalidEvidence, "Distinct FField fields require non-overlapping byte ranges"};
        }
    }
    auto published = snapshot;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto existing = snapshot.offsets.find(fields[i]);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) {
            if (existing->second.value != reports[i].candidates.front().value)
                return {Error::InvalidEvidence, "A FField observation contradicts an explicit user override"};
            if (auto result = overrideValid(existing->second, upstream); !result) return result;
        } else if (auto result = publishFieldProbe(published, fields[i], reports[i], dependencies); !result) return result;
    }
    if (auto status = validateEvidenceClosure(published, fields, evidenceLimits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "FField phase cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "FField phase deadline exceeded before publication"};
    if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
        snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
        return {Error::StaleIdentity, "FField identity changed before publication"};
    snapshot = std::move(published); return {};
}
}
