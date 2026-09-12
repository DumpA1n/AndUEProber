#include <andueprober/Export.hpp>
#include <andueprober/Evidence.hpp>
#include <andueprober/Probe.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#if COMPONENT_WITH_DUMPER
#include <andueprober/DumperAdapter.hpp>
#endif
#if COMPONENT_WITH_INSPECTOR
#include <andueprober/Inspector.hpp>
#include <imgui.h>
#endif
#if COMPONENT_WITH_MEMORY
#include <andueprober/ProcessMemory.hpp>
#endif

namespace {
struct OwnedReader final : andueprober::MemoryReader {
    std::array<std::uint32_t, 4> values{11, 0, 22, 0};
    std::uint64_t generation() const override { return 1; }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        const auto begin = reinterpret_cast<std::uintptr_t>(values.data());
        if (address < begin || address - begin > sizeof(values) ||
            destination.size() > sizeof(values) - (address - begin))
            return {0, andueprober::Error::Unmapped};
        std::memcpy(destination.data(), reinterpret_cast<const void*>(address), destination.size());
        return {destination.size()};
    }
};
}
extern "C" int andueprober_owned_component(const char* outputRoot) try {
    using namespace andueprober;
    OwnedReader memory;
    const std::array<FieldSample, 2> samples{{
        {reinterpret_cast<std::uintptr_t>(&memory.values[0]), 11, "first compiled object"},
        {reinterpret_cast<std::uintptr_t>(&memory.values[2]), 22, "second compiled object"}}};
    Snapshot initial;
    initial.sessionId = "owned-component-export";
    initial.moduleIdentity = "caller-owned-data";
    initial.layoutIdentity = "compiled-uint32-pair";
    initial.layout = Layout::FField;
    initial.generation = 1;
    Session session(initial);
    if (!session.start([&](Snapshot& working, const std::atomic<bool>& cancelled) {
        ReadBudget budget;
        budget.generation = 1;
        budget.cancelled = &cancelled;
        std::vector<Offset> candidates;
        auto status = probeUInt32Field(memory, samples, 8, budget, candidates);
        if (!status) return status;
        if (candidates.size() != 1 || candidates.front().value != 0)
            return Status{Error::InvalidEvidence, "The compiled field must be uniquely located at zero"};
        return publishOffset(working, "Owned::Value", std::move(candidates.front()));
    })) return 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!session.stop()) return 2;
    auto frozen = session.snapshot();
    if (frozen->state != TaskState::Succeeded || !validateSnapshot(*frozen)) return 3;
    const std::array<std::string, 1> roots{"Owned::Value"};
    if (!validateEvidenceClosure(*frozen, roots)) return 13;
    memory.values.fill(0);
    if (frozen->offsets.at("Owned::Value").value != 0) return 4;
    ExportOptions options;
    options.root = std::filesystem::absolute(outputRoot);
    options.maximumFiles = 1;
    options.maximumFileBytes = 4;
    options.maximumTotalBytes = 4;
    options.maximumManifestBytes = 4096;
    options.maximumMetadataEntries = 32;
    options.maximumPathBytes = 32;
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto published = publishExport(*frozen, {{"value.txt", "zero"}}, options);
    if (!published.status || published.completionUncertain || !isCompletedExport(published.publishedDirectory)) return 5;
    options.maximumTotalBytes = 3;
    const auto rejected = publishExport(*frozen, {{"value.txt", "zero"}}, options);
    if (rejected.status.code != Error::BudgetExceeded || !rejected.incompleteDirectory.empty()) return 6;
#if COMPONENT_WITH_DUMPER
    ReflectionSchema layout;
    layout.identity = "owned-compiled-value";
    layout.records = {{1, "OwnedValue", sizeof(std::uint32_t), alignof(std::uint32_t), "independent uint32 compiler layout", {
        {"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt32, 0, 1}, "Owned::Value"}}}};
    const auto reflection = freezeReflection(*frozen, layout);
    if (!reflection.status || !reflection.snapshot) return 11;
    const auto header = buildDumperHeader(*reflection.snapshot);
    if (!header.status || header.header.find("offsetof(OwnedValue, value) == 0") == std::string::npos) return 12;
#endif
#if COMPONENT_WITH_MEMORY
    ProcessMemory provider;
    if (provider.generation() != 0 || !provider.identity().empty() || provider.providerIdentity().empty()) return 7;
    std::array<std::byte, 1> byte{};
    if (provider.read(0, byte).error != Error::InvalidArgument) return 8;
#endif
#if COMPONENT_WITH_INSPECTOR
    CommandSession owner(*frozen, [](const auto&, auto&, const auto&) { return Status{}; });
    if (!owner.start()) return 9;
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Inspector inspector;
    ImGui::NewFrame();
    const auto draw = inspector.draw(owner, context);
    ImGui::Render();
    const bool valid = draw && ImGui::GetCurrentContext() == context && owner.view().completed == 0;
    ImGui::DestroyContext(context);
    if (!owner.stop() || !valid) return 10;
#endif
    std::puts("PASS: public static components assembled into caller DSO; production field probe, frozen snapshot, bounded export and selected optional APIs");
    return 0;
} catch (...) { return 11; }
