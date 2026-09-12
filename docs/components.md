# Component and DSO consumption

| Target | Artifact and interface | Installed component |
|---|---|---|
| `AndUEProber::Core` | PIC static library; bounded reads, evidence, sessions, commands, engine admission and export through public headers | `Core` |
| `AndUEProber::Probe` | PIC static library; data-only reflection probes; propagates Core | `Probe` |
| `AndUEProber::Inspector` | Optional PIC static library; public Inspector header and compatible caller-owned ImGui context | `Inspector` |
| `AndUEProber::DumperAdapter` | Optional PIC static library; frozen typed data layouts emitted through the bounded pinned formatter; propagates Core | `DumperAdapter` |
| `AndUEProber::ProcessMemory` | Optional PIC static library; public process reader and normal-linker lease API; requires the selected public Memory producer | `ProcessMemory` |
| `AndUEProber::Agent` | Optional Android AArch64 shared library; the C API in `andueprober/Agent.h` | `Agent` |

Source consumers disable `ANDUEPROBER_BUILD_AGENT` and `ANDUEPROBER_BUILD_TESTS` when only reusable components are required. `ANDUEPROBER_BUILD_INSPECTOR=ON` selects Inspector; `ANDUEPROBER_WITH_PROCESS_MEMORY=ON` selects an installed public Memory producer. An explicit `ANDUEPROBER_MEMORY_SOURCE_DIR` selects a source producer. No adjacent checkout is discovered implicitly.

The public static components can be assembled into a caller-owned DSO:

```cmake
find_package(AndUEProber 0.2 CONFIG REQUIRED COMPONENTS Core Probe)
add_library(OwnedAnalysis SHARED OwnedAnalysis.cpp)
target_link_libraries(OwnedAnalysis PRIVATE AndUEProber::Probe)
```

`find_package` without components imports Core and Probe. An installed package containing ProcessMemory does not require its producer for Core, Probe or Inspector consumption. `COMPONENTS ProcessMemory` loads its separate export and resolves `AndSwapChainHook::Memory`. Inspector is likewise loaded only when requested; its provider and context contract are defined in [Inspector consumption](inspector.md). Public include directories use `CMAKE_INSTALL_INCLUDEDIR`, including `include/custom`. Relocated consumers supply `CMAKE_PREFIX_PATH`; Android cross-compilers also need the prefixes in `CMAKE_FIND_ROOT_PATH` under the NDK's package lookup rules.

`OPTIONAL_COMPONENTS` preserves available required components when an optional component or its provider is unavailable. Required missing components, an unavailable required provider and unsupported component names such as `DumperEmitter` fail package lookup. `DumperAdapter` has a separate optional export and is described in [the frozen reflection adapter contract](dumper-adapter.md).

The Agent package supports a C-only consumer:

```cmake
project(OwnedAgentConsumer LANGUAGES C)
find_package(AndUEProber 0.2 CONFIG REQUIRED COMPONENTS Agent)
add_executable(OwnedAgentConsumer main.c)
target_link_libraries(OwnedAgentConsumer PRIVATE AndUEProber::Agent)
```

Agent-only package lookup imports the DSO and public include directory. It does not import static Core, Probe, Inspector or ProcessMemory targets or look for their build-time packages. The deployed DSO still needs its ordinary Android runtime dependencies. The C API retains its explicit initialization, single execution owner, cancellation and stop contract; package installation does not change runtime behavior.

`AndUEProberAgentRuntime` is an internal PIC static target with an explicit source list: `UEProber.cpp`, `DumperBridge.cpp` and `ConfiguredProbeBridge.cpp`. The final `AndUEProber` DSO compiles `Library.cpp` as its entry point and links that runtime and the public Inspector. AgentRuntime links the optional DumperAdapter when enabled. `AndUEDumperLib` and `KittyMemoryExLib` remain internal static dependencies; their targets and headers are not exported. The public DumperAdapter consumes only frozen typed layouts and uses separately extracted formatter objects, with no upstream global-manager contract.

Installations retain the repository license and provenance notice. Agent installations additionally retain the original AndUEDumper, KittyMemoryEx, fmt, utfcpp and nlohmann license files under `share/licenses/AndUEProber`; the configured `CMAKE_INSTALL_DATADIR` controls that location. The default ImGui provider installs its own original license. These retained notices do not replace the unresolved provenance gates in `THIRD_PARTY_NOTICES.md`.

`tests/component_consumer` builds a caller-owned DSO and an executable that loads it. The DSO calls the production scalar probe, freezes a session snapshot, validates its [bounded evidence closure](evidence.md), publishes a four-byte file under explicit Export limits and checks a budget rejection. Optional modes create a local ImGui context for production Inspector drawing or instantiate the public ProcessMemory API without opening a process reader. The executable links only the platform loader. Public headers, including `Evidence.hpp` and `Export.hpp`, compile independently.

The full consumer also builds a typed frozen layout from its actual scalar observation and calls DumperAdapter in the same DSO. Inspector, ProcessMemory and DumperAdapter share the producer's Core target. The separate [DumperAdapter consumer](dumper-adapter.md) compiles the generated layout and checks integer extrema.

`tests/agent_consumer` compiles the C header and links the public Agent DSO. Its runtime fixture contains only query, invalid initialization and idempotent stop operations. `tests/verify_components.py --build-type Debug|Release` executes host source, sanitizer and relocated installed consumers and optionally cross-builds Android source/installed consumers. The default build type is Release; `host-sanitized` always uses Debug with ASan/UBSan. Each run has a separate configuration-qualified archive, and `tests/component-verification.json` indexes the latest run. Android binaries are not executed by that runner. It records exact command results, source consistency, selected Memory dependency artifacts and produced artifacts; it does not update Inspector, Engine or device-runtime evidence.

`tests/verify_component_runtime.py --device SERIAL --ndk /path/to/ndk` executes the seven Android consumers from that frozen package manifest. An optional `--manifest` selects an archived producer record. Every executable and loaded DSO must match the producer's SHA-256 before transfer and the installed device digest before execution. The runner records the device fingerprint, API level, page size, native build IDs, commands and runtime logs under a new `artifacts/component-runtime` directory. Source changes after the package build are outside that frozen execution scope. The fixture owns its temporary device directory and does not render an Android UI or invoke UE code.

[Current package acceptance](../tests/component-acceptance.json) identifies both Debug and Release archives, each with 54 package checks and seven actual Android consumers. Both configurations consume explicitly selected Release Memory producer packages whose complete selected source inputs, installation artifacts and matching archive members are recorded separately. The index records 108 package checks, including expected dependency-admission failures, and 14 owned Android consumer executions. Artifact hashes, build IDs, symbol tables and source hashes match their respective frozen records. This does not establish a platform renderer, an actual UE engine binding or immutable public dependency retrieval.
