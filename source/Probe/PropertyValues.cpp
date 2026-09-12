#include "andueprober/PropertyValues.hpp"
#include "PropertyContext.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 4> boolFields{"FBoolProperty::FieldSize", "FBoolProperty::ByteOffset",
    "FBoolProperty::ByteMask", "FBoolProperty::FieldMask"};
const std::array<std::string, 1> pathFields{"FFieldPathProperty::PropertyClass"};
struct Failure { Error code; const char* message; };
[[noreturn]] void fail(Error code, const char* message) { throw Failure{code, message}; }
struct Meter {
    ReadBudget& budget;
    std::size_t remaining = 4 * 1024 * 1024;
    void check() const {
        if (budget.cancelled && budget.cancelled->load()) fail(Error::Cancelled, "Property metadata observation cancelled");
        if (std::chrono::steady_clock::now() >= budget.deadline) fail(Error::DeadlineExceeded, "Property metadata observation deadline exceeded");
    }
    void charge(std::size_t count, std::size_t width = 1) {
        check();
        if (count > remaining / width) fail(Error::BudgetExceeded, "Property metadata budget exceeded");
        remaining -= count * width;
    }
    void text(const std::string& value) {
        charge(value.size());
        if (!detail::propertyTextValid(value)) fail(Error::InvalidArgument, "Property metadata requires bounded nonempty UTF-8 text");
    }
    void proof(const Evidence& value) {
        charge(1, sizeof(Evidence)); charge(value.check.size()); charge(value.source.size());
        charge(value.relativeAddresses.size(), sizeof(std::uintptr_t));
        charge(value.sampleIdentities.size(), sizeof(std::string));
        for (const auto& identity : value.sampleIdentities) charge(identity.size());
    }
};
template<class Sample> Status validateSamples(std::span<const Sample> samples, std::uint32_t extent, Meter& meter) {
    if (samples.size() < 3 || samples.size() > 16) return {Error::InvalidArgument, "Property metadata requires 3-16 independent anchors"};
    meter.charge(samples.size(), sizeof(Sample));
    std::set<std::string> identities;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i]; meter.text(sample.identity);
        if (!sample.object || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "Property metadata requires distinct named objects"};
        if (extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Property metadata object range overflows"};
        for (std::size_t j = 0; j < i; ++j)
            if (sample.object < samples[j].object + extent && samples[j].object < sample.object + extent)
                return {Error::InvalidArgument, "Independent property metadata object ranges must not overlap"};
    }
    return {};
}
void reject(FieldProbeReport& report, Meter& meter, std::uint32_t offset, const std::string& identity,
    Error code, const std::string& message) {
    meter.charge(1, sizeof(CandidateRejection)); meter.charge(identity.size()); meter.charge(message.size());
    report.rejected.push_back({offset, code, identity, message});
}
// Retained raw observations require the same metadata bytes, including the
// inline FName representation, even when the decoded name remains equal.
template<class Sample, class Observe> Status scan(std::span<const Sample> samples, std::span<const std::string> outputs,
    std::uint32_t prefix, std::uint32_t extent, std::uint32_t width, std::uint32_t alignment,
    std::span<const Evidence> proofs, const Evidence& inherited, ReadBudget& budget, Snapshot& snapshot, bool rejectMappingCandidates, Observe observe) {
    Meter meter{budget};
    for (const auto& field : outputs) { auto& report = snapshot.fieldReports[field]; report = {}; report.generation = budget.generation; }
    using Bytes = std::vector<std::byte>;
    std::vector<std::map<std::uint32_t, std::vector<Bytes>>> observations(outputs.size());
    for (std::size_t column = 0; column < outputs.size(); ++column) {
        auto& report = snapshot.fieldReports.at(outputs[column]);
        for (std::uint32_t offset = (prefix + alignment - 1) / alignment * alignment; offset <= extent - width; offset += alignment) {
            meter.check(); ++report.examinedOffsets;
            std::vector<Bytes> raw; bool matched = true;
            for (const auto& sample : samples) {
                Bytes bytes;
                if (auto status = observe(sample, column, offset, bytes); !status) {
                    if (status.code != Error::InvalidEvidence && (!rejectMappingCandidates || (status.code != Error::Unmapped && status.code != Error::Overflow))) return status;
                    reject(report, meter, offset, sample.identity, status.code, status.message); matched = false; break;
                }
                raw.push_back(std::move(bytes));
            }
            if (matched) {
                meter.charge(1, sizeof(Offset)); meter.proof(proofs[column]); meter.proof(inherited); meter.charge(1, sizeof(std::uintptr_t));
                meter.charge(raw.size(), sizeof(Bytes)); for (const auto& bytes : raw) meter.charge(bytes.size());
                auto proof = proofs[column]; proof.relativeAddresses = {offset};
                Offset candidate; candidate.value = offset; candidate.evidence.push_back(std::move(proof)); candidate.evidence.push_back(inherited);
                report.candidates.push_back(std::move(candidate)); observations[column].emplace(offset, std::move(raw));
            }
        }
    }
    for (std::size_t column = 0; column < outputs.size(); ++column) {
        auto& report = snapshot.fieldReports.at(outputs[column]);
        for (auto& candidate : report.candidates) {
            for (std::size_t i = 0; i < samples.size(); ++i) {
                Bytes raw; const auto status = observe(samples[i], column, *candidate.value, raw);
                if (!status) return status;
                if (raw != observations[column].at(*candidate.value)[i]) {
                    reject(report, meter, *candidate.value, samples[i].identity, Error::InvalidEvidence, "Property metadata bytes changed before publication");
                    report.candidates.clear(); return {Error::InvalidEvidence, "Property metadata changed before publication"};
                }
            }
            auto proof = proofs[column]; proof.check = "complete property metadata final readback matches every declaration";
            proof.relativeAddresses = {*candidate.value}; meter.proof(proof); candidate.evidence.push_back(std::move(proof));
        }
        if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& report = snapshot.fieldReports.at(outputs[i]);
        if (report.candidates.size() != 1) return {Error::InvalidEvidence, "Each property metadata field requires one unambiguous candidate"};
        const auto offset = *report.candidates.front().value;
        for (std::size_t j = 0; j < i; ++j) {
            const auto other = *snapshot.fieldReports.at(outputs[j]).candidates.front().value;
            if (offset < other + width && other < offset + width)
                return {Error::InvalidEvidence, "Distinct property metadata fields require disjoint byte ranges"};
        }
    }
    return {};
}
Evidence proof(const std::string& field, const std::string& identity, std::uint32_t prefix,
    Meter& input, std::size_t samples) {
    Evidence result{"property metadata matches independent declarations", true, samples, {}, {}, {}};
    result.source = "property-value-profile:" + identity + ";field:" + field + ";occupied-property-prefix:" +
        std::to_string(prefix);
    input.text(result.source); return result;
}
Evidence inheritedProof(const detail::PropertyContext& context, const Snapshot& snapshot, Meter& input) {
    Evidence result{"all inherited property fields match their complete observation identity", true,
        detail::propertyDependencies.size(), {}, context.source, {}};
    input.text(result.source);
    for (const auto& field : detail::propertyDependencies) {
        result.relativeAddresses.push_back(*snapshot.offsets.at(field).value);
        result.sampleIdentities.push_back(field);
    }
    input.proof(result); return result;
}
std::array<std::uint8_t, 4> values(const BoolPropertySample& sample) {
    return {sample.fieldSize, sample.byteOffset, sample.byteMask, sample.fieldMask};
}
}
Status probeBoolProperty(MemoryReader& reader, const BoolPropertyProfile& profile, std::span<const BoolPropertySample> samples,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    try {
        if (!detail::propertyTextValid(profile.identity) || samples.size() < 3 || samples.size() > 16)
            return {Error::InvalidArgument, "Bool metadata requires a named profile and 3-16 anchors"};
        for (const auto& sample : samples) if (sample.encoding != BoolEncoding::NativeByte && sample.encoding != BoolEncoding::SingleBit)
            return {Error::Unsupported, "Bool metadata requires an explicit NativeByte or SingleBit encoding"};
        detail::PropertyContext context;
        if (auto status = detail::preparePropertyContext(reader, profile.property, profile.propertyBaseExtent, profile.extent,
            1, names, pool, poolProfile, boolFields, budget, snapshot, context); !status) return status;
        Meter input{budget};
        if (auto status = validateSamples(samples, profile.extent, input); !status) return status;
        const auto inherited = inheritedProof(context, snapshot, input);
        std::array<Evidence, 4> proofs;
        for (std::size_t i = 0; i < proofs.size(); ++i) proofs[i] = proof(boolFields[i], profile.identity, profile.propertyBaseExtent, input, samples.size());
        std::map<std::string, std::uint32_t> storageExtents;
        for (const auto& sample : samples) {
            input.text(sample.storageIdentity);
            if (!sample.storageExtent || sample.storageExtent > 255 || !sample.fieldSize || sample.fieldSize > sample.storageExtent || sample.byteOffset >= sample.fieldSize ||
                (sample.encoding == BoolEncoding::NativeByte && values(sample) != std::array<std::uint8_t, 4>{1, 0, 1, 255}) ||
                (sample.encoding == BoolEncoding::SingleBit && (!std::has_single_bit(sample.byteMask) || sample.fieldMask != sample.byteMask)))
                return {Error::InvalidArgument, "Bool declarations require bounded storage and the exact declared byte encoding"};
            const auto [storage, inserted] = storageExtents.emplace(sample.storageIdentity, sample.storageExtent);
            if (!inserted && storage->second != sample.storageExtent)
                return {Error::InvalidEvidence, "A Bool storage identity denotes contradictory extents"};
            auto identity = sample.identity + ";storage:" + sample.storageIdentity + ";extent:" + std::to_string(sample.storageExtent) +
                ";encoding:" + (sample.encoding == BoolEncoding::NativeByte ? "NativeByte" : "SingleBit");
            for (auto value : values(sample)) identity += ";byte:" + std::to_string(value);
            input.text(identity);
            for (auto& value : proofs) { input.charge(identity.size()); value.sampleIdentities.push_back(identity); }
        }
        auto observe = [&](const BoolPropertySample& sample, std::size_t column, std::uint32_t offset, std::vector<std::byte>& raw) -> Status {
            raw.resize(1);
            if (auto status = readExact(reader, sample.object + offset, raw, budget); !status) return status;
            if (std::to_integer<std::uint8_t>(raw.front()) != values(sample)[column])
                return {Error::InvalidEvidence, "Bool byte differs from independent metadata"};
            return {};
        };
        if (auto status = scan(samples, boolFields, profile.propertyBaseExtent, profile.extent, 1, 1, proofs, inherited, budget, snapshot, false, observe); !status) return status;
        return detail::publishPropertyContext(reader, profile.property, context, boolFields, budget, snapshot);
    } catch (const Failure& error) {
        try { return {error.code, error.message}; } catch (...) { return {error.code, {}}; }
    } catch (...) { return {Error::Internal, {}}; }
}
Status probeFieldPathProperty(MemoryReader& reader, const FieldPathPropertyProfile& profile, std::span<const FieldPathPropertySample> samples,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    try {
        if (profile.representation != FieldPathRepresentation::InlineFName)
            return {Error::Unsupported, "FieldPath metadata requires an explicitly declared inline FName representation"};
        if (!detail::propertyTextValid(profile.identity) || samples.size() < 3 || samples.size() > 16)
            return {Error::InvalidArgument, "FieldPath metadata requires a named profile and 3-16 anchors"};
        detail::PropertyContext context;
        if (auto status = detail::preparePropertyContext(reader, profile.property, profile.propertyBaseExtent, profile.extent,
            names.size, names, pool, poolProfile, pathFields, budget, snapshot, context); !status) return status;
        Meter input{budget};
        if (auto status = validateSamples(samples, profile.extent, input); !status) return status;
        const auto inherited = inheritedProof(context, snapshot, input);
        std::array<Evidence, 1> proofs{proof(pathFields[0], profile.identity, profile.propertyBaseExtent, input, samples.size())};
        proofs[0].source += ";representation:InlineFName"; input.text(proofs[0].source);
        std::set<std::string> distinct;
        for (const auto& sample : samples) {
            input.text(sample.expectedName); distinct.insert(sample.expectedName);
            auto identity = sample.identity + ";expected-name:" + sample.expectedName; input.text(identity);
            proofs[0].sampleIdentities.push_back(std::move(identity));
        }
        if (distinct.size() < 2) return {Error::InvalidEvidence, "FieldPath metadata requires two distinct full names"};
        auto observe = [&](const FieldPathPropertySample& sample, std::size_t, std::uint32_t offset, std::vector<std::byte>& raw) -> Status {
            raw.resize(names.size);
            if (auto status = readExact(reader, sample.object + offset, raw, budget); !status) return status;
            std::string name;
            if (auto status = readFName(reader, sample.object + offset, names, pool, poolProfile, budget, name); !status) return status;
            if (name != sample.expectedName) return {Error::InvalidEvidence, "FieldPath full name differs from independent metadata"};
            std::vector<std::byte> after(names.size);
            if (auto status = readExact(reader, sample.object + offset, after, budget); !status) return status;
            if (after != raw) return {Error::InvalidEvidence, "FieldPath bytes changed during name decoding"};
            return {};
        };
        if (auto status = scan(samples, pathFields, profile.propertyBaseExtent, profile.extent, names.size, 4, proofs, inherited, budget, snapshot, true, observe); !status) return status;
        return detail::publishPropertyContext(reader, profile.property, context, pathFields, budget, snapshot);
    } catch (const Failure& error) {
        try { return {error.code, error.message}; } catch (...) { return {error.code, {}}; }
    } catch (...) { return {Error::Internal, {}}; }
}
}
