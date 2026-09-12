#include "OwnedRelations.hpp"
#include "andueprober/ProcessMemory.hpp"
#include "andueprober/Export.hpp"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <unistd.h>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto path = std::filesystem::canonical(argv[1]).string();
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto anchor = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(anchor);
    andueprober::ProcessMemory memory; REQUIRE(memory.open(path, anchor));
    REQUIRE(dlclose(module) == 0);
    OwnedRelations fixture;
    andueprober::Snapshot initial;
    initial.sessionId = "relations-" + std::to_string(getpid()); initial.moduleIdentity = memory.identity();
    initial.generation = memory.generation(); initial.layout = andueprober::Layout::FField;
    andueprober::Session session(initial);
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        andueprober::ReadBudget budget; budget.cancelled = &cancelled; budget.generation = memory.generation();
        budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return observeOwnedRelations(memory, fixture, budget, snapshot);
    }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.snapshot()->state == andueprober::TaskState::Running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(session.stop());
    const auto frozen = session.snapshot();
    if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
    REQUIRE(frozen->state == andueprober::TaskState::Succeeded);
    REQUIRE(frozen->offsets.size() == 4);
    REQUIRE(frozen->offsets.at("UObject::ClassPrivate").value == 8);
    REQUIRE(frozen->offsets.at("UObject::OuterPrivate").value == 24);
    andueprober::ExportOptions options; options.root = argv[2];
    options.dependencyRevisions = {{"AndSwapChainHook.Memory", andueprober::ProcessMemory::providerIdentity()}};
    const auto result = andueprober::publishExport(*frozen, {{"observations.txt", "Owned index/name/class/outer observations\n"}}, options);
    REQUIRE(result.status); REQUIRE(andueprober::isCompletedExport(result.publishedDirectory));
    std::puts("PASS: native object relations -> public memory -> four Core phases -> frozen session export; no UE engine calls");
}
