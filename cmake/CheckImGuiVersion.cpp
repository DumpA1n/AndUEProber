#include <imgui.h>
#include <string_view>

static_assert(IMGUI_VERSION_NUM == 19222 && std::string_view(IMGUI_VERSION) == "1.92.2b",
              "AndUEProber Inspector requires ImGui 1.92.2b headers and a matching provider configuration");
