# Inspector package and context contract

`AndUEProber::Inspector` provides `andueprober::Inspector::draw` from `<andueprober/Inspector.hpp>`. The public header includes Core/std types and forward-declares `ImGuiContext`; it exposes no dumper or ImGui implementation types. The `source/UI/Inspector.hpp` forwarding header supports the agent's internal include path.

The command view reads immutable `CommandSession` observations and submits explicit commands. The default overload enables phase and export controls; `InspectorCapabilities` independently disables those controls for providers that do not implement them. `draw(shared_ptr<const Snapshot>, ...)` retains the observation for the frame and exposes no commands or cancellation. The Android Agent uses restricted controls for interactive discovery and the read-only overload for configured sessions, including after the worker has stopped. Automatic collection of independent anchor metadata and full UE SDK export remain unavailable. The caller owns one compatible ImGui 1.92.2b context, selects it with `ImGui::SetCurrentContext`, and calls `draw` between `NewFrame` and `Render` on the context's owner thread. Null or non-current contexts return `InvalidArgument`. The caller owns rendering, input delivery, frame lifetime and context destruction. The Inspector creates no context, renderer, input provider, worker or automatic operation; the separately started command owner executes submitted work.

## Source consumption

```cmake
set(ANDUEPROBER_BUILD_AGENT OFF)
set(ANDUEPROBER_BUILD_TESTS OFF)
set(ANDUEPROBER_BUILD_INSPECTOR ON)
add_subdirectory(AndUEProber)
target_link_libraries(my_view PRIVATE
    AndUEProber::Inspector AndUEProberImGui::Binding)
```

`AndUEProberImGui::Binding` propagates the selected provider's headers and link target to the context owner. With the default provider, it refers to `AndUEProberImGui::Core`. If the caller already builds a compatible ImGui target, set `ANDUEPROBER_IMGUI_TARGET` to that existing target before `add_subdirectory`. This suppresses the default ImGui build and makes both Inspector and its caller use the same provider. The target must propagate its matching `imgui.h`, `imconfig.h`, compile definitions and compiled core implementation. Contexts must not cross independently configured ImGui implementations.

The binding compiles a version check against the selected headers and requires the exact `1.92.2b` string and `19222` version number. CMake target existence and this header check cannot prove that an arbitrary prebuilt provider was compiled with the same effective `imconfig.h`, ABI definitions, data layout or C++ runtime. An installed Inspector and its replacement provider must preserve the producer's configuration; the caller is responsible for that match. `draw` does not recover from ABI mismatches. ImGui's `DebugCheckVersionAndDataLayout` may assert and is not a structured-error compatibility check.

## Installed consumption

```cmake
find_package(AndUEProber 0.2 CONFIG REQUIRED COMPONENTS Inspector)
target_link_libraries(my_view PRIVATE
    AndUEProber::Inspector AndUEProberImGui::Binding)
```

An Inspector producer using the default provider installs the `AndUEProberImGui` package, static archive, headers and original license in the same prefix. `CMAKE_INSTALL_INCLUDEDIR=include/custom` is supported. The prefix can be relocated; consumers provide its new location through `CMAKE_PREFIX_PATH`. Android cross-compilation additionally requires that prefix in `CMAKE_FIND_ROOT_PATH` under the NDK's package lookup rules.

A caller can select its existing target through `ANDUEPROBER_IMGUI_TARGET` before `find_package`, including when the package contains the default provider. The supplied target is used without loading the default ImGui package. An Inspector package produced with a caller target requires an explicit caller target at consumption time; it does not export the producer's private target name or source directory.

`find_package(AndUEProber ... COMPONENTS Core Probe)` does not load Inspector or look for ImGui, even when Inspector is installed in the prefix. Inspector is omitted from producer builds with `ANDUEPROBER_BUILD_INSPECTOR=OFF`; the Core/Probe header and package boundaries remain usable without ImGui.

## CPU consumer verification

`tests/inspector_consumer` supports `ANDUEPROBER_SOURCE` for source embedding and normal package discovery for installed consumption. `INSPECTOR_IMGUI_SOURCE_DIR` builds one caller-owned ImGui target from the explicitly supplied source directory. The consumer creates its own context, compiles the Inspector header without ImGui include paths, calls production `draw`, generates a mouse click, and verifies the resulting command executes on the `CommandSession` worker. Additional frames do not submit extra work. The in-repository CPU fixture clicks disabled phase/export controls and checks that both command and read counts remain unchanged, while Detect still executes on the worker. It also renders the frozen observation after the worker has joined. `tests/android_inspector.cpp` links the actual Agent DSO and its ImGui exports without a second ImGui implementation; it verifies interactive and configured-session dispatch, wrong-thread/context rejection, and read-only frames after caller memory is invalidated and the module lease is released. Its internal configured-provider fixture also verifies that a same-value Candidate override remains unvalidated, blocks reprobing, survives automatic-result clearing and can be explicitly removed before a successful new observation. The CPU source/install consumer uses no process provider. Neither fixture exercises a platform renderer, Android input provider or UE engine call.

The Android fixture links the non-installed `AndUEProberAgentRuntime` static target for its direct owner/override checks. Its public C API and shared-context checks use the Agent DSO. [The fixture evidence](../tests/inspector_link/verification.json) covers default Debug/Release compilation and explicit-Memory Debug/Release execution of both interactive and configured modes. The internal runtime is not an installed consumer API.

```sh
cmake -S tests/inspector_consumer -B build/inspector-consumer -G Ninja \
  -DANDUEPROBER_SOURCE="$PWD" -DCMAKE_BUILD_TYPE=Release
cmake --build build/inspector-consumer
ctest --test-dir build/inspector-consumer --output-on-failure
```

`python3 tests/verify_inspector.py` runs source, relocated installed, custom-include, caller-provider and sanitizer consumers, and checks rejection of mismatched version headers. `--ndk /path/to/android-ndk` adds Android package and consumer cross-builds without running Android binaries. Exact source/artifact hashes and command logs are indexed by `tests/inspector-verification.json`.
