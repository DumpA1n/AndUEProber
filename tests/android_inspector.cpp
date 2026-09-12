#include "OwnedAgentObjects.hpp"
#include "OwnedStructs.hpp"
#include "../source/UEProber/UEProber.h"
#include "andueprober/Export.hpp"
#include "imgui.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int threadCount() {
    DIR* directory = opendir("/proc/self/task"); REQUIRE(directory);
    int count = 0; while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path); REQUIRE(file);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
void configuredOverrides() {
    using namespace andueprober;
    OwnedStructs fixture;
    auto snapshot = fixture.initial();
    UEProber prober;
    ConfigureProbeOperation(nullptr);
    ReadBudget budget; budget.generation = fixture.epoch;
    REQUIRE(prober.RunConfiguredStructPhase(fixture, fixture.profile(Layout::FField), fixture.structSamples(),
        fixture.fieldSamples(), budget, snapshot));
    const auto frozen = std::make_shared<const Snapshot>(snapshot);
    const auto offset = *snapshot.offsets.at("UStruct::PropertiesSize").value;
    Command command; command.generation = snapshot.generation;
    command.kind = CommandKind::SetOverride; command.field = "UStruct::PropertiesSize"; command.value = offset;
    REQUIRE(prober.ExecuteCommand(command, snapshot));
    const auto user = snapshot.offsets.at(command.field);
    REQUIRE(user.origin == Origin::User && user.validation == Validation::Candidate && user.value == offset);
    REQUIRE(!prober.RunConfiguredStructPhase(fixture, fixture.profile(Layout::FField), fixture.structSamples(),
        fixture.fieldSamples(), budget, snapshot));
    REQUIRE(snapshot.offsets.at(command.field).origin == Origin::User);
    REQUIRE(snapshot.offsets.at(command.field).validation == Validation::Candidate);
    REQUIRE(snapshot.offsets.at(command.field).version == user.version);
    command.kind = CommandKind::ClearResults; command.value.reset();
    REQUIRE(prober.ExecuteCommand(command, snapshot));
    REQUIRE(snapshot.offsets.size() == 1 && snapshot.offsets.at(command.field).value == offset);
    command.kind = CommandKind::ClearOverride;
    REQUIRE(prober.ExecuteCommand(command, snapshot));
    REQUIRE(snapshot.offsets.at(command.field).origin == Origin::Probe && snapshot.offsets.at(command.field).validation == Validation::Stale);
    command.kind = CommandKind::ClearResults;
    REQUIRE(prober.ExecuteCommand(command, snapshot)); REQUIRE(snapshot.offsets.empty());
    snapshot = fixture.initial();
    REQUIRE(prober.RunConfiguredStructPhase(fixture, fixture.profile(Layout::FField), fixture.structSamples(),
        fixture.fieldSamples(), budget, snapshot));
    REQUIRE(validateSnapshot(snapshot) && frozen->offsets.at("UStruct::PropertiesSize").origin == Origin::Probe);
    REQUIRE(frozen->offsets.at("UStruct::PropertiesSize").validation == Validation::Validated);
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string mode = argv[2];
    REQUIRE(mode == "interactive" || mode == "configured");
    const bool interactive = mode == "interactive";
    const auto baseline = threadCount();
    configuredOverrides();
    const auto modulePath = std::filesystem::canonical(argv[1]).string();
    const auto output = std::filesystem::path(argv[3]) / (mode + "-" + std::to_string(getpid()));
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto address = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(address);
    OwnedRelations graph;
    OwnedAgentObjectConfig config(graph, modulePath, address, "ffield");
    config.indices.session_id = "owned-readonly-inspector";
    REQUIRE(AUEP_DrawInspector(nullptr) == AUEP_NOT_INITIALIZED);
    AUEP_Options options{sizeof(options), getprogname(), output.c_str()};
    REQUIRE(AUEP_Initialize(&options) == AUEP_OK);
    REQUIRE(AUEP_DrawInspector(nullptr) == AUEP_NOT_INITIALIZED);
    REQUIRE((interactive ? AUEP_StartInteractive() : AUEP_StartIndexProbe(&config.indices)) == AUEP_OK);
    REQUIRE((interactive ? AUEP_StartIndexProbe(&config.indices) : AUEP_StartInteractive()) == AUEP_BUSY);
    REQUIRE(AUEP_DrawInspector(nullptr) == AUEP_INVALID_ARGUMENT);
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&] {
        ImGui::NewFrame(); REQUIRE(AUEP_DrawInspector(context) == AUEP_OK); ImGui::Render();
        REQUIRE(ImGui::GetCurrentContext() == context);
        REQUIRE(ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0);
    };
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(x, y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    frame(); frame();
    AUEP_Error otherThread = AUEP_OK;
    std::thread rejected([&] { otherThread = AUEP_DrawInspector(context); }); rejected.join();
    REQUIRE(otherThread == AUEP_BUSY);
    auto* otherContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(otherContext);
    REQUIRE(AUEP_DrawInspector(context) == AUEP_INVALID_ARGUMENT);
    ImGui::DestroyContext(otherContext); ImGui::SetCurrentContext(context);
    AUEP_CommandResult commands{}; commands.struct_size = sizeof(commands);
    AUEP_Result state{};
    if (interactive) {
        REQUIRE(AUEP_QueryCommands(&commands) == AUEP_OK && commands.completed == 0 && commands.accepting);
        const auto& style = ImGui::GetStyle();
        const auto detectWidth = ImGui::CalcTextSize("Detect profile").x + 2 * style.FramePadding.x;
        const auto allWidth = ImGui::CalcTextSize("Run all phases").x + 2 * style.FramePadding.x;
        const auto exportWidth = ImGui::CalcTextSize("Validate and export").x + 2 * style.FramePadding.x;
        const auto x = 20 + style.WindowPadding.x;
        const auto y = 20 + ImGui::GetFontSize() + 2 * style.FramePadding.y + style.WindowPadding.y
            + (ImGui::GetFontSize() + 2 * style.FramePadding.y) / 2;
        click(x + detectWidth + style.ItemSpacing.x + allWidth / 2, y);
        click(x + detectWidth + allWidth + 2 * style.ItemSpacing.x + exportWidth / 2, y);
        click(x + 20, y + ImGui::GetFontSize() + 2 * style.FramePadding.y + style.ItemSpacing.y);
        REQUIRE(AUEP_QueryCommands(&commands) == AUEP_OK && commands.completed == 0 && !commands.pending);
        click(x + detectWidth / 2, y);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do { REQUIRE(AUEP_QueryCommands(&commands) == AUEP_OK); if (commands.completed) break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(commands.completed == 1 && commands.operation.error == AUEP_UNSUPPORTED);
        REQUIRE(AUEP_Query(&state) == AUEP_OK && state.state == AUEP_RUNNING && state.error == AUEP_UNSUPPORTED);
        REQUIRE(!std::filesystem::exists(output));
    } else {
        REQUIRE(AUEP_QueryCommands(&commands) == AUEP_NOT_INITIALIZED);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do { REQUIRE(AUEP_Query(&state) == AUEP_OK); if (state.state != AUEP_RUNNING) break;
            frame(); std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(state.state == AUEP_SUCCEEDED);
        click(65, 57); frame();
        REQUIRE(AUEP_Query(&state) == AUEP_OK && state.state == AUEP_SUCCEEDED);
        REQUIRE(AUEP_QueryCommands(&commands) == AUEP_NOT_INITIALIZED);
    }
    REQUIRE(AUEP_Stop() == AUEP_OK); REQUIRE(AUEP_Stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline);
    REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    // The joined worker's input is invalidated before further snapshot-only frames.
    graph.bytes.fill(std::byte{0});
    frame(); frame(); click(65, 57);
    REQUIRE(AUEP_Query(&state) == AUEP_OK && state.state == AUEP_STOPPED);
    if (!interactive) {
        std::size_t completed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(output)) {
            if (!andueprober::isCompletedExport(entry.path())) continue;
            ++completed; REQUIRE(contents(entry.path() / "observations.txt") == "UObject::InternalIndex = 0\n");
        }
        REQUIRE(completed == 1 && threadCount() == baseline);
    }
    ImGui::DestroyContext(context);
    std::printf("PASS: Agent inspector mode=%s, one caller context, strict configured override lifecycle, thread admission and joined-input release; %s\n",
        mode.c_str(), interactive ? "disabled phase/export buttons and explicit Detect" : "immutable configured observation after input invalidation");
}
