#include "OwnedRelations.hpp"
#include <andueprober/Inspector.hpp>
#include "imgui.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
int main() {
    const auto renderingThread = std::this_thread::get_id();
    OwnedRelations graph;
    std::atomic<int> calls = 0, reads = 0;
    graph.beforeRead = [&](auto&, auto) { ++reads; };
    Snapshot initial; initial.sessionId = "owned-ui-command"; initial.moduleIdentity = "owned-module";
    initial.layout = Layout::FField; initial.generation = 1;
    CommandSession owner(initial, [&](const Command& command, Snapshot& result, const auto& cancelled) {
        REQUIRE(std::this_thread::get_id() != renderingThread);
        REQUIRE(command.kind == CommandKind::Detect);
        ReadBudget budget; budget.generation = 1; budget.cancelled = &cancelled;
        ++calls;
        return observeOwnedRelations(graph, graph, budget, result);
    });
    REQUIRE(owner.start());
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Inspector inspector;
    REQUIRE(inspector.draw(owner, nullptr).code == Error::InvalidArgument);
    bool restricted = true, readOnly = false;
    std::shared_ptr<const Snapshot> observed;
    auto frame = [&] {
        ImGui::NewFrame();
        if (readOnly) REQUIRE(inspector.draw(observed, context));
        else if (restricted) REQUIRE(inspector.draw(owner, context, {false, false}));
        else REQUIRE(inspector.draw(owner, context));
        ImGui::Render();
        REQUIRE(ImGui::GetCurrentContext() == context);
        REQUIRE(ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0);
    };
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(x, y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    frame(); frame(); REQUIRE(calls == 0 && reads == 0);
    // Button centers use the active font and style of the caller's first row.
    const auto& style = ImGui::GetStyle();
    const auto detectWidth = ImGui::CalcTextSize("Detect profile").x + 2 * style.FramePadding.x;
    const auto allWidth = ImGui::CalcTextSize("Run all phases").x + 2 * style.FramePadding.x;
    const auto exportWidth = ImGui::CalcTextSize("Validate and export").x + 2 * style.FramePadding.x;
    const auto rowX = 20 + style.WindowPadding.x;
    const auto rowY = 20 + ImGui::GetFontSize() + 2 * style.FramePadding.y + style.WindowPadding.y
        + (ImGui::GetFontSize() + 2 * style.FramePadding.y) / 2;
    click(rowX + detectWidth + style.ItemSpacing.x + allWidth / 2, rowY);
    click(rowX + detectWidth + allWidth + 2 * style.ItemSpacing.x + exportWidth / 2, rowY);
    click(rowX + 20, rowY + ImGui::GetFontSize() + 2 * style.FramePadding.y + style.ItemSpacing.y);
    REQUIRE(calls == 0 && reads == 0 && owner.view().completed == 0 && owner.view().pending == 0);
    click(rowX + detectWidth / 2, rowY);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (owner.view().completed != 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(owner.view().completed == 1 && calls == 1);
    const auto frozen = owner.view().snapshot; REQUIRE(validateSnapshot(*frozen));
    const auto completedReads = reads.load(); REQUIRE(completedReads > 0);
    frame(); frame(); REQUIRE(calls == 1 && reads == completedReads);
    restricted = false; frame(); REQUIRE(calls == 1 && reads == completedReads);
    REQUIRE(owner.stop()); frame(); REQUIRE(calls == 1 && reads == completedReads);
    REQUIRE(inspector.draw(std::shared_ptr<const Snapshot>{}, context).code == Error::InvalidArgument);
    REQUIRE(inspector.draw(frozen, nullptr).code == Error::InvalidArgument);
    observed = frozen; readOnly = true;
    frame(); frame(); click(rowX + detectWidth / 2, rowY);
    REQUIRE(calls == 1 && reads == completedReads && owner.view().completed == 1);
    REQUIRE(observed == frozen && frozen->state == TaskState::Succeeded);
    REQUIRE(frozen->offsets.at("UObject::ClassPrivate").value == 8);
    ImGui::DestroyContext(context);
    std::puts("PASS: borrowed ImGui context, disabled provider operations, explicit Detect -> command owner -> Core phases, immutable read-only rendering and no implicit reads");
}
