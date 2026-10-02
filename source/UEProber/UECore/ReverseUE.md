# UE reflection probe model

Public Core/Probe components implement bounded observations and immutable evidence. `UEProber.cpp` serializes commands, `ConfiguredProbeBridge.cpp` executes declared-metadata fixtures, and `DumperBridge.cpp` owns live profile discovery, reflection collection and SDK publication. None establishes universal compatibility across Unreal Engine versions.

## Ownership and reads

One worker owns the ProcessMemory lease, byte budget, deadline and cancellation token. Every read checks address overflow and provider results, then rechecks cancellation, deadline and generation. The module lease identifies mutable engine memory; it does not make that memory a coherent snapshot. UI drawing receives only a deep-copied observation and sends explicit commands to the worker.

Configured operations copy arrays and strings at admission and borrow referenced fixture memory until Stop joins. Expected values are independent declarations; reading an unknown field to manufacture its expected value is not evidence. Automatic operations instead collect independent live relationships from the object registry and FField chains.

## Automatic phases

| Phase | Live relationships and published layout |
|---|---|
| 1 | Indexed UObjects validate `InternalIndex` against their slot. Every outer is a registered object and each outer chain ends at a `Package`. `RF_ClassDefaultObject` is set on `Default__<Class>` objects and clear on classes |
| 2 | Typed UStruct objects validate `UField::Next`, `SuperStruct`, `Children`, `ChildProperties` and `PropertiesSize` |
| 3 | UClass/default-object ownership validates `CastFlags` and `ClassDefaultObject` |
| 4 | Coherent UFunction records validate flags, parameter count/size, return offset and native-function pointer |
| 5 | Independent FField owner/next/class/name relationships, FProperty scalars, base size, subclass base, pointer members and Bool metadata validate the reflection property model |
| 6 | UEnum arrays and names are validated; ProcessEvent is found through executable UObject vtable relationships and recorded separately |

Names are composed as `FName::ToString` does: the display entry when names preserve case, then `_<Number - 1>` when the inline Number is nonzero. Objects that differ only in Number therefore keep distinct names. Object collection skips class default objects, as `GetObjectsOfClass` does by default, because a default object is never linked and its reflected members describe no live struct or function. CoreUObject intrinsic classes are found by exact class, FName and `/Script/CoreUObject` outer, and a duplicate match is refused rather than chosen.

At least three distinct named anchors are required for scalar/relationship fields where the algorithm admits comparison. Ambiguity, invalid text, changed relationships or read failures stop the phase without publishing a partially completed automatic result. User overrides survive rediscovery and must agree with current live evidence. Replacing an upstream value invalidates its dependency closure.

`sizeof(FProperty)` and `FProperty::SubPropertyBase` are inferred from independent pointer-tail and NativeBool anchors, not from a configured constant. `FBoolProperty::FieldSize` is the offset at which every NativeBool anchor holds `01 00 01 ff`; the anchors must agree, and the offset must lie in the aligned slot that starts at `sizeof(FProperty)`. ByteOffset, ByteMask and FieldMask follow it. Delta Force stores one byte before FieldSize, so its quartet starts at `sizeof(FProperty) + 1`. Container pointer members establish key/value/element relationships. They do not claim the complete runtime representation of `TArray`, `TSet`, `TMap`, allocators or element storage.

ProcessEvent discovery validates an executable vtable entry and records its index and module-relative address. Invocation is absent because the repository has no verified target signature, object-ownership proof, game-thread executor or matching allocation/release contract. FName decoding uses bounded pool reads; no FName-to-FString engine call is used.

## Reflection export

After all phases complete, the frozen offset set configures the pinned AndUEDumper collector. The bounded adapter supplies serialized reads only; export performs no unbounded module scan and does not rescan ProcessEvent. The collector gathers UObject-derived and FField-derived reflection, inheritance, properties, enums, functions, parameter layouts and package relationships, then emits the SDK, object inventory, offset evidence and completion manifest into a staging directory.

Publication checks creation, write, flush, close, rename and directory synchronization. A failure retains the preceding completed export. `completion.json` is written only after the staged file inventory is frozen. ProcessEvent is available to generated wrappers as a verified address/index but is never executed by the analysis process.

Configured Core operations and the public DumperAdapter remain useful independent validation paths. Their metadata is an oracle for fixtures, not a requirement for the automatic production workflow.
