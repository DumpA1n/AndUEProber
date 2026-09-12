# AndUEProber

AndUEProber provides bounded Unreal Engine reflection analysis and SDK generation for Android AArch64 software that is owned or explicitly authorized. Public Core and Probe targets remain independent of Android, ImGui, graphics APIs, AndUEDumper and AndCommon.

## Components

| Target | Current behavior |
|---|---|
| `AndUEProber::Core` | Bounded reads, optional offsets, dependency evidence, immutable sessions, serialized command ownership and transactional export |
| `AndUEProber::Probe` | ELF/AArch64 discovery plus configured UObject, UField, UStruct, UClass, UFunction, FField, FProperty, property-subclass and UEnum observations |
| `AndUEProber::ProcessMemory` | Optional exact-process, exact-module read-only provider with Build ID, process-start, mapping and inode generation checks |
| `AndUEProber::Agent` | Explicit initialize, automatic/configured/interactive start, cancellation, stop and query operations |
| `AndUEProber::Inspector` | Caller-owned ImGui command view for detection, six phases, candidate selection, overrides, bounded memory inspection and export |
| `AndUEProber::DumperAdapter` | Frozen declared-layout formatter for configured operations |
| DumperBridge | Bounded live reflection collection and full AndUEDumper SDK publication for an admitted profile |

## Automatic workflow

`AUEP_Start` and `andueprober_target_runner` perform the same synchronous worker-owned workflow:

1. Select the initialized package profile and one exact `libUE4.so` or `libUnreal.so` mapping.
2. Discover the object registry and name pool through bounded ELF/AArch64 providers.
3. Validate UObject, UField/UStruct, UClass and UFunction fields against live independent relationships.
4. Collect FField/FProperty chains; discover the FProperty base size, subclass base, pointer relationships, Bool metadata and container member relationships.
5. Discover and validate UEnum names and ProcessEvent separately. ProcessEvent is recorded but never invoked.
6. Freeze validated offsets, collect reflection objects through the bounded memory adapter, stage a complete SDK and atomically publish it with a completion manifest.

The 33 compiled profiles use bounded common discovery providers; profile-specific overrides can refine those providers. Profile layouts and name encodings remain version-specific and must pass live validation. The Delta Force profile includes its current encrypted-name contract. A package identifier is source inventory, not proof of authorization or compatibility.

ProcessEvent invocation is deliberately absent. Enabling it requires a verified target signature, object ownership, game-thread executor and matching allocation/release contracts. Property pointer observations establish reflection relationships; they do not establish the target's complete runtime container storage ABI.

The capability-parity matrix is in [functional-migration.md](docs/functional-migration.md). Exact target evidence is indexed by [verification.json](docs/verification.json).

## Build

CMake 3.22.1+, Ninja and C++20 are required. Host components use no Android dependency:

```sh
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug

cmake --preset host-release
cmake --build --preset host-release
ctest --preset host-release

cmake --preset host-sanitizers
cmake --build --preset host-sanitizers
ctest --preset host-sanitizers
```

Android uses NDK r29 (`29.0.14206865`), `arm64-v8a` and API 27. The reproducible analysis presets require an explicit Memory producer:

```sh
git -c credential.helper= submodule update --init
git -C external/AndSwapChainHook -c credential.helper= submodule update --init external/KittyMemory external/KittyMemoryEx
export NDK_HOME=/path/to/android-ndk-r29
export AUEP_MEMORY_SOURCE_DIR=/absolute/path/to/AndSwapChainHook
cmake --preset android-analysis-release
cmake --build --preset android-analysis-release
```

`android-debug` and `android-release` intentionally omit ProcessMemory; analysis starts return `AUEP_MISSING_DEPENDENCY`, while inert loading and lifecycle operations remain usable. `android-analysis-debug` and `android-analysis-release` build the explicit source provider and record its Git revision and content fingerprint. No sibling checkout is selected implicitly. An installed `AndSwapChainHook 0.2` Memory component may instead be selected with `ANDUEPROBER_WITH_PROCESS_MEMORY=ON`.

The read-only external-process runner is produced only by analysis builds:

```sh
adb push build/android-analysis-release/andueprober_target_runner /data/local/tmp/
adb shell /data/local/tmp/andueprober_target_runner \
  <pid> <exact-package> /data/local/tmp/andueprober-output
```

The runner opens the existing procfs read channel and never attaches, injects, changes memory protection, elevates privilege or changes target process state.

## Consume

A source consumer links `AndUEProber::Core` or `AndUEProber::Probe` with `ANDUEPROBER_BUILD_AGENT=OFF`. Installed consumption uses:

```cmake
find_package(AndUEProber 0.2 CONFIG REQUIRED COMPONENTS Core Probe)
target_link_libraries(my_analyzer PRIVATE AndUEProber::Probe)
```

Inspector is selected with `ANDUEPROBER_BUILD_INSPECTOR=ON` or installed `COMPONENTS Inspector`. A caller with a compatible ImGui 1.92.2b target supplies `ANDUEPROBER_IMGUI_TARGET`; otherwise the fixed dependency is built. [Inspector consumption](docs/inspector.md) defines context and rendering ownership.

## Lifecycle and publication

`JNI_OnLoad` only returns `JNI_VERSION_1_6`. `AUEP_Initialize` copies the exact package and absolute output root. `AUEP_Start`, configured starts and `AUEP_StartInteractive` own one joinable worker. `AUEP_Cancel` closes admission and requests cooperative cancellation. `AUEP_Stop` joins and is idempotent. Loading never starts analysis, hooks, graphics, permission loops or detached workers.

Configured probe APIs remain available for independent fixtures and owned metadata. They validate supplied metadata rather than replacing automatic discovery. All operations distinguish absent offsets from valid zero, preserve explicit user overrides, invalidate dependent results on reprobing and publish only immutable snapshots.

SDK publication writes a staging directory, checks every filesystem operation, finalizes `completion.json`, syncs the publication point and preserves the previous successful export on failure. Live reflection reads are bounded by byte budgets, deadlines, cancellation and module generation. Remaining upstream global state is serialized behind the agent composition owner.

The Inspector borrows a caller-selected ImGui context and active frame. Rendering performs no process reads. Explicit commands execute on the persistent analysis worker, and configured sessions expose a read-only frozen observation after input memory and module leases are released. AndUEProber supplies no Android renderer, input provider, Vulkan hook or overlay.

## Documentation

- [Functional migration and parity](docs/functional-migration.md)
- [Bounded discovery](docs/discovery.md)
- [Probe model](source/UEProber/UECore/ReverseUE.md)
- [Lab guide](docs/lab-guide.md)
- [Research scope](docs/research-scope.md)
- [Data handling](docs/data-handling.md)
- [Dependency provenance](THIRD_PARTY_NOTICES.md)
- [Security reporting](SECURITY.md)

First-party code is MIT licensed. No platform approval, target authorization or software security certification is claimed.
