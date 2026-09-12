#include "OwnedFunctions.hpp"
#include <andueprober/Reflection.hpp>
#include <cstdio>
#include <cstdlib>

using namespace andueprober;
namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
ReflectionSchema functionSchema() {
    ReflectionSchema schema;
    schema.identity = "owned-function-layout-subset";
    ReflectionRecordSpec record{1, "FunctionMetadata", sizeof(OwnedNativeFunction), alignof(OwnedNativeFunction),
        "owned compiled sizeof and alignof", {}};
    for (const auto& [name, scalar] : std::array<std::pair<const char*, ReflectionScalar>, 5>{{
        {"FunctionFlags", ReflectionScalar::UInt32}, {"NumParms", ReflectionScalar::UInt8},
        {"ParmsSize", ReflectionScalar::UInt16}, {"ReturnValueOffset", ReflectionScalar::UInt16},
        {"Func", ReflectionScalar::Address64}}})
        record.fields.push_back({name, {ReflectionTypeKind::Scalar, scalar, 0, 1}, std::string("UFunction::") + name});
    schema.records.push_back(std::move(record));
    return schema;
}
Snapshot observed() {
    OwnedFunctions fixture;
    auto initial = fixture.initial();
    const auto samples = fixture.samples();
    ReadBudget budget; budget.generation = fixture.epoch;
    CHECK(probeFunctionFields(fixture, fixture.profile(), samples, budget, initial));
    CHECK(validateSnapshot(initial));
    return initial;
}
void compiledOffset(Snapshot& snapshot, const std::string& name, std::uint32_t value) {
    Offset offset;
    offset.value = value; offset.origin = Origin::Profile; offset.validation = Validation::Validated;
    offset.evidence.push_back({"independent compiled field layout", true, 2, {value}, "owned native metadata", {"first", "second"}});
    CHECK(publishOffset(snapshot, name, std::move(offset)));
}
void rejected(const Snapshot& snapshot, const ReflectionSchema& schema, Error error, const ReflectionLimits& limits = {}) {
    const auto result = freezeReflection(snapshot, schema, limits);
    CHECK(result.status.code == error && !result.snapshot);
}
void observationsAndOwnership() {
    auto snapshot = observed(); auto schema = functionSchema();
    const auto frozen = freezeReflection(snapshot, schema);
    if (!frozen.status) std::fprintf(stderr, "%s\n", frozen.status.message.c_str());
    CHECK(frozen.status && frozen.snapshot);
    CHECK(frozen.snapshot->records().size() == 1 && frozen.snapshot->records()[0].fields.size() == 5);
    CHECK(frozen.snapshot->records()[0].size == sizeof(OwnedNativeFunction));
    CHECK(frozen.snapshot->records()[0].alignment == alignof(OwnedNativeFunction));
    CHECK(frozen.snapshot->analysis().fieldReports.empty() && frozen.snapshot->analysis().messages.empty());
    for (const auto& field : frozen.snapshot->records()[0].fields) {
        CHECK(field.offset == snapshot.offsets.at(field.offsetSource).value);
        CHECK(field.offsetVersion == snapshot.offsets.at(field.offsetSource).version);
    }
    const auto original = frozen.snapshot->records()[0].fields[0].offset;
    snapshot.offsets.at("UFunction::FunctionFlags").value = 1;
    schema.records[0].name = "Mutated"; schema.records[0].size = 1;
    CHECK(frozen.snapshot->records()[0].name == "FunctionMetadata" && frozen.snapshot->records()[0].fields[0].offset == original);
    CHECK(frozen.snapshot->analysis().offsets.at("UFunction::FunctionFlags").value == original);
    auto changed = frozen.snapshot->analysis();
    auto dependency = changed.offsets.at("UStruct::PropertiesSize");
    CHECK(publishOffset(changed, "UStruct::PropertiesSize", std::move(dependency)));
    rejected(changed, functionSchema(), Error::InvalidEvidence);
    CHECK(validateSnapshot(frozen.snapshot->analysis()));
    CHECK(ownedNativeFunctionCalls.load() == 0);
}
void typedLayouts() {
    auto snapshot = observed();
    compiledOffset(snapshot, "Compiled::Zero", 0);
    compiledOffset(snapshot, "Compiled::One", 1);
    compiledOffset(snapshot, "Compiled::Eight", 8);
    for (const auto scalar : {ReflectionScalar::UInt8, ReflectionScalar::Int8, ReflectionScalar::UInt16,
        ReflectionScalar::Int16, ReflectionScalar::UInt32, ReflectionScalar::Int32, ReflectionScalar::UInt64,
        ReflectionScalar::Int64, ReflectionScalar::Float32, ReflectionScalar::Float64, ReflectionScalar::Address64}) {
        ReflectionSchema schema{"scalar-layout", {{1, "ScalarRecord", 8, 8, "owned scalar storage", {
            {"value", {ReflectionTypeKind::Scalar, scalar, 0, 1}, "Compiled::Zero"}}}}, {}};
        const auto result = freezeReflection(snapshot, schema);
        CHECK(result.status && result.snapshot->records()[0].fields[0].offset == 0);
    }
    ReflectionSchema nested{"nested-layout", {
        {1, "Pair", 16, 8, "owned pair declaration", {{"values", {ReflectionTypeKind::Record, ReflectionScalar::UInt8, 2, 2}, "Compiled::Zero"}}},
        {2, "Word", 8, 8, "owned word declaration", {{"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt64, 0, 1}, "Compiled::Zero"}}}}, {}};
    auto result = freezeReflection(snapshot, nested);
    CHECK(result.status && result.snapshot->records()[0].id == 2 && result.snapshot->records()[1].id == 1);
    CHECK(result.snapshot->records()[1].fields[0].type.count == 2);
    auto cycle = nested;
    cycle.records[0].size = 8; cycle.records[0].fields[0].type.count = 1;
    cycle.records[1].fields[0].type = {ReflectionTypeKind::Record, ReflectionScalar::UInt8, 1, 1};
    rejected(snapshot, cycle, Error::InvalidEvidence);
    nested.records[0].fields[0].type.reference = 1; nested.records[0].fields[0].type.count = 1;
    rejected(snapshot, nested, Error::InvalidEvidence);
    for (const auto scalar : {ReflectionScalar::UInt8, ReflectionScalar::Int8, ReflectionScalar::UInt16,
        ReflectionScalar::Int16, ReflectionScalar::UInt32, ReflectionScalar::Int32, ReflectionScalar::UInt64, ReflectionScalar::Int64}) {
        ReflectionSchema schema{"enum-layout", {{1, "EnumRecord", 8, 8, "owned enum storage", {
            {"value", {ReflectionTypeKind::Enumeration, ReflectionScalar::UInt8, 2, 1}, "Compiled::Zero"}}}}, {
            {2, "EnumType", scalar, "owned enum declaration", {{"Zero", 0}, {"One", 1}}}}};
        CHECK(freezeReflection(snapshot, schema).status);
    }
    ReflectionSchema signedEnum{"signed-enum-layout", {}, {{1, "SignedByte", ReflectionScalar::Int8, "owned signed bits", {
        {"Minimum", 0x80}, {"NegativeOne", 0xff}, {"Maximum", 0x7f}}}}};
    CHECK(freezeReflection(snapshot, signedEnum).status);
    signedEnum.enumerations[0].values[0].bits = 0x100;
    rejected(snapshot, signedEnum, Error::InvalidEvidence);
    signedEnum.enumerations[0].underlying = ReflectionScalar::Int64;
    signedEnum.enumerations[0].values[0].bits = 0x8000000000000000ULL;
    signedEnum.enumerations[0].values[1].bits = UINT64_MAX;
    CHECK(freezeReflection(snapshot, signedEnum).status);
}
void invalidInputs() {
    const auto snapshot = observed(); const auto original = functionSchema();
    for (unsigned mode = 0; mode < 18; ++mode) {
        auto schema = original; auto error = Error::InvalidArgument;
        auto& record = schema.records[0];
        switch (mode) {
        case 0: schema.identity.clear(); break;
        case 1: record.id = 0; break;
        case 2: record.name = "class"; break;
        case 3: record.name = "Text*/\n#error injected"; break;
        case 4: record.name = "_Reserved"; break;
        case 5: record.name = "Nested__Reserved"; break;
        case 6: record.metadataSource = std::string("\xc0\xaf", 2); break;
        case 7: record.alignment = 3; break;
        case 8: record.size = 0; break;
        case 9: record.fields[1].name = record.fields[0].name; break;
        case 10: record.fields[0].type.reference = 1; break;
        case 11: record.fields[0].type.kind = ReflectionTypeKind::Record; record.fields[0].type.reference = 900; break;
        case 12: record.fields[0].offsetSource = "Missing"; error = Error::InvalidEvidence; break;
        case 13: record.fields[1].offsetSource = record.fields[0].offsetSource; error = Error::InvalidEvidence; break;
        case 14: record.fields[0].type.scalar = static_cast<ReflectionScalar>(99); error = Error::Unsupported; break;
        case 15: record.fields[0].type.kind = static_cast<ReflectionTypeKind>(99); error = Error::Unsupported; break;
        case 16: record.fields[0].type.count = 0; break;
        case 17: schema.records.push_back(record); break;
        }
        rejected(snapshot, schema, error);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto schema = original;
        schema.enumerations.push_back({2, "EnumType", ReflectionScalar::UInt8, "owned enum", {{"First", 0}, {"Second", 1}}});
        auto error = Error::InvalidArgument;
        switch (mode) {
        case 0: schema.enumerations[0].id = 1; break;
        case 1: schema.enumerations[0].name = schema.records[0].name; break;
        case 2: schema.enumerations[0].values.clear(); break;
        case 3: schema.enumerations[0].values[1].name = "First"; break;
        case 4: schema.enumerations[0].underlying = ReflectionScalar::Float32; error = Error::Unsupported; break;
        case 5: schema.enumerations[0].values[1].bits = 256; error = Error::InvalidEvidence; break;
        }
        rejected(snapshot, schema, error);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto invalid = snapshot; auto& offset = invalid.offsets.at("UFunction::FunctionFlags");
        switch (mode) {
        case 0: offset.validation = Validation::Candidate; break;
        case 1: offset.dependencies.begin()->second++; break;
        case 2: offset.evidence.front().sampleIdentities.pop_back(); break;
        case 3: offset.evidence.front().passed = false; break;
        case 4: invalid.result = {Error::Io, "owned failed analysis"}; break;
        case 5: offset.value = UINT32_MAX; break;
        }
        rejected(invalid, original, Error::InvalidEvidence);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        ReflectionLimits limits;
        switch (mode) {
        case 0: limits.maximumFields = 4; break;
        case 1: limits.maximumMetadataBytes = 1; break;
        case 2: limits.maximumTypeBytes = 1; break;
        case 3: limits.maximumTypes = 0; break;
        case 4: limits.maximumArrayElements = 0; break;
        case 5: limits.maximumEnumValues = 0; break;
        }
        rejected(snapshot, original, mode >= 3 ? Error::InvalidArgument : Error::BudgetExceeded, limits);
    }
    std::atomic<bool> cancelled{true}; ReflectionLimits limits; limits.cancelled = &cancelled;
    rejected(snapshot, original, Error::Cancelled, limits);
    limits = {}; limits.deadline = std::chrono::steady_clock::time_point::min();
    rejected(snapshot, original, Error::DeadlineExceeded, limits);
}
}
int main() {
    observationsAndOwnership(); typedLayouts(); invalidInputs();
    std::printf("PASS: %u reflection checks; actual function observations, immutable data-layout subset, declared size provenance and bounded typed schemas\n", checks);
}
