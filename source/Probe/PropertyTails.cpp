#include "andueprober/PropertyTails.hpp"
#include "PropertyContext.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>

namespace andueprober {
namespace {
constexpr std::size_t maximumMetadata = 4 * 1024 * 1024;
struct Failure { Error code; const char* message; };
[[noreturn]] void fail(Error code, const char* message) { throw Failure{code, message}; }
bool textValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
struct Meter {
    ReadBudget& budget;
    std::size_t remaining = maximumMetadata;
    void check() const {
        if (budget.cancelled && budget.cancelled->load()) fail(Error::Cancelled, "Property tail observation cancelled");
        if (std::chrono::steady_clock::now() >= budget.deadline) fail(Error::DeadlineExceeded, "Property tail observation deadline exceeded");
    }
    void charge(std::size_t count, std::size_t width = 1) {
        check();
        if (count > remaining / width) fail(Error::BudgetExceeded, "Property tail metadata budget exceeded");
        remaining -= count * width;
    }
    void text(const std::string& value) {
        charge(value.size());
        if (!textValid(value)) fail(Error::InvalidArgument, "Property tail metadata requires nonempty bounded UTF-8 text");
    }
};
std::vector<std::string> fields(PropertyTailKind kind) {
    switch (kind) {
    case PropertyTailKind::Enum: return {"FEnumProperty::UnderlyingType", "FEnumProperty::Enum"};
    case PropertyTailKind::Array: return {"FArrayProperty::Inner"};
    case PropertyTailKind::Set: return {"FSetProperty::ElementProp"};
    case PropertyTailKind::Map: return {"FMapProperty::KeyProp", "FMapProperty::ValueProp"};
    case PropertyTailKind::Object: return {"FObjectPropertyBase::PropertyClass"};
    case PropertyTailKind::Struct: return {"FStructProperty::Struct"};
    case PropertyTailKind::Byte: return {"FByteProperty::Enum"};
    case PropertyTailKind::Class: return {"FObjectPropertyBase::PropertyClass", "FClassProperty::MetaClass"};
    case PropertyTailKind::Interface: return {"FInterfaceProperty::InterfaceClass"};
    default: return {};
    }
}
Status at(MemoryReader& reader, const PropertyTailSample& sample, std::uint32_t offset,
    ReadBudget& budget, std::uintptr_t& value) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - sample.object)
        return {Error::Overflow, "Property tail field address overflows"};
    return readExact(reader, sample.object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
}
std::uintptr_t expected(const PropertyTailSample& sample, std::size_t column) {
    return column ? *sample.expectedSecond : sample.expectedFirst;
}
const std::string& expectedIdentity(const PropertyTailSample& sample, std::size_t column) {
    return column ? sample.secondIdentity : sample.firstIdentity;
}
void proofCharge(Meter& meter, const Evidence& evidence) {
    meter.charge(1, sizeof(Evidence)); meter.charge(evidence.check.size()); meter.charge(evidence.source.size());
    meter.charge(evidence.relativeAddresses.size(), sizeof(std::uintptr_t));
    meter.charge(evidence.sampleIdentities.size(), sizeof(std::string));
    for (const auto& identity : evidence.sampleIdentities) meter.charge(identity.size());
}
}

Status probePropertyTails(MemoryReader& reader, const PropertyTailProfile& profile, std::span<const PropertyTailSample> samples,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    try {
        const auto outputs = fields(profile.kind); const auto& property = profile.property;
        if (outputs.empty() || property.layout != Layout::FField || sizeof(std::uintptr_t) != 8 || std::endian::native != std::endian::little)
            return {Error::Unsupported, "Property tails support only the declared little-endian 64-bit pointer kinds; bool layouts remain unsupported"};
        if (!textValid(profile.identity) || !textValid(property.identity) || !textValid(property.moduleIdentity) ||
            !textValid(property.fieldBaseProfileIdentity) || !textValid(poolProfile.identity) || !pool || !property.generation ||
            profile.extent < 8 || profile.extent > 4096 || !property.fieldBaseExtent || property.extent > 4096 ||
            property.fieldBaseExtent > property.extent || profile.propertyBaseExtent < property.fieldBaseExtent ||
            profile.propertyBaseExtent > profile.extent - 8 || samples.size() < 3 || samples.size() > 16)
            return {Error::InvalidArgument, "Property tails require independent bounded prefixes, scan extents and 3-16 anchors"};
        detail::PropertyContext context;
        if (auto status = detail::preparePropertyContext(reader, property, profile.propertyBaseExtent, profile.extent,
            8, names, pool, poolProfile, outputs, budget, snapshot, context); !status) return status;
        const auto& propertySource = context.source;
        Meter input{budget}; input.charge(samples.size(), sizeof(PropertyTailSample));
        std::set<std::string> identities;
        std::map<std::string, std::uintptr_t> targets;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
        std::array<std::set<std::uintptr_t>, 2> distinct;
        std::vector<Evidence> proofs;
        for (const auto& field : outputs) {
            Evidence proof{"opaque property-tail pointers match independent named metadata", true, samples.size(), {}, {}, {}};
            proof.source = "property-tail-profile:" + profile.identity + ";field:" + field +
                ";occupied-property-prefix:" + std::to_string(profile.propertyBaseExtent) + ";" + propertySource;
            input.text(proof.source); proofs.push_back(std::move(proof));
        }
        for (const auto& sample : samples) {
            input.text(sample.identity);
            if (!sample.object || !identities.insert(sample.identity).second ||
                (outputs.size() == 1 && (sample.expectedSecond || !sample.secondIdentity.empty())) ||
                (outputs.size() == 2 && !sample.expectedSecond))
                return {Error::InvalidArgument, "Property tail samples require distinct objects and exactly the fields of their kind"};
            if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
                return {Error::Overflow, "Property tail sample range overflows"};
            const auto end = sample.object + profile.extent;
            for (const auto& [other, otherEnd] : ranges)
                if (sample.object < otherEnd && other < end) return {Error::InvalidArgument, "Independent property tail sample ranges must not overlap"};
            ranges.emplace_back(sample.object, end);
            for (std::size_t column = 0; column < outputs.size(); ++column) {
                const auto value = expected(sample, column); const auto& identity = expectedIdentity(sample, column);
                input.text(identity);
                const auto [target, inserted] = targets.emplace(identity, value);
                if (!inserted && target->second != value) return {Error::InvalidEvidence, "A named property-tail target denotes contradictory addresses"};
                if (value) distinct[column].insert(value);
                auto named = sample.identity + ";expected-pointer:" + identity; input.text(named);
                proofs[column].sampleIdentities.push_back(std::move(named));
            }
        }
        for (std::size_t column = 0; column < outputs.size(); ++column)
            if (distinct[column].size() < 2) return {Error::InvalidEvidence, "Each property tail field requires two distinct non-null named targets"};
        Meter reportsBudget{budget};
        std::vector<std::reference_wrapper<FieldProbeReport>> reports;
        for (const auto& field : outputs) {
            auto& report = snapshot.fieldReports[field]; report = {}; report.generation = budget.generation;
            reports.emplace_back(report);
        }
        const auto reject = [&](FieldProbeReport& report, std::uint32_t offset, const PropertyTailSample& sample, const char* reason) {
            reportsBudget.charge(1, sizeof(CandidateRejection)); reportsBudget.charge(sample.identity.size());
            const std::string message = reason; reportsBudget.charge(message.size());
            report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity, message});
        };
        for (std::size_t column = 0; column < outputs.size(); ++column) {
            auto& report = reports[column].get(); report.generation = budget.generation;
            for (std::uint32_t offset = (profile.propertyBaseExtent + 7) / 8 * 8; offset <= profile.extent - 8; offset += 8) {
                reportsBudget.check(); ++report.examinedOffsets; bool matched = true;
                for (const auto& sample : samples) {
                    std::uintptr_t value = 0;
                    if (auto status = at(reader, sample, offset, budget, value); !status) { return status; }
                    if (value != expected(sample, column)) {
                        reject(report, offset, sample, "Pointer differs from independently declared tail metadata"); matched = false; break;
                    }
                }
                if (matched) {
                    reportsBudget.charge(1, sizeof(Offset)); proofCharge(reportsBudget, proofs[column]);
                    reportsBudget.charge(1, sizeof(std::uintptr_t));
                    auto proof = proofs[column]; proof.relativeAddresses = {offset};
                    Offset candidate; candidate.value = offset; candidate.evidence.push_back(std::move(proof));
                    report.candidates.push_back(std::move(candidate));
                }
            }
        }
        for (std::size_t column = 0; column < outputs.size(); ++column) {
            auto& report = reports[column].get();
            for (auto& candidate : report.candidates) {
                for (const auto& sample : samples) {
                    std::uintptr_t value = 0;
                    if (auto status = at(reader, sample, *candidate.value, budget, value); !status) { return status; }
                    if (value != expected(sample, column)) {
                        reject(report, *candidate.value, sample, "Property tail changed before phase publication");
                        report.candidates.clear(); return {Error::InvalidEvidence, "Property tail observation changed before publication"};
                    }
                }
                auto proof = proofs[column]; proof.check = "complete property-tail phase final readback matches every declared pointer";
                proof.relativeAddresses = {*candidate.value}; proofCharge(reportsBudget, proof); candidate.evidence.push_back(std::move(proof));
            }
            if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
        }
        for (const auto& report : reports) if (report.get().candidates.size() != 1)
            return {Error::InvalidEvidence, "Each property-tail field requires one unambiguous candidate"};
        if (outputs.size() == 2 && reports[0].get().candidates.front().value == reports[1].get().candidates.front().value)
            return {Error::InvalidEvidence, "Distinct property-tail fields require separate pointer slots"};
        return detail::publishPropertyContext(reader, property, context, outputs, budget, snapshot);
    } catch (const Failure& error) {
        try { return {error.code, error.message}; } catch (...) { return {error.code, {}}; }
    } catch (...) { return {Error::Internal, {}}; }
}
}
