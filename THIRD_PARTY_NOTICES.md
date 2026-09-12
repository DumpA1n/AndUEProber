# Third-party provenance

Inventory baseline: `74b7c356a0a80719968fcba6fefdfd1cebcc6c76`. This records the checked-in snapshot, not a complete release SBOM. Gitlink revisions remain fixed. The AndUEDumper retrieval URL uses HTTPS and has been retrieved without private credentials.

## Git submodules

| Path | Pinned commit | Configured source |
|---|---|---|
| `external/AndSwapChainHook` | `61e022a850ade7aa173e252a32e4c0059d2a5134` | `https://github.com/DumpA1n/AndSwapChainHook.git` |
| `external/AndUEDumper` | `5db6c1f8b4f6a7e567f8fbd0c35ddf3c3ded52d0` | `https://github.com/DumpA1n/AndUEDumper.git` |

Branch labels in .gitmodules do not replace the gitlink pin. Nested dependencies and license files must be resolved at those exact commits for a release. AndUEProber pins its own AndSwapChainHook snapshot; no adjacent checkout is selected implicitly. The optional public Memory component can use an explicitly supplied producer path or installed package.

## Source attribution gaps

Directory names identify components but do not prove the upstream revision or local patch set. The inventory does not infer a legal incompatibility from missing files. Before distributing binaries, resolve source/build provenance and include the required notices for all linked and nested components. Existing MIT licenses in public repositories remain unchanged.

## Current composition

Core and Probe require only the C++20 runtime and thread library. The Android agent explicitly enumerates the pinned KittyMemoryEx and AndUEDumper sources and links the public Inspector target. Inspector resolves its own ImGui dependency. Swap graphics/input/hook modules and the Dobby binary are not agent build inputs. Complete migration of the remaining pinned internal sources to producer component targets remains open.

KittyMemoryEx is fixed at `aa128114d98cc31fbe0955e9820b91657c6cb87d`; KittyMemory is fixed at `4bc7691498d94a521884f6ff3327cbe56cb5437c` in the nested repository but is not linked by the current agent. The generated SDK subset fixture uses the pinned AndUEDumper `SDKCoreGen.hpp` and `UECoreEmbed.hpp` directly; generated third-party/license text is preserved.

The refactored `AndSwapChainHook::Memory` dependency is not present in gitlink `61e022a850ade7aa173e252a32e4c0059d2a5134`. `ANDUEPROBER_MEMORY_SOURCE_DIR` is an explicit producer-validation override. Its header/implementation fingerprint is recorded separately from immutable pins. Public retrieval of a future updated producer revision remains an acceptance gate. Default builds omit the component and reject analysis start with `AUEP_MISSING_DEPENDENCY`.

## Inspector dependency

The default Inspector provider builds only the four core Dear ImGui translation units from [official v1.92.2b revision `45acd5e0e82f4c954432533ae9985ff0e1aad6d5`](https://github.com/ocornut/imgui/tree/45acd5e0e82f4c954432533ae9985ff0e1aad6d5). The [source archive](https://codeload.github.com/ocornut/imgui/tar.gz/45acd5e0e82f4c954432533ae9985ff0e1aad6d5) has SHA-256 `97484925aec2f4d3e913d6644d46b234f8d6d8d98c6aa9c50109e0f0df772090`. Upstream source and `imconfig.h` are unmodified; `IMGUI_DEFINE_MATH_OPERATORS` is a public compile definition.

The package `AndUEProberImGui` exports `AndUEProberImGui::Core`, records the upstream version/revision, and installs public/internal headers with their embedded stb notices. The unmodified upstream MIT `LICENSE.txt`, copyright Omar Cornut 2014–2025, is installed under `${CMAKE_INSTALL_DATADIR}/licenses/AndUEProberImGui`. No platform renderer or input backend is part of that package. An explicitly supplied `ANDUEPROBER_IMGUI_TARGET` replaces the default provider; its source identity, configuration and notices belong to the caller's dependency selection.

## Frozen reflection emitter

The optional DumperAdapter uses a checked extraction of `UPackageGenerator.hpp` and `UPackageGenerator.cpp` from pinned AndUEDumper revision `5db6c1f8b4f6a7e567f8fbd0c35ddf3c3ded52d0`. The full files have SHA-256 `07968a1ed5126044c5557082fc02fabe7695fe70238362f2a7673898f02b7b6d` and `787a18ff03b0684274b2392e41030ab93c3354a408ab15ff0529375cfa8cd298`, respectively. The generated private translation unit retains CyberCat attribution and applies the explicit transformations described in [the adapter contract](docs/dumper-adapter.md). It compiles the pinned fmt implementation through its three header files; no live wrapper or global manager implementation is part of the adapter.

`tools/extract_dumper_emitter.py` records exact input and derived-source hashes. Original license files are preserved: AndUEDumper MIT, Copyright (c) 2022 CyberCat, SHA-256 `6ad5720f2b9b0f5670438cf7feb7552c7103668fe21fa67a0409af2a2c9c7b95`; fmt, Copyright (c) 2012–present Victor Zverovich and contributors, SHA-256 `07580f2a3b35709ce703d523f447b242f6dfec7582a8c0df102c7fa2849375f8`. Both complete notices and the generated provenance JSON are installed with the adapter.
