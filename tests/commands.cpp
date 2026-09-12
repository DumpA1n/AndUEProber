#include "OwnedRelations.hpp"
#include "andueprober/Commands.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
static void until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(predicate());
}
int main() {
    {
        OwnedRelations graph;
        Snapshot initial; initial.sessionId = "owned-command-graph"; initial.moduleIdentity = "owned-module";
        initial.layout = Layout::FField; initial.generation = 1;
        std::thread::id worker;
        std::atomic<int> calls = 0;
        CommandSession owner(initial, [&](const Command& command, Snapshot& result, const auto& cancelled) {
            if (!calls++) worker = std::this_thread::get_id();
            REQUIRE(worker == std::this_thread::get_id());
            ReadBudget budget; budget.generation = graph.generation(); budget.cancelled = &cancelled;
            if (command.kind == CommandKind::ProbeAll) return observeOwnedRelations(graph, graph, budget, result);
            Offset user; user.value = command.value; user.origin = Origin::User;
            return publishOffset(result, command.field, user);
        });
        REQUIRE(owner.start()); REQUIRE(owner.start().code == Error::Busy);
        std::uint64_t id;
        Command observe; observe.kind = CommandKind::ProbeAll;
        REQUIRE(owner.submit(observe, id) && id == 1);
        until([&] { return owner.view().completed == id; });
        const auto frozen = owner.view().snapshot;
        REQUIRE(validateSnapshot(*frozen)); REQUIRE(frozen->offsets.size() == 4);
        REQUIRE(frozen->fieldReports.size() == 4 && !frozen->fieldReports.at("UObject::ClassPrivate").rejected.empty());
        REQUIRE(worker != std::this_thread::get_id());
        Command override; override.kind = CommandKind::SetOverride; override.field = "UObject::InternalIndex"; override.value = 0;
        REQUIRE(owner.submit(override, id) && id == 2);
        until([&] { return owner.view().completed == id; });
        REQUIRE(owner.view().snapshot->offsets.at("UObject::NamePrivate").validation == Validation::Stale);
        REQUIRE(frozen->offsets.at("UObject::NamePrivate").validation == Validation::Validated);
        REQUIRE(owner.stop()); REQUIRE(owner.stop()); REQUIRE(owner.submit(observe, id).code == Error::Busy && !id);
    }
    {
        std::atomic<bool> entered = false, release = false;
        std::atomic<int> calls = 0;
        CommandSession owner({}, [&](const auto&, auto&, const auto& cancelled) {
            ++calls; entered = true;
            while (!release && !cancelled.load()) std::this_thread::yield();
            return Status{};
        }, 2);
        REQUIRE(owner.start()); std::uint64_t first, second, third, rejected;
        REQUIRE(owner.submit({}, first)); until([&] { return entered.load(); });
        REQUIRE(owner.submit({}, second)); REQUIRE(owner.submit({}, third));
        REQUIRE(owner.submit({}, rejected).code == Error::Busy && rejected == 0);
        REQUIRE(owner.view().pending == 2);
        std::thread a([&] { REQUIRE(owner.stop()); });
        std::thread b([&] { REQUIRE(owner.stop()); });
        a.join(); b.join();
        REQUIRE(calls == 1 && !owner.view().accepting && owner.view().pending == 0);
        REQUIRE(owner.view().snapshot->state == TaskState::Cancelled);
    }
    {
        CommandSession* current = nullptr;
        Status self;
        CommandSession owner({}, [&](const auto&, auto&, const auto&) { self = current->stop(); return Status{}; });
        current = &owner; REQUIRE(owner.start()); std::uint64_t id; REQUIRE(owner.submit({}, id));
        until([&] { return owner.view().completed == id; });
        REQUIRE(owner.stop()); REQUIRE(self.code == Error::Busy);
    }
    {
        CommandSession owner({}, [](const auto&, auto&, const auto&) -> Status { throw std::runtime_error("owned handler failure"); });
        REQUIRE(owner.start()); std::uint64_t id; REQUIRE(owner.submit({}, id));
        until([&] { return !owner.view().ownerResult; });
        REQUIRE(!owner.view().accepting && owner.view().ownerResult.code == Error::Internal);
        REQUIRE(owner.submit({}, id).code == Error::Busy); REQUIRE(owner.stop());
    }
    {
        CommandSession owner({}, [](const auto&, auto&, const auto&) { return Status{}; });
        REQUIRE(owner.stop()); REQUIRE(owner.start().code == Error::Busy);
        CommandSession invalid({}, {}, 0); REQUIRE(invalid.start().code == Error::InvalidArgument);
        CommandSession active({}, [](const auto&, auto&, const auto&) { return Status{}; }); REQUIRE(active.start());
        Command command; command.kind = CommandKind::ProbePhase; command.phase = 7; std::uint64_t id;
        REQUIRE(active.submit(command, id).code == Error::InvalidArgument && !id);
        command.kind = CommandKind::SetOverride; command.field = "Index";
        REQUIRE(active.submit(command, id).code == Error::InvalidArgument);
        command.value = 0xffffffff;
        REQUIRE(active.submit(command, id).code == Error::InvalidArgument);
        command = {}; command.kind = CommandKind::InspectMemory; command.address = 0x1000; command.size = 513;
        REQUIRE(active.submit(command, id).code == Error::InvalidArgument);
        command.size = 16;
        REQUIRE(active.submit(command, id));
        until([&] { return active.view().completed == id; });
        REQUIRE(active.stop());
    }
    std::puts("PASS: bounded command admission, persistent worker, immutable Core results, override invalidation, cancellation, shutdown and callback failures");
}
