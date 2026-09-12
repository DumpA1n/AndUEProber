#include "andueprober/ObjectFlags.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::string field = "UObject::ObjectFlags";
const std::array<std::string, 4> dependencies{"UObject::InternalIndex", "UObject::NamePrivate",
    "UObject::ClassPrivate", "UObject::OuterPrivate"};
bool textValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
Status identity(MemoryReader& reader, const ObjectFlagProbeProfile& profile, const ReadBudget& budget, const Snapshot& snapshot) {
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "Object flag profile, reader and snapshot identities do not match"};
    return {};
}
Status checkpoint(MemoryReader& reader, const ObjectFlagProbeProfile& profile, const ReadBudget& budget, const Snapshot& snapshot) {
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Object flag probing cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Object flag probing deadline exceeded"};
    return identity(reader, profile, budget, snapshot);
}
Status closure(MemoryReader& reader, const ObjectFlagProbeProfile& profile, const ReadBudget& budget,
    const Snapshot& snapshot, std::span<const std::string> roots) {
    if (auto status = checkpoint(reader, profile, budget, snapshot); !status) return status;
    EvidenceLimits limits;
    limits.cancelled = budget.cancelled;
    limits.deadline = budget.deadline;
    if (auto status = validateEvidenceClosure(snapshot, roots, limits); !status) return status;
    return checkpoint(reader, profile, budget, snapshot);
}
}

Status probeObjectFlags(MemoryReader& reader, const ObjectFlagProbeProfile& profile,
    std::span<const ObjectFlagSample> samples, const NameLayout& names, std::uintptr_t pool,
    const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) try {
    if (sizeof(std::uintptr_t) != 8 || std::endian::native != std::endian::little)
        return {Error::Unsupported, "Object flag probing requires little-endian 64-bit addresses"};
    if (!textValid(profile.identity) || !textValid(profile.moduleIdentity) || !profile.generation ||
        (profile.layout != Layout::UProperty && profile.layout != Layout::FField) || profile.extent < 4 || profile.extent > 4096 ||
        !pool || !textValid(poolProfile.identity) || samples.size() < 3 || samples.size() > 16)
        return {Error::InvalidArgument, "Object flag probing requires bounded layout metadata and 3-16 anchors"};
    if (auto status = validateNameLayout(names); !status) return status;
    if (auto status = validateNamePoolProfile(poolProfile); !status) return status;
    if (auto status = identity(reader, profile, budget, snapshot); !status) return status;
    const auto existing = snapshot.offsets.find(field);
    if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) snapshot.fieldReports.erase(field);
    else if (auto status = beginFieldProbe(snapshot, field); !status) return status;
    if (auto status = closure(reader, profile, budget, snapshot, dependencies); !status) return status;
    const auto nameIdentity = nameObservationIdentity(names, poolProfile, pool, profile.generation);
    const auto& nameEvidence = snapshot.offsets.at("UObject::NamePrivate").evidence;
    if (std::none_of(nameEvidence.begin(), nameEvidence.end(), [&](const auto& evidence) { return evidence.source == nameIdentity; }))
        return {Error::InvalidEvidence, "Object flags require the same Phase 1 name layout and physical pool observation"};
    std::map<std::string, std::uint64_t> upstream;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> occupied;
    for (const auto& name : dependencies) {
        const auto found = snapshot.offsets.find(name);
        upstream.emplace(name, found->second.version);
        const auto width = name == "UObject::InternalIndex" ? 4u : name == "UObject::NamePrivate" ? names.size : 8u;
        const auto begin = *found->second.value;
        if (begin > profile.extent || width > profile.extent - begin)
            return {Error::InvalidEvidence, "Validated object prefix fields exceed the declared extent"};
        occupied.emplace_back(begin, begin + width);
    }
    std::sort(occupied.begin(), occupied.end());
    for (std::size_t i = 1; i < occupied.size(); ++i)
        if (occupied[i].first < occupied[i - 1].second)
            return {Error::InvalidEvidence, "Validated object prefix fields overlap"};
    std::set<std::string> identities;
    std::set<std::uint32_t> flags;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
    for (const auto& sample : samples) {
        if (!sample.object || !textValid(sample.identity) || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "Object flag anchors require distinct bounded identities and non-null addresses"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Object flag sample extent overflows the address space"};
        ranges.emplace_back(sample.object, sample.object + profile.extent);
        flags.insert(sample.expectedFlags);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second)
            return {Error::InvalidArgument, "Object flag anchor extents overlap"};
    if (flags.size() < 2) return {Error::InvalidEvidence, "Object flag anchors require distinct declared uint32 values"};
    const auto provenance = "object-flags-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";layout:" + (profile.layout == Layout::FField ? "FField" : "UProperty") +
        ";memory-generation:" + std::to_string(profile.generation);
    if (!textValid(provenance)) return {Error::InvalidArgument, "Object flag provenance exceeds the supported text length"};
    FieldProbeReport report; report.generation = profile.generation;
    const auto read = [&](const ObjectFlagSample& sample, std::uint32_t offset, std::uint32_t& value) {
        return readExact(reader, sample.object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
    };
    for (std::uint32_t offset = 0; offset <= profile.extent - 4; offset += 4) {
        ++report.examinedOffsets;
        if (std::any_of(occupied.begin(), occupied.end(), [&](const auto& range) {
            return offset < range.second && range.first < offset + 4;
        })) {
            report.rejected.push_back({offset, Error::InvalidEvidence, profile.identity, "Flags overlap a validated object prefix field"});
            continue;
        }
        bool matched = true;
        for (const auto& sample : samples) {
            std::uint32_t observed = 0;
            if (auto status = read(sample, offset, observed); !status) { snapshot.fieldReports[field] = std::move(report); return status; }
            if (observed != sample.expectedFlags) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, "Flags differ from independent uint32 metadata"});
                matched = false; break;
            }
        }
        if (!matched) continue;
        Offset candidate; candidate.value = offset;
        Evidence evidence{"uint32 matches independent object metadata", true, samples.size(), {offset}, provenance, {}};
        for (const auto& sample : samples)
            evidence.sampleIdentities.push_back(sample.identity + ";declared-uint32:" + std::to_string(sample.expectedFlags));
        candidate.evidence.push_back(std::move(evidence));
        report.candidates.push_back(std::move(candidate));
    }
    snapshot.fieldReports[field] = report;
    if (report.candidates.size() != 1) return {Error::InvalidEvidence, "Object flags require one unambiguous candidate"};
    auto& candidate = report.candidates.front();
    for (const auto& sample : samples) {
        std::uint32_t observed = 0;
        if (auto status = read(sample, *candidate.value, observed); !status) return status;
        if (observed != sample.expectedFlags) {
            report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity, "Object flags changed before publication"});
            report.candidates.clear(); snapshot.fieldReports[field] = std::move(report);
            return {Error::InvalidEvidence, "Object flags changed before publication"};
        }
    }
    candidate.validation = Validation::Validated;
    Evidence recheck{"complete object flag final readback", true, samples.size(), {*candidate.value}, provenance, {}};
    for (const auto& sample : samples) recheck.sampleIdentities.push_back(sample.identity);
    candidate.evidence.push_back(std::move(recheck));
    snapshot.fieldReports[field] = report;
    for (const auto& [name, version] : upstream) {
        const auto current = snapshot.offsets.find(name);
        if (current == snapshot.offsets.end() || current->second.version != version)
            return {Error::InvalidEvidence, "Phase 1 evidence changed during object flag probing"};
    }
    const auto retained = snapshot.offsets.find(field);
    if (retained != snapshot.offsets.end() && retained->second.origin == Origin::User) {
        const auto& override = retained->second;
        const std::array<std::string, 1> overrideRoot{field};
        if (auto status = closure(reader, profile, budget, snapshot, overrideRoot); !status) return status;
        if (override.value != candidate.value)
            return {Error::InvalidEvidence, "An object flag override requires matching validated evidence"};
        for (const auto& [name, version] : upstream) {
            const auto prior = override.dependencies.find(name);
            if (prior == override.dependencies.end() || prior->second != version)
                return {Error::InvalidEvidence, "An object flag override requires current Phase 1 dependencies"};
        }
    }
    if (auto status = closure(reader, profile, budget, snapshot, dependencies); !status) return status;
    if (auto status = checkpoint(reader, profile, budget, snapshot); !status) return status;
    auto published = snapshot;
    if (retained == snapshot.offsets.end() || retained->second.origin != Origin::User)
        if (auto status = publishFieldProbe(published, field, report, dependencies); !status) return status;
    const std::array<std::string, 1> outputRoot{field};
    if (auto status = closure(reader, profile, budget, published, outputRoot); !status) return status;
    snapshot = std::move(published);
    return {};
} catch (...) {
    return {Error::Internal, {}};
}
}
