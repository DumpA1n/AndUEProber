#include "andueprober/Enums.hpp"
#include "andueprober/Evidence.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <set>

namespace andueprober {
namespace {
const std::string output = "UEnum::Names";
const std::array<std::string, 5> dependencies{"UObject::InternalIndex", "UObject::NamePrivate",
    "UObject::ClassPrivate", "UObject::OuterPrivate", "UField::Next"};
constexpr std::size_t maximumMetadata = 4 * 1024 * 1024;
struct Failure { Error code; const char* message; };
[[noreturn]] void fail(Error code, const char* message) { throw Failure{code, message}; }
bool textValid(const std::string& text) {
    return !text.empty() && text.size() <= 1024 && text.find('\0') == std::string::npos && validateUtf8(text);
}
struct Meter {
    ReadBudget& budget;
    std::size_t remaining = maximumMetadata;
    void check() const {
        if (budget.cancelled && budget.cancelled->load()) fail(Error::Cancelled, "Enum observation cancelled");
        if (std::chrono::steady_clock::now() >= budget.deadline) fail(Error::DeadlineExceeded, "Enum observation deadline exceeded");
    }
    void charge(std::size_t count, std::size_t width = 1) {
        check();
        if (count > remaining / width) fail(Error::BudgetExceeded, "Enum metadata budget exceeded");
        remaining -= count * width;
    }
    void text(const std::string& text) {
        charge(text.size());
        if (!textValid(text))
            fail(Error::InvalidArgument, "Enum metadata requires nonempty bounded UTF-8 text");
    }
};
struct Range { std::uintptr_t begin, end; };
bool overlaps(const Range& a, const Range& b) { return a.begin < b.end && b.begin < a.end; }
Status range(std::uintptr_t begin, std::uint64_t size, Range& result) {
    if (!begin || !size) return {Error::InvalidEvidence, "Enum storage requires a nonempty address range"};
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin)
        return {Error::Overflow, "Enum storage address overflows"};
    result = {begin, begin + size}; return {};
}
bool disjoint(std::uint32_t a, std::uint32_t as, std::uint32_t b, std::uint32_t bs) {
    return std::uint64_t(a) + as <= b || std::uint64_t(b) + bs <= a;
}
Status layouts(const EnumProbeProfile& profile, const NameLayout& names) {
    const auto& a = profile.array; const auto& e = profile.entry;
    if (!a.data || !a.count || !a.capacity || a.size < 16 || a.size > 64 ||
        *a.data > a.size - 8 || *a.count > a.size - 4 || *a.capacity > a.size - 4 ||
        *a.data % 8 || *a.count % 4 || *a.capacity % 4 ||
        !disjoint(*a.data, 8, *a.count, 4) || !disjoint(*a.data, 8, *a.capacity, 4) || !disjoint(*a.count, 4, *a.capacity, 4))
        return {Error::InvalidArgument, "Enum array layout requires disjoint bounded pointer, count and capacity fields"};
    if (!e.name || !e.value || e.stride < names.size || e.stride < 8 || e.stride > 128 ||
        *e.name > e.stride - names.size || *e.value > e.stride - 8 || *e.name % 4 || *e.value % 8 ||
        !disjoint(*e.name, names.size, *e.value, 8))
        return {Error::InvalidArgument, "Enum entry layout requires disjoint bounded FName and int64 fields"};
    if (!profile.fieldBaseExtent || profile.fieldBaseExtent > profile.extent || profile.extent < a.size || profile.extent > 4096 || profile.fieldBaseExtent > profile.extent - a.size ||
        !profile.maximumValues || profile.maximumValues > 16384 || profile.maximumCapacity < profile.maximumValues || profile.maximumCapacity > 1048576)
        return {Error::InvalidArgument, "Enum probing requires explicit bounded prefix, extent and array limits"};
    return {};
}
struct Header {
    std::uintptr_t data = 0;
    std::int32_t count = 0, capacity = 0;
    bool operator==(const Header&) const = default;
};
struct ArrayObservation { Header header; std::vector<std::byte> bytes; };
struct Candidate { std::uint32_t offset; std::vector<ArrayObservation> arrays; };
Status readHeader(MemoryReader& reader, std::uintptr_t object, std::uint32_t offset,
    const EnumProbeProfile& profile, ReadBudget& budget, Header& result) {
    Range address;
    if (auto status = range(object, std::uint64_t(offset) + profile.array.size, address); !status) return status;
    std::array<std::byte, 64> bytes;
    if (auto status = readExact(reader, object + offset, std::span(bytes).first(profile.array.size), budget); !status) return status;
    std::memcpy(&result.data, bytes.data() + *profile.array.data, 8);
    std::memcpy(&result.count, bytes.data() + *profile.array.count, 4);
    std::memcpy(&result.capacity, bytes.data() + *profile.array.capacity, 4);
    return {};
}
bool candidateFailure(Error code) {
    return code == Error::InvalidEvidence || code == Error::Unmapped || code == Error::Overflow;
}
Status entries(MemoryReader& reader, const EnumProbeProfile& profile, const EnumSample& sample,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile,
    const ArrayObservation& observation, ReadBudget& budget) {
    for (std::size_t i = 0; i < sample.values.size(); ++i) {
        const auto& expected = sample.values[i];
        const auto start = observation.header.data + i * profile.entry.stride;
        std::string name;
        if (auto status = readFName(reader, start + *profile.entry.name, names, pool, poolProfile, budget, name); !status) return status;
        std::int64_t value = 0;
        std::memcpy(&value, observation.bytes.data() + i * profile.entry.stride + *profile.entry.value, sizeof(value));
        if (name != expected.expectedName || value != expected.expectedValue)
            return {Error::InvalidEvidence, "Enum entry differs from the independent full name or signed value"};
    }
    return {};
}
std::string entryIdentity(const EnumSample& sample, const EnumValueSample& value) {
    return sample.identity + "/" + value.identity + ";name:" + value.expectedName + ";value:" + std::to_string(value.expectedValue);
}
void proofCharge(Meter& meter, const Evidence& evidence) {
    meter.charge(1, sizeof(Evidence)); meter.charge(evidence.check.size()); meter.charge(evidence.source.size());
    meter.charge(evidence.relativeAddresses.size(), sizeof(std::uintptr_t));
    meter.charge(evidence.sampleIdentities.size(), sizeof(std::string));
    for (const auto& name : evidence.sampleIdentities) meter.charge(name.size());
}
}

Status probeEnumNames(MemoryReader& reader, const EnumProbeProfile& profile, std::span<const EnumSample> samples,
    const NameLayout& names, std::uintptr_t pool, const NamePoolProfile& poolProfile, ReadBudget& budget, Snapshot& snapshot) {
    try {
        if (sizeof(std::uintptr_t) != 8 || std::endian::native != std::endian::little ||
            (profile.layout != Layout::UProperty && profile.layout != Layout::FField))
            return {Error::Unsupported, "Enum probing supports only explicitly selected little-endian 64-bit reflection layouts"};
        if (auto status = validateNameLayout(names); !status) return status;
        if (auto status = validateNamePoolProfile(poolProfile); !status) return status;
        if (auto status = layouts(profile, names); !status) return status;
        if (!textValid(profile.identity) || !textValid(profile.moduleIdentity) || !textValid(poolProfile.identity))
            return {Error::InvalidArgument, "Enum profiles require nonempty bounded UTF-8 identities"};
        if (!pool || !profile.generation || samples.size() < 3 || samples.size() > 16)
            return {Error::InvalidArgument, "Enum probing requires a canonical pool and 3-16 independent objects"};
        if (profile.moduleIdentity != snapshot.moduleIdentity || profile.generation != snapshot.generation ||
            profile.layout != snapshot.layout || budget.generation != profile.generation || reader.generation() != profile.generation)
            return {Error::StaleIdentity, "Enum profile, reader and snapshot identities do not match"};
        const auto existing = snapshot.offsets.find(output);
        if (existing != snapshot.offsets.end() && existing->second.origin == Origin::User) snapshot.fieldReports.erase(output);
        else if (auto status = beginFieldProbe(snapshot, output); !status) return status;
        Meter input{budget}; input.text(profile.identity); input.text(profile.moduleIdentity); input.text(poolProfile.identity);
        EvidenceLimits limits; limits.deadline = budget.deadline; limits.cancelled = budget.cancelled;
        if (auto status = validateEvidenceClosure(snapshot, dependencies, limits); !status) return status;
        std::map<std::string, std::uint64_t> versions;
        for (const auto& name : dependencies) versions[name] = snapshot.offsets.at(name).version;
        const auto nameIdentity = nameObservationIdentity(names, poolProfile, pool, profile.generation);
        const auto& nameEvidence = snapshot.offsets.at("UObject::NamePrivate").evidence;
        if (std::none_of(nameEvidence.begin(), nameEvidence.end(), [&](const auto& item) { return item.source == nameIdentity; }))
            return {Error::InvalidEvidence, "Enum names require the preceding canonical name-pool observation"};
        const std::array<std::uint32_t, 5> widths{4, names.size, 8, 8, 8};
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            const auto start = *snapshot.offsets.at(dependencies[i]).value;
            if (widths[i] > profile.fieldBaseExtent || start > profile.fieldBaseExtent - widths[i])
                return {Error::InvalidEvidence, "A validated UField range exceeds the declared reserved prefix"};
            for (std::size_t j = 0; j < i; ++j)
                if (!disjoint(start, widths[i], *snapshot.offsets.at(dependencies[j]).value, widths[j]))
                    return {Error::InvalidEvidence, "Validated UField ranges must be disjoint"};
        }
        input.charge(samples.size(), sizeof(EnumSample));
        std::set<std::string> objects, identities;
        std::vector<Range> objectsRange;
        std::size_t totalValues = 0;
        Evidence proof{"all enum entries match independent complete names and signed values", true, 0, {}, {}, {}};
        proof.source = "enum-profile:" + profile.identity + ";module:" + profile.moduleIdentity +
            ";generation:" + std::to_string(profile.generation) + ";prefix:" + std::to_string(profile.fieldBaseExtent) +
            ";array:" + std::to_string(*profile.array.data) + "," + std::to_string(*profile.array.count) + "," +
            std::to_string(*profile.array.capacity) + "," + std::to_string(profile.array.size) + ";entry:" +
            std::to_string(*profile.entry.name) + "," + std::to_string(*profile.entry.value) + "," +
            std::to_string(profile.entry.stride) + ";" + nameIdentity;
        input.text(proof.source);
        for (const auto& sample : samples) {
            input.text(sample.identity);
            if (!sample.object || !objects.insert(sample.identity).second || sample.values.empty() || sample.values.size() > profile.maximumValues)
                return {Error::InvalidArgument, "Enum anchors require distinct identities and bounded nonempty declared arrays"};
            Range objectRange;
            if (auto status = range(sample.object, profile.extent, objectRange); !status) return status;
            if (std::any_of(objectsRange.begin(), objectsRange.end(), [&](const auto& prior) { return overlaps(objectRange, prior); }))
                return {Error::InvalidArgument, "Independent enum object ranges must not overlap"};
            objectsRange.push_back(objectRange);
            if (sample.values.size() > 16384 - totalValues) return {Error::BudgetExceeded, "Enum declared entry count exceeds its budget"};
            totalValues += sample.values.size(); input.charge(sample.values.size(), sizeof(EnumValueSample));
            std::set<std::string> fullNames;
            for (const auto& value : sample.values) {
                input.text(value.identity); input.text(value.expectedName);
                if (!identities.insert(value.identity).second || !fullNames.insert(value.expectedName).second)
                    return {Error::InvalidArgument, "Enum entry identities and names within each enum must be distinct"};
                auto identity = entryIdentity(sample, value); input.text(identity); proof.sampleIdentities.push_back(std::move(identity));
            }
        }
        proof.samples = totalValues;
        Meter reportBudget{budget}; proofCharge(reportBudget, proof);
        auto& report = snapshot.fieldReports[output]; report = {}; report.generation = budget.generation;
        const auto reject = [&](std::uint32_t offset, const EnumSample& sample, const Status& status) {
            reportBudget.charge(1, sizeof(CandidateRejection)); reportBudget.charge(sample.identity.size()); reportBudget.charge(status.message.size());
            report.rejected.push_back({offset, status.code, sample.identity, status.message});
        };
        std::vector<Candidate> candidates;
        for (std::uint32_t offset = (profile.fieldBaseExtent + 7) / 8 * 8; offset <= profile.extent - profile.array.size; offset += 8) {
            reportBudget.check(); ++report.examinedOffsets;
            Candidate candidate{offset, {}}; std::vector<Range> arrays;
            bool matched = true;
            for (const auto& sample : samples) {
                ArrayObservation observation;
                auto status = readHeader(reader, sample.object, offset, profile, budget, observation.header);
                const auto& header = observation.header;
                if (status && (header.count <= 0 || header.capacity < header.count || std::uint32_t(header.capacity) > profile.maximumCapacity ||
                    std::size_t(header.count) != sample.values.size())) status = {Error::InvalidEvidence, "Enum count or capacity contradicts the declared array"};
                Range storage{};
                if (status) status = range(header.data, std::uint64_t(header.capacity) * profile.entry.stride, storage);
                if (status && (std::any_of(objectsRange.begin(), objectsRange.end(), [&](const auto& item) { return overlaps(storage, item); }) ||
                    std::any_of(arrays.begin(), arrays.end(), [&](const auto& item) { return overlaps(storage, item); })))
                    status = {Error::InvalidEvidence, "Enum array capacity ranges must be disjoint from every object and array"};
                if (status) {
                    const auto bytes = std::size_t(header.count) * profile.entry.stride;
                    reportBudget.charge(1, sizeof(ArrayObservation)); reportBudget.charge(bytes);
                    observation.bytes.resize(bytes);
                    status = readExact(reader, header.data, observation.bytes, budget);
                }
                if (status) status = entries(reader, profile, sample, names, pool, poolProfile, observation, budget);
                if (!status) {
                    if (!candidateFailure(status.code)) return status;
                    reject(offset, sample, status); matched = false; break;
                }
                arrays.push_back(storage); candidate.arrays.push_back(std::move(observation));
            }
            if (matched) {
                reportBudget.charge(1, sizeof(Candidate) + sizeof(Offset)); proofCharge(reportBudget, proof);
                auto evidence = proof; evidence.relativeAddresses = {offset};
                reportBudget.charge(1, sizeof(std::uintptr_t));
                Offset result; result.value = offset; result.evidence.push_back(std::move(evidence));
                report.candidates.push_back(std::move(result)); candidates.push_back(std::move(candidate));
            }
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            for (std::size_t i = 0; i < samples.size(); ++i) {
                ArrayObservation current;
                auto status = readHeader(reader, samples[i].object, candidate.offset, profile, budget, current.header);
                if (status && current.header != candidate.arrays[i].header)
                    status = {Error::InvalidEvidence, "Enum array header changed before publication"};
                if (status) {
                    reportBudget.charge(candidate.arrays[i].bytes.size()); current.bytes.resize(candidate.arrays[i].bytes.size());
                    status = readExact(reader, current.header.data, current.bytes, budget);
                }
                if (status && current.bytes != candidate.arrays[i].bytes)
                    status = {Error::InvalidEvidence, "Enum entry storage changed before publication"};
                if (status) status = entries(reader, profile, samples[i], names, pool, poolProfile, current, budget);
                if (status) status = readExact(reader, current.header.data, current.bytes, budget);
                if (status && current.bytes != candidate.arrays[i].bytes)
                    status = {Error::InvalidEvidence, "Enum entry storage changed during final name validation"};
                Header finalHeader;
                if (status) status = readHeader(reader, samples[i].object, candidate.offset, profile, budget, finalHeader);
                if (status && finalHeader != candidate.arrays[i].header)
                    status = {Error::InvalidEvidence, "Enum array header changed during final entry validation"};
                if (!status) {
                    reject(candidate.offset, samples[i], status); report.candidates.clear(); return status;
                }
            }
            auto evidence = proof; evidence.check = "complete enum headers and entry arrays match the final readback";
            evidence.relativeAddresses = {candidate.offset}; proofCharge(reportBudget, evidence);
            report.candidates[index].evidence.push_back(std::move(evidence));
        }
        if (report.candidates.size() != 1) return {Error::InvalidEvidence, "Enum probing requires one unambiguous complete array observation"};
        report.candidates.front().validation = Validation::Validated;
        if (auto status = validateEvidenceClosure(snapshot, dependencies, limits); !status) return status;
        for (const auto& [name, version] : versions)
            if (snapshot.offsets.at(name).version != version) return {Error::InvalidEvidence, "Enum dependency changed during observation"};
        auto published = snapshot;
        const auto retained = snapshot.offsets.find(output);
        if (retained != snapshot.offsets.end() && retained->second.origin == Origin::User) {
            if (retained->second.value != report.candidates.front().value)
                return {Error::InvalidEvidence, "Enum observation contradicts an explicit user override"};
            for (const auto& [name, version] : versions) {
                const auto prior = retained->second.dependencies.find(name);
                if (prior == retained->second.dependencies.end() || prior->second != version)
                    return {Error::InvalidEvidence, "A matching enum override requires every current prerequisite"};
            }
        } else if (auto status = publishFieldProbe(published, output, report, dependencies); !status) return status;
        const std::array<std::string, 1> outputs{output};
        if (auto status = validateEvidenceClosure(published, outputs, limits); !status) return status;
        reportBudget.check();
        if (reader.generation() != profile.generation || snapshot.generation != profile.generation ||
            snapshot.moduleIdentity != profile.moduleIdentity || snapshot.layout != profile.layout)
            return {Error::StaleIdentity, "Enum identity changed before publication"};
        snapshot = std::move(published); return {};
    } catch (const Failure& error) {
        try { return {error.code, error.message}; } catch (...) { return {error.code, {}}; }
    } catch (...) { return {Error::Internal, {}}; }
}
}
