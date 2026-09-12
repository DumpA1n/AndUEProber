# FField base contracts

`probeFieldFields` observes only `FField::NamePrivate`, `Owner`, `Next`,
`ClassPrivate`, and `FlagsPrivate`. It does not implement property descriptors,
container metadata, or complete Phase 5 acceptance. The profile explicitly supplies
module identity, generation, FField layout, object extent and owner representation.
UProperty, tagged owners and unknown owner representations return `Unsupported`.

`SeparateBoolean` declares the relative pointer offset, boolean offset and total
owner size. The pointer is an unmodified 64-bit value, and the disjoint boolean is
a single byte encoded as 0/1. Size is bounded by 64 bytes and the complete owner
range participates in field overlap checks. Neither pointer masking nor presumed
adjacency establishes an owner layout. Owned fixtures exercise pointer/kind offsets
0/8 and 8/0 with both UObject and FField owner kinds.

Three to sixteen distinct named anchors independently specify canonical names,
owner pointer/kind, next pointer, opaque FFieldClass pointer and uint32 flags.
Named identities cannot denote contradictory addresses. Declared Next cycles and
FField-owner cycles among supplied anchors fail before reads. ClassPrivate is
compared without dereferencing or assuming a class-name member at offset zero.
Unsupplied nodes and the types of arbitrary target addresses are not verified.

Names use the production bounded `readFName`/`probeNameField` provider. The input
NameLayout, NamePoolProfile, physical pool address and generation must match
validated Phase 1 name evidence through `nameObservationIdentity`. A second pool
with identical contents remains a different observation source. Pool residency,
module ownership and synchronized metadata lifetime remain caller responsibilities.

All five results require current validated Phase 1 Index/Name/Class/Outer and
Phase 2 Next/SuperStruct/Children/PropertiesSize/ChildProperties versions. Unique
candidates must occupy disjoint byte ranges and pass a complete final readback.
Results publish together. Failed attempts retain reports and make prior automatic
results stale. Matching user overrides require passing validation and every current
required dependency; value, origin, version and validation remain unchanged.
Candidate/Stale, conflicting, incomplete and expired overrides fail explicitly. The required dependency
closure and tentative outputs are validated iteratively, bounded by 4096 nodes,
16384 dependency edges and 4 MiB of evidence metadata, using the operation's
deadline and cancellation. Deep invalid dependencies and cycles reject before
publication; unrelated stale results are outside the operation's closure.

`OwnedFields.hpp` defines deterministic native records, compiled owner layouts,
a bounded canonical pool, and named pointer relationships. Its actual fixture
workflow executes the production Phase 1/2/3/4 probes before the five-field unit.
Opaque FFieldClass and native function addresses are outside its readable ranges;
no engine or fixture native function is invoked. `Session` tests exercise immutable
publication and cancellation while a real reader is active.

The fixture covers zero NamePrivate offset, both owner layouts, metadata bounds,
identity contradictions, unsupported owner kinds, invalid boolean bytes, dependency
and physical pool changes, byte budgets, deadlines, cancellation, generation
changes, permission/mapping/short-read failures, duplicate candidates, per-field
final-read mutation, overlap, strict overrides and downstream invalidation.
Exact execution, source hashes, artifact hashes and matching symbols are in
`verification.json` with logs under `evidence/`.

```sh
cmake -S . -B build/fields-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDUEPROBER_BUILD_AGENT=OFF -DANDUEPROBER_BUILD_INSPECTOR=OFF \
  -DANDUEPROBER_BUILD_TESTS=ON
cmake --build build/fields-host --target andueprober_fields header_Fields_hpp
ctest --test-dir build/fields-host -R '^andueprober.fields$' --output-on-failure
```

Host ASan/UBSan uses `ANDUEPROBER_SANITIZERS=ON`. Android execution uses the same
fixture with the NDK AArch64 configuration in an owned directory. Actual UE object
metadata, complete property/container support, engine ABI, allocator contracts,
and SDK completeness require separate owned-asset verification.
