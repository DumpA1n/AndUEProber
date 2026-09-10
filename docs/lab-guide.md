# Lab guide

## Source and build baseline

The review baseline is `74b7c356a0a80719968fcba6fefdfd1cebcc6c76`. Build commands are in [README](../README.md). The review used Android NDK r29 (`29.0.14206865`), Clang 21, CMake/Ninja, C++20 and `arm64-v8a`, with API 27 selected by the project. Dependency pins are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Default Release compilation and linking passed in an isolated source snapshot. Debug compilation and linking also passed. No Android device execution was performed in that review. Build results do not establish runtime compatibility, safe unloading or absence of network activity.

## SDK smoke target

[misc/sdk_smoke/CMakeLists.txt](../misc/sdk_smoke/CMakeLists.txt) creates `sdk_smoke_objs` only when `SDK_A_DIR/SDK.hpp` exists. It compiles generated SDK sources as an object library; it does not link or execute an engine application.

The review had no generated SDK fixture, so this configuration skipped its target. That result is not a test pass. An authorized, versioned SDK fixture is required before the smoke target provides evidence. The current default SDK path contains a third-party package identifier and is not an owned fixture.

## Runtime gap

No first-party owned UE application, fixed engine build or validated deployment procedure is provided by this revision. Device/target selection, scope, loading, UI/worker behavior, engine calls and export error paths require a later controlled experiment. Loading can start automatic work as disclosed in README.

## Evidence requirements

A runtime record should identify the source/dependency revisions, toolchain/options, fixture owner or permission basis, application build, device/API/GPU, operations performed, expected result, actual result and retained artifact hashes. Failed, skipped and unexecuted checks remain separate states. Raw target data and identity documents do not belong in a public repository.
