# UE reflection probe model

The public Core/Probe components provide bounded observation and evidence algorithms. [UEProber.cpp](../UEProber.cpp) serializes adapter commands; [ConfiguredProbeBridge.cpp](../ConfiguredProbeBridge.cpp) consumes the public phases in configured Agent operations. [DumperBridge.cpp](../DumperBridge.cpp) owns the normal-linker profile boundary. None establishes universal UE-version compatibility.

## Inputs and ownership

A configured operation supplies an explicit object-array layout, canonical length-prefixed name pool, reflection model, bounded extents and independent expected metadata. Expected values must come from declared owned/profile knowledge; reading the unknown field to manufacture its expected value does not validate an offset. Arrays and strings are copied at admission. Referenced memory remains caller-owned until Stop joins.

One worker owns the ProcessMemory module lease, budget and cancellation token. Every Core read checks overflow, budget, deadline, cancellation and generation before and after the provider returns. Module lifetime keeps the normal loader instance resident; it does not freeze mutable object memory. UI drawing receives only a deep-copied immutable snapshot. The generic command view submits explicit supported commands; configured Agent observations are read only.

Seven first-party automatic profiles use bounded module/object-array discovery. Their custom names require a bounded provider and remain unsupported. The 26 pinned upstream profiles have no admitted bounded discovery provider. Profile identifiers record source inventory and do not grant target authorization.

## Phase boundaries

| Phase | Production boundary | Evidence and limits |
|---|---|---|
| UObject | Core index/name/class/outer and independent uint32 flags | Current validated layout, samples and dependency versions; zero/high-bit flags do not use a guessed mask |
| UField/UStruct | `probeStructFields` through configured Phase2 | Independent Next, SuperStruct, Children, PropertiesSize and applicable ChildProperties metadata; optional MinAlignment; no fabricated UObject size |
| UClass | `probeClassFields` through configured Phase3 | Independent uint64 CastFlags and default-object pointers; atomic publication after readback; default objects are not dereferenced |
| UFunction | `probeFunctionFields` through configured Phase4 | Independent flags, parameter count/size, return offset and native pointer values; five non-overlapping fields publish atomically; no invocation |
| FField | `probeFieldFields` through configured base operation | Names reuse the exact physical pool observation; independent owner/next/class/flags metadata; five fields publish atomically |
| FProperty | `probePropertyFields` through configured property operation | Four scalar fields with independent containing-value bounds and occupied FField prefix; exact preceding owner/name provenance |
| Property subclass pointers | `probePropertyTails` through configured tail operation | Explicit Enum/Array/Set/Map/Object/Struct/Byte/Class/Interface pointer columns; complete FProperty prefix and provenance; pointed-to memory is not accessed |
| Complete container layout/ownership | Complete Phase5 returns Unsupported | Bool, FieldPath, container storage semantics and complete collector remain implementation gates |
| UEnum | `probeEnumNames` through configured enum operation | Complete ordered names and signed int64 values, explicit header/entry/prefix layouts, all-array readback and exact name observation identity |
| ProcessEvent | Unsupported | Requires a verified engine signature and thread/allocator contract |

Configured struct/class/function phases require current validated upstream versions. Ambiguous observations and read failures do not publish partial phase results. Unconfigured phase commands return Unsupported. Reflection model and read extents are explicit metadata; a version number alone is insufficient.

Every configured phase validates the complete required evidence dependency closure before reads and after tentative publication, with bounded node/edge/metadata counts and the same cancellation/deadline. Unrelated stale observations do not block an independent phase. No automatic anchor collector or property-tail ranking path is admitted.

## Results and export

Core offsets distinguish absence from zero, probe results from user overrides, and candidate/validated/stale evidence. Reprobing invalidates downstream versions. User values are preserved; agreement with a read does not establish that an unvalidated user override is validated. Sessions freeze an independent deep copy before publication. The inspector projects Core state without obtaining a memory-reader reference.

`AUEP_StartStructProbe` executes UObject and UStruct phases; `AUEP_StartClassProbe` adds UClass; `AUEP_StartFunctionProbe` adds UFunction. Each operation shares one 64 MiB read budget and 30-second deadline and publishes one frozen observation only after its complete configured pipeline succeeds. A later phase failure does not publish an intermediate result.

Full live SDK export returns Unsupported because complete property/container collection and the full UE reflection model are unavailable. `AUEP_StartFunctionLayoutProbe` emits a frozen declared function data-layout subset using actual observed offsets and independent record size/alignment. Its pinned formatter reads no process memory. `AUEP_StartFieldBaseProbe` adds five FField observations; `AUEP_StartPropertyProbe` adds four FProperty scalars with independently declared bounds. `AUEP_StartEnumProbe` adds complete UEnum array observations after the UObject/UField prerequisites. `AUEP_StartPropertyTailProbe` adds the declared opaque subclass pointer fields. Complete container storage, Bool and FieldPath coverage remain implementation gates. The standalone SDKCoreGen fixture compiles and executes a generated owned object-array subset from frozen index offsets. A complete owned UE reflection SDK remains an acceptance gate.

Signal/longjmp recovery is absent. Raw FName-to-FString and ProcessEvent entry points are not callable through these operations. The separate compiled text-shim executor requires explicit thread, signature, module lease and allocator ownership; its owned native fixtures do not establish a verified UE binding.
