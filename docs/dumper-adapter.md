# Frozen reflection text adapter

`AndUEProber::DumperAdapter` is an optional static C++20 component. It consumes `FrozenReflection`, links `AndUEProber::Core`, and returns a generated header in memory. `buildDumperHeader` does not read processes, initialize a global manager, call engine methods, or write files. `publishExport` owns filesystem publication.

The supported output is a data-layout subset: naturally aligned fixed-width integers, IEEE binary32/binary64, 64-bit address values, enums, fixed arrays, and acyclic by-value record references. Record sizes and alignment come from declared metadata; field offsets and versions come from the frozen analysis evidence. Generated records include explicit padding and `sizeof`, `alignof`, and `offsetof` assertions. Callable methods, inheritance, bitfields, containers and full UE SDK generation are outside this representation.

The header uses namespace `andueprober_sdk` and includes only standard headers. Validated C++ identifiers and typed integers form the generated declarations. Schema identities, metadata source text and evidence source strings are excluded from C++ output. Signed enum bits are emitted as exact signed constants, including the 64-bit minimum; unsigned constants use `ULL`. The pinned upstream emitter renames enumerator `EM_MAX` to `<EnumName>_EM_MAX`; an identifier collision after this qualification returns `Unsupported`.

Standard-header macro spellings, including `NULL`, `offsetof`, integer limit/constant/width macros and the supported platforms' `linux`/`unix` macros, return `Unsupported` in declaration names. Record and field name `EM_MAX` is also rejected. Names are not silently changed to avoid these macros. References to standard and generated types use fully qualified names, so valid names such as `std` and a field sharing a referenced type name remain usable. Consumers must avoid defining additional macros that collide with their declared identifiers.

## Composition

Source consumption enables the component explicitly:

```cmake
set(ANDUEPROBER_BUILD_AGENT OFF)
set(ANDUEPROBER_BUILD_TESTS OFF)
set(ANDUEPROBER_BUILD_INSPECTOR OFF)
set(ANDUEPROBER_BUILD_DUMPER_ADAPTER ON)
add_subdirectory("${ANDUEPROBER_SOURCE}" andueprober)
target_link_libraries(CallerLibrary PRIVATE AndUEProber::DumperAdapter)
```

Installed consumption requires its component:

```cmake
find_package(AndUEProber 0.2 CONFIG REQUIRED COMPONENTS DumperAdapter)
target_link_libraries(CallerLibrary PRIVATE AndUEProber::DumperAdapter)
```

The archive contains the private emitter objects. Installed consumers require Core and the standard thread/runtime libraries; they do not find fmt, ImGui, KittyMemoryEx or the live AndUEDumper package. Core/Probe-only builds omit the adapter by default. Agent builds enable it by default; explicit `ANDUEPROBER_BUILD_DUMPER_ADAPTER=OFF` keeps the optional layout entry unavailable.

## Bounds and failure results

Defaults allow 65,536 input items, 4 MiB of typed metadata strings and 8 MiB of output. The output cap applies to the complete header and each intermediate formatting buffer; its supported range is 1 through 64 MiB. It does not describe total peak heap use. Input records and strings are checked before adapting them. Formatting sinks check capacity before each character, with cancellation/deadline checkpoints at append boundaries and at most every 256 output characters. Intermediate member/function segments use the same bounded sink.

`BudgetExceeded`, `Cancelled`, `DeadlineExceeded`, unsupported representations and internal allocation/formatting errors return an empty header. `DumperOptions` and the immutable frozen input remain valid throughout the call. Cancellation uses the supplied atomic flag. Checkpoints do not interrupt allocation, copying, or thread scheduling; they do not establish a real-time deadline.

## Pinned extraction and notices

The private emitter derives from AndUEDumper revision `3cf3a8a81b6dc42cda1cd9f6c603532f2c90fd6e`. `tools/extract_dumper_emitter.py` verifies the Git revision and SHA-256 of complete source/header, fmt headers and original license files. Exact-match transformations extract only plain data declarations, two `Append` functions and the enum macro helper into a private namespace. The live function-address expression becomes `nullptr`, with nonzero function addresses rejected at the private entry point. Member/function temporary formatting uses bounded buffers. The two enum value expressions use a closed fixed-width integer formatter. No global-manager stub or live wrapper source is compiled into this component.

Configuration produces `generated/dumper-emitter/provenance.json` with source and derived file hashes. `dumperDependencyIdentity()` exposes the pinned revision and derived translation-unit hash. Installations include this provenance file and the original AndUEDumper and fmt licenses. The generated translation unit retains attribution. `BufferFmt.cpp` and its file-writing helpers are not build inputs.

## Validation

`tests/dumper_consumer` builds a caller-owned DSO from source or an installed package. Its executable links only the dynamic loader and calls the DSO. The fixture freezes independently compiled layouts, emits a header, and publishes it through the production exporter. A separate compiler invocation verifies generated record layouts, fixed arrays and signed/unsigned enum extrema. Metadata containing comment terminators and preprocessor text is absent from the generated code.

`tests/dumper_emitter/fixtures.cpp` directly exercises the extracted upstream functions with owned data, including input/output capacity, unsupported values and cancellation. Its 37 checks execute in host Debug, Release and Debug ASan/UBSan, and Android Debug/Release. The sanitizer configuration does not establish LeakSanitizer coverage.

`tests/verify_dumper.py --ndk /path/to/ndk` runs the source, relocated install, generated-code compilation and extraction-rejection matrix. [The exact evidence](../tests/dumper_emitter/verification.json) records selected Core/adapter inputs and hashes separately from supporting package artifacts and unrelated probe APIs.

The recorded Android execution runs the 47-check caller DSO in source Debug/Release and installed Release configurations. Each actual Android-generated header is compiled and executed on both the host and Android. The host-installed generated header is also compiled and executed on Android. Source hashes before and after execution, device artifact hashes, matching symbols and complete commands remain in the runtime archive. These fixtures validate the declared data-layout subset, not a complete reflected UE SDK.

The `host-dumper` CI job runs the host portion of this script, including Debug, Release and sanitizer consumers. The Android CI job cross-compiles the Agent and enabled adapter; it does not execute Android fixtures. Local command evidence and remote CI run status are separate records.
