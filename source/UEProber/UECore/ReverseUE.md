# UE reflection probe model

The current implementation is a profile-assisted, heuristic probe in [UEProber.cpp](../UEProber.cpp), with result types in [UEProber.h](../UEProber.h) and a pinned-Dumper adapter in [DumperBridge.cpp](../DumperBridge.cpp). It does not discover every UE layout from zero knowledge and does not establish universal engine-version compatibility.

## Inputs and assumptions

A matched profile supplies object-array access, name resolution and target-specific behavior. The probe reads the host process through the available memory utilities and can call profile functions. The profile list is documented in [research scope](../../../docs/research-scope.md).

Object names, object-array positions, class relationships and expected field values are candidate-selection constraints. The first object entries and values such as an ExecuteUbergraph parameter size are not portable guarantees across arbitrary UE builds. A missing anchor, custom engine layout or changed name resolver can invalidate dependent results.

The implementation distinguishes UObject-derived UProperty instances from FField/FProperty reflection by inspecting object instances. The presence of a property UClass or its CDO alone is insufficient to distinguish the models. This is a runtime heuristic; a version number is not a complete layout description.

## Implemented phases

| Phase | Entry | Candidate outputs and dependencies |
|---|---|---|
| 1: UObject | `Phase1_AutoProbe` | Internal index, name, class, outer and flags, using object-array anchors and profile name resolution |
| 2: UField/UStruct | `Phase2_AutoProbe` | Reflection-model detection, bootstrap UObject size, super, properties size/alignment, children, UField next and applicable ChildProperties |
| 3: UClass | `Phase3_AutoProbe` | Cast flags and class default object, constrained by previously resolved sizes and class identities |
| 4: UFunction | `Phase4_AutoProbe` | Anchor collection, function flags, parameter count/size, return offset and native function pointer |
| 5: FField/FProperty | `Phase5_AutoProbe` | Name, owner, next, class, flags, array dimension, element size, property flags and offsets; subclass and container tails when anchors permit |
| 6: ProcessEvent/UEnum | `Phase6_AutoProbe` | Profile-assisted ProcessEvent virtual index and UEnum name-array probing |

Phase 4 uses named functions such as ReceiveBeginPlay, ReceiveTick, IsValid, PrintString and K2_GetActorLocation to cross-check candidates with different signatures. Their availability, parameter representation and FVector size remain target-dependent.

Phase 5 uses property instances rather than treating FField as a UObject. Name/class/owner/next candidates constrain subsequent property candidates. A subclass member offset is not automatically equivalent to `sizeof(FProperty)`: base layout, padding and per-subclass tail placement can differ. FEnumProperty and container-tail results have dedicated paths; the adapter also has defaults when more specific results are absent.

Phase 6 delegates ProcessEvent discovery to the matched profile and stores the returned index/address. It is not a general implementation of every possible static, stack-trace or signature-based discovery method.

## Results and state limitations

`OffsetResult` stores an offset, size, type, evidence text, `autoDetected` and `confirmed`. The implementation can set `confirmed` automatically; this field does not prove human review. Candidate confidence is a heuristic score, not a calibrated probability.

Some consumers interpret zero as missing even where zero is a valid offset. Defaults, manual values and probed values are not separated by a complete provenance model. Phase status transitions do not consistently establish `Completed` or `Failed` for all paths. The UI must therefore not be used as a complete verification record.

`RunAutoDumpFlow` orchestrates detection, probing and export; export starts a detached worker. Result access, UI operations and workers lack a complete immutable-session and lifetime contract. Signal-based probing can cross nontrivial C++ objects and locks. Address masks and the current `.text` bounds helper do not establish general pointer or executable-range validity.

## Engine calls and export

The engine-version path calls ProcessEvent without a verified game-thread executor and can proceed after a range warning. Name/FString allocation ownership is not fully established. These are active correctness limits, not safe-call guarantees.

DumperBridge copies available values into the matched profile, with defaults and per-subclass overrides. Export deletes the existing package directory before replacing output, and nested writes are not fully checked. See [data handling](../../../docs/data-handling.md).

## Verification boundary

The [lab guide](../../../docs/lab-guide.md) records compilation and the skipped SDK smoke target. A complete probe regression requires an owned, fixed engine fixture with known layouts, module identity, positive/negative anchors and expected output. That fixture and Android runtime result are not provided by this revision.
