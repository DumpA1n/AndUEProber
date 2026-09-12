#include "andueprober/Names.hpp"
#include "andueprober/ProcessMemory.hpp"
#include "andueprober/Export.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    alignas(4) std::array<std::byte, 512> block{};
    const auto entry = [&](std::size_t offset, const char* name) {
        const auto length = std::strlen(name); const auto header = static_cast<std::uint16_t>(length << 6);
        std::memcpy(block.data() + offset, &header, sizeof(header));
        std::memcpy(block.data() + offset + 2, name, length);
    };
    entry(0, "Object"); entry(32, "Package");
    std::array<std::uintptr_t, 1> pool{reinterpret_cast<std::uintptr_t>(block.data())};
    struct Object { std::uint64_t flags; std::uint32_t comparison, number; };
    std::array<Object, 2> objects{{{UINT64_MAX, 0, 0}, {UINT64_MAX, 16, 0}}};
    andueprober::NamePoolProfile profile; profile.identity = "owned-native-name-pool-v1";
    profile.blocks = 0; profile.header = 0; profile.string = 2; profile.blockBits = 8; profile.maximumBlocks = 1;
    andueprober::NameLayout layout{0, {}, 4, 8};
    std::array<andueprober::NameSample, 2> samples{{
        {reinterpret_cast<std::uintptr_t>(&objects[0]), "Object", "owned-object"},
        {reinterpret_cast<std::uintptr_t>(&objects[1]), "Package", "owned-package"}}};
    andueprober::Snapshot initial;
    initial.sessionId = "names-" + std::to_string(getpid()); initial.moduleIdentity = memory.identity();
    initial.generation = memory.generation(); initial.layout = andueprober::Layout::FField;
    initial.layoutIdentity = andueprober::nameLayoutIdentity(layout, profile);
    andueprober::Session session(initial);
    REQUIRE(session.start([&](auto& snapshot, const auto& cancelled) {
        andueprober::ReadBudget budget; budget.cancelled = &cancelled; budget.generation = memory.generation();
        budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        andueprober::FieldProbeReport report;
        if (auto status = andueprober::probeNameField(memory, samples, sizeof(Object), layout,
            reinterpret_cast<std::uintptr_t>(pool.data()), profile, budget, report); !status) return status;
        if (report.candidates.size() != 1 || report.candidates[0].value != offsetof(Object, comparison))
            return andueprober::Status{andueprober::Error::InvalidEvidence, "Owned name offset did not match"};
        return andueprober::publishOffset(snapshot, "UObject::NamePrivate", report.candidates.front());
    }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.snapshot()->state == andueprober::TaskState::Running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(session.stop());
    const auto frozen = session.snapshot();
    if (!frozen->result) std::fprintf(stderr, "%s\n", frozen->result.message.c_str());
    REQUIRE(frozen->state == andueprober::TaskState::Succeeded);
    andueprober::ExportOptions options; options.root = argv[2];
    options.dependencyRevisions = {{"AndSwapChainHook.Memory", andueprober::ProcessMemory::providerIdentity()}};
    const auto result = andueprober::publishExport(*frozen, {{"observations.txt", "Owned FName offset = 8\n"}}, options);
    REQUIRE(result.status); REQUIRE(andueprober::isCompletedExport(result.publishedDirectory));
    std::puts("PASS: owned native name pool -> public memory -> Core phase -> frozen session export; no UE engine calls");
}
