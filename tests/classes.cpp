#include "OwnedClasses.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
int checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
Status observe(OwnedClasses& fixture, Snapshot& snapshot, bool compact = false) {
    const auto samples = fixture.samples(compact);
    ReadBudget budget; budget.generation = fixture.epoch;
    return probeClassFields(fixture, fixture.profile(snapshot.layout, compact), samples, budget, snapshot);
}
void requireOffsets(const Snapshot& snapshot, bool compact = false) {
    REQUIRE(snapshot.offsets.at("UClass::CastFlags").value == (compact ? offsetof(OwnedCompactClass, castFlags) : offsetof(OwnedNativeClass, castFlags)));
    REQUIRE(snapshot.offsets.at("UClass::ClassDefaultObject").value == (compact ? offsetof(OwnedCompactClass, defaultObject) : offsetof(OwnedNativeClass, defaultObject)));
    for (const auto& field : {"UClass::CastFlags", "UClass::ClassDefaultObject"}) {
        const auto& offset = snapshot.offsets.at(field);
        const auto& report = snapshot.fieldReports.at(field);
        REQUIRE(offset.validation == Validation::Validated && offset.evidence.size() >= 2);
        REQUIRE(offset.dependencies.size() == (snapshot.layout == Layout::FField ? 9 : 8));
        for (const auto& [dependency, version] : offset.dependencies)
            REQUIRE(version == snapshot.offsets.at(dependency).version);
        REQUIRE(!offset.dependencies.contains("UStruct::MinAlignment"));
        REQUIRE(offset.evidence.front().source.find("owned-compiled-class-metadata-v1") != std::string::npos);
        REQUIRE(offset.evidence.front().sampleIdentities.size() == 3);
        REQUIRE(report.generation == snapshot.generation && report.candidates.size() == 1 && report.examinedOffsets > 0);
        REQUIRE(!report.rejected.empty());
    }
}
void successAndSessions() {
    for (auto layout : {Layout::UProperty, Layout::FField}) {
        OwnedClasses fixture; auto initial = fixture.initial(layout);
        initial.offsets.erase("UStruct::MinAlignment");
        Session session(initial); Snapshot* working = nullptr;
        const auto samples = fixture.samples();
        REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
            working = &snapshot;
            ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
            return probeClassFields(fixture, fixture.profile(layout), samples, budget, snapshot);
        }));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(session.stop());
        const auto frozen = session.snapshot();
        REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen));
        requireOffsets(*frozen);
        working->offsets.at("UClass::CastFlags").value = 888;
        REQUIRE(frozen->offsets.at("UClass::CastFlags").value == offsetof(OwnedNativeClass, castFlags));
        for (const auto& upstream : {"UObject::NamePrivate", "UStruct::PropertiesSize", "UField::Next"}) {
            auto changed = *frozen;
            auto previous = changed.offsets.at(upstream);
            REQUIRE(publishOffset(changed, upstream, std::move(previous)));
            REQUIRE(changed.offsets.at("UClass::CastFlags").validation == Validation::Stale);
            REQUIRE(changed.offsets.at("UClass::ClassDefaultObject").validation == Validation::Stale);
            REQUIRE(frozen->offsets.at("UClass::CastFlags").validation == Validation::Validated);
        }
    }
    {
        OwnedClasses fixture; auto snapshot = fixture.initial();
        REQUIRE(observe(fixture, snapshot, true)); requireOffsets(snapshot, true);
        REQUIRE(snapshot.offsets.at("UClass::CastFlags").value == 0);
    }
    {
        OwnedClasses fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        fixture.classes[2].defaultObject = nullptr;
        samples[2].expectedDefaultObject = 0; samples[2].defaultObjectIdentity = "null";
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeClassFields(fixture, fixture.profile(), samples, budget, snapshot));
        requireOffsets(snapshot);
    }
}
void metadataContracts() {
    for (int mode = 0; mode < 20; ++mode) {
        OwnedClasses fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        auto profile = fixture.profile(); ReadBudget budget; budget.generation = fixture.epoch;
        Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: samples[1].object = samples[0].object; break;
        case 1: samples[1].identity = samples[0].identity; break;
        case 2: samples[0].defaultObjectIdentity.clear(); break;
        case 3: for (auto& sample : samples) sample.expectedCastFlags = 0; expected = Error::InvalidEvidence; break;
        case 4: for (auto& sample : samples) sample.expectedDefaultObject = 0; expected = Error::InvalidEvidence; break;
        case 5: for (auto& sample : samples) sample.expectedDefaultObject = samples[0].expectedDefaultObject; expected = Error::InvalidEvidence; break;
        case 6: samples[0].expectedDefaultObject = samples[0].object; expected = Error::InvalidEvidence; break;
        case 7: profile.moduleIdentity = "another-image"; expected = Error::StaleIdentity; break;
        case 8: profile.layout = Layout::Unknown; break;
        case 9: profile.extent = 4097; break;
        case 10: profile.extent = 7; break;
        case 11: samples[0].object = std::numeric_limits<std::uintptr_t>::max() - 7; expected = Error::Overflow; break;
        case 12: snapshot.offsets.erase("UStruct::ChildProperties"); expected = Error::InvalidEvidence; break;
        case 13: snapshot.offsets.at("UStruct::PropertiesSize").validation = Validation::Candidate; expected = Error::InvalidEvidence; break;
        case 14: snapshot.offsets.at("UField::Next").evidence[0].passed = false; expected = Error::InvalidEvidence; break;
        case 15: ++snapshot.offsets.at("UStruct::SuperStruct").dependencies.at("UObject::NamePrivate"); expected = Error::InvalidEvidence; break;
        case 16: samples[0].identity = std::string("bad\0name", 8); break;
        case 17: profile.identity = "\xED\xA0\x80"; break;
        case 18: ++budget.generation; expected = Error::StaleIdentity; break;
        case 19: samples[1].defaultObjectIdentity = samples[0].defaultObjectIdentity; expected = Error::InvalidEvidence; break;
        }
        REQUIRE(probeClassFields(fixture, profile, samples, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0);
        REQUIRE(!snapshot.offsets.contains("UClass::CastFlags"));
    }
    OwnedClasses fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
    ReadBudget budget; budget.generation = fixture.epoch;
    REQUIRE(probeClassFields(fixture, fixture.profile(), std::span(samples).first(2), budget, snapshot).code == Error::InvalidArgument);
    std::vector<ClassSample> oversized(17, samples[0]);
    REQUIRE(probeClassFields(fixture, fixture.profile(), oversized, budget, snapshot).code == Error::InvalidArgument);
    REQUIRE(fixture.reads == 0);
}
void failuresAndMutation() {
    for (auto failure : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Cancelled,
        Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedClasses fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto samples = fixture.samples(); ReadBudget budget; budget.generation = fixture.epoch;
        std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied || failure == Error::Unmapped) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto, auto) { ++memory.epoch; };
        REQUIRE(probeClassFields(fixture, fixture.profile(), samples, budget, snapshot).code == failure);
        REQUIRE(snapshot.offsets.at("UClass::CastFlags").validation == Validation::Stale);
        REQUIRE(snapshot.offsets.at("UClass::ClassDefaultObject").validation == Validation::Stale);
        REQUIRE(snapshot.fieldReports.at("UClass::CastFlags").candidates.empty());
        REQUIRE(!validateSnapshot(snapshot));
    }
    for (bool flags : {false, true}) {
        OwnedClasses fixture; auto snapshot = fixture.initial();
        for (auto& object : fixture.classes) {
            if (flags) object.duplicateFlags = object.castFlags;
            else object.duplicateDefault = object.defaultObject;
        }
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(flags ? "UClass::CastFlags" : "UClass::ClassDefaultObject");
        REQUIRE(report.candidates.size() == 2 && report.candidates.front().validation == Validation::Candidate);
        REQUIRE(!snapshot.offsets.contains("UClass::CastFlags") && !snapshot.offsets.contains("UClass::ClassDefaultObject"));
    }
    for (bool flags : {false, true}) {
        OwnedClasses fixture; auto snapshot = fixture.initial(); int reads = 0;
        fixture.beforeRead = [&](auto& memory, auto address, auto) {
            if (flags && address == reinterpret_cast<std::uintptr_t>(&memory.classes[0].castFlags) && ++reads == 3)
                memory.classes[0].castFlags = 777;
            if (!flags && address == reinterpret_cast<std::uintptr_t>(&memory.classes[0].defaultObject) && ++reads == 4)
                memory.classes[0].defaultObject = nullptr;
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(flags ? "UClass::CastFlags" : "UClass::ClassDefaultObject");
        REQUIRE(report.candidates.empty());
        REQUIRE(report.rejected.back().reason.find("changed") != std::string::npos);
        REQUIRE(!snapshot.offsets.contains("UClass::CastFlags") && !snapshot.offsets.contains("UClass::ClassDefaultObject"));
    }
    {
        OwnedClasses fixture; auto snapshot = fixture.initial();
        fixture.classes[1].castFlags ^= 0x100000000ULL;
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at("UClass::CastFlags").candidates.empty());
        REQUIRE(!snapshot.offsets.contains("UClass::ClassDefaultObject"));
    }
}
void overridesAndDependencies() {
    for (const auto* field : {"UClass::CastFlags", "UClass::ClassDefaultObject"}) {
        for (bool conflict : {false, true}) {
            OwnedClasses fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
            auto selected = snapshot.offsets.at(field); selected.origin = Origin::User;
            if (conflict) selected.value = 0;
            REQUIRE(publishOffset(snapshot, field, selected));
            const auto preserved = snapshot.offsets.at(field);
            REQUIRE(observe(fixture, snapshot).code == (conflict ? Error::InvalidEvidence : Error::None));
            REQUIRE(snapshot.offsets.at(field).value == preserved.value);
            REQUIRE(snapshot.offsets.at(field).version == preserved.version);
            REQUIRE(snapshot.offsets.at(field).origin == Origin::User);
        }
    }
    for (int mode = 0; mode < 7; ++mode) {
        OwnedClasses fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto selected = snapshot.offsets.at("UClass::CastFlags"); selected.origin = Origin::User;
        switch (mode) {
        case 0: selected.validation = Validation::Candidate; break;
        case 1: selected.validation = Validation::Stale; break;
        case 2: selected.evidence.clear(); break;
        case 3: selected.evidence[0].passed = false; break;
        case 4: selected.dependencies.erase("UStruct::PropertiesSize"); break;
        case 5: ++selected.dependencies.at("UObject::NamePrivate"); break;
        case 6: selected.dependencies["missing-user-dependency"] = 1; break;
        }
        REQUIRE(publishOffset(snapshot, "UClass::CastFlags", selected));
        const auto retained = snapshot.offsets.at("UClass::CastFlags");
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& result = snapshot.offsets.at("UClass::CastFlags");
        REQUIRE(result.value == retained.value && result.version == retained.version && result.origin == retained.origin);
        REQUIRE(result.validation == retained.validation && result.dependencies == retained.dependencies);
        REQUIRE(snapshot.offsets.at("UClass::ClassDefaultObject").validation == Validation::Stale);
        REQUIRE(snapshot.fieldReports.at("UClass::CastFlags").candidates.front().value == retained.value);
    }
    for (const auto* field : {"UObject::OuterPrivate", "UStruct::PropertiesSize", "UStruct::ChildProperties"}) {
        OwnedClasses fixture; auto snapshot = fixture.initial(); bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (changed) return;
            changed = true;
            auto previous = snapshot.offsets.at(field);
            REQUIRE(publishOffset(snapshot, field, std::move(previous)));
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(!snapshot.offsets.contains("UClass::CastFlags"));
        REQUIRE(!snapshot.offsets.contains("UClass::ClassDefaultObject"));
    }
}
void activeCancellation() {
    OwnedClasses fixture; const auto initial = fixture.initial(); const auto samples = fixture.samples();
    std::atomic<bool> entered{false}, resume{false};
    fixture.beforeRead = [&](auto&, auto, auto) {
        if (entered.exchange(true)) return;
        while (!resume) std::this_thread::yield();
    };
    Session session(initial); const auto retained = session.snapshot();
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probeClassFields(fixture, fixture.profile(), samples, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(entered);
    session.cancel(); resume = true; REQUIRE(session.stop());
    const auto result = session.snapshot();
    REQUIRE(result->state == TaskState::Cancelled && result->result.code == Error::Cancelled);
    REQUIRE(!result->offsets.contains("UClass::CastFlags") && !result->offsets.contains("UClass::ClassDefaultObject"));
    REQUIRE(retained->state == TaskState::Pending && retained->offsets.size() == initial.offsets.size());
}
}
int main() {
    successAndSessions(); metadataContracts(); failuresAndMutation(); overridesAndDependencies(); activeCancellation();
    std::printf("PASS: %d complete Phase 3 class checks; independent uint64/CDO metadata, Phase 1/2 dependencies, bounded reads and immutable sessions; no engine calls\n", checks);
}
