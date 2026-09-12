#include "andueprober/Properties.hpp"
#include "andueprober/Evidence.hpp"
#include "andueprober/Relations.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 2> outputs{"sizeof(FProperty)", "FProperty::SubPropertyBase"};
const std::array<std::string, 18> dependencies{"UObject::InternalIndex", "UObject::NamePrivate",
    "UObject::ClassPrivate", "UObject::OuterPrivate", "UField::Next", "UStruct::SuperStruct",
    "UStruct::Children", "UStruct::PropertiesSize", "UStruct::ChildProperties", "FField::NamePrivate",
    "FField::Owner", "FField::Next", "FField::ClassPrivate", "FField::FlagsPrivate",
    "FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal"};
bool textValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
template<class T>
Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset,
    ReadBudget& budget, T& value) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "Property-tail address overflow"};
    auto status = readExact(reader, object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
    if (!status) status.message = "Property-tail read failed at offset " + std::to_string(offset) + ": " + status.message;
    return status;
}
template<class Sample>
Status validateSamples(std::span<const Sample> samples, std::uint32_t extent, const char* message) {
    if (samples.size() < 2 || samples.size() > 16) return {Error::InvalidArgument, message};
    std::set<std::uintptr_t> objects;
    std::set<std::string> identities;
    for (const auto& sample : samples) {
        if (!sample.object || !textValid(sample.identity) || !objects.insert(sample.object).second ||
            !identities.insert(sample.identity).second || extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::InvalidArgument, message};
    }
    return {};
}
Status retainOverride(const Offset& retained, std::uint32_t value,
    const std::map<std::string, std::uint64_t>& upstream) {
    if (retained.value != value || retained.validation != Validation::Validated || retained.evidence.empty())
        return {Error::InvalidEvidence, "Property-base evidence contradicts an explicit user override"};
    for (const auto& [name, version] : upstream) {
        const auto found = retained.dependencies.find(name);
        if (found == retained.dependencies.end() || found->second != version)
            return {Error::InvalidEvidence, "Property-base override has stale dependencies"};
    }
    return {};
}
}

Status probePropertyBases(MemoryReader& reader, const PropertyBaseProbeProfile& profile,
    std::span<const PropertyPointerTailSample> pointers, std::span<const PropertyBoolTailSample> booleans,
    ReadBudget& budget, Snapshot& snapshot) {
    if (profile.layout != Layout::FField || sizeof(std::uintptr_t) != 8)
        return {Error::Unsupported, "Property-base discovery requires the 64-bit FField layout"};
    if (profile.boolRepresentation != BoolTailRepresentation::NativeBool)
        return {Error::Unsupported, "Property-base discovery requires the declared NativeBool representation"};
    if (!textValid(profile.identity) || !textValid(profile.moduleIdentity) ||
        !textValid(profile.propertyObservationIdentity) || !profile.generation ||
        profile.extent < 32 || profile.extent > 4096)
        return {Error::InvalidArgument, "Property-base discovery requires bounded profile metadata"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != profile.generation ||
        reader.generation() != profile.generation)
        return {Error::StaleIdentity, "Property-base profile, reader and snapshot identities do not match"};
    for (const auto& output : outputs) {
        const auto found = snapshot.offsets.find(output);
        if (found != snapshot.offsets.end() && found->second.origin == Origin::User) snapshot.fieldReports.erase(output);
        else if (auto status = beginFieldProbe(snapshot, output); !status) return status;
    }
    EvidenceLimits limits; limits.cancelled = budget.cancelled; limits.deadline = budget.deadline;
    if (auto status = validateEvidenceClosure(snapshot, dependencies, limits); !status) return status;
    for (const auto* field : {"FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal"}) {
        const auto& evidence = snapshot.offsets.at(field).evidence;
        if (std::none_of(evidence.begin(), evidence.end(), [&](const auto& item) {
            return item.source == profile.propertyObservationIdentity;
        })) return {Error::InvalidEvidence, "Property-base anchors require the matching scalar observation identity"};
    }
    if (auto status = validateSamples(pointers, profile.extent,
        "Property-base discovery requires 2-16 distinct pointer anchors"); !status) return status;
    if (auto status = validateSamples(booleans, profile.extent,
        "Property-base discovery requires 2-16 distinct NativeBool anchors"); !status) return status;
    std::set<std::uintptr_t> expectedPointers;
    for (const auto& sample : pointers)
        if (!sample.expectedPointer || !expectedPointers.insert(sample.expectedPointer).second)
            return {Error::InvalidArgument, "Property pointer anchors require distinct nonzero expected values"};
    const auto scalarOffset = *snapshot.offsets.at("FProperty::Offset_Internal").value;
    if (scalarOffset > profile.extent - 4)
        return {Error::InvalidEvidence, "Offset_Internal exceeds the property-tail extent"};
    const auto scalarEnd = scalarOffset + 4;
    const auto searchStart = (scalarEnd + 7u) & ~7u;
    const auto searchEnd = std::min<std::uint32_t>(profile.extent - 8, searchStart + 64);
    if (searchStart > searchEnd) return {Error::InvalidEvidence, "No bounded property-tail search range remains"};

    FieldProbeReport pointerReport; pointerReport.generation = profile.generation;
    for (std::uint32_t offset = searchStart; offset <= searchEnd; offset += 8) {
        ++pointerReport.examinedOffsets;
        bool matched = true;
        for (const auto& sample : pointers) {
            std::uintptr_t value = 0;
            if (auto status = at(reader, sample.object, offset, budget, value); !status) return status;
            if (value != sample.expectedPointer) {
                pointerReport.rejected.push_back({offset, Error::InvalidEvidence, sample.identity,
                    "Derived-property pointer differs from its independent relationship"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset; candidate.validation = Validation::Candidate;
            candidate.evidence.push_back({"derived-property pointer anchors agree", true, pointers.size(), {offset},
                profile.identity, {}});
            for (const auto& sample : pointers) candidate.evidence.back().sampleIdentities.push_back(sample.identity);
            pointerReport.candidates.push_back(std::move(candidate));
        }
    }
    snapshot.fieldReports[outputs[1]] = pointerReport;
    if (pointerReport.candidates.size() != 1)
        return {Error::InvalidEvidence, "SubPropertyBase requires one unambiguous pointer-tail offset"};
    pointerReport.candidates.front().validation = Validation::Validated;
    const auto subPropertyBase = *pointerReport.candidates.front().value;

    const auto boolStart = std::max<std::uint32_t>(scalarEnd, subPropertyBase > 16 ? subPropertyBase - 16 : 0);
    const auto boolEnd = std::min<std::uint32_t>(profile.extent - 4, subPropertyBase + 4);
    FieldProbeReport boolReport; boolReport.generation = profile.generation;
    constexpr std::array<std::uint8_t, 4> nativeBool{1, 0, 1, 0xff};
    for (std::uint32_t offset = boolStart; offset <= boolEnd; ++offset) {
        ++boolReport.examinedOffsets;
        bool matched = true;
        for (const auto& sample : booleans) {
            std::array<std::uint8_t, 4> value{};
            if (auto status = at(reader, sample.object, offset, budget, value); !status) return status;
            if (value != nativeBool) {
                boolReport.rejected.push_back({offset, Error::InvalidEvidence, sample.identity,
                    "NativeBool metadata does not match the supported contract"});
                matched = false; break;
            }
        }
        if (matched) {
            const auto size = offset & ~7u;
            if (size < scalarEnd || size > subPropertyBase) continue;
            Offset candidate; candidate.value = size; candidate.validation = Validation::Candidate;
            candidate.evidence.push_back({"NativeBool metadata establishes aligned FProperty boundary", true,
                booleans.size(), {offset, size}, profile.identity, {}});
            for (const auto& sample : booleans) candidate.evidence.back().sampleIdentities.push_back(sample.identity);
            if (std::none_of(boolReport.candidates.begin(), boolReport.candidates.end(), [&](const auto& prior) {
                return prior.value == candidate.value;
            })) boolReport.candidates.push_back(std::move(candidate));
        }
    }
    snapshot.fieldReports[outputs[0]] = boolReport;
    if (boolReport.candidates.size() != 1)
        return {Error::InvalidEvidence, "sizeof(FProperty) requires one unambiguous NativeBool boundary"};
    boolReport.candidates.front().validation = Validation::Validated;
    const auto propertySize = *boolReport.candidates.front().value;

    for (const auto& sample : pointers) {
        std::uintptr_t value = 0;
        if (auto status = at(reader, sample.object, subPropertyBase, budget, value); !status) return status;
        if (value != sample.expectedPointer) return {Error::InvalidEvidence, "Property pointer anchor changed before publication"};
    }
    for (const auto& sample : booleans) {
        std::array<std::uint8_t, 4> value{};
        const auto metadataOffset = boolReport.candidates.front().evidence.front().relativeAddresses.front();
        if (auto status = at(reader, sample.object, static_cast<std::uint32_t>(metadataOffset), budget, value); !status) return status;
        if (value != nativeBool) return {Error::InvalidEvidence, "NativeBool anchor changed before publication"};
    }
    std::map<std::string, std::uint64_t> upstream;
    for (const auto& dependency : dependencies) upstream.emplace(dependency, snapshot.offsets.at(dependency).version);
    for (const auto& [name, version] : upstream) {
        const auto current = snapshot.offsets.find(name);
        if (current == snapshot.offsets.end() || current->second.version != version ||
            current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Property scalar evidence changed before base publication"};
    }
    auto published = snapshot;
    const std::array<std::uint32_t, 2> values{propertySize, subPropertyBase};
    const std::array<FieldProbeReport, 2> reports{boolReport, pointerReport};
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto retained = snapshot.offsets.find(outputs[index]);
        if (retained != snapshot.offsets.end() && retained->second.origin == Origin::User) {
            if (auto status = retainOverride(retained->second, values[index], upstream); !status) return status;
        } else if (auto status = publishFieldProbe(published, outputs[index], reports[index], dependencies); !status) return status;
    }
    if (auto status = validateEvidenceClosure(published, outputs, limits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Property-base discovery cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline)
        return {Error::DeadlineExceeded, "Property-base discovery deadline exceeded before publication"};
    if (reader.generation() != profile.generation) return {Error::StaleIdentity, "Property-base generation changed before publication"};
    snapshot = std::move(published);
    return {};
}
} // namespace andueprober
