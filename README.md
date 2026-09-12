# AndUEProber

AndUEProber provides UE reflection analysis components and an experimental Android AArch64 adapter for owned or explicitly authorized software. Public Core and Probe targets are independent of Android, ImGui, graphics APIs, AndUEDumper and AndCommon.

## Components and support

| Target | Current behavior | Verification boundary |
|---|---|---|
| `AndUEProber::Core` | Bounded reads, UTF-16 decoding, optional offsets, evidence, immutable sessions, bounded command owners and transactional export | Host contracts and synthetic owned Android execution |
| `AndUEProber::Probe` | Bounded scalar/object-array index probing, length-prefixed names, class/outer relations, declared UObject flags, UField/UStruct/UClass/UFunction/FField/FProperty metadata and UEnum arrays, ELF/AArch64 discovery candidates and field evidence; consumed by the adapter | Host contracts, generated array SDK compilation/execution and owned Android memory fixtures |
| `AndUEProber::ProcessMemory` | Optional public Memory provider adapter with exact module-name selection, GNU build ID and normal-linker lease generation | Android real reads and eight lease/reload cycles; explicit producer selection required |
| `AndUEProber::Agent` | Android DSO with explicit automatic/index/flags/struct/class/function/field-base/property/property-tail/enum/layout/interactive start, command submission, cancel, stop and query operations | Inert loading, bounded native observations and explicit command failures on PLQ110/API 36/4 KB |
| `AndUEProber::Inspector` | Optional public command view with provider capabilities and snapshot-only observation view; borrows the caller's ImGui context | CPU interaction fixture and independent source/installed consumers; [package and context contract](docs/inspector.md) |
| `AndUEProber::DumperAdapter` | Optional frozen declared scalar, enum, fixed-array and record layout formatter backed by a checked pinned emitter | Actual generated subset headers compile and run; complete UE reflection collection and engine methods are unavailable |
| DumperBridge | Commands execute on one worker with a thread-owned memory adapter | Full profile extraction and owned UE application validation remain incomplete |

The Core `InternalIndex`, canonical `NamePrivate`, `ClassPrivate` and `OuterPrivate` phases carry module, layout and independent sample provenance. Name evidence also identifies the physical pool address and memory generation; FField name probing must reuse that observation identity. Name parsing supports explicitly described length-prefixed pools, UTF-16, ASCII and opt-in UTF-8. Explicit canonical name profiles accept ASCII narrow entries and wide UTF-16; the seven first-party automatic profiles require a custom name contract and cannot use an upstream resolver fallback. Outline-number, encrypted and legacy GNames layouts are unsupported. Profile package/class anchors remain declared assumptions until verified in an owned UE application. Reprobing invalidates the previous automatic value and dependent observations; inspector confirmation derives from the current Core value. Name and class scans retain structured rejected-candidate reasons and abort on operation-level read failures. Automatic profile anchor collection is unavailable; unconfigured phase commands return `Unsupported`. Full live SDK export remains `Unsupported` because complete property/container collection and a full UE reflection model are unavailable. The public DumperAdapter emits the supported frozen declared data-layout subset. Live engine calls and FName-to-FString calls without a verified game-thread executor and matching deallocator are unavailable. This revision does not claim full UE runtime/export compatibility.

## Build and consume

CMake 3.22.1+, Ninja and C++20 are required. The toolchain belongs to the caller. Host components require no submodules or private credentials:

```sh
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
cmake --preset host-release
cmake --build --preset host-release
ctest --preset host-release
```

For the Android agent, initialize the exact public gitlinks and select NDK r29 (`29.0.14206865`):

```sh
git -c credential.helper= submodule update --init
git -C external/AndSwapChainHook -c credential.helper= submodule update --init external/KittyMemory external/KittyMemoryEx
export NDK_HOME=/path/to/android-ndk-r29
cmake --preset android-release
cmake --build --preset android-release
```

The default agent build has no process-memory producer and returns `AUEP_MISSING_DEPENDENCY` from analysis start operations. Inert loading and initialize/stop remain available. The agent targets `arm64-v8a`, API 27. `android-debug` uses the same pinned toolchain configuration. Output is `build/android-release/libAndUEProber.so`; Release is not stripped during linking, so matching symbols remain available. Agent builds enumerate the required KittyMemoryEx sources and link the public Inspector component. Inspector uses a caller-provided ImGui target or its fixed standalone dependency package. The agent does not compile the pinned Swap graphics, input, hook or concealment modules. The declared-layout adapter isolates its checked pinned emitter; profile detection still consumes data-only pinned profile definitions. Complete UE reflection collection is unavailable.

The optional process-memory component consumes `AndSwapChainHook::Memory`. The current pinned Swap gitlink does not provide that refactored target. Until a validated producer revision is pinned, select a producer explicitly:

```sh
cmake --preset android-debug -DANDUEPROBER_MEMORY_SOURCE_DIR=/absolute/public/AndSwapChainHook
cmake --build --preset android-debug
```

Alternatively, `-DANDUEPROBER_WITH_PROCESS_MEMORY=ON` resolves an installed `AndSwapChainHook 0.2` package. No sibling checkout is discovered automatically. Explicit source builds record a Memory source fingerprint; this is not an immutable dependency revision or proof of public retrieval. The pin transition remains open.

A source consumer uses `add_subdirectory()` and links `AndUEProber::Core` or `AndUEProber::Probe`. `ANDUEPROBER_BUILD_AGENT=OFF` and `ANDUEPROBER_BUILD_TESTS=OFF` select the reusable components. Installed consumption:

```sh
cmake --install build/host-release --prefix /tmp/andueprober-install
cmake -S tests/consumer -B build/installed-consumer -DCMAKE_PREFIX_PATH=/tmp/andueprober-install
cmake --build build/installed-consumer
```

The consumer calls `find_package(AndUEProber 0.2 CONFIG REQUIRED)` and links the exported target. `Agent.h` declares the Android DSO C entry points; linking only Core does not provide those entry points.

Inspector is selected explicitly with `ANDUEPROBER_BUILD_INSPECTOR=ON` or installed `COMPONENTS Inspector`. Agent and regression-test builds enable it by default; disabling the component leaves Core/Probe independent of ImGui. Its default source archive is fetched by an exact revision and SHA-256, and its installed headers, static library, package metadata and license form a relocatable dependency chain. A caller with an existing compatible ImGui target supplies `ANDUEPROBER_IMGUI_TARGET` to share that instance. [Inspector consumption](docs/inspector.md) records both forms and the context ownership contract.

## Explicit lifecycle

`JNI_OnLoad` returns `JNI_VERSION_1_6` for every reserved argument. Loading starts no analysis, export, hooks, signal handlers, permission workers or graphics/input providers.

`AUEP_Initialize` copies an exact expected process package and an absolute output root. A mismatch fails. It does not execute profile analysis or create an output directory. `AUEP_Start` owns one joinable automatic session; `AUEP_StartIndexProbe` accepts an explicit object-array profile and publishes only validated index observations, without engine calls or SDK generation. `AUEP_StartInteractive` starts an idle command owner. `AUEP_Submit` copies explicit detect, phase, override, clear or export commands into a queue of at most 16 pending commands. Generation checks reject commands against a stale or uninitialized profile; each command receives a fresh bounded budget on the same worker. `AUEP_QueryCommands` reports completion IDs and failures. `AUEP_Cancel` closes command admission and requests cancellation; `AUEP_Stop` joins and is idempotent. The agent permits one session per DSO lifetime and rejects restart after stop. `AUEP_Query` copies status into caller-owned storage. The owner must stop the session and ensure no caller can enter the DSO before unloading it.

`AUEP_StartObjectFlagProbe` runs the four UObject foundation fields and observes independent uint32 flags under one shared budget. Zero and high-bit flags are valid. The output is `object-flag-observations.txt`; no flag mask or unrelated structure metadata is inferred.

`AUEP_StartStructProbe` accepts copied canonical name, object-array and independent structure metadata. It runs the four Core UObject fields and the configured Phase2 command under one module lease, 64 MiB read budget and 30-second deadline, then exports a frozen `struct-observations.txt`. UField `Next` has its own bounded extent; UStruct `SuperStruct`, `Children`, `PropertiesSize` and FField `ChildProperties` require declared relationships and scalar values. `MinAlignment` is probed only when independent metadata supplies it. The operation does not infer `sizeof(UObject)` or select a reflection model. Missing structure metadata and unsupported profile contracts return `AUEP_UNSUPPORTED`; no full SDK or engine call is admitted. The arrays and strings are copied before start returns; referenced memory remains caller-owned until Stop joins.

`AUEP_StartClassProbe` extends the configured struct operation with three to sixteen independent UClass anchors. CastFlags values and default-object addresses are declared metadata; the probe only compares bounded fields and does not call or dereference those default objects. Both fields publish atomically with current UObject/UStruct dependency versions. Phase1, Phase2 and Phase3 share one budget and produce one frozen `class-observations.txt`; a later phase failure produces no intermediate completed export.

`AUEP_StartFunctionProbe` extends that pipeline with independently declared FunctionFlags, NumParms, ParmsSize, ReturnValueOffset and native pointer values. Return offset `0xffff` means no return value; offset zero remains valid. Five unique, non-overlapping fields must pass readback before publication. The operation shares the original budget and emits only `function-observations.txt` from the final frozen result. Comparing a native pointer does not establish a callable engine signature or authorize invocation.

`AUEP_StartFieldBaseProbe` adds independently declared FField names, owner representation, next/class pointers and uint32 flags. Only an explicit separate pointer plus byte boolean owner representation is supported; unknown or tagged layouts return `AUEP_UNSUPPORTED`. The operation reuses the exact physical NamePool and publishes `field-base-observations.txt`. This completes five base fields, not the full FProperty/container phase. The unconfigured complete Phase5 command returns `Unsupported`.

`AUEP_StartPropertyProbe` adds ArrayDim, ElementSize, PropertyFlags and Offset_Internal from independently declared FProperty records. A separate containing-value size bounds ArrayDim × ElementSize and Offset_Internal. The occupied FField prefix, owner layout and preceding profile/name observation identities are explicit metadata. It publishes `property-observations.txt`; subclass metadata uses separate explicit operations and complete Phase5 remains unavailable.

`AUEP_StartPropertyTailProbe` extends the property pipeline with declared opaque pointer fields for Enum, Array, Set, Map, Object, Struct, Byte, Class and Interface metadata. Enum/Map/Class have two independently located columns; no adjacency or fixed base size is assumed. The complete FProperty occupied prefix and preceding name/owner/scalar observation identities are checked before probing. Pointed-to storage is not read. `property-tail-observations.txt` records only the supported fields; complete container storage/ownership remains unavailable.

`AUEP_StartBoolPropertyProbe` observes four independently located uint8 metadata fields from explicit NativeByte/SingleBit declarations and independent storage bounds. Equivalent columns require an ambiguity error; no default byte order is assumed. `AUEP_StartFieldPathPropertyProbe` accepts only an explicitly declared inline FName representation and matches complete names through the same physical pool. They publish `bool-property-observations.txt` and `field-path-property-observations.txt`. [Declared property metadata](docs/property-values.md) defines these bounded contracts and their limits.

`AUEP_StartEnumProbe` runs the UObject/UField foundation then observes UEnum::Names from complete ordered names and int64 values in three to sixteen independent enum objects. Negative values, INT64 boundaries and aliases are supported. Array/entry layouts and the occupied UField prefix are explicit; all entries and array headers are reread before publication. The operation produces `enum-observations.txt` without Class, Function or Property prerequisites. ProcessEvent remains unsupported.

`AUEP_StartFunctionLayoutProbe` accepts the function pipeline plus a copied, typed single-record schema. Five field offsets come from the final validated observations; record size and alignment are independent caller declarations. The frozen layout emits `SDK/FunctionLayout.hpp` through the pinned detached dumper formatter and publishes it together with the observations, declaration and completion manifest in one operation. No memory is read during formatting. The adapter checks internal data-layout consistency; it does not establish an external UE type's true size or generate a complete reflected SDK. `ANDUEPROBER_BUILD_DUMPER_ADAPTER` is enabled by default for the agent; explicitly disabling it makes this operation report `AUEP_MISSING_DEPENDENCY`.

`AUEP_DrawInspector` borrows a caller-selected ImGui 1.92.2b context with the pinned configuration and data layout. The caller owns the active frame, render/input integration and one drawing thread. The function does not create a context or install a graphics provider. Interactive views disable unavailable phase/export controls while keeping explicit discovery and override commands. Configured sessions use a read-only frozen observation view. Neither path performs process reads or engine execution; drawing after Stop can display the retained final observation.

Core `Session` also permits one execution. Concurrent starts fail with `Busy`; concurrent stops join the same worker. A callback may request stop, which returns `Busy` after cancellation and requires an external owner to join. A callback must not destroy its session. Publication deep-copies the working snapshot; callback aliases cannot mutate the published data. Callback or freeze exceptions report `Internal` without allocating in the exception path. `CommandSession` applies the same worker ownership to multiple serialized commands; cancellation discards pending commands and handlers must cooperate with cancellation. Standard agent operations do not call the upstream live dumper.

## Verification and limitations

`host-sanitizers` enables ASan/UBSan. Tests exercise the production Core/Probe/export implementation, including allocation denial, filesystem-operation failures and an interruption between directory publication and completion marking. Public headers compile independently. Installed consumers cover default and `include/custom` header directories. The SDK fixture probes owned flat/chunked arrays through a Core session, validates its frozen result, then invokes pinned `GenUObjectArray` and generates UObject field padding from the observed index offset (0 and 12 in the owned fixtures). It compiles and executes the generated array accessors. This validates an array-generation subset, not a complete reflected UE SDK. Missing generated SDK and ImGui fixture inputs are explicit failures or explicit CTest skips.

Remaining implementation gates include automatic independent-anchor collection, complete property/container subclasses, bounded providers for the 26 upstream profiles, module handling outside the supported normal-linker lease, a verified UE binding for the compiled-shim executor/allocator contract, full generated SDK compilation from an owned UE application, Android framework renderer/input integration, and immutable public dependency pin transitions. The Agent ImGui fixture validates caller context and CPU draw generation, not a platform renderer. No source or build result establishes third-party target authorization.

- [Bounded module and profile discovery](docs/discovery.md)
- [Lab guide and validation boundaries](docs/lab-guide.md)
- [Research scope](docs/research-scope.md)
- [Data handling](docs/data-handling.md)
- [Dependency provenance](THIRD_PARTY_NOTICES.md)
- [Security reporting](SECURITY.md)
- [Contributing](CONTRIBUTING.md)

First-party code is MIT licensed. No platform approval, cyber-access approval or software certification is claimed.
