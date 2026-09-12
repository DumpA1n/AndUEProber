#include "OwnedRelations.hpp"
#include "andueprober/Export.hpp"
#include <cstdio>
#include <algorithm>
#include <limits>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
int main() {
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch;
        Snapshot initial; initial.sessionId = "owned-relations"; initial.moduleIdentity = "owned-image";
        initial.generation = fixture.epoch; initial.layout = Layout::FField;
        Session session(initial);
        REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
            budget.cancelled = &cancelled;
            return observeOwnedRelations(fixture, fixture, budget, snapshot);
        }));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(session.stop());
        const auto frozen = session.snapshot();
        if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
        REQUIRE(frozen->state == TaskState::Succeeded && validateSnapshot(*frozen));
        REQUIRE(frozen->offsets.at("UObject::InternalIndex").value == 0);
        REQUIRE(frozen->offsets.at("UObject::NamePrivate").value == 16);
        REQUIRE(frozen->offsets.at("UObject::ClassPrivate").value == 8);
        REQUIRE(frozen->offsets.at("UObject::OuterPrivate").value == 24);
        REQUIRE(frozen->offsets.at("UObject::ClassPrivate").dependencies.at("UObject::NamePrivate") == 1);
        const auto poolIdentity = nameObservationIdentity(fixture.nameLayout, fixture.pool, fixture.address(64), fixture.epoch);
        REQUIRE(frozen->offsets.at("UObject::NamePrivate").evidence[0].source == poolIdentity);
        REQUIRE(frozen->offsets.at("UObject::ClassPrivate").evidence[0].source == poolIdentity + ";name-offset:16");
        auto failed = *frozen; failed.state = TaskState::Failed;
        REQUIRE(!validateSnapshot(failed));
        failed.state = TaskState::Cancelled; REQUIRE(!validateSnapshot(failed));
        auto changed = *frozen;
        REQUIRE(beginFieldProbe(changed, "UObject::NamePrivate"));
        REQUIRE(changed.offsets.at("UObject::NamePrivate").validation == Validation::Stale);
        REQUIRE(changed.offsets.at("UObject::ClassPrivate").validation == Validation::Stale);
        REQUIRE(changed.offsets.at("UObject::OuterPrivate").validation == Validation::Stale);
        REQUIRE(!validateSnapshot(changed));
        Offset selected; selected.value = 0; selected.origin = Origin::User;
        REQUIRE(publishOffset(changed, "UObject::InternalIndex", selected));
        REQUIRE(!beginFieldProbe(changed, "UObject::InternalIndex"));
        REQUIRE(changed.offsets.at("UObject::InternalIndex").value == 0);
        REQUIRE(frozen->offsets.at("UObject::NamePrivate").validation == Validation::Validated);
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch;
        FieldProbeReport first, second, reloaded;
        const auto samples = fixture.classes();
        REQUIRE(probeClassField(fixture, samples, 32, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, first));
        fixture.put<std::uintptr_t>(96, fixture.address(512));
        REQUIRE(probeClassField(fixture, samples, 32, 16, fixture.nameLayout, fixture.address(96), fixture.pool, budget, second));
        REQUIRE(first.candidates.front().value == second.candidates.front().value);
        REQUIRE(first.candidates.front().evidence[0].source != second.candidates.front().evidence[0].source);
        budget.generation = ++fixture.epoch;
        REQUIRE(probeClassField(fixture, samples, 32, 16, fixture.nameLayout, fixture.address(96), fixture.pool, budget, reloaded));
        REQUIRE(second.candidates.front().evidence[0].source != reloaded.candidates.front().evidence[0].source);
        REQUIRE(reloaded.candidates.front().evidence[0].source ==
            nameObservationIdentity(fixture.nameLayout, fixture.pool, fixture.address(96), fixture.epoch) + ";name-offset:16");
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        const auto classes = fixture.classes();
        REQUIRE(probeClassField(fixture, classes, 64, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report));
        REQUIRE(report.candidates.size() == 1 && report.candidates[0].value == 8);
        REQUIRE(report.examinedOffsets == 8 && report.rejected.size() == 7);
        REQUIRE(std::any_of(report.rejected.begin(), report.rejected.end(), [](const auto& r) { return r.error == Error::Unmapped; }));
        REQUIRE(std::any_of(report.rejected.begin(), report.rejected.end(), [](const auto& r) { return r.error == Error::Overflow; }));
        fixture.put<std::uintptr_t>(2048 + 32, fixture.object(3)); fixture.put<std::uintptr_t>(2112 + 32, fixture.object(2));
        REQUIRE(probeClassField(fixture, classes, 40, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report));
        REQUIRE(report.candidates.size() == 2 && report.candidates[0].validation == Validation::Candidate);
        Snapshot snapshot; snapshot.generation = fixture.epoch;
        REQUIRE(publishFieldProbe(snapshot, "Class", report, {}).code == Error::InvalidEvidence);
        REQUIRE(snapshot.offsets.empty());
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        auto outers = fixture.outers();
        REQUIRE(probePointerField(fixture, outers, 32, fixture.array.identity, budget, report));
        REQUIRE(report.candidates.size() == 1 && report.candidates[0].value == 24);
        auto copy = report; copy.generation++;
        Snapshot snapshot; snapshot.generation = fixture.epoch;
        REQUIRE(publishFieldProbe(snapshot, "Outer", copy, {}).code == Error::StaleIdentity);
        const std::array<std::string, 1> dependency{"Name"};
        REQUIRE(publishFieldProbe(snapshot, "Outer", report, dependency).code == Error::InvalidEvidence);
        Offset source; source.value = 16;
        REQUIRE(publishOffset(snapshot, "Name", source));
        REQUIRE(publishFieldProbe(snapshot, "Outer", report, dependency));
        REQUIRE(snapshot.offsets.at("Outer").validation == Validation::Candidate);
        fixture.put<std::uintptr_t>(2112, fixture.object(0)); fixture.put<std::uintptr_t>(2048, 0);
        REQUIRE(probePointerField(fixture, outers, 8, fixture.array.identity, budget, report));
        REQUIRE(report.candidates[0].value == 0);
        outers[1].object = outers[0].object;
        REQUIRE(probePointerField(fixture, outers, 32, fixture.array.identity, budget, report).code == Error::InvalidArgument);
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        auto classes = fixture.classes();
        fixture.put<std::uintptr_t>(2048, fixture.object(3)); fixture.put<std::uintptr_t>(2112, fixture.object(2));
        REQUIRE(probeClassField(fixture, classes, 8, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report));
        REQUIRE(report.candidates.size() == 1 && report.candidates[0].value == 0);
        fixture.put<std::uint32_t>(2176, 32); fixture.put<std::uint32_t>(2180, 0);
        fixture.put<std::uint32_t>(2240, 48); fixture.put<std::uint32_t>(2244, 0);
        REQUIRE(probeClassField(fixture, classes, 8, 0, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report));
        REQUIRE(report.candidates.size() == 1);
        classes[0].object = std::numeric_limits<std::uintptr_t>::max() - 3;
        REQUIRE(probeClassField(fixture, classes, 8, 0, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report).code == Error::Overflow);
        REQUIRE(report.candidates.empty());
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        const auto classes = fixture.classes();
        fixture.beforeRead = [](auto& memory, auto offset) {
            if (offset == 2176 + 16) memory.failure = Error::PermissionDenied;
        };
        REQUIRE(probeClassField(fixture, classes, 32, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report).code == Error::PermissionDenied);
        REQUIRE(report.examinedOffsets == 2 && report.rejected.size() == 1 && report.candidates.empty());
    }
    for (auto failure : {Error::PermissionDenied, Error::ShortRead, Error::Cancelled, Error::DeadlineExceeded, Error::BudgetExceeded, Error::StaleIdentity}) {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        const auto classes = fixture.classes(); std::atomic<bool> cancelled{false};
        if (failure == Error::PermissionDenied) fixture.failure = failure;
        if (failure == Error::ShortRead) fixture.shortRead = true;
        if (failure == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (failure == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::now();
        if (failure == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (failure == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto) { ++memory.epoch; };
        REQUIRE(probeClassField(fixture, classes, 32, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report).code == failure);
        REQUIRE(report.candidates.empty());
    }
    for (bool isClass : {false, true}) {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        const auto observedOffset = isClass ? 2112 + 8 : 2112 + 24;
        int observed = 0;
        fixture.beforeRead = [&](auto& memory, auto offset) {
            if (offset == observedOffset && ++observed == 2) memory.template put<std::uintptr_t>(offset, 0);
        };
        const auto classes = fixture.classes(); const auto outers = fixture.outers();
        const auto status = isClass ? probeClassField(fixture, classes, 32, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report) :
            probePointerField(fixture, outers, 32, fixture.array.identity, budget, report);
        REQUIRE(status.code == Error::InvalidEvidence && report.candidates.empty());
    }
    {
        OwnedRelations fixture; ReadBudget budget; budget.generation = fixture.epoch; FieldProbeReport report;
        const auto classes = fixture.classes();
        REQUIRE(probeClassField(fixture, classes, 32, {}, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report).code == Error::InvalidArgument);
        fixture.pool.outlineNumbers = true;
        REQUIRE(probeClassField(fixture, classes, 32, 16, fixture.nameLayout, fixture.address(64), fixture.pool, budget, report).code == Error::Unsupported);
    }
    std::puts("PASS: index/name/class/outer phases, observed failures, session snapshots and dependency invalidation; no engine calls");
}
