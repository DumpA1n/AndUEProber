#include "OwnedFields.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
int checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::array<std::string, 5> fieldNames{"FField::NamePrivate", "FField::Owner", "FField::Next", "FField::ClassPrivate", "FField::FlagsPrivate"};
constexpr std::array<std::uint32_t, 5> offsets{offsetof(OwnedNativeFField, name), offsetof(OwnedNativeFField, owner),
    offsetof(OwnedNativeFField, next), offsetof(OwnedNativeFField, klass), offsetof(OwnedNativeFField, flags)};
constexpr std::array<std::uint32_t, 5> reverseOffsets{offsetof(OwnedReverseFField, name), offsetof(OwnedReverseFField, owner),
    offsetof(OwnedReverseFField, next), offsetof(OwnedReverseFField, klass), offsetof(OwnedReverseFField, flags)};
Status observe(OwnedFields& fixture, Snapshot& snapshot, bool reverse = false) {
    const auto samples = fixture.samples(reverse); const auto& pool = fixture.nameMemory();
    ReadBudget budget; budget.generation = fixture.epoch;
    return probeFieldFields(fixture, fixture.profile(reverse), samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot);
}
void requireOffsets(const Snapshot& snapshot, bool reverse = false) {
    for (std::size_t i = 0; i < fieldNames.size(); ++i) {
        const auto& offset = snapshot.offsets.at(fieldNames[i]); const auto& report = snapshot.fieldReports.at(fieldNames[i]);
        REQUIRE(offset.value == (reverse ? reverseOffsets[i] : offsets[i]));
        REQUIRE(offset.validation == Validation::Validated && offset.evidence.size() >= 2 && offset.dependencies.size() == 9);
        for (const auto& [dependency, version] : offset.dependencies) REQUIRE(snapshot.offsets.at(dependency).version == version);
        REQUIRE(!offset.dependencies.contains("UClass::CastFlags") && !offset.dependencies.contains("UFunction::Func"));
        REQUIRE(offset.evidence.back().source.find("owned-compiled-ffield-metadata-v1") != std::string::npos);
        REQUIRE(offset.evidence.back().sampleIdentities.size() == 3);
        REQUIRE(report.candidates.size() == 1 && report.generation == snapshot.generation && report.examinedOffsets > 0);
        REQUIRE(!report.rejected.empty());
    }
}
void successAndSession() {
    OwnedFields fixture; const auto initial = fixture.initial(); Session session(initial); Snapshot* working = nullptr;
    const auto samples = fixture.samples(); const auto& pool = fixture.nameMemory();
    REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
        working = &snapshot; ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probeFieldFields(fixture, fixture.profile(), samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(session.stop()); const auto frozen = session.snapshot();
    if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
    REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen)); requireOffsets(*frozen);
    REQUIRE(frozen->offsets.at(fieldNames[0]).value == 0);
    working->offsets.at(fieldNames[0]).value = 888;
    REQUIRE(frozen->offsets.at(fieldNames[0]).value == 0);
    for (const auto& dependency : {"UObject::NamePrivate", "UStruct::ChildProperties", "UStruct::PropertiesSize"}) {
        auto changed = *frozen; auto prior = changed.offsets.at(dependency);
        REQUIRE(publishOffset(changed, dependency, std::move(prior)));
        for (const auto& field : fieldNames) REQUIRE(changed.offsets.at(field).validation == Validation::Stale);
        REQUIRE(frozen->offsets.at(fieldNames[0]).validation == Validation::Validated);
    }
    OwnedFields reverse; auto snapshot = reverse.initial();
    REQUIRE(observe(reverse, snapshot, true)); requireOffsets(snapshot, true);
    REQUIRE(reverse.profile(true).ownerLayout.pointerOffset == 8 && reverse.profile(true).ownerLayout.kindOffset == 0);
    REQUIRE(ownedNativeFunctionCalls == 0);
}
void metadataContracts() {
    for (int mode = 0; mode < 21; ++mode) {
        OwnedFields fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        auto profile = fixture.profile(); auto names = fixture.nameMemory().nameLayout; auto pool = fixture.nameMemory().pool;
        auto address = fixture.nameMemory().address(64); ReadBudget budget; budget.generation = fixture.epoch;
        Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.ownerLayout.representation = FieldOwnerRepresentation::Unknown; expected = Error::Unsupported; break;
        case 1: profile.ownerLayout.representation = FieldOwnerRepresentation::Tagged; expected = Error::Unsupported; break;
        case 2: profile.layout = Layout::UProperty; expected = Error::Unsupported; break;
        case 3: profile.ownerLayout.kindOffset = 0; break;
        case 4: profile.ownerLayout.pointerOffset = 1; break;
        case 5: profile.ownerLayout.size = 65; break;
        case 6: profile.ownerLayout.kindOffset = 16; break;
        case 7: samples[1].object = samples[0].object; break;
        case 8: samples[1].identity = samples[0].identity; break;
        case 9: samples[0].expectedClass = 0; expected = Error::InvalidEvidence; break;
        case 10: samples[2].expectedNext = samples[0].object; samples[2].nextIdentity = samples[0].identity; expected = Error::InvalidEvidence; break;
        case 11: samples[0].expectedOwner = samples[1].object; samples[0].ownerIdentity = samples[1].identity; samples[0].ownerIsUObject = false; expected = Error::InvalidEvidence; break;
        case 12: samples[1].classIdentity = samples[0].classIdentity; expected = Error::InvalidEvidence; break;
        case 13: samples[0].object = std::numeric_limits<std::uintptr_t>::max() - 7; expected = Error::Overflow; break;
        case 14: ++profile.generation; expected = Error::StaleIdentity; break;
        case 15: pool.identity = "another-name-profile"; expected = Error::InvalidEvidence; break;
        case 16: address += 8; expected = Error::InvalidEvidence; break;
        case 17: snapshot.offsets.at("UStruct::ChildProperties").validation = Validation::Candidate; expected = Error::InvalidEvidence; break;
        case 18: snapshot.offsets.at("UField::Next").evidence[0].passed = false; expected = Error::InvalidEvidence; break;
        case 19: ++snapshot.offsets.at("UStruct::SuperStruct").dependencies.at("UObject::NamePrivate"); expected = Error::InvalidEvidence; break;
        case 20: samples[0].expectedName = "\xED\xA0\x80"; break;
        }
        REQUIRE(probeFieldFields(fixture, profile, samples, names, address, pool, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0); REQUIRE(!snapshot.offsets.contains(fieldNames[0]));
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        const auto& pool = fixture.nameMemory(); ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeFieldFields(fixture, fixture.profile(), std::span(samples).first(2), pool.nameLayout, pool.address(64), pool.pool, budget, snapshot).code == Error::InvalidArgument);
        std::vector<FieldBaseSample> oversized(17, samples[0]);
        REQUIRE(probeFieldFields(fixture, fixture.profile(), oversized, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot).code == Error::InvalidArgument);
        OwnedRelations clone;
        for (std::size_t i = 0; i < samples.size(); ++i) clone.entry(fixture.nameIds[i], fixture.names[i]);
        REQUIRE(probeFieldFields(fixture, fixture.profile(), samples, clone.nameLayout, clone.address(64), clone.pool, budget, snapshot).code == Error::InvalidEvidence);
        ++fixture.epoch; snapshot.generation = fixture.epoch; budget.generation = fixture.epoch;
        REQUIRE(probeFieldFields(fixture, fixture.profile(), samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(fixture.reads == 0);
    }
}
void failuresAndMutation() {
    for (auto failure : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Cancelled,
        Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedFields fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        const auto samples = fixture.samples(); const auto& pool = fixture.nameMemory();
        ReadBudget budget; budget.generation = fixture.epoch; std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied || failure == Error::Unmapped) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto, auto) { ++memory.epoch; };
        REQUIRE(probeFieldFields(fixture, fixture.profile(), samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot).code == failure);
        for (const auto& field : fieldNames) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
        REQUIRE(!validateSnapshot(snapshot));
    }
    for (std::size_t selected = 0; selected < fieldNames.size(); ++selected) {
        OwnedFields fixture; auto snapshot = fixture.initial();
        for (auto& object : fixture.fields) {
            if (selected == 0) object.duplicateName = object.name;
            if (selected == 1) { object.duplicateOwner.pointer = object.owner.pointer; object.duplicateOwner.isUObject = object.owner.isUObject; }
            if (selected == 2) object.duplicateNext = object.next;
            if (selected == 3) object.duplicateClass = object.klass;
            if (selected == 4) object.duplicateFlags = object.flags;
        }
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fieldNames[selected]).candidates.size() == 2);
        for (const auto& field : fieldNames) REQUIRE(!snapshot.offsets.contains(field));
    }
    for (std::size_t selected = 0; selected < fieldNames.size(); ++selected) {
        OwnedFields fixture; auto snapshot = fixture.initial(); std::size_t visits = 0;
        const auto address = reinterpret_cast<std::uintptr_t>(&fixture.fields[0]) + offsets[selected];
        const std::size_t width = selected == 4 ? 4 : 8;
        fixture.beforeRead = [&](auto&, auto current, auto size) { if (current == address && size == width) ++visits; };
        REQUIRE(observe(fixture, snapshot)); REQUIRE(visits >= 2);
        const auto mutationVisit = selected == 0 ? visits - 1 : visits; visits = 0;
        fixture.beforeRead = [&](auto& memory, auto current, auto size) {
            if (current != address || size != width || ++visits != mutationVisit) return;
            if (selected == 0) memory.fields[0].name.index = memory.nameIds[1];
            if (selected == 1) memory.fields[0].owner.pointer = 0;
            if (selected == 2) memory.fields[0].next = nullptr;
            if (selected == 3) memory.fields[0].klass = nullptr;
            if (selected == 4) memory.fields[0].flags ^= 0x1000;
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(fieldNames[selected]);
        REQUIRE(report.candidates.empty()); REQUIRE(report.rejected.back().reason.find("changed") != std::string::npos);
        for (const auto& field : fieldNames) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); fixture.fields[0].owner.isUObject = 2;
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fieldNames[1]).candidates.empty());
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); auto profile = fixture.profile(); profile.ownerLayout.size = 24;
        const auto samples = fixture.samples(); const auto& pool = fixture.nameMemory(); ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeFieldFields(fixture, profile, samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fieldNames[1]).rejected.back().reason.find("overlap") != std::string::npos);
        for (const auto& field : fieldNames) REQUIRE(!snapshot.offsets.contains(field));
    }
}
void overridesAndDependencies() {
    for (const auto& field : fieldNames) for (int mode = 0; mode < 9; ++mode) {
        OwnedFields fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto selected = snapshot.offsets.at(field); selected.origin = Origin::User;
        switch (mode) {
        case 0: break;
        case 1: selected.value = 999; break;
        case 2: selected.validation = Validation::Candidate; break;
        case 3: selected.validation = Validation::Stale; break;
        case 4: selected.evidence.clear(); break;
        case 5: selected.evidence[0].passed = false; break;
        case 6: selected.dependencies.erase("UStruct::PropertiesSize"); break;
        case 7: ++selected.dependencies.at("UObject::NamePrivate"); break;
        case 8: selected.dependencies["missing-user-dependency"] = 1; break;
        }
        REQUIRE(publishOffset(snapshot, field, selected)); const auto retained = snapshot.offsets.at(field);
        REQUIRE(observe(fixture, snapshot).code == (mode == 0 ? Error::None : Error::InvalidEvidence));
        const auto& result = snapshot.offsets.at(field);
        REQUIRE(result.value == retained.value && result.version == retained.version && result.origin == retained.origin);
        REQUIRE(result.validation == retained.validation && result.dependencies == retained.dependencies);
    }
    for (const auto* dependency : {"UObject::NamePrivate", "UStruct::ChildProperties", "UStruct::PropertiesSize"}) {
        OwnedFields fixture; auto snapshot = fixture.initial(); bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (changed) return;
            changed = true; auto prior = snapshot.offsets.at(dependency);
            REQUIRE(publishOffset(snapshot, dependency, std::move(prior)));
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        for (const auto& field : fieldNames) REQUIRE(!snapshot.offsets.contains(field));
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
        OwnedFields fixture; auto snapshot = fixture.initial(); chain(snapshot, mode == 6 ? 4100 : 4);
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
        for (const auto& field : fieldNames) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); chain(snapshot, 4);
        bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (!changed) { changed = true; snapshot.offsets.at("owned-closure:3").validation = Validation::Candidate; }
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        for (const auto& field : fieldNames) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); chain(snapshot, 4);
        Offset unrelated; unrelated.validation = Validation::Stale;
        snapshot.offsets["unrelated-stale-result"] = unrelated;
        REQUIRE(observe(fixture, snapshot));
        for (const auto& field : fieldNames) REQUIRE(snapshot.offsets.at(field).validation == Validation::Validated);
        REQUIRE(snapshot.offsets.at("unrelated-stale-result").validation == Validation::Stale);
    }
    {
        OwnedFields fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto& first = snapshot.offsets.at(fieldNames[0]); auto& second = snapshot.offsets.at(fieldNames[1]);
        first.origin = Origin::User; second.origin = Origin::User;
        first.dependencies[fieldNames[1]] = second.version; second.dependencies[fieldNames[0]] = first.version;
        const auto firstVersion = first.version, secondVersion = second.version;
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.offsets.at(fieldNames[0]).version == firstVersion);
        REQUIRE(snapshot.offsets.at(fieldNames[1]).version == secondVersion);
    }
}
void activeCancellation() {
    OwnedFields fixture; const auto initial = fixture.initial(); const auto samples = fixture.samples(); const auto& pool = fixture.nameMemory();
    std::atomic<bool> entered{false}, resume{false};
    fixture.beforeRead = [&](auto&, auto, auto) { if (!entered.exchange(true)) while (!resume) std::this_thread::yield(); };
    Session session(initial); const auto retained = session.snapshot();
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probeFieldFields(fixture, fixture.profile(), samples, pool.nameLayout, pool.address(64), pool.pool, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield(); REQUIRE(entered);
    session.cancel(); resume = true; REQUIRE(session.stop()); const auto result = session.snapshot();
    REQUIRE(result->state == TaskState::Cancelled && result->result.code == Error::Cancelled);
    for (const auto& field : fieldNames) REQUIRE(!result->offsets.contains(field));
    REQUIRE(retained->state == TaskState::Pending && retained->offsets.size() == initial.offsets.size());
}
}
int main() {
    successAndSession(); metadataContracts(); failuresAndMutation(); overridesAndDependencies(); evidenceClosureContracts(); activeCancellation();
    REQUIRE(ownedNativeFunctionCalls == 0);
    std::printf("PASS: %d FField base checks; five independent fields, explicit SeparateBoolean owner, physical name-pool identity and immutable publication; property/container metadata not exercised\n", checks);
}
