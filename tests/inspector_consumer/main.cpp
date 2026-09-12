#include <andueprober/Inspector.hpp>
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

int main() {
    using namespace andueprober;
    const auto drawingThread = std::this_thread::get_id();
    std::atomic<int> handled{0};
    Snapshot initial;
    initial.sessionId = "public-inspector-consumer";
    initial.moduleIdentity = "owned-fixture";
    initial.generation = 1;
    initial.layout = Layout::FField;
    CommandSession owner(initial, [&](const Command& command, Snapshot& snapshot, const auto&) {
        REQUIRE(std::this_thread::get_id() != drawingThread);
        REQUIRE(command.kind == CommandKind::Detect);
        snapshot.messages.push_back("Owned consumer command completed");
        ++handled;
        return Status{};
    });
    REQUIRE(owner.start());
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
    REQUIRE(inspector.draw(owner, nullptr).code == Error::InvalidArgument);
    auto* other = ImGui::CreateContext();
    ImGui::SetCurrentContext(other);
    REQUIRE(inspector.draw(owner, context).code == Error::InvalidArgument);
    ImGui::DestroyContext(other);
    ImGui::SetCurrentContext(context);
    auto frame = [&] {
        ImGui::NewFrame();
        REQUIRE(inspector.draw(owner, context));
        ImGui::Render();
        REQUIRE(ImGui::GetCurrentContext() == context);
        REQUIRE(ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0);
    };
    frame(); frame();
    REQUIRE(handled == 0);
    io.AddMousePosEvent(65, 57); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (owner.view().completed != 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto completed = owner.view();
    REQUIRE(completed.completed == 1 && handled == 1);
    REQUIRE(completed.snapshot->messages.back() == "Owned consumer command completed");
    frame(); frame();
    REQUIRE(handled == 1);
    const InspectorCapabilities capabilities{false, false};
    ImGui::NewFrame();
    REQUIRE(inspector.draw(owner, context, capabilities));
    ImGui::Render();
    REQUIRE(handled == 1);
    REQUIRE(owner.stop());
    frame();
    REQUIRE(handled == 1);
    ImGui::NewFrame();
    REQUIRE(inspector.draw(completed.snapshot, context));
    ImGui::Render();
    REQUIRE(ImGui::GetCurrentContext() == context && handled == 1);
    ImGui::DestroyContext(context);
    std::puts("PASS: public Inspector, caller context, explicit click, capability selection and immutable snapshot drawing");
}
