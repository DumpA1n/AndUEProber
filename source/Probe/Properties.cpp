#include "andueprober/Properties.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 4> fields{"FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal"};
constexpr std::array<std::uint32_t, 4> widths{4, 4, 8, 4};
const std::array<std::string, 14> dependencies{"UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate",
    "UObject::OuterPrivate", "UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize", "UStruct::ChildProperties",
    "FField::NamePrivate", "FField::Owner", "FField::Next", "FField::ClassPrivate", "FField::FlagsPrivate"};
bool textValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
template<class T>
Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset, ReadBudget& budget, T& value) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "FProperty field address overflow"};
    return readExact(reader, object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
}
Status samplesValid(const PropertyProbeProfile& profile, std::span<const PropertySample> samples) {
    if (samples.size() < 3 || samples.size() > 16)
        return {Error::InvalidArgument, "FProperty probing requires 3-16 independent named anchors"};
    std::set<std::uintptr_t> objects;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
    std::set<std::string> identities;
    std::array<std::set<std::uint64_t>, 4> distinct;
    std::map<std::string, std::uint32_t> containingExtents;
    for (const auto& sample : samples) {
        if (!sample.object || !textValid(sample.identity) || !textValid(sample.containingValueIdentity) ||
            !objects.insert(sample.object).second || !identities.insert(sample.identity).second ||
            sample.expectedArrayDim <= 0 || sample.expectedElementSize <= 0 || sample.expectedOffsetInternal < 0 ||
            !sample.containingValueSize || sample.containingValueSize > 16 * 1024 * 1024)
            return {Error::InvalidArgument, "FProperty anchors require distinct names, positive dimensions and sizes, and a bounded containing value"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "FProperty sample extent overflows the address space"};
        const auto endAddress = sample.object + profile.extent;
        for (const auto& [begin, end] : ranges)
            if (sample.object < end && begin < endAddress)
                return {Error::InvalidArgument, "Independent FProperty sample ranges must not overlap"};
        ranges.emplace_back(sample.object, endAddress);
        const auto bytes = std::uint64_t(sample.expectedArrayDim) * std::uint64_t(sample.expectedElementSize);
        const auto end = bytes + std::uint64_t(sample.expectedOffsetInternal);
        if (end > std::numeric_limits<std::uint32_t>::max())
            return {Error::Overflow, "Declared FProperty value range exceeds the 32-bit size domain"};
        if (end > sample.containingValueSize)
            return {Error::InvalidEvidence, "Declared FProperty value does not fit its independent containing extent"};
        const auto [containing, inserted] = containingExtents.emplace(sample.containingValueIdentity, sample.containingValueSize);
        if (!inserted && containing->second != sample.containingValueSize)
            return {Error::InvalidEvidence, "A containing-value identity denotes contradictory sizes"};
        distinct[0].insert(sample.expectedArrayDim); distinct[1].insert(sample.expectedElementSize);
        distinct[2].insert(sample.expectedPropertyFlags); distinct[3].insert(sample.expectedOffsetInternal);
    }
    if (std::any_of(distinct.begin(), distinct.end(), [](const auto& values) { return values.size() < 2; }))
        return {Error::InvalidEvidence, "FProperty anchors require at least two independent expectations per field"};
    return {};
}
template<class T, class Expected>
Status scalar(MemoryReader& reader, std::span<const PropertySample> samples, Expected expected, std::uint32_t extent, std::uint32_t prefix,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    for (std::uint32_t offset = (prefix + sizeof(T) - 1) / sizeof(T) * sizeof(T); offset <= extent - sizeof(T); offset += sizeof(T)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, offset, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, "FProperty scalar differs from independent metadata"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"property scalar matches independent containing-value metadata", true, samples.size(), {offset}};
            evidence.source = provenance + ";scalar-bytes:" + std::to_string(sizeof(T));
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";declared-scalar:" + std::to_string(expected(sample)) +
                    ";containing-value:" + sample.containingValueIdentity + ";containing-bytes:" + std::to_string(sample.containingValueSize));
            candidate.evidence.push_back(std::move(evidence)); report.candidates.push_back(std::move(candidate));
        }
    }
    if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    return {};
}
template<class T, class Expected>
Status recheck(MemoryReader& reader, std::span<const PropertySample> samples, Expected expected,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    for (auto& candidate : report.candidates) {
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, *candidate.value, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity, "FProperty observation changed before phase publication"});
                report.candidates.clear(); return {Error::InvalidEvidence, "FProperty observations changed before phase publication"};
            }
        }
        Evidence evidence{"complete property scalar phase final recheck matches declared anchors", true, samples.size(), {*candidate.value}};
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
            return {Error::InvalidEvidence, "A matching FProperty override requires current upstream dependency versions"};
    }
    return {};
}
}

std::string propertyObservationIdentity(const PropertyProbeProfile& profile, const NameLayout& names,
    std::uintptr_t pool, const NamePoolProfile& poolProfile) {
    const FieldProbeProfile inherited{profile.fieldBaseProfileIdentity, profile.moduleIdentity, profile.generation,
        profile.layout, profile.fieldBaseExtent, profile.ownerLayout};
    return "property-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";layout:FField;memory-generation:" + std::to_string(profile.generation) +
        ";inherited-prefix-bytes:" + std::to_string(profile.fieldBaseExtent) + ";" +
        fieldObservationIdentity(inherited, names, pool, poolProfile);
}

Status probePropertyFields(MemoryReader& reader, const PropertyProbeProfile& profile,
    std::span<const PropertySample> samples, const NameLayout& names, std::uintptr_t pool,
    const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    if (profile.layout != Layout::FField || sizeof(std::uintptr_t) != sizeof(std::uint64_t))
        return {Error::Unsupported, "FProperty probing requires the explicit 64-bit FField layout"};
    if (!textValid(profile.identity) || !textValid(profile.moduleIdentity) || !textValid(profile.fieldBaseProfileIdentity) ||
        !profile.generation || !pool || !profile.fieldBaseExtent || profile.fieldBaseExtent > profile.extent ||
        profile.extent < sizeof(std::uint64_t) || profile.extent > 4096)
        return {Error::InvalidArgument, "FProperty probing requires explicit bounded module and scan metadata"};
    if (auto status = validateNameLayout(names); !status) return status;
    if (auto status = validateNamePoolProfile(poolProfile); !status) return status;
    const auto& owner = profile.ownerLayout;
    if (owner.representation != FieldOwnerRepresentation::SeparateBoolean)
        return {Error::Unsupported, "FProperty requires the validated SeparateBoolean FField owner representation"};
    if (owner.size < sizeof(std::uintptr_t) + 1 || owner.size > 64 || owner.size > profile.fieldBaseExtent ||
        owner.pointerOffset > owner.size - sizeof(std::uintptr_t) || owner.pointerOffset % sizeof(std::uintptr_t) ||
        owner.kindOffset >= owner.size ||
        (owner.kindOffset >= owner.pointerOffset && owner.kindOffset < owner.pointerOffset + sizeof(std::uintptr_t)))
        return {Error::InvalidArgument, "FProperty inherited owner layout requires bounded disjoint pointer and boolean fields"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "FProperty profile, reader and snapshot identities do not match"};
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
    const FieldProbeProfile inherited{profile.fieldBaseProfileIdentity, profile.moduleIdentity, profile.generation,
        profile.layout, profile.fieldBaseExtent, owner};
    const auto baseIdentity = fieldObservationIdentity(inherited, names, pool, poolProfile);
    const auto hasSource = [&](const std::string& field, const std::string& source) {
        const auto& evidence = snapshot.offsets.at(field).evidence;
        return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) { return item.source == source; });
    };
    if (!hasSource("UObject::NamePrivate", nameIdentity) || !hasSource("FField::NamePrivate", nameIdentity) ||
        !hasSource("FField::Owner", baseIdentity))
        return {Error::InvalidEvidence, "FProperty inherited name and owner layouts require matching profile and canonical pool evidence"};
    const std::array<std::pair<std::string, std::uint32_t>, 5> baseFields{{
        {"FField::NamePrivate", names.size}, {"FField::Owner", owner.size}, {"FField::Next", 8},
        {"FField::ClassPrivate", 8}, {"FField::FlagsPrivate", 4}}};
    for (std::size_t i = 0; i < baseFields.size(); ++i) {
        const auto& [field, width] = baseFields[i];
        const auto begin = *snapshot.offsets.at(field).value;
        if (width > profile.fieldBaseExtent || begin > profile.fieldBaseExtent - width)
            return {Error::InvalidEvidence, "Validated FField field range exceeds the independently declared inherited prefix"};
        for (std::size_t j = 0; j < i; ++j) {
            const auto other = *snapshot.offsets.at(baseFields[j].first).value;
            if (begin < std::uint64_t(other) + baseFields[j].second && other < std::uint64_t(begin) + width)
                return {Error::InvalidEvidence, "Validated FField base ranges must remain disjoint"};
        }
    }
    if (auto status = samplesValid(profile, samples); !status) return status;
    const auto provenance = propertyObservationIdentity(profile, names, pool, poolProfile);
    const auto dim = [](const auto& sample) { return sample.expectedArrayDim; };
    const auto size = [](const auto& sample) { return sample.expectedElementSize; };
    const auto flags = [](const auto& sample) { return sample.expectedPropertyFlags; };
    const auto offset = [](const auto& sample) { return sample.expectedOffsetInternal; };
    std::array<FieldProbeReport, 4> reports;
    const auto retain = [&] {
        for (std::size_t i = 0; i < fields.size(); ++i) if (reports[i].generation) snapshot.fieldReports[fields[i]] = reports[i];
    };
    auto status = scalar<std::int32_t>(reader, samples, dim, profile.extent, profile.fieldBaseExtent, provenance, budget, reports[0]);
    if (status) status = scalar<std::int32_t>(reader, samples, size, profile.extent, profile.fieldBaseExtent, provenance, budget, reports[1]);
    if (status) status = scalar<std::uint64_t>(reader, samples, flags, profile.extent, profile.fieldBaseExtent, provenance, budget, reports[2]);
    if (status) status = scalar<std::int32_t>(reader, samples, offset, profile.extent, profile.fieldBaseExtent, provenance, budget, reports[3]);
    if (status) status = recheck<std::int32_t>(reader, samples, dim, provenance, budget, reports[0]);
    if (status) status = recheck<std::int32_t>(reader, samples, size, provenance, budget, reports[1]);
    if (status) status = recheck<std::uint64_t>(reader, samples, flags, provenance, budget, reports[2]);
    if (status) status = recheck<std::int32_t>(reader, samples, offset, provenance, budget, reports[3]);
    retain(); if (!status) return status;
    for (const auto& [dependency, version] : upstream) {
        const auto current = snapshot.offsets.find(dependency);
        if (current == snapshot.offsets.end() || current->second.version != version || current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Upstream evidence changed during FProperty probing"};
    }
    for (const auto& report : reports) if (report.candidates.size() != 1)
        return {Error::InvalidEvidence, "FProperty probing requires one unambiguous observation per field"};
    for (std::size_t i = 0; i < fields.size(); ++i) for (std::size_t j = i + 1; j < fields.size(); ++j) {
        const auto begin = *reports[i].candidates.front().value, other = *reports[j].candidates.front().value;
        if (begin < other + widths[j] && other < begin + widths[i]) {
            reports[i].rejected.push_back({begin, Error::InvalidEvidence, fields[j], "FProperty candidate ranges overlap"});
            retain(); return {Error::InvalidEvidence, "Distinct FProperty fields require non-overlapping byte ranges"};
        }
    }
    auto published = snapshot;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto existing = snapshot.offsets.find(fields[i]);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) {
            if (existing->second.value != reports[i].candidates.front().value)
                return {Error::InvalidEvidence, "A FProperty observation contradicts an explicit user override"};
            if (auto result = overrideValid(existing->second, upstream); !result) return result;
        } else if (auto result = publishFieldProbe(published, fields[i], reports[i], dependencies); !result) return result;
    }
    if (auto status = validateEvidenceClosure(published, fields, evidenceLimits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "FProperty phase cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "FProperty phase deadline exceeded before publication"};
    if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
        snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
        return {Error::StaleIdentity, "FProperty identity changed before publication"};
    snapshot = std::move(published); return {};
}
}
