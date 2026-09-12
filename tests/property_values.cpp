#include "OwnedPropertyValues.hpp"
#include <cstdio>
#include <cstdlib>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::array<std::string, 4> boolFields{"FBoolProperty::FieldSize", "FBoolProperty::ByteOffset", "FBoolProperty::ByteMask", "FBoolProperty::FieldMask"};
const std::array<std::string, 1> pathFields{"FFieldPathProperty::PropertyClass"};
Status observe(OwnedPropertyValues& fixture, bool path, ReadBudget& budget, Snapshot& snapshot) {
    const auto& source = fixture.phase5.phase5.nameMemory();
    return path ? probeFieldPathProperty(fixture, fixture.pathProfile(), fixture.pathSamples(), source.nameLayout, source.address(64), source.pool, budget, snapshot) :
        probeBoolProperty(fixture, fixture.boolProfile(), fixture.boolSamples(), source.nameLayout, source.address(64), source.pool, budget, snapshot);
}
Status observe(OwnedPropertyValues& fixture, bool path, Snapshot& snapshot) { ReadBudget budget; budget.generation = 1; return observe(fixture, path, budget, snapshot); }
void success() {
    OwnedPropertyValues fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, false, snapshot)); REQUIRE(observe(fixture, true, snapshot)); REQUIRE(validateSnapshot(snapshot));
    for (std::size_t i = 0; i < boolFields.size(); ++i) {
        const auto& result = snapshot.offsets.at(boolFields[i]); REQUIRE(result.value == fixture.boolOffsets[i]);
        REQUIRE(result.dependencies.size() == 9 && result.evidence.size() == 3); REQUIRE(result.validation == Validation::Validated);
        REQUIRE(result.evidence.front().sampleIdentities.size() == 3);
    }
    REQUIRE(snapshot.offsets.at(pathFields[0]).value == offsetof(OwnedPathMetadata, propertyClass));
    REQUIRE(snapshot.offsets.at(pathFields[0]).dependencies.size() == 9); REQUIRE(ownedNativeFunctionCalls == 0);
    auto changed = snapshot; auto prior = changed.offsets.at("FProperty::ArrayDim"); REQUIRE(publishOffset(changed, "FProperty::ArrayDim", std::move(prior)));
    for (const auto& field : boolFields) REQUIRE(changed.offsets.at(field).validation == Validation::Stale);
    REQUIRE(changed.offsets.at(pathFields[0]).validation == Validation::Stale);
}
void metadata() {
    for (unsigned mode = 0; mode < 22; ++mode) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); auto profile = fixture.boolProfile(); auto samples = fixture.boolSamples();
        ReadBudget budget; budget.generation = 1; Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: samples[0].encoding = BoolEncoding::Unknown; expected = Error::Unsupported; break;
        case 1: samples[0].fieldSize = 2; break;
        case 2: samples[1].byteMask = 3; break;
        case 3: samples[1].fieldMask = 2; break;
        case 4: samples[1].byteOffset = samples[1].fieldSize; break;
        case 5: samples[1].storageExtent = 256; break;
        case 6: samples[1].storageExtent = 0; break;
        case 7: samples[1].fieldSize = 0; break;
        case 8: samples[1].storageIdentity.clear(); break;
        case 9: samples[1].identity = samples[0].identity; break;
        case 10: samples[1].object = samples[0].object + 1; break;
        case 11: samples[0].object = UINTPTR_MAX - 2; expected = Error::Overflow; break;
        case 12: profile.identity.clear(); break;
        case 13: profile.extent = 4097; break;
        case 14: profile.propertyBaseExtent = offsetof(OwnedNativeProperty, offsetInternal); expected = Error::InvalidEvidence; break;
        case 15: profile.property.identity += "changed"; expected = Error::InvalidEvidence; break;
        case 16: profile.property.fieldBaseProfileIdentity += "changed"; expected = Error::InvalidEvidence; break;
        case 17: ++profile.property.generation; expected = Error::StaleIdentity; break;
        case 18: profile.property.layout = Layout::UProperty; expected = Error::Unsupported; break;
        case 19: profile.property.ownerLayout.representation = FieldOwnerRepresentation::Unknown; expected = Error::Unsupported; break;
        case 20: samples[2].storageIdentity = std::string("bad\0text", 8); break;
        case 21: samples[2].storageIdentity = samples[1].storageIdentity; expected = Error::InvalidEvidence; break;
        }
        const auto& source = fixture.phase5.phase5.nameMemory();
        REQUIRE(probeBoolProperty(fixture, profile, samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0); REQUIRE(!snapshot.offsets.contains(boolFields[0]));
    }
    for (unsigned mode = 0; mode < 9; ++mode) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); auto profile = fixture.pathProfile(); auto samples = fixture.pathSamples();
        auto& source = fixture.phase5.phase5.nameMemory(); auto names = source.nameLayout; auto pool = source.address(64); auto poolProfile = source.pool;
        ReadBudget budget; budget.generation = 1; Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.representation = FieldPathRepresentation::Unknown; expected = Error::Unsupported; break;
        case 1: names.size = 0; break;
        case 2: ++pool; expected = Error::InvalidEvidence; break;
        case 3: --poolProfile.maximumUnits; expected = Error::InvalidEvidence; break;
        case 4: samples[0].expectedName = "\xC0\xAF"; break;
        case 5: samples[0].expectedName.clear(); break;
        case 6: for (auto& value : samples) value.expectedName = "same"; expected = Error::InvalidEvidence; break;
        case 7: profile.propertyBaseExtent = profile.extent; break;
        case 8: snapshot.offsets.at("FProperty::PropertyFlags").value = snapshot.offsets.at("FProperty::ArrayDim").value; expected = Error::InvalidEvidence; break;
        }
        REQUIRE(probeFieldPathProperty(fixture, profile, samples, names, pool, poolProfile, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0); REQUIRE(!snapshot.offsets.contains(pathFields[0]));
    }
    for (bool path : {false, true}) for (std::size_t count : {2u, 17u}) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); const auto& source = fixture.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1;
        if (path) {
            const std::vector<FieldPathPropertySample> samples(count, fixture.pathSamples()[0]);
            REQUIRE(probeFieldPathProperty(fixture, fixture.pathProfile(), samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == Error::InvalidArgument);
        } else {
            const std::vector<BoolPropertySample> samples(count, fixture.boolSamples()[0]);
            REQUIRE(probeBoolProperty(fixture, fixture.boolProfile(), samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == Error::InvalidArgument);
        }
        REQUIRE(fixture.reads == 0);
    }
}
void ambiguity() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); auto samples = fixture.boolSamples();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (mode < 2) {
                const auto values = mode == 0 ? fixture.expected[0] : fixture.expected[i ? i : 1];
                fixture.setBool(i, values); samples[i].fieldSize = values[0]; samples[i].byteOffset = values[1]; samples[i].byteMask = values[2]; samples[i].fieldMask = values[3];
                samples[i].storageExtent = values[0]; samples[i].encoding = mode == 0 ? BoolEncoding::NativeByte : BoolEncoding::SingleBit;
            } else fixture.booleans[i].duplicate[mode - 2] = mode == 2 ? samples[i].fieldSize : samples[i].fieldMask;
        }
        const auto& source = fixture.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1;
        REQUIRE(probeBoolProperty(fixture, fixture.boolProfile(), samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == Error::InvalidEvidence);
        for (const auto& field : boolFields) REQUIRE(!snapshot.offsets.contains(field));
    }
    OwnedPropertyValues fixture; auto snapshot = fixture.initial(); for (auto& value : fixture.paths) value.duplicate = value.propertyClass;
    REQUIRE(observe(fixture, true, snapshot).code == Error::InvalidEvidence); REQUIRE(snapshot.fieldReports.at(pathFields[0]).candidates.size() == 2);
}
void failures() {
    for (bool path : {false, true}) for (auto code : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Unsupported, Error::BudgetExceeded,
        Error::Cancelled, Error::DeadlineExceeded, Error::StaleIdentity, Error::Internal}) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, path, snapshot));
        ReadBudget budget; budget.generation = 1; std::atomic<bool> cancelled{false};
        if (code == Error::PermissionDenied || code == Error::Unsupported || code == Error::Unmapped) fixture.failure = code;
        if (code == Error::ShortRead) fixture.shortRead = true;
        if (code == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (code == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (code == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now() - 1s;
        if (code == Error::StaleIdentity) fixture.beforeRead = [](auto& value, auto, auto) { ++value.epoch; };
        if (code == Error::Internal) fixture.beforeRead = [](auto&, auto, auto) { throw 1; };
        const auto status = observe(fixture, path, budget, snapshot);
        REQUIRE(status.code == (path && code == Error::Unmapped ? Error::InvalidEvidence : code));
        for (const auto& field : path ? std::span<const std::string>(pathFields) : std::span<const std::string>(boolFields)) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
    }
}
void lateChanges() {
    for (bool path : {false, true}) for (unsigned mode = 0; mode < 5; ++mode) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); ReadBudget budget; budget.generation = 1; std::atomic<bool> cancelled{false}; budget.cancelled = &cancelled;
        unsigned seen = 0; bool reached = false;
        const auto address = path ? reinterpret_cast<std::uintptr_t>(&fixture.paths[0].propertyClass) : reinterpret_cast<std::uintptr_t>(&fixture.booleans[0].fieldSize);
        fixture.beforeRead = [&](auto& current, auto at, auto size) {
            if (at != address || size != (path ? sizeof(OwnedFieldName) : 1) || ++seen != (path ? 3u : 2u)) return;
            reached = true;
            if (mode == 0) { if (path) ++current.paths[0].propertyClass.number; else ++current.booleans[0].fieldSize; }
            if (mode == 1) cancelled = true;
            if (mode == 2) ++current.epoch;
            if (mode == 3) ++snapshot.offsets.at("FProperty::ArrayDim").version;
            if (mode == 4) current.phase5.phase5.nameMemory().entry(OwnedPropertyValues::nameIds[0], "Changed");
        };
        const auto status = observe(fixture, path, budget, snapshot); REQUIRE(reached);
        const auto expected = mode == 1 ? Error::Cancelled : mode == 2 ? Error::StaleIdentity : Error::InvalidEvidence;
        if (mode == 4 && !path) REQUIRE(status); else REQUIRE(status.code == expected);
    }
}
void evidenceAndOverrides() {
    for (bool path : {false, true}) for (unsigned mode = 0; mode < 13; ++mode) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, path, snapshot));
        const auto field = path ? pathFields[0] : boolFields[0]; auto& value = snapshot.offsets.at(field); value.origin = Origin::User;
        const auto original = value;
        if (mode == 1) ++*value.value;
        if (mode == 2) value.validation = Validation::Stale;
        if (mode == 3) value.evidence.clear();
        if (mode == 4) value.dependencies.clear();
        if (mode == 5) value.dependencies["FProperty::ArrayDim"] = 99;
        if (mode == 6) value.dependencies[field] = value.version;
        if (mode == 7) snapshot.offsets.at("FProperty::ArrayDim").dependencies["missing"] = 1;
        if (mode == 8) snapshot.offsets.at("UObject::InternalIndex").validation = Validation::Stale;
        if (mode == 9) snapshot.offsets.at("UObject::InternalIndex").dependencies["FProperty::ArrayDim"] = snapshot.offsets.at("FProperty::ArrayDim").version;
        if (mode == 10) snapshot.offsets.at("UObject::InternalIndex").evidence.clear();
        if (mode == 11) { Offset unrelated; unrelated.value = 0; unrelated.validation = Validation::Stale; snapshot.offsets["unrelated"] = unrelated; }
        if (mode == 12) for (auto& proof : snapshot.offsets.at("FProperty::ArrayDim").evidence) proof.source = "wrong-property-source";
        const auto before = value; const auto status = observe(fixture, path, snapshot);
        REQUIRE(static_cast<bool>(status) == (mode == 0 || mode == 11));
        const auto& after = snapshot.offsets.at(field);
        REQUIRE(after.value == before.value && after.version == original.version && after.origin == Origin::User);
        if (status) REQUIRE(after.validation == before.validation);
    }
}
void boundaryAndClosure() {
    for (bool path : {false, true}) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); const auto& source = fixture.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1;
        if (path) {
            auto profile = fixture.pathProfile(); profile.identity = std::string(700, 'P');
            REQUIRE(probeFieldPathProperty(fixture, profile, fixture.pathSamples(), source.nameLayout, source.address(64), source.pool, budget, snapshot));
        } else {
            auto profile = fixture.boolProfile(); profile.identity = std::string(700, 'B');
            REQUIRE(probeBoolProperty(fixture, profile, fixture.boolSamples(), source.nameLayout, source.address(64), source.pool, budget, snapshot));
        }
        const auto& proofs = snapshot.offsets.at(path ? pathFields[0] : boolFields[0]).evidence;
        REQUIRE(proofs.size() == 3); REQUIRE(proofs[0].source.size() > 700 && proofs[0].source.size() <= 1024);
        REQUIRE(proofs[1].source == propertyObservationIdentity(fixture.phase5.profile(), source.nameLayout, source.address(64), source.pool));
    }

    for (bool path : {false, true}) {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial();
        for (unsigned i = 0; i < 4100; ++i) {
            Offset value; value.value = 0; value.version = 1; value.validation = Validation::Validated;
            value.evidence.push_back({"owned named ancestor", true, 1, {0}, "owned", {"anchor"}});
            if (i + 1 < 4100) value.dependencies["deep-property:" + std::to_string(i + 1)] = 1;
            snapshot.offsets["deep-property:" + std::to_string(i)] = std::move(value);
        }
        snapshot.offsets.at("UObject::InternalIndex").dependencies["deep-property:0"] = 1;
        REQUIRE(observe(fixture, path, snapshot).code == Error::BudgetExceeded); REQUIRE(fixture.reads == 0);
    }
    {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, false, snapshot));
        auto& first = snapshot.offsets.at(boolFields[0]); auto& second = snapshot.offsets.at(boolFields[1]);
        first.origin = second.origin = Origin::User; first.dependencies[boolFields[1]] = second.version; second.dependencies[boolFields[0]] = first.version;
        REQUIRE(observe(fixture, false, snapshot).code == Error::InvalidEvidence);
    }
    {
        OwnedPropertyValues fixture; auto snapshot = fixture.initial(); auto samples = fixture.boolSamples();
        samples[2].fieldSize = 255; samples[2].byteOffset = 254; samples[2].storageExtent = 255;
        fixture.setBool(2, {255, 254, 128, 128}); const auto& source = fixture.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1;
        REQUIRE(probeBoolProperty(fixture, fixture.boolProfile(), samples, source.nameLayout, source.address(64), source.pool, budget, snapshot));
        REQUIRE(snapshot.offsets.at(boolFields[0]).value == fixture.boolOffsets[0]);
    }
}
void reportBudget() {
    struct Record { OwnedNativeProperty property; std::array<std::uint8_t, 3904> bytes; };
    static_assert(sizeof(Record) <= 4096);
    struct Memory final : MemoryReader {
        OwnedPropertyValues predecessor; std::array<Record, 16> records; std::size_t reads = 0;
        Memory() {
            for (std::size_t i = 0; i < records.size(); ++i) {
                records[i].property = predecessor.phase5.properties[i % 3];
                for (std::size_t j = 0; j < records[i].bytes.size(); ++j) records[i].bytes[j] = predecessor.expected[i % 3][j % 4];
            }
        }
        ReadResult read(std::uintptr_t address, std::span<std::byte> output) override {
            ++reads;
            for (const auto& record : records) {
                const auto begin = reinterpret_cast<std::uintptr_t>(&record);
                if (address < begin || address - begin > sizeof(record) || output.size() > sizeof(record) - (address - begin)) continue;
                std::memcpy(output.data(), reinterpret_cast<const std::byte*>(&record) + address - begin, output.size()); return {output.size(), Error::None};
            }
            return predecessor.read(address, output);
        }
        std::uint64_t generation() const override { return 1; }
    } fixture;
    auto snapshot = fixture.predecessor.initial(); auto profile = fixture.predecessor.boolProfile(); profile.extent = sizeof(Record);
    const auto sourceSamples = fixture.predecessor.boolSamples(); std::vector<BoolPropertySample> samples;
    for (std::size_t i = 0; i < fixture.records.size(); ++i) {
        auto sample = sourceSamples[i % 3]; sample.object = reinterpret_cast<std::uintptr_t>(&fixture.records[i]);
        sample.identity = "repeated:" + std::to_string(i) + std::string(800, 'A'); samples.push_back(std::move(sample));
    }
    const auto& source = fixture.predecessor.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1; budget.remainingBytes = 1024 * 1024;
    REQUIRE(probeBoolProperty(fixture, profile, samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == Error::BudgetExceeded);
    REQUIRE(fixture.reads > 1000 && budget.remainingBytes > 0); REQUIRE(!snapshot.fieldReports.at(boolFields[0]).candidates.empty());
    for (const auto& field : boolFields) REQUIRE(!snapshot.offsets.contains(field));
}

}
int main() { success(); metadata(); ambiguity(); failures(); lateChanges(); evidenceAndOverrides(); boundaryAndClosure(); reportBudget(); std::printf("PASS: %u owned Bool/FieldPath checks; independent metadata and bounded reads, no engine execution\n", checks); }
