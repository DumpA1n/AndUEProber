#include "OwnedProperties.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
int checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::array<std::string, 4> fields{"FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal"};
constexpr std::array<std::uint32_t, 4> offsets{offsetof(OwnedNativeProperty, arrayDim), offsetof(OwnedNativeProperty, elementSize),
    offsetof(OwnedNativeProperty, propertyFlags), offsetof(OwnedNativeProperty, offsetInternal)};
Status probe(OwnedProperties& fixture, const PropertyProbeProfile& profile, std::span<const PropertySample> samples,
    ReadBudget& budget, Snapshot& snapshot) {
    const auto& names = fixture.phase5.nameMemory();
    return probePropertyFields(fixture, profile, samples, names.nameLayout, names.address(64), names.pool, budget, snapshot);
}
Status observe(OwnedProperties& fixture, Snapshot& snapshot) {
    const auto samples = fixture.samples(); ReadBudget budget; budget.generation = fixture.epoch;
    return probe(fixture, fixture.profile(), samples, budget, snapshot);
}
void requireOffsets(const Snapshot& snapshot) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& offset = snapshot.offsets.at(fields[i]); const auto& report = snapshot.fieldReports.at(fields[i]);
        REQUIRE(offset.value == offsets[i]);
        REQUIRE(offset.validation == Validation::Validated && offset.evidence.size() >= 2 && offset.dependencies.size() == 14);
        for (const auto& [dependency, version] : offset.dependencies) REQUIRE(snapshot.offsets.at(dependency).version == version);
        REQUIRE(offset.evidence.front().source.find("owned-compiled-property-metadata-v1") != std::string::npos);
        REQUIRE(offset.evidence.front().sampleIdentities.size() == 3);
        REQUIRE(offset.evidence.front().sampleIdentities[0].find("containing-value:owned-containing-value:0") != std::string::npos);
        REQUIRE(report.candidates.size() == 1 && report.generation == snapshot.generation && report.examinedOffsets > 0);
        REQUIRE(!report.rejected.empty());
    }
    REQUIRE(snapshot.offsets.at(fields[2]).evidence.front().source.find("scalar-bytes:8") != std::string::npos);
}
void successAndSession() {
    OwnedProperties fixture; const auto initial = fixture.initial(); Session session(initial); Snapshot* working = nullptr;
    const auto samples = fixture.samples();
    REQUIRE(samples[0].expectedOffsetInternal == 0 && samples[0].expectedPropertyFlags == 0);
    REQUIRE(samples[0].containingValueSize != fixture.profile().extent);
    REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
        working = &snapshot; ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probe(fixture, fixture.profile(), samples, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(session.stop()); const auto frozen = session.snapshot();
    if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
    REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen)); requireOffsets(*frozen);
    working->offsets.at(fields[0]).value = 888; REQUIRE(frozen->offsets.at(fields[0]).value == offsets[0]);
    for (const auto& dependency : {"UObject::NamePrivate", "UStruct::PropertiesSize", "FField::Owner", "FField::FlagsPrivate"}) {
        auto changed = *frozen; auto prior = changed.offsets.at(dependency);
        REQUIRE(publishOffset(changed, dependency, std::move(prior)));
        for (const auto& field : fields) REQUIRE(changed.offsets.at(field).validation == Validation::Stale);
        REQUIRE(frozen->offsets.at(fields[0]).validation == Validation::Validated);
    }
    REQUIRE(ownedNativeFunctionCalls == 0);
}
void metadataContracts() {
    for (int mode = 0; mode < 24; ++mode) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples(); auto profile = fixture.profile();
        ReadBudget budget; budget.generation = fixture.epoch; Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.layout = Layout::UProperty; expected = Error::Unsupported; break;
        case 1: samples[0].expectedArrayDim = 0; break;
        case 2: samples[0].expectedArrayDim = -1; break;
        case 3: samples[0].expectedElementSize = 0; break;
        case 4: samples[0].expectedElementSize = -1; break;
        case 5: samples[0].expectedOffsetInternal = -1; break;
        case 6: samples[0].containingValueSize = 0; break;
        case 7: samples[0].containingValueSize = 16 * 1024 * 1024 + 1; break;
        case 8: --samples[0].containingValueSize; expected = Error::InvalidEvidence; break;
        case 9: samples[0].expectedOffsetInternal = 1; expected = Error::InvalidEvidence; break;
        case 10: samples[0].expectedArrayDim = INT32_MAX; samples[0].expectedElementSize = INT32_MAX; expected = Error::Overflow; break;
        case 11: samples[1].object = samples[0].object; break;
        case 12: samples[1].identity = samples[0].identity; break;
        case 13: samples[1].containingValueIdentity = samples[0].containingValueIdentity; expected = Error::InvalidEvidence; break;
        case 14: samples[0].object = std::numeric_limits<std::uintptr_t>::max() - 7; expected = Error::Overflow; break;
        case 15: ++profile.generation; expected = Error::StaleIdentity; break;
        case 16: snapshot.offsets.erase("FField::Owner"); expected = Error::InvalidEvidence; break;
        case 17: snapshot.offsets.at("FField::NamePrivate").validation = Validation::Candidate; expected = Error::InvalidEvidence; break;
        case 18: snapshot.offsets.at("FField::FlagsPrivate").evidence[0].passed = false; expected = Error::InvalidEvidence; break;
        case 19: ++snapshot.offsets.at("FField::Next").dependencies.at("UObject::NamePrivate"); expected = Error::InvalidEvidence; break;
        case 20: samples[0].containingValueIdentity = "\xED\xA0\x80"; break;
        case 21: for (auto& sample : samples) sample.expectedPropertyFlags = 0; expected = Error::InvalidEvidence; break;
        case 22: profile.extent = 4097; break;
        case 23: samples[1].object = samples[0].object + 8; break;
        }
        REQUIRE(probe(fixture, profile, samples, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0); REQUIRE(!snapshot.offsets.contains(fields[0]));
    }
    OwnedProperties fixture; auto snapshot = fixture.initial(); const auto samples = fixture.samples(); ReadBudget budget; budget.generation = fixture.epoch;
    REQUIRE(probe(fixture, fixture.profile(), std::span(samples).first(2), budget, snapshot).code == Error::InvalidArgument);
    const std::vector<PropertySample> oversized(17, samples[0]);
    REQUIRE(probe(fixture, fixture.profile(), oversized, budget, snapshot).code == Error::InvalidArgument);
    REQUIRE(fixture.reads == 0);
}
void inheritedLayoutContracts() {
    for (int mode = 0; mode < 12; ++mode) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); const auto samples = fixture.samples();
        auto profile = fixture.profile(); const auto& source = fixture.phase5.nameMemory();
        auto names = source.nameLayout; auto pool = source.address(64); auto poolProfile = source.pool;
        Error expected = Error::InvalidEvidence;
        switch (mode) {
        case 0: profile.fieldBaseExtent = 0; expected = Error::InvalidArgument; break;
        case 1: profile.fieldBaseExtent = profile.extent + 1; expected = Error::InvalidArgument; break;
        case 2: profile.fieldBaseExtent = offsetof(OwnedNativeFField, flags) + sizeof(std::uint32_t) - 1; break;
        case 3: profile.fieldBaseProfileIdentity = "another-field-profile"; break;
        case 4: names = {0, std::nullopt, std::nullopt, 4}; break;
        case 5: profile.ownerLayout.size = 24; break;
        case 6: profile.ownerLayout.pointerOffset = 8; profile.ownerLayout.kindOffset = 0; break;
        case 7: profile.ownerLayout.representation = FieldOwnerRepresentation::Tagged; expected = Error::Unsupported; break;
        case 8: ++pool; break;
        case 9: --poolProfile.maximumUnits; break;
        case 10: snapshot.offsets.at("FField::FlagsPrivate").value = snapshot.offsets.at("FField::Next").value; break;
        case 11: for (auto& item : snapshot.offsets.at("FField::Owner").evidence) item.source = "unbound-owner-proof"; break;
        }
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probePropertyFields(fixture, profile, samples, names, pool, poolProfile, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial(); const auto samples = fixture.samples();
        auto profile = fixture.profile(); profile.fieldBaseExtent = profile.extent;
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probe(fixture, profile, samples, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(fixture.reads == 0);
        for (const auto& field : fields) REQUIRE(snapshot.fieldReports.at(field).candidates.empty());
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial();
        for (std::size_t i = 0; i < fixture.properties.size(); ++i) {
            fixture.properties[i].field.name.index = fixture.properties[i].arrayDim;
            fixture.properties[i].arrayDim = -1;
        }
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(fields[0]);
        REQUIRE(report.candidates.empty());
        for (const auto& rejected : report.rejected) REQUIRE(rejected.offset >= fixture.profile().fieldBaseExtent);
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial();
        auto prior = fixture.phase5.profile(); auto bounded = prior; bounded.extent = sizeof(OwnedNativeFField) + 64;
        const auto& source = fixture.phase5.nameMemory();
        REQUIRE(fieldObservationIdentity(prior, source.nameLayout, source.address(64), source.pool) ==
            fieldObservationIdentity(bounded, source.nameLayout, source.address(64), source.pool));
        REQUIRE(observe(fixture, snapshot));
        REQUIRE(snapshot.offsets.at(fields[0]).value == sizeof(OwnedNativeFField));
    }
}
void failuresAndMutation() {
    for (auto failure : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Cancelled,
        Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        const auto samples = fixture.samples(); ReadBudget budget; budget.generation = fixture.epoch; std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied || failure == Error::Unmapped) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto, auto) { ++memory.epoch; };
        REQUIRE(probe(fixture, fixture.profile(), samples, budget, snapshot).code == failure);
        for (const auto& field : fields) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
        REQUIRE(!snapshot.fieldReports.contains(fields[0]) || snapshot.fieldReports.at(fields[0]).candidates.empty()); REQUIRE(!validateSnapshot(snapshot));
    }
    for (std::size_t selected = 0; selected < fields.size(); ++selected) {
        OwnedProperties fixture; auto snapshot = fixture.initial();
        for (auto& object : fixture.properties) {
            if (selected == 0) object.duplicateArrayDim = object.arrayDim;
            if (selected == 1) object.duplicateElementSize = object.elementSize;
            if (selected == 2) object.duplicatePropertyFlags = object.propertyFlags;
            if (selected == 3) object.duplicateOffsetInternal = object.offsetInternal;
        }
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields[selected]).candidates.size() == 2);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    for (std::size_t selected = 0; selected < fields.size(); ++selected) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); std::size_t visits = 0;
        const auto address = reinterpret_cast<std::uintptr_t>(&fixture.properties[0]) + offsets[selected]; const std::size_t width = selected == 2 ? 8 : 4;
        fixture.beforeRead = [&](auto&, auto current, auto size) { if (current == address && size == width) ++visits; };
        REQUIRE(observe(fixture, snapshot)); REQUIRE(visits >= 2); const auto mutationVisit = visits; visits = 0;
        fixture.beforeRead = [&](auto& memory, auto current, auto size) {
            if (current != address || size != width || ++visits != mutationVisit) return;
            if (selected == 0) memory.properties[0].arrayDim = -1;
            if (selected == 1) memory.properties[0].elementSize = -1;
            if (selected == 2) memory.properties[0].propertyFlags ^= 0x100000000ULL;
            if (selected == 3) memory.properties[0].offsetInternal = -1;
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence); const auto& report = snapshot.fieldReports.at(fields[selected]);
        REQUIRE(report.candidates.empty()); REQUIRE(report.rejected.back().reason.find("changed") != std::string::npos);
        for (const auto& field : fields) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        for (std::size_t i = 0; i < samples.size(); ++i) { samples[i].expectedElementSize = samples[i].expectedArrayDim; fixture.properties[i].elementSize = 77; }
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probe(fixture, fixture.profile(), samples, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields[0]).rejected.back().reason.find("overlap") != std::string::npos);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
}
void overridesAndDependencies() {
    for (const auto& field : fields) for (int mode = 0; mode < 9; ++mode) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto selected = snapshot.offsets.at(field); selected.origin = Origin::User;
        switch (mode) {
        case 0: break;
        case 1: selected.value = 999; break;
        case 2: selected.validation = Validation::Candidate; break;
        case 3: selected.validation = Validation::Stale; break;
        case 4: selected.evidence.clear(); break;
        case 5: selected.evidence[0].passed = false; break;
        case 6: selected.dependencies.erase("FField::Owner"); break;
        case 7: ++selected.dependencies.at("UObject::NamePrivate"); break;
        case 8: selected.dependencies["missing-user-dependency"] = 1; break;
        }
        REQUIRE(publishOffset(snapshot, field, selected)); const auto retained = snapshot.offsets.at(field);
        REQUIRE(observe(fixture, snapshot).code == (mode == 0 ? Error::None : Error::InvalidEvidence)); const auto& result = snapshot.offsets.at(field);
        REQUIRE(result.value == retained.value && result.version == retained.version && result.origin == retained.origin);
        REQUIRE(result.validation == retained.validation && result.dependencies == retained.dependencies);
    }
    for (const auto* dependency : {"UObject::NamePrivate", "FField::Owner", "FField::FlagsPrivate"}) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (changed) return;
            changed = true; auto prior = snapshot.offsets.at(dependency); REQUIRE(publishOffset(snapshot, dependency, std::move(prior)));
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
}
void evidenceClosureContracts() {
    const auto chain = [](Snapshot& snapshot, std::size_t count) {
        Offset node; node.value = 0; node.version = 1; node.validation = Validation::Validated;
        node.evidence.push_back({"independently declared fixture dependency", true, 1, {0}, "owned-metadata-declaration", {"owned-anchor"}});
        for (std::size_t i = 0; i < count; ++i) {
            auto item = node;
            if (i + 1 < count) item.dependencies["owned-closure:" + std::to_string(i + 1)] = 1;
            snapshot.offsets["owned-closure:" + std::to_string(i)] = std::move(item);
        }
        snapshot.offsets.at("UObject::InternalIndex").dependencies["owned-closure:0"] = 1;
    };
    for (int mode = 0; mode < 7; ++mode) {
        OwnedProperties fixture; auto snapshot = fixture.initial(); chain(snapshot, mode == 6 ? 4100 : 4);
        auto& deepest = snapshot.offsets.at("owned-closure:3");
        switch (mode) {
        case 0: deepest.validation = Validation::Candidate; break;
        case 1: deepest.validation = Validation::Stale; break;
        case 2: deepest.evidence.clear(); break;
        case 3: deepest.dependencies["owned-closure:1"] = 1; break;
        case 4: deepest.dependencies["missing-deep-proof"] = 1; break;
        case 5: ++snapshot.offsets.at("owned-closure:2").dependencies.at("owned-closure:3"); break;
        case 6: break;
        }
        REQUIRE(observe(fixture, snapshot).code == (mode == 6 ? Error::BudgetExceeded : Error::InvalidEvidence));
        REQUIRE(fixture.reads == 0);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial(); chain(snapshot, 4);
        bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (!changed) { changed = true; snapshot.offsets.at("owned-closure:3").validation = Validation::Candidate; }
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial(); chain(snapshot, 4);
        Offset unrelated; unrelated.validation = Validation::Stale;
        snapshot.offsets["unrelated-stale-result"] = unrelated;
        REQUIRE(observe(fixture, snapshot));
        for (const auto& field : fields) REQUIRE(snapshot.offsets.at(field).validation == Validation::Validated);
        REQUIRE(snapshot.offsets.at("unrelated-stale-result").validation == Validation::Stale);
    }
    {
        OwnedProperties fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto& first = snapshot.offsets.at(fields[0]); auto& second = snapshot.offsets.at(fields[1]);
        first.origin = Origin::User; second.origin = Origin::User;
        first.dependencies[fields[1]] = second.version; second.dependencies[fields[0]] = first.version;
        const auto firstVersion = first.version, secondVersion = second.version;
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.offsets.at(fields[0]).version == firstVersion);
        REQUIRE(snapshot.offsets.at(fields[1]).version == secondVersion);
    }
}
void activeCancellation() {
    OwnedProperties fixture; const auto initial = fixture.initial(); const auto samples = fixture.samples();
    std::atomic<bool> entered{false}, resume{false};
    fixture.beforeRead = [&](auto&, auto, auto) { if (!entered.exchange(true)) while (!resume) std::this_thread::yield(); };
    Session session(initial); const auto retained = session.snapshot();
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probe(fixture, fixture.profile(), samples, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield(); REQUIRE(entered);
    session.cancel(); resume = true; REQUIRE(session.stop()); const auto result = session.snapshot();
    REQUIRE(result->state == TaskState::Cancelled && result->result.code == Error::Cancelled);
    for (const auto& field : fields) REQUIRE(!result->offsets.contains(field));
    REQUIRE(retained->state == TaskState::Pending && retained->offsets.size() == initial.offsets.size());
}
}
int main() {
    successAndSession(); metadataContracts(); inheritedLayoutContracts(); failuresAndMutation(); overridesAndDependencies(); evidenceClosureContracts(); activeCancellation();
    REQUIRE(ownedNativeFunctionCalls == 0);
    std::printf("PASS: %d FProperty scalar checks; independent containing-value bounds, four typed fields, validated FField dependencies and immutable publication; containers and enum metadata not exercised\n", checks);
}
