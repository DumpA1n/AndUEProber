# Lab guide

## Reproducible configurations

Host `host-debug`, `host-release` and `host-sanitizers` presets build the production Core and Probe libraries and independent header fixtures. `ctest --preset <name>` executes the registered tests. Android `android-debug` and `android-release` require `NDK_HOME` pointing to NDK r29 (`29.0.14206865`) and build AArch64/API 27. Android test executables require an explicitly selected owned environment; host CTest cannot execute Android binaries.

The applicable findings are R09–R13 and R21–R23; evidence categories are C01, C07, C09 and E01, E02, E05, E06, E07. A category is not globally passed because one fixture succeeds.

## Host contracts

`tests/contracts.cpp` calls the actual production libraries. Its checks cover address overflow, short read, permission failure, read budget, deadline and cancellation after a provider returns, generation changes during reads, UTF-16 surrogate conversion, valid offset zero, explicit user override preservation, downstream invalidation, immutable snapshots, duplicate start, self-stop, concurrent stop and stop-before-start. A callback's mutable working alias is changed after join; the independently frozen publication remains unchanged. `tests/allocation_failure.cpp` denies worker allocations after callback execution begins and verifies callback/freeze failures publish `Internal` without an allocating exception fallback.

Export cases preserve a previous successful result while injecting errors at directory creation, file open/write/flush/close, publication and completion finalization. Interruption before marker finalization leaves a directory without `completion.json`, and `isCompletedExport` rejects it. `tests/export_limits.cpp` also fails the final directory flush after the marker exists and exercises successful retraction and an obstructed rollback. The latter returns a failed status with `completionUncertain=true` while the marker remains visible. The operation's error takes precedence over `isCompletedExport`, which detects marker presence without validating contents or durability. These fixtures do not simulate every filesystem, power-loss or hostile concurrent-writer behavior.

## SDK compilation

The pinned `AndUEDumper` `SDKCoreGen.hpp` generator emits owned synthetic flat and chunked `FUObjectArray` layouts. A Core session probes and freezes the input before generation. The observed index offsets, 0 and 12 in the owned flat/chunked fixtures, determine generated UObject padding and offset assertions. `andueprober.generated_sdk_compile` publishes these fixture files through the production exporter, configures `misc/sdk_smoke`, and compiles the generated `.cpp` files and aggregator header. The generated accessors are linked and executed over owned fixture objects. The generated code is an array-layout subset. It is not a complete UE application's reflection export, and it is not executed against an engine.

The fixture is explicitly skipped if the pinned dumper generator is absent. Independently invoking `misc/sdk_smoke` without `SDK_A_DIR/SDK.hpp` or generated source files fails configuration. `andueprober.sdk_missing_is_error` tests that failure contract; its success does not mean an SDK compiled.

## Independent consumers

`tests/consumer` supports both `-DANDUEPROBER_SOURCE=/absolute/source` and installed `-DCMAKE_PREFIX_PATH=/absolute/prefix`. It includes public headers and links only `AndUEProber::Probe`, which propagates Core and thread dependencies. The consumer executes session and command-owner lifecycle checks and links the name-layout and field-publication APIs. Installed validation includes `CMAKE_INSTALL_INCLUDEDIR=include/custom` to verify exported usage requirements follow the configured install layout.

## Android loading fixture

`tests/android_load.cpp` is compiled as `andueprober_android_load`. Its argument is the absolute path to the freshly built agent DSO. On an owned native harness process it checks loading, calls `JNI_OnLoad` with the old reserved key, compares thread counts, signal registrations and dumpability, verifies mismatched target rejection, explicitly initializes the matching process and stops twice. It does not start profile analysis, graphics, input or engine calls.

This fixture does not observe every file or network operation, prove general DSO unload safety, or substitute for an owned Android UE app. Record the executable and DSO hashes, source/dependency revisions, Android API, page size, CPU/device, GPU/driver where relevant, build options and actual exit status. Device results must be distinguished from successful cross-compilation.

## Open acceptance gates

Automatic collection, FField property subclasses, ProcessEvent discovery and full reflection SDK publication are implemented. ProcessEvent invocation remains unavailable because no verified target signature, game-thread executor, object-ownership proof or matching allocator/release contract is admitted. Common bounded providers compile for all 33 profiles, but only profile/version combinations named in runtime evidence are verified. Complete runtime container storage layouts, manually mapped modules, 16 KiB-page devices and Android framework renderer/input integration remain outside current evidence.

## Recorded device execution

Default and explicit-Memory Debug/Release configurations pass owned native fixtures on the recorded PLQ110/API 36/AArch64/4096-byte-page device. Loading checks cover inert JNI, target mismatch, initialize/query and idempotent stop. Separate executables exercise Core/export/name/relation contracts, command ownership, allocation denial and CPU ImGui interaction. The current target record additionally covers automatic six-phase analysis, full reflection export and compilation of every generated translation unit. No run invokes an engine function or validates an Android graphics provider. Source and artifact hashes, exact scope and local evidence paths are recorded in `verification.json`.

## Owned object-array and module fixtures

`tests/object_array.cpp` exercises the production flat/chunked reader and index phase with zero offsets, corrupt counts, insufficient chunks, duplicate pointers, overflow, changed headers, reload generations, budgets, deadlines and cancellation. The same production reader supplies configured Agent index and foundation operations. Other phase results do not inherit validated status from this fixture.

`andueprober_android_index` loads the agent and selects an explicit synthetic array in its own process. Modes `flat` and `chunked` require a completed observation export; `corrupt` requires failure without a completion marker; `missing` requires `AUEP_MISSING_DEPENDENCY` from a default build. The fixture never calls an engine or probes a third-party package.

`andueprober_process_memory` opens `libandueprober_owned_module.so`, establishes a module lease, closes the caller reference, reads an exported fixture value, rejects a stale generation, releases the lease and verifies the module can unload. Eight cycles exercise new generation allocation, wrong-module/address rejection and retry after initialization failure. `tests/memory_consumer` performs the same executable check using installed `AndUEProber::ProcessMemory` and public producer targets. Cross-compilers may require explicit `AndUEProber_DIR` and `AndSwapChainHook_DIR` package paths.

These fixtures passed on PLQ110/API 36/4 KB using the explicitly selected Memory producer source. They verify bounded synthetic analysis and publication, ordinary linker reference lifetime and actual public-provider reads. Full UE reflection, engine calls, arbitrary/manual module loaders, general concurrent mutation consistency, 16 KB devices and full generated UE SDKs remain unverified. Exact producer source fingerprints and executable hashes are in `verification.json`; the working-tree producer is not an advanced gitlink pin.

## Owned name fixtures

`tests/names.cpp` exercises production name-pool reads, FName layout and field probing: ASCII, explicit UTF-8, UTF-16 surrogate pairs, malformed encoding, embedded terminators, header/block bounds, zero offsets, comparison/display indices, nonnegative int32 suffix bounds, duplicate/ambiguous anchors, changed data, reload, permission/short reads, deadlines, cancellation and budget exhaustion. The inline number contract follows the pinned wrapper and the int32 return type of [FName::GetNumber](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/UObject/FName/GetNumber?application_version=5.5); negative encodings are rejected. The validated name result passes through a real Session; a user override invalidates a dependent offset while preserving the frozen snapshot.

`andueprober_android_names` links the actual optional ProcessMemory producer adapter, constructs an owned native name pool and two declared anchors, runs Core name probing in a Session, freezes its result and publishes observations. Its arguments are the owned module DSO and an absolute output root. It does not discover a game profile, execute a UE engine or generate a full SDK. Automatic profiles use the same bounded pool reads through common or target-specific providers; each profile/version still requires runtime admission.

The Android adapter copies module metadata inside `dl_iterate_phdr` and validates the copied path before reading ELF notes through the leased module. This relies on the normal Bionic linker's callback serialization, documented by its [loader implementation](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/dlfcn.cpp). The owned device fixture verifies the supported Bionic path; other Linux loaders are compiled support paths without runtime evidence here.

## Owned object relations

`tests/relations.cpp` calls the production index, name, class and outer phase functions over one independently defined owned object graph, publishes their dependency versions in a real Session and freezes the result. Cases cover zero field/name offsets, null relationships, ambiguous and duplicate anchors, source overflow, unmapped candidate diagnostics, permission failure while reading a candidate, short reads, budgets, deadlines, cancellation, reload, final pointer changes, stale generations at publication, user overrides and transitive invalidation on reprobe. Known failed or cancelled snapshots cannot pass export validation.

`andueprober_android_relations` runs those production phases through the actual public ProcessMemory adapter over an owned native object graph, freezes the Session result and publishes its four validated offsets. It does not discover a game, invoke engine code or establish a full UE UObject ABI. The inspector delegates `ClassPrivate` and `OuterPrivate` algorithms to these functions and projects Core publication state into its UI.

Automatic analysis derives FField/FProperty subclass relationships and ProcessEvent evidence before the full live dumper is admitted. Configured operations keep reflection-model selection and independent metadata explicit. Runtime container storage ABI and ProcessEvent invocation are not inferred from those observations.

The discovery adapter's memory reader, budget and cancellation binding belong to its operation thread. Both automatic and interactive agent operations retain their inspector instance until the owner joins. Interactive commands execute on one persistent worker; the UI renders a deep-copied immutable snapshot, including bounded field reports and rejected-candidate reasons, and has no memory-provider reference.

## Owned command and UI fixtures

`tests/commands.cpp` exercises the production command owner, queue-capacity failures, pending-work cancellation, callback-initiated and concurrent stop, exception closure, single-worker execution, actual four-phase Core observations and frozen override invalidation. `tests/inspector.cpp` creates a compatible caller-owned ImGui context, renders inert frames, sends mouse events to the visible Detect button and checks that the resulting command executes Core phases on a different thread. Additional frames and rendering after cancellation do not start work. This is CPU UI execution; no Android graphics provider or live UE profile is involved.

`andueprober_android_commands` loads the actual agent. Default builds must reject interactive start with `AUEP_MISSING_DEPENDENCY`. Explicit Memory builds start idle, reject invalid command admission, execute stale or unmatched-profile commands in their owner, cancel, join and restore the thread count. The owned process has no matching game profile. Target-runtime evidence separately covers automatic discovery and SDK output; neither path proves an Android rendered overlay or engine invocation.

## Bounded discovery fixtures

[Discovery contracts](discovery.md) describe the owned ELF, actual profile adapter and normal-linker module tests. `andueprober_discovery` executes on host and Android. `andueprober_profile_discovery` is an Android native executable linked to the pinned profile definitions; it uses synthetic memory and never opens a game. `andueprober_process_memory` additionally tests exact module selection, bounded identity/header reads, cancellation, deadline and read budgets across eight owned DSO lease cycles.

## Owned structure metadata

`tests/structs.cpp` calls the production Phase2 implementation after actual Core index/name/class/outer probing. Independently compiled layouts supply expected relationships, payload sizes and optional alignments; candidate offsets come from the reader. Tests cover null relationships, offset zero, UProperty/FField differences, ambiguous fields, cycles, override conflicts, generation/read failures and atomic publication. The exact host/sanitizer/Android check counts and evidence are indexed in `tests/structs/verification.json`.

`andueprober_android_structs` loads the actual Agent and owned module DSO. Arguments are the two absolute DSO paths, a mode and an absolute output root. Explicit-Memory modes `ffield`, `uproperty` and `no-alignment` must publish the corresponding validated field set; `corrupt` must fail without a completion marker. Default mode `missing` must return `AUEP_MISSING_DEPENDENCY` without a worker. Admission rejects excessive counts/extents, address overflow, oversized strings and missing FField metadata; mutating copied caller metadata after start cannot change the observation. The fixture checks Stop joins and releases the module lease. It does not exercise a UE application, engine call or full SDK.

## Owned class metadata

`tests/classes.cpp` probes independently declared uint64 CastFlags and default-object pointer values after the production UObject and UStruct phases. Default objects are deliberately unreadable in the fixture; offset probing compares their declared addresses without dereferencing or invoking them. The pure Core tests cover zero offsets and flag values, independent metadata, ambiguous candidates, stale dependencies, override conflicts, complete readback and atomic publication.

`andueprober_android_classes` loads the actual statically assembled Agent runtime and owned module. Arguments match the struct fixture. Explicit-Memory `ffield` and `uproperty` modes must publish both class fields and prior validated fields once; `corrupt-flags`, `corrupt-default` and `ambiguous` must fail without publishing an intermediate struct export. Default `missing` mode verifies dependency rejection. Caller metadata is overwritten after admission to verify deep-copy ownership; Stop must join and release the module lease. These are owned native metadata fixtures, not a UE runtime or engine ABI fixture.

## Owned function metadata

`tests/functions.cpp` probes five UFunction fields from independent uint32, uint8, uint16, uint16 and pointer metadata after the production UObject/UStruct phases. The owned metadata derives parameter sizes and return positions from separately compiled parameter records. The generator does not infer expected values from the fields being scanned. Unique fields must occupy non-overlapping byte ranges; all five readbacks and dependency checks precede atomic publication. Native addresses are never dereferenced or invoked.

`andueprober_android_functions` executes the configured four-phase Agent pipeline over owned native data. Explicit-Memory `ffield` and `uproperty` modes require one final function observation; `corrupt-flags`, `corrupt-native` and `ambiguous` require failure with no intermediate struct/class export. Default `missing` mode requires dependency rejection. All modes verify native-call count zero and joined ownership. The exact CMake/build/device results distinguish this from a verified UE function signature or full SDK export.

## Owned FField base and declared layout fixtures

`tests/fields.cpp` executes the production five-field probe against independently declared native records, including both pointer/boolean owner orders, physical NamePool and generation mismatches, corruption, cycles, ambiguity and override states. `tests/android_fields.cpp` executes the actual DSO C operation after the four upstream phases. Its success covers only the FField base subset; FProperty and subclass metadata have separate configured operations and complete container layouts remain unavailable.

`tests/android_layout.cpp` executes the function pipeline and frozen data-layout publication through `AUEP_StartFunctionLayoutProbe`. The `SDK_PATH` output names the generated header from that exact run. Compiling `tests/layout_smoke.cpp` with that header compares its size, alignment and five offsets against the separately compiled owned record, then executes a byte-layout check. The Android operation and generated-header compilation are separate acceptance checks. These fixtures neither load a game nor call the observed native function addresses.

## Declared flags, property scalars, enum arrays and UI

`tests/android_object_flags.cpp` executes the actual C operation against independently declared uint32 flags, including zero/high bits, an offset-zero layout, corruption and duplicate candidates. `tests/android_properties.cpp` executes the four FProperty scalar fields after the real configured prerequisites and rejects corrupt sizes, offsets, flags and ambiguity. It declares the occupied FField prefix and containing value size independently.

`tests/android_enums.cpp` executes the complete ordered UEnum::Names operation after UObject/UField probing. Owned values include signed boundaries, negative and large values and aliases; alternate array and entry orders prove explicit layout consumption. Corrupt final values, names, counts and ambiguous fields produce no completed export. These native records are not a running UE engine.

`tests/inspector.cpp` exercises the public command capabilities and read-only snapshot overload using an owned CPU context. `tests/android_inspector.cpp` links the actual Agent's ImGui exports without a second ImGui implementation. Interactive Detect remains explicit; phase/export, candidate selection, overrides and bounded memory inspection enqueue work only through the command owner. Configured sessions render immutable results before and after Stop; further frames remain valid after the input is invalidated and the module lease is released. Neither fixture renders through Android EGL/Vulkan or delivers Android framework input.

`tests/android_property_tails.cpp` executes nine declared property kinds through the actual DSO. Owned Enum/Map columns are reversed and nonadjacent; Class reuses the actual compiled Object prefix and declares a separate MetaClass field. Every target pointer refers to a PROT_NONE owned mapping. Success requires all bounded metadata comparisons without dereferencing those targets; corrupted pointers, ambiguous candidates and invalid occupied prefixes cannot publish a completed result. Bool and Unknown are rejected before a worker starts.

`python3 tests/verify_agent.py --serial <owned-device-serial> --ndk <ndk-r29-path>` rebuilds the explicitly configured default, Memory and no-adapter Debug/Release trees, executes all configured Agent fixtures, verifies device artifact hashes, compiles and runs actual emitted layout headers on host and Android, and executes the three host Inspector/name/relation configurations. It archives first-party inputs, build caches, dependency identities, logs and hashes under `build/slices/<source-digest>`. Missing configured inputs or source changes during execution fail the gate. It does not configure a private dependency or choose a device implicitly. The named builds must already contain the matching explicit provider settings.

`tests/property_values.cpp` observes four independently placed Bool bytes and explicit inline-FName FieldPath metadata using the actual preceding FField/FProperty providers. Pure NativeByte and pure SingleBit samples reject indistinguishable columns. Fixtures cover independent storage bounds, exact physical NamePool identity, original bytes around name decoding, final readback, cancellation, deep dependency failures and report budgets. `tests/android_property_values.cpp` exercises both actual C APIs, deep-copy ownership, corrupted/ambiguous/prefix failures, missing-Memory admission and frozen exports. These declarations do not establish an arbitrary UE version's property representation. [Property metadata contracts](property-values.md) define the supported subset.
