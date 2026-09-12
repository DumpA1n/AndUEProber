# Functional migration parity

This matrix compares original revision `3304244255c662d5896556c32573de6ad776e751` with the current architecture. Runtime results refer only to evidence indexed by `verification.json`.

| Original capability | Current implementation | State |
|---|---|---|
| Package-profile selection | Exact initialized package plus exact module mapping and generation-bound ProcessMemory lease | Restored and bounded |
| GUObjectArray and name discovery | File/live ELF image, bounded FinishDestroy and shared/profile-specific AArch64 providers | Restored; Delta Force runtime validated |
| Profile name resolution | Direct bounded pool decoding; Delta Force encrypted narrow-name contract | Restored for the validated target; other profiles require runtime admission |
| Phase 1 UObject fields | Independent indexed/name/class/outer/flag relationships | Restored |
| Phase 2 UField/UStruct fields | Typed live structure relationships with coherent pointer and size checks | Restored |
| Phase 3 UClass fields | Cast flags plus class/default-object ownership | Restored |
| Phase 4 UFunction fields | Flags, parameter metadata, return position and executable native pointer validation | Restored |
| Phase 5 FField/FProperty | FField relations, property scalars, automatic base-size/subclass-base discovery and subclass pointer/Bool layouts | Restored for FField reflection |
| Container property discovery | Array/Set/Map element/key/value reflection relationships | Restored as reflection metadata; complete runtime container storage ABI is not claimed |
| Phase 6 UEnum | Live enum array/name/value validation | Restored |
| ProcessEvent discovery | Executable UObject vtable candidate with separate index/address evidence | Restored |
| ProcessEvent and engine invocation | Requires verified signature, ownership, game-thread and allocation/release contracts | Not enabled; no safe admitted target contract exists |
| Reflection-to-SDK dump | Bounded AndUEDumper collection, full package/header/source generation and transactional publication | Restored and target-runtime validated |
| Six-phase UI | Explicit Detect, individual/all phases and export through one command worker | Restored |
| Candidate interaction | Candidate/rejection evidence, candidate selection through preserved override semantics | Restored |
| Memory inspection | Explicit 1-512 byte read-only command with lease, budget, deadline and generation checks | Restored |
| Loading/worker model | Inert JNI load, explicit initialization, joinable owner, cooperative cancellation and idempotent Stop | Replaced with bounded lifecycle |
| Graphics/input ownership | Caller supplies compatible ImGui context, frame, renderer and input | Replaced; no bundled overlay/input/Vulkan hook |

The current workflow does not restore detached workers, signal/longjmp recovery, permission loops, concealment, protection changes or implicit engine calls. Those behaviors are outside the admitted architecture rather than missing parity items.
