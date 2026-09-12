#include "OwnedFunctions.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
int checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::array<std::string, 5> fields{"UFunction::FunctionFlags", "UFunction::NumParms", "UFunction::ParmsSize", "UFunction::ReturnValueOffset", "UFunction::Func"};
constexpr std::array<std::uint32_t, 5> offsets{offsetof(OwnedNativeFunction, flags), offsetof(OwnedNativeFunction, numParms),
    offsetof(OwnedNativeFunction, parmsSize), offsetof(OwnedNativeFunction, returnOffset), offsetof(OwnedNativeFunction, nativeFunction)};
constexpr std::array<std::uint32_t, 5> compactOffsets{offsetof(OwnedCompactFunction, flags), offsetof(OwnedCompactFunction, numParms),
    offsetof(OwnedCompactFunction, parmsSize), offsetof(OwnedCompactFunction, returnOffset), offsetof(OwnedCompactFunction, nativeFunction)};
constexpr std::array<std::uint32_t, 5> widths{4, 1, 2, 2, sizeof(std::uintptr_t)};
Status observe(OwnedFunctions& fixture, Snapshot& snapshot, bool compact = false) {
    const auto samples = fixture.samples(compact); ReadBudget budget; budget.generation = fixture.epoch;
    return probeFunctionFields(fixture, fixture.profile(snapshot.layout, compact), samples, budget, snapshot);
}
void requireOffsets(const Snapshot& snapshot, bool compact = false) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& offset = snapshot.offsets.at(fields[i]);
        const auto& report = snapshot.fieldReports.at(fields[i]);
        REQUIRE(offset.value == (compact ? compactOffsets[i] : offsets[i]));
        REQUIRE(offset.validation == Validation::Validated && offset.evidence.size() >= 2);
        REQUIRE(offset.dependencies.size() == (snapshot.layout == Layout::FField ? 9 : 8));
        for (const auto& [dependency, version] : offset.dependencies) REQUIRE(snapshot.offsets.at(dependency).version == version);
        REQUIRE(!offset.dependencies.contains("UClass::CastFlags") && !offset.dependencies.contains("UStruct::MinAlignment"));
        REQUIRE(offset.evidence.front().source.find("owned-compiled-function-metadata-v1") != std::string::npos);
        REQUIRE(offset.evidence.front().sampleIdentities.size() == 3);
        REQUIRE(report.candidates.size() == 1 && report.generation == snapshot.generation && report.examinedOffsets > 0);
        REQUIRE(!report.rejected.empty());
    }
}
void successAndSessions() {
    for (auto layout : {Layout::UProperty, Layout::FField}) {
        OwnedFunctions fixture; auto initial = fixture.initial(layout);
        Session session(initial); Snapshot* working = nullptr;
        const auto samples = fixture.samples();
        REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
            working = &snapshot;
            ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
            return probeFunctionFields(fixture, fixture.profile(layout), samples, budget, snapshot);
        }));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(session.stop());
        const auto frozen = session.snapshot();
        if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
        REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen));
        requireOffsets(*frozen);
        working->offsets.at(fields[0]).value = 888;
        REQUIRE(frozen->offsets.at(fields[0]).value == offsets[0]);
        for (const auto& dependency : {"UObject::NamePrivate", "UStruct::PropertiesSize", "UField::Next"}) {
            auto changed = *frozen; auto prior = changed.offsets.at(dependency);
            REQUIRE(publishOffset(changed, dependency, std::move(prior)));
            for (const auto& field : fields) REQUIRE(changed.offsets.at(field).validation == Validation::Stale);
            REQUIRE(frozen->offsets.at(fields[0]).validation == Validation::Validated);
        }
    }
    {
        OwnedFunctions fixture; auto snapshot = fixture.initial();
        REQUIRE(observe(fixture, snapshot, true)); requireOffsets(snapshot, true);
        REQUIRE(snapshot.offsets.at(fields[0]).value == 0);
    }
    {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        fixture.functions[0].flags = 0; fixture.functions[0].numParms = 0; fixture.functions[0].parmsSize = 0;
        fixture.functions[0].returnOffset = 0xffff;
        samples[0].expectedFlags = 0; samples[0].expectedNumParms = 0; samples[0].expectedParmsSize = 0;
        samples[0].expectedReturnOffset = 0xffff;
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeFunctionFields(fixture, fixture.profile(), samples, budget, snapshot));
        requireOffsets(snapshot);
    }
    REQUIRE(ownedNativeFunctionCalls == 0);
}
void metadataContracts() {
    for (int mode = 0; mode < 22; ++mode) {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        auto profile = fixture.profile(); ReadBudget budget; budget.generation = fixture.epoch;
        Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: samples[1].object = samples[0].object; break;
        case 1: samples[1].identity = samples[0].identity; break;
        case 2: samples[0].nativeFunctionIdentity.clear(); break;
        case 3: for (auto& sample : samples) sample.expectedFlags = 0; expected = Error::InvalidEvidence; break;
        case 4: for (auto& sample : samples) sample.expectedNumParms = 0; expected = Error::InvalidEvidence; break;
        case 5: for (auto& sample : samples) sample.expectedParmsSize = 99; expected = Error::InvalidEvidence; break;
        case 6: for (auto& sample : samples) sample.expectedReturnOffset = 0xffff; expected = Error::InvalidEvidence; break;
        case 7: for (auto& sample : samples) sample.expectedNativeFunction = 0; expected = Error::InvalidEvidence; break;
        case 8: samples[1].nativeFunctionIdentity = samples[0].nativeFunctionIdentity; expected = Error::InvalidEvidence; break;
        case 9: samples[0].expectedReturnOffset = samples[0].expectedParmsSize; expected = Error::InvalidEvidence; break;
        case 10: samples[0].expectedReturnOffset = 0xfffe; expected = Error::InvalidEvidence; break;
        case 11: profile.moduleIdentity = "another-module"; expected = Error::StaleIdentity; break;
        case 12: profile.layout = Layout::Unknown; break;
        case 13: profile.extent = 4097; break;
        case 14: profile.extent = 7; break;
        case 15: samples[0].object = std::numeric_limits<std::uintptr_t>::max() - 7; expected = Error::Overflow; break;
        case 16: snapshot.offsets.erase("UStruct::ChildProperties"); expected = Error::InvalidEvidence; break;
        case 17: snapshot.offsets.at("UStruct::PropertiesSize").validation = Validation::Candidate; expected = Error::InvalidEvidence; break;
        case 18: snapshot.offsets.at("UField::Next").evidence[0].passed = false; expected = Error::InvalidEvidence; break;
        case 19: ++snapshot.offsets.at("UStruct::SuperStruct").dependencies.at("UObject::NamePrivate"); expected = Error::InvalidEvidence; break;
        case 20: samples[0].identity = std::string("bad\0name", 8); break;
        case 21: profile.identity = "\xED\xA0\x80"; break;
        }
        REQUIRE(probeFunctionFields(fixture, profile, samples, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0);
        REQUIRE(!snapshot.offsets.contains(fields[0]));
    }
    OwnedFunctions fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
    ReadBudget budget; budget.generation = fixture.epoch;
    REQUIRE(probeFunctionFields(fixture, fixture.profile(), std::span(samples).first(2), budget, snapshot).code == Error::InvalidArgument);
    std::vector<FunctionSample> oversized(17, samples[0]);
    REQUIRE(probeFunctionFields(fixture, fixture.profile(), oversized, budget, snapshot).code == Error::InvalidArgument);
    REQUIRE(fixture.reads == 0);
}
void failuresAndMutation() {
    for (auto failure : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead, Error::Cancelled,
        Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
        auto samples = fixture.samples(); ReadBudget budget; budget.generation = fixture.epoch;
        std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied || failure == Error::Unmapped) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto, auto) { ++memory.epoch; };
        REQUIRE(probeFunctionFields(fixture, fixture.profile(), samples, budget, snapshot).code == failure);
        for (const auto& field : fields) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
        REQUIRE(snapshot.fieldReports.at(fields[0]).candidates.empty());
        REQUIRE(!validateSnapshot(snapshot));
    }
    for (std::size_t selected = 0; selected < fields.size(); ++selected) {
        OwnedFunctions fixture; auto snapshot = fixture.initial();
        for (auto& object : fixture.functions) {
            if (selected == 0) object.duplicateFlags = object.flags;
            if (selected == 1) object.duplicateNumParms = object.numParms;
            if (selected == 2) object.duplicateParmsSize = object.parmsSize;
            if (selected == 3) object.duplicateReturnOffset = object.returnOffset;
            if (selected == 4) object.duplicateNativeFunction = object.nativeFunction;
        }
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields[selected]).candidates.size() == 2);
        REQUIRE(snapshot.fieldReports.at(fields[selected]).candidates.front().validation == Validation::Candidate);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    for (std::size_t selected = 0; selected < fields.size(); ++selected) {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); int reads = 0;
        fixture.beforeRead = [&](auto& memory, auto address, auto size) {
            if (address != reinterpret_cast<std::uintptr_t>(&memory.functions[0]) + offsets[selected] || size != widths[selected]) return;
            if (++reads != (selected < 2 ? 2 : 3)) return;
            if (selected == 0) memory.functions[0].flags ^= 0x10000;
            if (selected == 1) ++memory.functions[0].numParms;
            if (selected == 2) ++memory.functions[0].parmsSize;
            if (selected == 3) ++memory.functions[0].returnOffset;
            if (selected == 4) memory.functions[0].nativeFunction = 0;
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(fields[selected]);
        REQUIRE(report.candidates.empty());
        REQUIRE(report.rejected.back().reason.find("changed") != std::string::npos);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
    {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i].expectedFlags = OwnedFunctions::counts[i];
            fixture.functions[i].flags = OwnedFunctions::counts[i]; fixture.functions[i].numParms = 0xA0;
        }
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probeFunctionFields(fixture, fixture.profile(), samples, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields[0]).rejected.back().reason.find("overlap") != std::string::npos);
        for (const auto& field : fields) REQUIRE(!snapshot.offsets.contains(field));
    }
}
void overridesAndDependencies() {
    for (const auto& field : fields) {
        for (int mode = 0; mode < 9; ++mode) {
            OwnedFunctions fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, snapshot));
            auto selected = snapshot.offsets.at(field); selected.origin = Origin::User;
            switch (mode) {
            case 0: break;
            case 1: selected.value = 0; break;
            case 2: selected.validation = Validation::Candidate; break;
            case 3: selected.validation = Validation::Stale; break;
            case 4: selected.evidence.clear(); break;
            case 5: selected.evidence[0].passed = false; break;
            case 6: selected.dependencies.erase("UStruct::PropertiesSize"); break;
            case 7: ++selected.dependencies.at("UObject::NamePrivate"); break;
            case 8: selected.dependencies["missing-user-dependency"] = 1; break;
            }
            REQUIRE(publishOffset(snapshot, field, selected));
            const auto retained = snapshot.offsets.at(field);
            REQUIRE(observe(fixture, snapshot).code == (mode == 0 ? Error::None : Error::InvalidEvidence));
            const auto& result = snapshot.offsets.at(field);
            REQUIRE(result.value == retained.value && result.version == retained.version && result.origin == retained.origin);
            REQUIRE(result.validation == retained.validation && result.dependencies == retained.dependencies);
        }
    }
    for (const auto* field : {"UObject::OuterPrivate", "UStruct::PropertiesSize", "UStruct::ChildProperties"}) {
        OwnedFunctions fixture; auto snapshot = fixture.initial(); bool changed = false;
        fixture.beforeRead = [&](auto&, auto, auto) {
            if (changed) return;
            changed = true; auto previous = snapshot.offsets.at(field);
            REQUIRE(publishOffset(snapshot, field, std::move(previous)));
        };
        REQUIRE(observe(fixture, snapshot).code == Error::InvalidEvidence);
        for (const auto& target : fields) REQUIRE(!snapshot.offsets.contains(target));
    }
}
void activeCancellation() {
    OwnedFunctions fixture; const auto initial = fixture.initial(); const auto samples = fixture.samples();
    std::atomic<bool> entered{false}, resume{false};
    fixture.beforeRead = [&](auto&, auto, auto) {
        if (entered.exchange(true)) return;
        while (!resume) std::this_thread::yield();
    };
    Session session(initial); const auto retained = session.snapshot();
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        ReadBudget budget; budget.generation = fixture.epoch; budget.cancelled = &cancelled;
        return probeFunctionFields(fixture, fixture.profile(), samples, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(entered);
    session.cancel(); resume = true; REQUIRE(session.stop());
    const auto result = session.snapshot();
    REQUIRE(result->state == TaskState::Cancelled && result->result.code == Error::Cancelled);
    for (const auto& field : fields) REQUIRE(!result->offsets.contains(field));
    REQUIRE(retained->state == TaskState::Pending && retained->offsets.size() == initial.offsets.size());
}
}
int main() {
    successAndSessions(); metadataContracts(); failuresAndMutation(); overridesAndDependencies(); activeCancellation();
    REQUIRE(ownedNativeFunctionCalls == 0);
    std::printf("PASS: %d complete Phase 4 function checks; independent typed metadata, bounded reads, disjoint fields, Phase 1/2 dependencies and immutable sessions; native functions never called\n", checks);
}
