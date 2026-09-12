# Enum array observations

`AndUEProber::Probe` exposes `probeEnumNames()` through
`<andueprober/Enums.hpp>`. It observes the `UEnum::Names` offset using
independently declared enum objects and ordered name/value metadata. It does not
invoke engine functions or infer a ProcessEvent signature.

The caller supplies 3–16 distinct enum objects, each with a nonempty ordered
entry list. Entry identities are globally unique; complete names are unique
within an enum. Values are exact signed 64-bit integers, including negative
values, `INT64_MIN`, `INT64_MAX`, and aliases with equal values. A small-value
heuristic is not used.

## Layout and evidence contracts

The platform is little-endian with 64-bit addresses. Array layout declares the
pointer, signed 32-bit count and capacity offsets and a 16–64 byte header size.
Entry layout declares the FName and signed 64-bit value offsets and a stride up
to 128 bytes. Optional offsets distinguish an absent field from offset zero.
Members must fit within their declared storage and remain disjoint.

The reserved `fieldBaseExtent` is independently declared occupied UField prefix
storage. It is not inferred from a candidate or a scan extent and does not claim
a universal `sizeof(UField)`. Current validated InternalIndex, NamePrivate,
ClassPrivate, OuterPrivate and Next ranges must fit within this prefix and be
disjoint. Array candidates start at the next 8-byte boundary after the prefix.
The complete name layout, pool profile, physical pool address and generation
must match preceding Phase 1 name evidence.

Object extents and array capacity ranges must be disjoint. The provider reads
only `count` entries; it does not read unused capacity or establish allocator
ownership. Count must equal the independently declared entry count. Capacity
must contain count and satisfy the configured maximum. Address multiplication
and addition are checked before data access.

Every declared entry is decoded and compared. Candidate raw entry storage and
array headers are retained for final readback. Final validation compares the
header, raw entries, decoded names and signed values, then reads the raw entries
and header again. Publication requires exactly one candidate and current
transitive dependencies. Matching user overrides retain their value, origin,
version and validation only with valid evidence and all required versions.
Failed admitted attempts invalidate earlier automatic results and retain
available rejection reports.

## Bounds and failures

Scan extents are at most 4096 bytes. Default limits allow 4096 entries per enum
and capacity 65536; explicit limits cannot exceed 16384 entries per enum or
capacity 1048576. Total declared entries are at most 16384. Input metadata and
candidate/report state each have a separate 4 MiB accounting budget. Individual
text and combined provenance identities are at most 1024 bytes of nonempty UTF-8
without NUL. These are accounting limits, not total process heap limits.

All reads share the supplied byte budget, cancellation token, deadline and
reader generation. Invalid, unmapped, overflowing or nonmatching candidates
receive rejection records. Permission, short-read, unsupported-provider,
internal, cancellation, deadline and generation failures terminate the operation.
The caller retains immutable inputs and synchronizes target storage throughout
the call. Equal final bytes cannot prove that no unobservable intermediate
write or address reuse occurred.

## Verification scope

Owned fixtures run actual Phase 1/2 providers before the enum operation and use
compiled native array and entry declarations in both member orders. They cover
signed boundaries, aliases, corrupted headers and names, overlapping storage,
ambiguity, final raw/header/name changes, provider failures, deep invalid
dependencies and strict overrides. Repeated valid headers exercise the report
budget while the ordinary read budget remains available.

Configuration, executable hashes and actual execution results are recorded in
`tests/enums/verification.json`. These fixtures do not establish compatibility
with an external UE application, a complete reflected SDK, or engine calls.
Traceability: R09, R13, R18, C04, E05.
