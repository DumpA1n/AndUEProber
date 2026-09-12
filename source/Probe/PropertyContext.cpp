#include "PropertyContext.hpp"
#include "andueprober/Evidence.hpp"
#include <algorithm>
#include <bit>

namespace andueprober::detail {
const std::array<std::string, 9> propertyDependencies{"FField::NamePrivate", "FField::Owner", "FField::Next",
    "FField::ClassPrivate", "FField::FlagsPrivate", "FProperty::ArrayDim", "FProperty::ElementSize",
    "FProperty::PropertyFlags", "FProperty::Offset_Internal"};
bool propertyTextValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
Status preparePropertyContext(MemoryReader& reader, const PropertyProbeProfile& property, std::uint32_t occupiedPrefix,
    std::uint32_t extent, std::uint32_t minimumWidth, const NameLayout& names, std::uintptr_t pool,
    const NamePoolProfile& poolProfile, std::span<const std::string> outputs, ReadBudget& budget,
    Snapshot& snapshot, PropertyContext& context) {
    if (property.layout != Layout::FField || sizeof(std::uintptr_t) != 8 || std::endian::native != std::endian::little)
        return {Error::Unsupported, "Property metadata requires a declared little-endian 64-bit FField layout"};
    if (!propertyTextValid(property.identity) || !propertyTextValid(property.moduleIdentity) ||
        !propertyTextValid(property.fieldBaseProfileIdentity) || !propertyTextValid(poolProfile.identity) || !pool ||
        !property.generation || !minimumWidth || extent < minimumWidth || extent > 4096 || !property.fieldBaseExtent ||
        property.extent > 4096 || property.fieldBaseExtent > property.extent || occupiedPrefix < property.fieldBaseExtent ||
        occupiedPrefix > extent - minimumWidth)
        return {Error::InvalidArgument, "Property metadata requires bounded independent profiles and occupied prefixes"};
    if (auto status = validateNameLayout(names); !status) return status;
    if (auto status = validateNamePoolProfile(poolProfile); !status) return status;
    const auto& owner = property.ownerLayout;
    if (owner.representation != FieldOwnerRepresentation::SeparateBoolean)
        return {Error::Unsupported, "Property metadata requires the preceding SeparateBoolean owner layout"};
    if (owner.size < 9 || owner.size > 64 || owner.size > property.fieldBaseExtent || owner.pointerOffset > owner.size - 8 ||
        owner.pointerOffset % 8 || owner.kindOffset >= owner.size ||
        (owner.kindOffset >= owner.pointerOffset && owner.kindOffset < owner.pointerOffset + 8))
        return {Error::InvalidArgument, "Property metadata requires a bounded disjoint inherited owner layout"};
    if (property.moduleIdentity != snapshot.moduleIdentity || property.generation != snapshot.generation || property.layout != snapshot.layout ||
        reader.generation() != property.generation || budget.generation != property.generation)
        return {Error::StaleIdentity, "Property metadata profile, reader and snapshot identities do not match"};
    for (const auto& field : outputs) {
        const auto old = snapshot.offsets.find(field);
        if (old != snapshot.offsets.end() && old->second.origin == Origin::User) snapshot.fieldReports.erase(field);
        else if (auto status = beginFieldProbe(snapshot, field); !status) return status;
    }
    EvidenceLimits limits; limits.deadline = budget.deadline; limits.cancelled = budget.cancelled;
    if (auto status = validateEvidenceClosure(snapshot, propertyDependencies, limits); !status) return status;
    auto& versions = context.versions;
    for (const auto& field : propertyDependencies) versions[field] = snapshot.offsets.at(field).version;
    const auto nameSource = nameObservationIdentity(names, poolProfile, pool, property.generation);
    const FieldProbeProfile base{property.fieldBaseProfileIdentity, property.moduleIdentity, property.generation,
        property.layout, property.fieldBaseExtent, owner};
    const auto ownerSource = fieldObservationIdentity(base, names, pool, poolProfile);
    context.source = propertyObservationIdentity(property, names, pool, poolProfile);
    const auto& propertySource = context.source;
    const auto hasSource = [&](const std::string& field, const std::string& source) {
        const auto& proof = snapshot.offsets.at(field).evidence;
        return std::any_of(proof.begin(), proof.end(), [&](const auto& item) { return item.source == source; });
    };
    if (!hasSource(propertyDependencies[0], nameSource) || !hasSource(propertyDependencies[1], ownerSource))
        return {Error::InvalidEvidence, "Property metadata requires the exact inherited Name and Owner observations"};
    for (std::size_t i = 5; i < propertyDependencies.size(); ++i)
        if (!hasSource(propertyDependencies[i], propertySource))
            return {Error::InvalidEvidence, "Property metadata requires the exact preceding scalar property observation"};
    const std::array<std::uint32_t, 9> widths{names.size, owner.size, 8, 8, 4, 4, 4, 8, 4};
    for (std::size_t i = 0; i < propertyDependencies.size(); ++i) {
        const auto begin = *snapshot.offsets.at(propertyDependencies[i]).value;
        const auto extent = i < 5 ? property.fieldBaseExtent : std::min(property.extent, occupiedPrefix);
        if (widths[i] > extent || begin > extent - widths[i] || (i >= 5 && begin < property.fieldBaseExtent))
            return {Error::InvalidEvidence, "A validated base field exceeds its independently declared occupied prefix"};
        for (std::size_t j = 0; j < i; ++j) {
            const auto other = *snapshot.offsets.at(propertyDependencies[j]).value;
            if (std::uint64_t(begin) < std::uint64_t(other) + widths[j] && std::uint64_t(other) < std::uint64_t(begin) + widths[i])
                return {Error::InvalidEvidence, "All inherited FField and FProperty byte ranges must remain disjoint"};
        }
    }
    return {};
}
Status publishPropertyContext(MemoryReader& reader, const PropertyProbeProfile& property, const PropertyContext& context,
    std::span<const std::string> outputs, ReadBudget& budget, Snapshot& snapshot) {
    EvidenceLimits limits; limits.deadline = budget.deadline; limits.cancelled = budget.cancelled;
    if (auto status = validateEvidenceClosure(snapshot, propertyDependencies, limits); !status) return status;
    for (const auto& [field, version] : context.versions)
        if (snapshot.offsets.at(field).version != version) return {Error::InvalidEvidence, "Property metadata dependency changed during observation"};
    auto published = snapshot;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto old = snapshot.offsets.find(outputs[i]);
        if (old != snapshot.offsets.end() && old->second.origin == Origin::User) {
            if (old->second.value != snapshot.fieldReports.at(outputs[i]).candidates.front().value)
                return {Error::InvalidEvidence, "Property metadata observation contradicts an explicit user override"};
            for (const auto& [dependency, version] : context.versions) {
                const auto prior = old->second.dependencies.find(dependency);
                if (prior == old->second.dependencies.end() || prior->second != version)
                    return {Error::InvalidEvidence, "A matching property metadata override requires every current prerequisite"};
            }
        } else if (auto status = publishFieldProbe(published, outputs[i], snapshot.fieldReports.at(outputs[i]), propertyDependencies); !status) return status;
    }
    if (auto status = validateEvidenceClosure(published, outputs, limits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Property metadata publication cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Property metadata publication deadline exceeded"};
    if (reader.generation() != property.generation || snapshot.generation != property.generation ||
        snapshot.moduleIdentity != property.moduleIdentity || snapshot.layout != property.layout)
        return {Error::StaleIdentity, "Property metadata identity changed before publication"};
    snapshot = std::move(published); return {};
}
}
