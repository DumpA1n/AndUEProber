#include "andueprober/Core.hpp"
#include "andueprober/Export.hpp"
#include "andueprober/Probe.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace andueprober;
#define REQUIRE(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " #condition "\n"; return 1; } } while (false)
struct FixtureReader final : MemoryReader {
    std::array<std::byte, 128> bytes{};
    std::uint64_t epoch = 1;
    std::size_t truncate = 0;
    Error failure = Error::None;
    bool reload = false;
    std::function<void()> beforeReturn;
    std::uint64_t generation() const override { return epoch; }
    ReadResult read(std::uintptr_t address, std::span<std::byte> out) override {
        if (failure != Error::None) return {0, failure};
        if (address > bytes.size() || out.size() > bytes.size() - address) return {0, Error::Unmapped};
        const auto size = out.size() - std::min(out.size(), truncate);
        std::memcpy(out.data(), bytes.data() + address, size);
        if (reload) ++epoch;
        if (beforeReturn) beforeReturn();
        return {size, Error::None};
    }
};
Snapshot validSnapshot() {
    Snapshot snapshot;
    snapshot.sessionId = "owned-fixture";
    snapshot.moduleIdentity = "synthetic-fixture-v1";
    snapshot.generation = 1;
    snapshot.layout = Layout::FField;
    snapshot.layoutIdentity = "owned-field-fixture-v1";
    Offset offset;
    offset.value = 0;
    offset.validation = Validation::Validated;
    offset.evidence.push_back({"owned sample value", true, 2, {0}});
    publishOffset(snapshot, "Index", offset);
    return snapshot;
}
int main() {
    {
        auto graph = validSnapshot();
        graph.offsets.clear();
        constexpr unsigned count = 16384;
        for (unsigned i = 0; i < count; ++i) {
            auto offset = validSnapshot().offsets.at("Index");
            if (i + 1 < count) offset.dependencies["Node" + std::to_string(i + 1)] = 1;
            graph.offsets.emplace("Node" + std::to_string(i), std::move(offset));
        }
        REQUIRE(validateSnapshot(graph));
        auto& tail = graph.offsets.at("Node" + std::to_string(count - 1));
        tail.dependencies["Node0"] = 1;
        REQUIRE(validateSnapshot(graph).code == Error::InvalidEvidence);
        tail.dependencies.clear();
        graph.offsets.at("Node17").dependencies["Node17"] = 1;
        REQUIRE(validateSnapshot(graph).code == Error::InvalidEvidence);
        graph.offsets.at("Node17").dependencies.erase("Node17");
        graph.offsets.at("Node18").dependencies["Node9000"] = 1;
        REQUIRE(validateSnapshot(graph));
        graph.offsets.at("Node9000").version = 2;
        REQUIRE(validateSnapshot(graph).code == Error::InvalidEvidence);
    }
    FixtureReader reader;
    std::array<std::byte, 4> output;
    ReadBudget budget;
    budget.generation = 1;
    REQUIRE(readExact(reader, 0, output, budget));
    REQUIRE(readExact(reader, std::numeric_limits<std::uintptr_t>::max(), output, budget).code == Error::Overflow);
    reader.truncate = 1;
    REQUIRE(readExact(reader, 0, output, budget).code == Error::ShortRead);
    reader.truncate = 0;
    reader.failure = Error::PermissionDenied;
    REQUIRE(readExact(reader, 0, output, budget).code == Error::PermissionDenied);
    reader.failure = Error::None;
    budget.remainingBytes = 1;
    REQUIRE(readExact(reader, 0, output, budget).code == Error::BudgetExceeded);
    budget.remainingBytes = 1024;
    budget.deadline = std::chrono::steady_clock::now();
    REQUIRE(readExact(reader, 0, output, budget).code == Error::DeadlineExceeded);
    budget.deadline = std::chrono::steady_clock::time_point::max();
    std::atomic<bool> cancel{true}; budget.cancelled = &cancel;
    REQUIRE(readExact(reader, 0, output, budget).code == Error::Cancelled);
    cancel = false;
    reader.beforeReturn = [&] { cancel = true; };
    REQUIRE(readExact(reader, 0, output, budget).code == Error::Cancelled);
    cancel = false;
    bool completedRead = false;
    reader.beforeReturn = [&] {
        budget.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        std::this_thread::sleep_until(budget.deadline);
        completedRead = true;
    };
    REQUIRE(readExact(reader, 0, output, budget).code == Error::DeadlineExceeded && completedRead);
    budget.deadline = std::chrono::steady_clock::time_point::max(); reader.beforeReturn = {};
    reader.reload = true;
    REQUIRE(readExact(reader, 0, output, budget).code == Error::StaleIdentity);
    reader.reload = false; budget.generation = reader.epoch;
    const std::u16string unicode = u"UE\u4e2d\U0001f600";
    std::memcpy(reader.bytes.data(), unicode.c_str(), (unicode.size() + 1) * sizeof(char16_t));
    std::string decoded;
    REQUIRE(readUtf16(reader, 0, -1, 1, 1024, budget, decoded).code == Error::InvalidArgument);
    // Address zero is a valid reader address, but a null FString data pointer is not.
    std::memcpy(reader.bytes.data() + 16, unicode.c_str(), (unicode.size() + 1) * sizeof(char16_t));
    REQUIRE(readUtf16(reader, 16, unicode.size() + 1, unicode.size() + 1, 1024, budget, decoded));
    REQUIRE(decoded == "UE\xe4\xb8\xad\xf0\x9f\x98\x80");
    std::uint32_t a = 123, b = 456;
    reader.bytes.fill(std::byte{});
    std::memcpy(reader.bytes.data(), &a, 4); std::memcpy(reader.bytes.data() + 32, &b, 4);
    std::array<FieldSample, 2> samples{{{0, a}, {32, b}}};
    std::vector<Offset> candidates;
    REQUIRE(probeUInt32Field(reader, samples, 16, budget, candidates));
    REQUIRE(candidates.size() == 1 && candidates.front().value == 0 && candidates.front().validation == Validation::Validated);
    auto snapshot = validSnapshot();
    REQUIRE(validateSnapshot(snapshot));
    auto exhausted = snapshot;
    exhausted.offsets.at("Index").version = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(publishOffset(exhausted, "Index", snapshot.offsets.at("Index")).code == Error::Overflow);
    REQUIRE(exhausted.offsets.at("Index").version == std::numeric_limits<std::uint64_t>::max());
    auto cyclic = snapshot;
    cyclic.offsets.at("Index").dependencies["Index"] = cyclic.offsets.at("Index").version;
    REQUIRE(validateSnapshot(cyclic).code == Error::InvalidEvidence);
    auto parent = snapshot.offsets.at("Index"); parent.origin = Origin::User;
    REQUIRE(publishOffset(snapshot, "Index", parent));
    parent.origin = Origin::Probe;
    REQUIRE(publishOffset(snapshot, "Index", parent).code == Error::InvalidEvidence);
    auto child = parent; child.dependencies["Index"] = snapshot.offsets.at("Index").version;
    REQUIRE(publishOffset(snapshot, "Child", child));
    parent.origin = Origin::User; parent.value = 4;
    REQUIRE(publishOffset(snapshot, "Index", parent));
    REQUIRE(snapshot.offsets.at("Child").validation == Validation::Stale);
    REQUIRE(validateSnapshot(snapshot).code == Error::InvalidEvidence);

    std::mutex mutex; std::condition_variable entered; bool active = false;
    Session session(validSnapshot());
    const auto oldSnapshot = session.snapshot();
    REQUIRE(session.start([&](Snapshot&, const std::atomic<bool>& cancelled) {
        { std::lock_guard lock(mutex); active = true; } entered.notify_one();
        while (!cancelled.load()) std::this_thread::yield();
        return Status{Error::Cancelled, "Cancelled by fixture"};
    }));
    { std::unique_lock lock(mutex); entered.wait(lock, [&] { return active; }); }
    REQUIRE(session.start([](Snapshot&, const std::atomic<bool>&) { return Status{}; }).code == Error::Busy);
    REQUIRE(session.stop()); REQUIRE(session.stop());
    REQUIRE(oldSnapshot->state == TaskState::Pending && session.snapshot()->state == TaskState::Cancelled);

    Session immutable(validSnapshot());
    Snapshot* workingAlias = nullptr;
    REQUIRE(immutable.start([&](Snapshot& working, const auto&) { workingAlias = &working; return Status{}; }));
    while (immutable.snapshot()->state == TaskState::Running) std::this_thread::yield();
    REQUIRE(immutable.stop());
    const auto published = immutable.snapshot();
    REQUIRE(published->state == TaskState::Succeeded && workingAlias != published.get());
    workingAlias->offsets.at("Index").value = 40;
    workingAlias->offsets.at("Index").evidence.front().check = "changed owned working alias";
    REQUIRE(published->offsets.at("Index").value == 0);
    REQUIRE(published->offsets.at("Index").evidence.front().check == "owned sample value");

    Session stopped(validSnapshot());
    REQUIRE(stopped.stop());
    REQUIRE(stopped.snapshot()->state == TaskState::Cancelled);
    REQUIRE(stopped.start([](Snapshot&, const std::atomic<bool>&) { return Status{}; }).code == Error::Busy);
    Session selfStop(validSnapshot());
    std::atomic<bool> selfRequested{false};
    REQUIRE(selfStop.start([&](Snapshot&, const std::atomic<bool>&) {
        auto result = selfStop.stop();
        selfRequested = result.code == Error::Busy;
        return Status{};
    }));
    std::thread firstStop([&] { selfStop.stop(); });
    std::thread secondStop([&] { selfStop.stop(); });
    firstStop.join(); secondStop.join();
    REQUIRE(selfRequested.load());

    auto root = std::filesystem::temp_directory_path() / ("andueprober-contracts-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::vector<ExportFile> files{{"SDK_A/SDK.hpp", "#pragma once\nstruct Owned { int value; };\n"}, {"SDK_A/Basic.cpp", "#include \"SDK.hpp\"\n"}};
    ExportOptions options{root};
    auto good = publishExport(validSnapshot(), files, options);
    REQUIRE(good.status && std::filesystem::exists(good.publishedDirectory / "completion.json"));
    REQUIRE(publishExport(validSnapshot(), files, options).status.code == Error::Io);
    for (auto operation : {ExportOperation::CreateDirectory, ExportOperation::Open, ExportOperation::Write, ExportOperation::Flush, ExportOperation::Close, ExportOperation::Publish}) {
        auto next = validSnapshot(); next.sessionId += "-" + std::to_string(static_cast<int>(operation));
        options.beforeOperation = [operation](ExportOperation current, const auto&) {
            return current == operation ? Status{Error::Io, "Injected filesystem failure"} : Status{};
        };
        auto failed = publishExport(next, files, options);
        REQUIRE(failed.status.code == Error::Io);
        REQUIRE(failed.publishedDirectory.empty());
        REQUIRE(std::filesystem::exists(good.publishedDirectory / "SDK_A/SDK.hpp"));
        REQUIRE(!std::filesystem::exists(root / next.sessionId));
    }
    options.beforeOperation = [](ExportOperation operation, const auto& directory) {
        if (operation == ExportOperation::Finalize) {
            if (isCompletedExport(directory)) return Status{Error::Internal, "Premature completion marker"};
            return Status{Error::Io, "Interrupted after directory publication"};
        }
        return Status{};
    };
    auto interrupted = validSnapshot(); interrupted.sessionId = "interrupted";
    auto partial = publishExport(interrupted, files, options);
    REQUIRE(partial.status.code == Error::Io && !isCompletedExport(root / "interrupted"));
    REQUIRE(std::filesystem::exists(root / "interrupted/.manifest.pending"));
    REQUIRE(isCompletedExport(good.publishedDirectory));
    options.beforeOperation = {};
    auto next = validSnapshot(); next.sessionId = "cancelled"; cancel = true; options.cancelled = &cancel;
    REQUIRE(publishExport(next, files, options).status.code == Error::Cancelled);
    options.cancelled = nullptr;
    REQUIRE(publishExport(next, {{"../escape", "invalid"}}, options).status.code == Error::InvalidArgument);
    std::filesystem::remove_all(root);
    return 0;
}
