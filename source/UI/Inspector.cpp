#include <andueprober/Inspector.hpp>
#include "imgui.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace andueprober {
namespace {
const char* validationName(Validation value) {
    switch (value) {
        case Validation::Candidate: return "Candidate";
        case Validation::Validated: return "Validated";
        case Validation::Rejected: return "Rejected";
        case Validation::Stale: return "Stale";
    }
    return "Unknown";
}
const char* taskName(TaskState value) {
    switch (value) {
        case TaskState::Pending: return "pending";
        case TaskState::Running: return "running";
        case TaskState::Succeeded: return "succeeded";
        case TaskState::Failed: return "failed";
        case TaskState::Cancelled: return "cancelled";
    }
    return "unknown";
}
}
Status Inspector::draw(CommandSession& owner, ImGuiContext* context, bool* open) {
    return draw(owner, context, InspectorCapabilities{}, open);
}
Status Inspector::draw(CommandSession& owner, ImGuiContext* context, const InspectorCapabilities& capabilities, bool* open) {
    return drawView(owner.view(), &owner, context, capabilities, open);
}
Status Inspector::draw(std::shared_ptr<const Snapshot> snapshot, ImGuiContext* context, bool* open) {
    CommandView view; view.snapshot = std::move(snapshot);
    return drawView(view, nullptr, context, {false, false}, open);
}
Status Inspector::drawView(const CommandView& view, CommandSession* owner, ImGuiContext* context,
    const InspectorCapabilities& capabilities, bool* open) {
    if (!view.snapshot) return {Error::InvalidArgument, "An immutable observation is required"};
    if (!context || ImGui::GetCurrentContext() != context)
        return {Error::InvalidArgument, "The caller must select its owned ImGui context"};
    const auto& snapshot = *view.snapshot;
    auto submit = [&](CommandKind kind, std::uint32_t phase = 0) {
        if (!owner) return;
        Command command; command.kind = kind; command.phase = phase;
        command.generation = snapshot.generation;
        if (kind == CommandKind::SetOverride || kind == CommandKind::ClearOverride) command.field = field_.data();
        if (kind == CommandKind::SetOverride) {
            std::uint32_t value = 0;
            const auto end = offset_.data() + std::strlen(offset_.data());
            auto parsed = std::from_chars(offset_.data(), end, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != end) { submissionError_ = "Enter a hexadecimal offset"; return; }
            command.value = value;
        }
        if (kind == CommandKind::InspectMemory) {
            std::uintptr_t address = 0;
            std::uint32_t size = 0;
            const auto addressEnd = address_.data() + std::strlen(address_.data());
            const auto sizeEnd = size_.data() + std::strlen(size_.data());
            auto parsedAddress = std::from_chars(address_.data(), addressEnd, address, 16);
            auto parsedSize = std::from_chars(size_.data(), sizeEnd, size, 10);
            if (parsedAddress.ec != std::errc{} || parsedAddress.ptr != addressEnd || !address ||
                parsedSize.ec != std::errc{} || parsedSize.ptr != sizeEnd || !size || size > 512) {
                submissionError_ = "Enter a hexadecimal address and a decimal size from 1 to 512";
                return;
            }
            command.address = address;
            command.size = size;
        }
        std::uint64_t id;
        submissionError_ = owner->submit(std::move(command), id).message;
    };
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UE Reflection Inspector", open)) { ImGui::End(); return {}; }
    if (owner) {
        ImGui::BeginDisabled(!view.accepting);
        if (ImGui::Button("Detect profile")) submit(CommandKind::Detect);
        ImGui::SameLine();
        ImGui::BeginDisabled(!capabilities.phaseCommands);
        if (ImGui::Button("Run all phases")) submit(CommandKind::ProbeAll);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!capabilities.exportCommand);
        if (ImGui::Button("Validate and export")) submit(CommandKind::Export);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!capabilities.phaseCommands);
        for (std::uint32_t phase = 1; phase <= 6; ++phase) {
            if (phase > 1) ImGui::SameLine();
            const auto label = "Phase " + std::to_string(phase);
            if (ImGui::Button(label.c_str())) submit(CommandKind::ProbePhase, phase);
        }
        ImGui::EndDisabled();
        if (!capabilities.phaseCommands)
            ImGui::TextWrapped("This provider requires independently declared metadata for configured probing.");
        if (!capabilities.exportCommand)
            ImGui::TextWrapped("Full reflected SDK export is unavailable for this provider.");
        ImGui::InputText("Field", field_.data(), field_.size());
        ImGui::InputText("Hexadecimal offset", offset_.data(), offset_.size(), ImGuiInputTextFlags_CharsHexadecimal);
        if (ImGui::Button("Set override")) submit(CommandKind::SetOverride);
        ImGui::SameLine();
        if (ImGui::Button("Remove override")) submit(CommandKind::ClearOverride);
        ImGui::SameLine();
        if (ImGui::Button("Clear automatic results")) submit(CommandKind::ClearResults);
        ImGui::InputText("Memory address", address_.data(), address_.size(), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputText("Memory bytes", size_.data(), size_.size(), ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::Button("Inspect memory")) submit(CommandKind::InspectMemory);
        ImGui::EndDisabled();
        if (ImGui::Button("Cancel session")) owner->cancel();
        ImGui::Text("Worker: %s; pending: %zu; completed command: %llu", view.running ? "running" : "idle",
            view.pending, static_cast<unsigned long long>(view.completed));
        if (!submissionError_.empty()) ImGui::TextWrapped("Submission: %s", submissionError_.c_str());
        if (!view.ownerResult) ImGui::TextWrapped("Owner error %d: %s", static_cast<int>(view.ownerResult.code), view.ownerResult.message.c_str());
    } else {
        ImGui::TextUnformatted("Immutable session observation (read only)");
        ImGui::Text("Session: %s; state: %s", snapshot.sessionId.c_str(), taskName(snapshot.state));
    }
    if (!snapshot.result) ImGui::TextWrapped("Operation error %d: %s", static_cast<int>(snapshot.result.code), snapshot.result.message.c_str());
    ImGui::TextWrapped("Module: %s", snapshot.moduleIdentity.c_str());
    ImGui::Text("Generation: %llu", static_cast<unsigned long long>(snapshot.generation));
    if (ImGui::BeginTable("offsets", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const auto* title : {"Field", "Value", "Validation", "Evidence"}) ImGui::TableSetupColumn(title);
        ImGui::TableHeadersRow();
        for (const auto& [name, offset] : snapshot.offsets) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            if (offset.value) ImGui::Text("0x%X", *offset.value); else ImGui::TextUnformatted("Absent");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(validationName(offset.validation));
            ImGui::TableNextColumn();
            for (const auto& evidence : offset.evidence) ImGui::TextWrapped("%s", evidence.check.c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::TreeNode("Operation messages")) {
        for (const auto& message : snapshot.messages) ImGui::TextWrapped("%s", message.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Bounded field observations")) {
        for (const auto& [field, report] : snapshot.fieldReports) {
            if (!ImGui::TreeNode(field.c_str())) continue;
            ImGui::Text("Examined: %zu; candidates: %zu", report.examinedOffsets, report.candidates.size());
            for (std::size_t index = 0; index < report.candidates.size(); ++index) {
                const auto& candidate = report.candidates[index];
                if (!candidate.value) continue;
                ImGui::PushID(static_cast<int>(index));
                ImGui::Text("0x%X (%s)", *candidate.value, validationName(candidate.validation));
                if (owner && ImGui::Button("Use candidate")) {
                    std::snprintf(field_.data(), field_.size(), "%s", field.c_str());
                    std::snprintf(offset_.data(), offset_.size(), "%X", *candidate.value);
                    submit(CommandKind::SetOverride);
                }
                ImGui::PopID();
            }
            for (const auto& rejected : report.rejected)
                ImGui::TextWrapped("0x%X, error %d, %s: %s", rejected.offset, static_cast<int>(rejected.error),
                    rejected.sampleIdentity.c_str(), rejected.reason.c_str());
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    if (snapshot.memoryInspection && ImGui::TreeNode("Memory inspection")) {
        const auto& inspection = *snapshot.memoryInspection;
        ImGui::Text("Address: 0x%llX; bytes: %zu", static_cast<unsigned long long>(inspection.address),
            inspection.bytes.size());
        for (std::size_t start = 0; start < inspection.bytes.size(); start += 16) {
            char line[128]{};
            auto cursor = std::snprintf(line, sizeof(line), "%016llX  ",
                static_cast<unsigned long long>(inspection.address + start));
            const auto count = std::min<std::size_t>(16, inspection.bytes.size() - start);
            for (std::size_t index = 0; index < 16; ++index)
                cursor += std::snprintf(line + cursor, sizeof(line) - static_cast<std::size_t>(cursor),
                    index < count ? "%02X " : "   ", index < count ? inspection.bytes[start + index] : 0);
            cursor += std::snprintf(line + cursor, sizeof(line) - static_cast<std::size_t>(cursor), " ");
            for (std::size_t index = 0; index < count && static_cast<std::size_t>(cursor + 1) < sizeof(line); ++index) {
                const auto byte = inspection.bytes[start + index];
                line[cursor++] = byte >= 0x20 && byte <= 0x7e ? static_cast<char>(byte) : '.';
            }
            line[cursor] = '\0';
            ImGui::TextUnformatted(line);
        }
        ImGui::TreePop();
    }
    ImGui::End();
    return {};
}
}
