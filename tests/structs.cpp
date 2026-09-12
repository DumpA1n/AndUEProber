#include "OwnedStructs.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
int checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
Status observe(OwnedStructs& fixture, Snapshot& snapshot) {
    auto structs = fixture.structSamples(); auto fields = fixture.fieldSamples();
    ReadBudget budget; budget.generation = fixture.epoch;
    return probeStructFields(fixture, fixture.profile(snapshot.layout), structs, fields, budget, snapshot);
}
void requireOffsets(const Snapshot& snapshot, Layout layout, bool alignment = true) {
    REQUIRE(snapshot.offsets.at("UField::Next").value == offsetof(OwnedNativeField, next));
    REQUIRE(snapshot.offsets.at("UField::Next").value != sizeof(OwnedObjectPrefix));
    REQUIRE(snapshot.offsets.at("UStruct::SuperStruct").value == offsetof(OwnedNativeStruct, super));
    REQUIRE(snapshot.offsets.at("UStruct::Children").value == offsetof(OwnedNativeStruct, children));
    REQUIRE(snapshot.offsets.at("UStruct::PropertiesSize").value == offsetof(OwnedNativeStruct, propertiesSize));
    if (layout == Layout::FField)
        REQUIRE(snapshot.offsets.at("UStruct::ChildProperties").value == offsetof(OwnedNativeStruct, childProperties));
    if (alignment) {
        REQUIRE(snapshot.offsets.at("UStruct::MinAlignment").value == offsetof(OwnedNativeStruct, minAlignment));
        REQUIRE(snapshot.offsets.at("UStruct::MinAlignment").value != *snapshot.offsets.at("UStruct::PropertiesSize").value + 4);
    }
    for (const auto& field : {"UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize"}) {
        const auto& value = snapshot.offsets.at(field);
        REQUIRE(value.validation == Validation::Validated && value.dependencies.size() == 4 && value.evidence.size() >= 2);
        REQUIRE(value.dependencies.at("UObject::InternalIndex") == snapshot.offsets.at("UObject::InternalIndex").version);
        REQUIRE(value.dependencies.at("UObject::NamePrivate") == snapshot.offsets.at("UObject::NamePrivate").version);
        REQUIRE(value.dependencies.at("UObject::ClassPrivate") == snapshot.offsets.at("UObject::ClassPrivate").version);
        REQUIRE(value.dependencies.at("UObject::OuterPrivate") == snapshot.offsets.at("UObject::OuterPrivate").version);
        const auto& report = snapshot.fieldReports.at(field);
        REQUIRE(report.generation == snapshot.generation && report.candidates.size() == 1 && report.examinedOffsets > 0);
        REQUIRE(!report.rejected.empty());
        REQUIRE(value.evidence.front().source.find("owned-compiled-struct-metadata-v1") != std::string::npos);
        REQUIRE(!value.evidence.front().sampleIdentities.empty());
    }
}
void successAndSession() {
    for (auto layout : {Layout::UProperty, Layout::FField}) {
        OwnedStructs fixture; auto initial = fixture.initial(layout);
        Session session(initial);
        auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
        Snapshot* working = nullptr;
        REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
            working = &snapshot;
            ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
            return probeStructFields(fixture, fixture.profile(layout), samples, fields, budget, snapshot);
        }));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(session.stop());
        auto frozen = session.snapshot();
        if (!frozen->result) std::fprintf(stderr, "result: %s\n", frozen->result.message.c_str());
        REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen));
        requireOffsets(*frozen, layout);
        working->offsets.at("UStruct::PropertiesSize").value = 999;
        REQUIRE(frozen->offsets.at("UStruct::PropertiesSize").value == offsetof(OwnedNativeStruct, propertiesSize));
        auto changed = *frozen;
        auto name = changed.offsets.at("UObject::NamePrivate");
        REQUIRE(publishOffset(changed, "UObject::NamePrivate", std::move(name)));
        REQUIRE(changed.offsets.at("UStruct::PropertiesSize").validation == Validation::Stale);
        REQUIRE(changed.offsets.at("UField::Next").validation == Validation::Stale);
        REQUIRE(frozen->offsets.at("UField::Next").validation == Validation::Validated);
    }
}
void metadataContracts() {
    for (int mode = 0; mode < 12; ++mode) {
        OwnedStructs fixture; auto snapshot = fixture.initial();
        auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
        auto profile = fixture.profile(); ReadBudget budget; budget.generation = fixture.epoch;
        Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: samples[1].object = samples[0].object; break;
        case 1: samples[1].identity = samples[0].identity; break;
        case 2: fields[1].object = fields[0].object; break;
        case 3: fields[2].expectedNext = fields[0].object; expected = Error::InvalidEvidence; break;
        case 4: samples[0].expectedSuper = samples[2].object; expected = Error::InvalidEvidence; break;
        case 5: samples[0].expectedChildProperties.reset(); expected = Error::Unsupported; break;
        case 6: samples[1].expectedMinAlignment.reset(); samples[2].expectedMinAlignment.reset(); expected = Error::InvalidEvidence; break;
        case 7: samples[0].expectedMinAlignment = 3; break;
        case 8: profile.moduleIdentity = "another-image"; expected = Error::StaleIdentity; break;
        case 9: snapshot.offsets.at("UObject::ClassPrivate").validation = Validation::Candidate; expected = Error::InvalidEvidence; break;
        case 10: snapshot.offsets.at("UObject::ClassPrivate").evidence[0].passed = false; expected = Error::InvalidEvidence; break;
        case 11: ++snapshot.offsets.at("UObject::ClassPrivate").dependencies.at("UObject::NamePrivate"); expected = Error::InvalidEvidence; break;
        }
        REQUIRE(probeStructFields(fixture, profile, samples, fields, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0);
        REQUIRE(!snapshot.offsets.contains("UField::Next"));
    }
    {
        OwnedStructs fixture; auto snapshot = fixture.initial();
        auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeStructFields(fixture, fixture.profile(), std::span(samples).first(1), fields, budget, snapshot).code == Error::InvalidArgument);
        REQUIRE(probeStructFields(fixture, fixture.profile(), samples, std::span(fields).first(2), budget, snapshot).code == Error::InvalidArgument);
        fields[0].object = std::numeric_limits<std::uintptr_t>::max() - 3;
        REQUIRE(probeStructFields(fixture, fixture.profile(), samples, fields, budget, snapshot).code == Error::Overflow);
        REQUIRE(fixture.reads == 0);
    }
    {
        OwnedStructs fixture; auto snapshot = fixture.initial();
        auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
        for (auto& sample : samples) sample.expectedMinAlignment.reset();
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeStructFields(fixture, fixture.profile(), samples, fields, budget, snapshot));
        requireOffsets(snapshot, Layout::FField, false);
        REQUIRE(!snapshot.offsets.contains("UStruct::MinAlignment"));
        REQUIRE(!snapshot.fieldReports.contains("UStruct::MinAlignment"));
    }
}
void failureContracts() {
    for (auto failure : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Cancelled,
        Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedStructs fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
        ReadBudget budget; budget.generation = fixture.epoch;
        std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied || failure == Error::Unmapped) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& value, auto, auto) { ++value.epoch; };
        REQUIRE(probeStructFields(fixture, fixture.profile(), samples, fields, budget, snapshot).code == failure);
        REQUIRE(snapshot.offsets.at("UField::Next").validation == Validation::Stale);
        REQUIRE(snapshot.offsets.at("UStruct::PropertiesSize").validation == Validation::Stale);
        REQUIRE(snapshot.fieldReports.at("UField::Next").candidates.empty());
        REQUIRE(!validateSnapshot(snapshot));
    }
    for (bool scalar : {false, true}) {
        OwnedStructs fixture; auto snapshot = fixture.initial();
        if (scalar) for (auto& object : fixture.structs) object.duplicateSize = object.propertiesSize;
        else for (auto& field : fixture.fields) field.duplicateNext = field.next;
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(scalar ? "UStruct::PropertiesSize" : "UField::Next");
        REQUIRE(report.candidates.size() == 2);
        REQUIRE(report.candidates.front().validation == Validation::Candidate);
        REQUIRE(!snapshot.offsets.contains("UStruct::SuperStruct"));
    }
    for (bool scalar : {false, true}) {
        OwnedStructs fixture; auto snapshot = fixture.initial(); int matchingReads = 0;
        fixture.beforeRead = [&](auto& memory, auto address, auto size) {
            if (scalar && address == reinterpret_cast<std::uintptr_t>(&memory.structs[0].propertiesSize) && size == 4 && ++matchingReads == 2)
                memory.structs[0].propertiesSize = 777;
            if (!scalar && address == reinterpret_cast<std::uintptr_t>(&memory.fields[0].next) && size == sizeof(std::uintptr_t) && ++matchingReads == 3)
                memory.fields[0].next = nullptr;
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(scalar ? "UStruct::PropertiesSize" : "UField::Next");
        REQUIRE(report.candidates.empty());
        REQUIRE(report.rejected.back().reason.find("changed") != std::string::npos);
        REQUIRE(!snapshot.offsets.contains("UStruct::SuperStruct"));
    }
}
void overridesAndDependencies() {
    for (bool conflict : {false, true}) {
        OwnedStructs fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto selected = snapshot.offsets.at("UStruct::PropertiesSize"); selected.origin = Origin::User;
        if (conflict) selected.value = 0;
        REQUIRE(publishOffset(snapshot, "UStruct::PropertiesSize", selected));
        const auto preserved = snapshot.offsets.at("UStruct::PropertiesSize");
        const auto status = observe(fixture, snapshot);
        REQUIRE(status.code == (conflict ? Error::InvalidEvidence : Error::None));
        REQUIRE(snapshot.offsets.at("UStruct::PropertiesSize").value == preserved.value);
        REQUIRE(snapshot.offsets.at("UStruct::PropertiesSize").version == preserved.version);
        REQUIRE(snapshot.offsets.at("UStruct::PropertiesSize").origin == Origin::User);
        REQUIRE(snapshot.fieldReports.at("UStruct::PropertiesSize").candidates.front().value == offsetof(OwnedNativeStruct, propertiesSize));
        if (conflict) REQUIRE(snapshot.offsets.at("UField::Next").validation == Validation::Stale);
    }
    for (int mode = 0; mode < 7; ++mode) {
        OwnedStructs fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto selected = snapshot.offsets.at("UStruct::PropertiesSize"); selected.origin = Origin::User;
        switch (mode) {
        case 0: selected.validation = Validation::Candidate; break;
        case 1: selected.validation = Validation::Stale; break;
        case 2: selected.evidence.clear(); break;
        case 3: selected.evidence[0].passed = false; break;
        case 4: selected.dependencies.erase("UObject::NamePrivate"); break;
        case 5: ++selected.dependencies.at("UObject::NamePrivate"); break;
        case 6: selected.dependencies["missing-user-dependency"] = 1; break;
        }
        REQUIRE(publishOffset(snapshot, "UStruct::PropertiesSize", selected));
        const auto retained = snapshot.offsets.at("UStruct::PropertiesSize");
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& result = snapshot.offsets.at("UStruct::PropertiesSize");
        REQUIRE(result.value == retained.value && result.version == retained.version && result.origin == retained.origin);
        REQUIRE(result.validation == retained.validation && result.dependencies == retained.dependencies);
        REQUIRE(snapshot.offsets.at("UField::Next").validation == Validation::Stale);
        REQUIRE(snapshot.fieldReports.at("UStruct::PropertiesSize").candidates.front().value == retained.value);
    }
    {
        OwnedStructs fixture; auto snapshot = fixture.initial(); bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (changed) return;
            changed = true;
            auto offset = snapshot.offsets.at("UObject::OuterPrivate");
            REQUIRE(publishOffset(snapshot, "UObject::OuterPrivate", std::move(offset)));
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(!snapshot.offsets.contains("UStruct::PropertiesSize"));
    }
}
void activeCancellation() {
    OwnedStructs fixture; const auto initial = fixture.initial();
    auto samples = fixture.structSamples(); auto fields = fixture.fieldSamples();
    std::atomic<bool> entered{false}, resume{false};
    fixture.beforeRead = [&](auto&, auto, auto) {
        if (entered.exchange(true)) return;
        while (!resume) std::this_thread::yield();
    };
    Session session(initial);
    const auto retained = session.snapshot();
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probeStructFields(fixture, fixture.profile(), samples, fields, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(entered);
    session.cancel(); resume = true;
    REQUIRE(session.stop());
    const auto result = session.snapshot();
    REQUIRE(result->state == TaskState::Cancelled && result->result.code == Error::Cancelled);
    REQUIRE(!result->offsets.contains("UField::Next") && result->fieldReports.at("UField::Next").candidates.empty());
    REQUIRE(retained->state == TaskState::Pending && retained->fieldReports.size() == initial.fieldReports.size());
}
}
int main() {
    successAndSession(); metadataContracts(); failureContracts(); overridesAndDependencies(); activeCancellation();
    std::printf("PASS: %d complete Phase 2 struct checks; independent compiled anchors, bounded reads, reports, versions and immutable sessions; no engine calls\n", checks);
}
