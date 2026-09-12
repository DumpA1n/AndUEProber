# FProperty scalar contracts

`probePropertyFields` observes four FField-based property metadata fields:
`FProperty::ArrayDim`, `ElementSize`, `PropertyFlags`, and `Offset_Internal`.
ArrayDim and ElementSize are positive int32 values; Offset_Internal is a
nonnegative int32 value, including zero. PropertyFlags uses the complete uint64
representation. The operation neither invokes engine functions nor follows
property, container or value pointers.

Three to sixteen distinct named anchors with non-overlapping metadata ranges provide independent expectations, with
at least two values per field. Every sample declares a containing-value identity
and extent separately from the property metadata object's scan extent. Checked
multiplication and addition require `Offset_Internal + ArrayDim * ElementSize`
to fit the containing value. Containing extents are positive and bounded by
16 MiB; metadata scans are bounded by 4096 bytes. A shared containing identity
cannot denote contradictory sizes. No size is derived from the scanned object
extent, a neighboring member or an existing game-profile name.

The four outputs require fourteen current validated dependency versions: Phase 1
Index/Name/Class/Outer, Phase 2 Next/SuperStruct/Children/PropertiesSize/ChildProperties,
and the five FField base fields. An independently declared occupied inherited
prefix contains all five base ranges, with Name/Owner representation bound to
the preceding profile, canonical pool and generation evidence. The prefix is
separate from either metadata scan extent and is not a universal `sizeof(FField)`.
Property candidates begin after the prefix, occupy disjoint ranges and pass
final readback before atomic publication. A new attempt makes automatic
results stale and retains field reports. Matching user overrides require existing
passing validation and every required dependency; value, origin, version and
validation remain unchanged. Candidate/Stale, expired, incomplete and conflicting
overrides fail explicitly. The required dependency
closure and tentative outputs are validated iteratively, bounded by 4096 nodes,
16384 dependency edges and 4 MiB of evidence metadata, using the operation's
deadline and cancellation. Deep invalid dependencies and cycles reject before
publication; unrelated stale results are outside the operation's closure.

`OwnedProperties.hpp` declares a scalar value, an array value and a fixed vector
array. Dimensions come from their compiled array types; element sizes, property
offsets and containing extents come from independent `sizeof`/`offsetof` metadata.
Their storage is not read. Actual native property records have deterministic
padding and embed the owned FField prefix. The complete fixture executes the
production Phase 1, Phase 2, Phase 3, Phase 4 and FField-base probes before this
four-field unit. The embedded owned FField is a complete member, so this fixture
independently declares its compiled size as the occupied inherited prefix.

The fixture covers signed bounds, zero value offsets and flags, high 64-bit flags,
containment failures, arithmetic overflow, identity contradictions, missing/stale
upstream evidence, permission/mapping/short-read errors, budgets, deadlines,
generation changes, cancellation, ambiguity and final-read mutation for all four
fields, inherited-prefix bounds, mismatched Name/Owner provenance, overlapping
sample/candidate ranges, strict overrides and immutable Session
publication. `verification.json` records actual executions, source/artifact hashes,
matching symbols and evidence logs.

```sh
cmake -S . -B build/properties-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDUEPROBER_BUILD_AGENT=OFF -DANDUEPROBER_BUILD_INSPECTOR=OFF \
  -DANDUEPROBER_BUILD_TESTS=ON
cmake --build build/properties-host --target andueprober_properties header_Properties_hpp
ctest --test-dir build/properties-host -R '^andueprober.properties$' --output-on-failure
```

Host ASan/UBSan uses `ANDUEPROBER_SANITIZERS=ON`; Android uses the same fixture
built with the NDK for AArch64 and executed in an owned directory. Actual UE
property metadata, containers, enum reflection, subclass-specific fields,
engine ABI and full SDK acceptance remain separate requirements. These four
scalars do not constitute complete Phase 5 or complete property support.
