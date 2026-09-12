#include "andueprober/Functions.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::array<std::string, 5> fields{"UFunction::FunctionFlags", "UFunction::NumParms", "UFunction::ParmsSize",
    "UFunction::ReturnValueOffset", "UFunction::Func"};
constexpr std::array<std::uint32_t, 5> widths{sizeof(std::uint32_t), sizeof(std::uint8_t), sizeof(std::uint16_t),
    sizeof(std::uint16_t), sizeof(std::uintptr_t)};
bool nameValid(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos && validateUtf8(value);
}
template<class T>
Status at(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset, ReadBudget& budget, T& value) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - object)
        return {Error::Overflow, "Function field address overflow"};
    return readExact(reader, object + offset, std::as_writable_bytes(std::span(&value, 1)), budget);
}
Status samplesValid(const FunctionProbeProfile& profile, std::span<const FunctionSample> samples) {
    if (samples.size() < 3 || samples.size() > 16)
        return {Error::InvalidArgument, "Function probing requires 3-16 independent named anchors"};
    std::set<std::uintptr_t> objects, functions;
    std::set<std::string> identities;
    std::array<std::set<std::uint64_t>, 4> scalars;
    std::map<std::string, std::uintptr_t> functionIdentities;
    for (const auto& sample : samples) {
        if (!sample.object || !nameValid(sample.identity) || !nameValid(sample.nativeFunctionIdentity) ||
            !objects.insert(sample.object).second || !identities.insert(sample.identity).second)
            return {Error::InvalidArgument, "Function anchors require distinct objects and bounded metadata identities"};
        if (profile.extent > std::numeric_limits<std::uintptr_t>::max() - sample.object)
            return {Error::Overflow, "Function sample extent overflows the address space"};
        if (sample.expectedReturnOffset != 0xffff && sample.expectedReturnOffset >= sample.expectedParmsSize)
            return {Error::InvalidEvidence, "A declared return offset must be inside the parameter extent or denote no return value"};
        const auto [target, inserted] = functionIdentities.emplace(sample.nativeFunctionIdentity, sample.expectedNativeFunction);
        if (!inserted && target->second != sample.expectedNativeFunction)
            return {Error::InvalidEvidence, "A native-function identity denotes contradictory addresses"};
        scalars[0].insert(sample.expectedFlags); scalars[1].insert(sample.expectedNumParms);
        scalars[2].insert(sample.expectedParmsSize); scalars[3].insert(sample.expectedReturnOffset);
        if (sample.expectedNativeFunction) functions.insert(sample.expectedNativeFunction);
    }
    if (functions.size() < 2 || std::any_of(scalars.begin(), scalars.end(), [](const auto& values) { return values.size() < 2; }))
        return {Error::InvalidEvidence, "Function anchors require distinct scalar metadata and two distinct non-null native addresses"};
    return {};
}
template<class T, class Expected>
Status scalar(MemoryReader& reader, std::span<const FunctionSample> samples, Expected expected, std::uint32_t extent,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    for (std::uint32_t offset = 0; offset <= extent - sizeof(T); offset += sizeof(T)) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, offset, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({offset, Error::InvalidEvidence, sample.identity,
                    "Function scalar differs from independently declared metadata"});
                matched = false; break;
            }
        }
        if (matched) {
            Offset candidate; candidate.value = offset;
            Evidence evidence{"scalar matches independent named function metadata", true, samples.size(), {offset}};
            evidence.source = provenance + ";scalar-bytes:" + std::to_string(sizeof(T));
            for (const auto& sample : samples)
                evidence.sampleIdentities.push_back(sample.identity + ";declared-scalar:" + std::to_string(expected(sample)));
            candidate.evidence.push_back(std::move(evidence));
            report.candidates.push_back(std::move(candidate));
        }
    }
    if (report.candidates.size() == 1) report.candidates.front().validation = Validation::Validated;
    return {};
}
template<class T, class Expected>
Status recheck(MemoryReader& reader, std::span<const FunctionSample> samples, Expected expected,
    const std::string& provenance, ReadBudget& budget, FieldProbeReport& report) {
    for (auto& candidate : report.candidates) {
        for (const auto& sample : samples) {
            T observed{};
            if (auto status = at(reader, sample.object, *candidate.value, budget, observed); !status) return status;
            if (observed != expected(sample)) {
                report.rejected.push_back({*candidate.value, Error::InvalidEvidence, sample.identity,
                    "Function observation changed before phase publication"});
                report.candidates.clear();
                return {Error::InvalidEvidence, "Function observations changed before phase publication"};
            }
        }
        Evidence evidence{"complete function phase final recheck matches declared anchors", true, samples.size(), {*candidate.value}};
        evidence.source = provenance;
        for (const auto& sample : samples) evidence.sampleIdentities.push_back(sample.identity);
        candidate.evidence.push_back(std::move(evidence));
    }
    return {};
}
Status retainedOverride(const Offset& retained, const std::map<std::string, std::uint64_t>& upstream) {
    for (const auto& [dependency, version] : upstream) {
        const auto prior = retained.dependencies.find(dependency);
        if (prior == retained.dependencies.end() || prior->second != version)
            return {Error::InvalidEvidence, "A matching function override requires current Phase 1 and Phase 2 dependency versions"};
    }

    return {};
}
}

Status probeFunctionFields(MemoryReader& reader, const FunctionProbeProfile& profile,
    std::span<const FunctionSample> samples, ReadBudget& budget, Snapshot& snapshot) {
    if (sizeof(std::uintptr_t) != sizeof(std::uint64_t))
        return {Error::Unsupported, "Function probing requires an explicit 64-bit pointer layout"};
    if (!nameValid(profile.identity) || !nameValid(profile.moduleIdentity) || !profile.generation ||
        (profile.layout != Layout::UProperty && profile.layout != Layout::FField) ||
        profile.extent < sizeof(std::uintptr_t) || profile.extent > 4096)
        return {Error::InvalidArgument, "Function probing requires explicit bounded layout and module metadata"};
    if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
        profile.layout != snapshot.layout || budget.generation != snapshot.generation || reader.generation() != snapshot.generation)
        return {Error::StaleIdentity, "Function profile, reader and snapshot identities do not match"};
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
    const auto provenance = "function-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
        ";layout:" + (profile.layout == Layout::FField ? "FField" : "UProperty") +
        ";memory-generation:" + std::to_string(profile.generation);
    std::vector<PointerSample> pointers;
    for (const auto& sample : samples)
        pointers.push_back({sample.object, sample.expectedNativeFunction, sample.identity, sample.nativeFunctionIdentity});
    const auto flags = [](const auto& sample) { return sample.expectedFlags; };
    const auto count = [](const auto& sample) { return sample.expectedNumParms; };
    const auto size = [](const auto& sample) { return sample.expectedParmsSize; };
    const auto result = [](const auto& sample) { return sample.expectedReturnOffset; };
    const auto function = [](const auto& sample) { return sample.expectedNativeFunction; };
    std::array<FieldProbeReport, 5> reports;
    const auto retain = [&] {
        for (std::size_t i = 0; i < fields.size(); ++i)
            if (reports[i].generation) snapshot.fieldReports[fields[i]] = reports[i];
    };
    auto status = scalar<std::uint32_t>(reader, samples, flags, profile.extent, provenance, budget, reports[0]);
    if (status) status = scalar<std::uint8_t>(reader, samples, count, profile.extent, provenance, budget, reports[1]);
    if (status) status = scalar<std::uint16_t>(reader, samples, size, profile.extent, provenance, budget, reports[2]);
    if (status) status = scalar<std::uint16_t>(reader, samples, result, profile.extent, provenance, budget, reports[3]);
    if (status) status = probePointerField(reader, pointers, profile.extent, provenance, budget, reports[4]);
    if (status) status = recheck<std::uint32_t>(reader, samples, flags, provenance, budget, reports[0]);
    if (status) status = recheck<std::uint8_t>(reader, samples, count, provenance, budget, reports[1]);
    if (status) status = recheck<std::uint16_t>(reader, samples, size, provenance, budget, reports[2]);
    if (status) status = recheck<std::uint16_t>(reader, samples, result, provenance, budget, reports[3]);
    if (status) status = recheck<std::uintptr_t>(reader, samples, function, provenance, budget, reports[4]);
    retain();
    if (!status) return status;
    for (const auto& [name, version] : upstream) {
        const auto current = snapshot.offsets.find(name);
        if (current == snapshot.offsets.end() || current->second.version != version || current->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "Upstream evidence changed during function probing"};
    }
    for (const auto& report : reports)
        if (report.candidates.size() != 1)
            return {Error::InvalidEvidence, "Function probing requires one unambiguous observation per field"};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto begin = *reports[i].candidates.front().value;
        for (std::size_t j = i + 1; j < fields.size(); ++j) {
            const auto other = *reports[j].candidates.front().value;
            if (begin < other + widths[j] && other < begin + widths[i]) {
                reports[i].rejected.push_back({begin, Error::InvalidEvidence, fields[j], "Function field candidates overlap"});
                retain();
                return {Error::InvalidEvidence, "Distinct function fields require non-overlapping byte ranges"};
            }
        }
    }
    if (auto status = validateEvidenceClosure(snapshot, dependencies, evidenceLimits); !status) return status;
    auto published = snapshot;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto existing = snapshot.offsets.find(fields[i]);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) {
            if (existing->second.value != reports[i].candidates.front().value)
                return {Error::InvalidEvidence, "A function observation contradicts an explicit user override"};
            if (auto result = retainedOverride(existing->second, upstream); !result) return result;
        } else if (auto result = publishFieldProbe(published, fields[i], reports[i], dependencies); !result) return result;
    }
    if (auto status = validateEvidenceClosure(published, fields, evidenceLimits); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Function phase cancelled before publication"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Function phase deadline exceeded before publication"};
    if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
        snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
        return {Error::StaleIdentity, "Function identity changed before publication"};
    snapshot = std::move(published);
    return {};
}
}
