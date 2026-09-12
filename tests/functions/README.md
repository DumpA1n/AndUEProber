# Function metadata contracts

`probeFunctionFields` observes `UFunction::FunctionFlags`, `NumParms`, `ParmsSize`,
`ReturnValueOffset`, and `Func` using explicit uint32, uint8, uint16, uint16, and
64-bit pointer reads. It requires an explicit module generation, reflection layout,
and extent of 8-4096 bytes. Inputs contain 3-16 independently declared named
function anchors, two distinct expectations for each scalar, and at least two
distinct non-null native addresses. Additional declared null functions are valid.
One function identity cannot denote contradictory addresses.

Parameter extents and return offsets come from independent metadata. Return
offset `0xffff` denotes no return value; other offsets must be below the parameter
extent, including valid offset zero. The parameter count is not used to infer
an ABI, parameter layout, size, or neighboring offset. Every candidate is read
with its own field width. Five unambiguous candidates must occupy disjoint byte
ranges and pass a final readback before publication.

Phase 1 Index/Name/Class/Outer and Phase 2 Next/SuperStruct/Children/PropertiesSize
must have passing validated evidence and current dependency versions. FField
also requires ChildProperties. MinAlignment and ClassFlags/CDO are not semantic
dependencies of these UFunction fields. The complete owned workflow executes
Phase 1, Phase 2, Phase 3 and Phase 4 through their production implementations.

A new attempt invalidates automatic results and retains available field reports.
The five offsets publish together. Matching user overrides must already have
passing validation and every current required dependency version; Candidate,
Stale, incomplete, or contradictory overrides fail while preserving the user's
value, origin, version and validation. Successful `Session` publication deep-copies
the working snapshot. The caller synchronizes mutable memory and metadata during
probing; bounded readback cannot freeze an independently mutating engine.

`OwnedFunctions.hpp` defines native records and separate parameter structs.
Expected parameter sizes and return offsets come from compiled `sizeof` and
`offsetof`; explicit counts identify the independently declared parameter members.
Two native function addresses have known compiled types and a call counter. The
probe never invokes those functions, and the fixture asserts that the counter
remains zero. Comparing an address establishes neither executable ownership nor
a valid engine calling convention. Native function memory is outside the reader's
readable fixture ranges.

`andueprober_functions` tests both reflection layouts, full-width fields,
parameter size zero, no-return sentinel, zero offsets in a separate compact
record, metadata bounds, identity contradictions, stale/missing dependencies,
read and permission failures, corruption, per-field ambiguity, overlapping
candidates, final-read mutation of every field, cancellation, budgets, deadlines,
generation changes, override failures and immutable snapshots. The compact record
does not establish a real UObject layout. `verification.json` records executed
configurations, source and artifact hashes, matching symbols and logs.

```sh
cmake -S . -B build/functions-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDUEPROBER_BUILD_AGENT=OFF -DANDUEPROBER_BUILD_INSPECTOR=OFF \
  -DANDUEPROBER_BUILD_TESTS=ON
cmake --build build/functions-host --target andueprober_functions header_Functions_hpp
ctest --test-dir build/functions-host -R '^andueprober.functions$' --output-on-failure
```

Host ASan/UBSan uses `ANDUEPROBER_SANITIZERS=ON`; the same fixture builds with the
Android NDK for AArch64 and executes in an owned directory. Actual UE application
metadata, engine ABI, executable lifetime, allocator ownership and complete SDK
acceptance remain independently unverified until an owned UE asset validates them.
