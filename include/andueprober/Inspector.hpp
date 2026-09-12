#pragma once
#include "andueprober/Commands.hpp"
#include <array>
#include <string>

struct ImGuiContext;
namespace andueprober {
struct InspectorCapabilities {
    bool phaseCommands = true;
    bool exportCommand = true;
};
// The caller owns a compatible ImGui context, active frame, rendering and input thread.
// This view performs no memory reads, profile discovery, engine calls or file publication.
class Inspector {
public:
    Status draw(CommandSession&, ImGuiContext*, bool* open = nullptr);
    Status draw(CommandSession&, ImGuiContext*, const InspectorCapabilities&, bool* open = nullptr);
    // The snapshot is retained for the frame. This overload exposes no commands
    // or cancellation controls and never calls a worker or memory provider.
    Status draw(std::shared_ptr<const Snapshot>, ImGuiContext*, bool* open = nullptr);
private:
    Status drawView(const CommandView&, CommandSession*, ImGuiContext*, const InspectorCapabilities&, bool*);
    std::array<char, 1025> field_{};
    std::array<char, 17> offset_{};
    std::string submissionError_;
};
}
