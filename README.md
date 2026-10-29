# AndUEProber

AndUEProber dumps an Unreal Engine SDK from an Android AArch64 game without hand-maintained reflection offsets. It is [AndUEDumper](https://github.com/MJx0/AndUEDumper) with one extra step: before dumping, the offsets in the game profile are probed in live memory and replaced by what was found.

## How it works

1. The game profile is selected by package name. It supplies the GUObjectArray and name pool addresses, the name decoding, and a starting `UE_Offsets` layout, exactly as in AndUEDumper.
2. `ProbeOffsets` locates every UObject, UField, UStruct, UClass, UFunction, FField, FProperty, property-subclass and UEnum offset from objects that exist in every UE 4.18+ build: the CoreUObject intrinsics, `KismetSystemLibrary` and `Actor` functions and their parameters, each with a value the engine fixes. A field that is found replaces the profile value; a field that is not found keeps it. Each result is logged next to the profile value.
3. The AndUEDumper fork in `external/AndUEDumper` dumps with the probed layout and finds GEngine, GWorld and ProcessEvent as upstream does. It also emits the FProperty subclasses the dumped structs use, each with a `static_assert` against the allocation size decoded from its `FFieldClass::ConstructFn`. That size does not come from the probed offsets, so a tail member placed past the engine's object fails the SDK build. A size that cannot be decoded is reported in `Logs.txt`. Each struct is declared with the engine's `UStruct::MinAlignment`, and each function's parameter block places every parameter at its engine offset with explicit padding up to `ParmsSize`; `static_assert`s check both, so a block that `ProcessEvent` would read or write past its end fails the SDK build.

## Build

The Android NDK and CMake 3.22.1+ are required.

```sh
git submodule update --init --recursive
export NDK_HOME=/path/to/android-ndk
cmake -B build -G Ninja
cmake --build build
```

This produces `libAndUEProber.so` and the `andueprober` executable for arm64-v8a.

## Usage

Injected: load `libAndUEProber.so` with [AndKittyInjector](https://github.com/MJx0/AndKittyInjector), which passes `1337` to `JNI_OnLoad`. The dump starts after 8 seconds, logs under the logcat tag `UEDump3r`, and is written to `/sdcard/Android/data/<package>/files/UEDump3r/<package>`.

External, as root, reading `/proc/<pid>/mem`:

```sh
adb push build/andueprober /data/local/tmp/
adb shell /data/local/tmp/andueprober -p <package> -o /data/local/tmp/dump
```

`-i <pid>` selects a process when several share the package name.

## Output

| File | Content |
|---|---|
| `Offsets.hpp` | The probed `UE_Offsets` and the GUObjectArray, name pool, GEngine, GWorld and ProcessEvent addresses |
| `SDK/` | Compilable per-package SDK: `SDK.hpp`, `Packages/<package>.hpp` and `_functions.cpp`, CoreUObject core types |
| `AIOHeader.hpp` | All packages in one browse-only header |
| `script.json` | Function names and addresses for IDA or Ghidra |
| `Objects.txt`, `Logs.txt` | Object list and dump log |

`misc/sdk_smoke` compiles every translation unit of a generated SDK. Compile it with the NDK toolchain: the generated layouts rely on the Itanium tail-padding rules that Android uses and Apple arm64 does not.

## Game profiles

The AndUEDumper profiles are used as they are. `source/GameProfiles` adds Arena Breakout, Delta Force, NiZhan, PUBG, PUBG Mobile HD, Roco Kingdom and Valorant, which share the AArch64 helpers in `IGameProfileAndroid.hpp`. GUObjectArray comes from the reference in `UGameEngine::FinishDestroy` or from byte patterns, whichever first reads as an object array.

Delta Force decodes its encrypted name entries from memory, so it works injected and externally. The other added profiles resolve names by calling the engine's `FName::ToString`, so they work only injected.

Only Delta Force (`com.tencent.tmgp.dfm`) has been verified on a device with this version.
