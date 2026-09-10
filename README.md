# AndUEProber

AndUEProber is an experimental Unreal Engine runtime-analysis tool for Android AArch64. It probes reflection layouts, object/name access and engine-call metadata, displays results through ImGui, and adapts discovered values to a pinned AndUEDumper fork for SDK export.

The intended use is engine analysis and security-tool development on owned or explicitly authorized software. A package profile identifies a code path; it does not establish authorization or verified compatibility with a current application version.

## Current capabilities

| Area | Source | Boundary |
|---|---|---|
| Reflection layouts | [UEProber.cpp](source/UEProber/UEProber.cpp) | Heuristic, profile-dependent results; no general UE-version guarantee |
| Target profiles | [GameProfiles](source/UEProber/GameProfiles) | Contains third-party package identifiers and name-resolution logic; see [scope inventory](docs/research-scope.md) |
| Engine calls | ProcessEvent and name-resolution paths | Execute host code; thread, range and allocation checks are incomplete |
| SDK export | [DumperBridge.cpp](source/UEProber/DumperBridge.cpp) | Replaces an existing output directory; write failures are not fully propagated |
| Graphics and input | Pinned AndSwapChainHook | Inherits the pinned implementation's hook, threading and lifecycle limits |

The [probe model](source/UEProber/UECore/ReverseUE.md) describes current assumptions. No generated SDK is supplied as an independently validated reference fixture.

## Build

Requirements: CMake 3.22.1 or newer, Ninja, Android NDK, recursive submodules. CMake selects `arm64-v8a`, API 27 and C++20.

```sh
export NDK_HOME=/path/to/android-ndk
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/libAndUEProber.so`. The Dumper submodule uses an SSH URL; initialization depends on Git access configuration. Release and Debug compiled and linked with NDK r29 in the repository review. Neither build was validated inside an Android target.

## Startup and side effects

[Library.cpp](source/Library.cpp) uses `JNI_OnLoad` key `1337` to start a detached worker. The key is an injector convention, not an authorization check. Initialization includes library scanning, graphics/input setup, an automatic dump attempt after eight seconds, and a package-dependent detached loop that repeatedly changes BSS memory protection. Scan failure can deliberately crash the host.

External injection is a deployment dependency. Associated injector options include `--memfd`, `--hide`, and `--watch`. Concealment and process watching are not authorization controls. This revision has no validated owned-app deployment walkthrough.

## Known limitations

Probe workers, UI access and export state lack a complete session/lifetime protocol. Signal-based recovery can cross C++ objects and locks. Engine calls lack a verified game-thread executor, and executable-range checks are incomplete. Zero offsets, defaults and confirmed results can be conflated. FName/FString allocation ownership and export replacement/error handling need repair.

The SDK smoke configuration skips its target when generated headers are absent; that is not a passing SDK test. Existing profiles and screenshots are not a versioned runtime compatibility matrix.

## Project documentation

- [Research scope](docs/research-scope.md)
- [Lab guide and verification limits](docs/lab-guide.md)
- [Data handling](docs/data-handling.md)
- [Dependency provenance](THIRD_PARTY_NOTICES.md)
- [Security reporting](SECURITY.md)
- [Contributing](CONTRIBUTING.md)

First-party code is [MIT licensed](LICENSE). Research and maintenance guidance does not amend that license. Third-party components retain their own terms.

No repository security certification or platform access approval is claimed.
