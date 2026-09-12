# Phase 2 structure contracts

`AndUEProber::Probe` exposes `probeStructFields` for complete bounded observation
of `UField::Next`, `UStruct::SuperStruct`, `UStruct::Children` and
`UStruct::PropertiesSize`. The FField layout additionally requires explicit
`UStruct::ChildProperties` expectations. `UStruct::MinAlignment` is observed only
when at least two struct anchors have independently declared scalar expectations.
It is never derived from a neighboring field. Missing optional alignment metadata
does not produce an alignment offset.

`StructProbeProfile` names the independent metadata source, module identity,
generation, reflection layout and separate struct/field scan extents. Each extent
is bounded by 4096 bytes. Inputs contain 2-4 distinct named struct anchors and
3-64 distinct named field anchors. Expected values must originate in independent
metadata, such as owned compiled types; reading an unknown target offset to
manufacture its expected value is unsupported. Duplicate objects or identities,
cycles among supplied Super/Next anchors, missing FField expectations, overflowing
ranges and invalid scalar metadata are rejected before memory reads. This does
not establish that every unsupplied node in an engine graph is acyclic.

Phase 1 Index, Name, Class and Outer fields must have validated evidence and
current dependency versions. Every new Phase 2 offset records all four versions.
Pointer observations use the production relationship probe. Size and alignment
observations read actual bounded uint32 values at each candidate location. Every
field has a `FieldProbeReport` with candidate and rejection information. All
candidate observations are rechecked before publication; read failures, changed
values, cancellation, deadlines, identity changes and changed Phase 1 versions
prevent the new offsets from being published. The publication phase stages the
complete required set before replacing the working snapshot.

Starting a new observation invalidates prior automatic Phase 2 offsets and their
downstream dependents. Failed attempts retain their available reports and leave
prior automatic values stale. User overrides retain their value, origin and
version; disagreement with an unambiguous observation fails the phase. A matching
override requires existing passing validated evidence and every current Phase 1
dependency version. Candidate/Stale overrides and incomplete evidence fail without
implicitly validating or replacing user data. A session
publishes a deep copy of the working snapshot, so subsequent working-data changes
cannot modify a completed observation.

`OwnedStructs.hpp` defines actual native object prefixes, fields and structs.
Its expected sizes and alignments come from `sizeof` and `alignof` of separate
compiled payload types. Expected links come from named fixture declarations,
independently of the mutable memory being probed. Its object prefix uses the
layout exercised by the production Phase 1 relation fixture. `Next` deliberately
differs from `sizeof(OwnedObjectPrefix)`, and `MinAlignment` deliberately differs
from `PropertiesSize + 4`.

The 319 checks cover both layouts, all required fields, missing optional alignment,
independent evidence versions, metadata errors, cycles, duplicate samples,
ambiguity, read and permission failures, overflow, deadline and byte budgets,
generation changes, final-recheck mutation, conflicting and matching user
overrides, immutable snapshots, and cancellation during a real session read.
Neither the scanner nor publication logic is replaced in the fixture.

```sh
cmake --preset host-sanitizers
cmake --build --preset host-sanitizers --target andueprober_structs
ctest --preset host-sanitizers -R '^andueprober.structs$' --output-on-failure
```

The same executable is built for Android AArch64/API 27 and must run in an
explicitly owned environment. `verification.json` records actual configurations
and artifact/source hashes. These synthetic native observations do not verify a
real UE application's metadata, allocator, engine functions or complete SDK.
