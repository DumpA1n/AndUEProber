# Object flag observations

`AndUEProber::Probe` exposes `probeObjectFlags()` through
`<andueprober/ObjectFlags.hpp>`. The caller supplies 3–16 distinct object
addresses, independent identities and exact unsigned 32-bit flag values. At
least two declared values must differ. Zero and every high bit are valid; the
provider does not infer a valid-bit mask or invoke engine functions.

The provider requires little-endian 64-bit addresses and a declared object
extent from 4 to 4096 bytes. Sample extents must be disjoint and cannot overflow
the address space. Current InternalIndex, NamePrivate, ClassPrivate and
OuterPrivate evidence must match the reader generation and module identity.
Their transitive dependencies must be valid. Name evidence also binds the
complete name layout, pool profile, physical pool address and generation.

Validated prefix fields must fit the extent and remain disjoint. The provider
scans 4-byte aligned candidates outside those fields and compares every sample
against its independent metadata. Publication requires exactly one candidate,
a complete final readback and unchanged dependency versions. Offset zero is
supported when it does not overlap a validated prefix field.

All reads share the supplied byte budget, cancellation token, deadline and
generation. Provider errors terminate the operation with their structured
status. Required evidence is bounded to 4096 nodes, 16384 dependencies and 4 MiB
of metadata. Text identities are nonempty UTF-8 without NUL and at most 1024
bytes; combined provenance must also fit that bound. Unrelated stale offsets
do not block this operation.

An admitted retry marks an earlier automatic result stale, including retries
cancelled before their first read. Available candidate and rejection reports
remain inspectable. A user override retains its value, origin, version and
validation only when it matches the observed candidate and has valid evidence
with all current required dependencies. Publication validates the tentative
output closure before replacing the snapshot.

The caller retains immutable input metadata and synchronizes target storage
throughout the call. Matching final bytes do not establish allocator ownership
or rule out an unobservable intermediate write or address reuse.

Owned fixtures execute the actual Phase 1 providers before flag probing. They
cover both layout identities, a native offset-zero flag layout, all flag bits,
corruption, ambiguity, prefix overlap, provider failures, late mutation,
generation changes, strict overrides, transitive evidence failures and
cancellation. Configuration, hashes and execution results are recorded in
`tests/object_flags/verification.json`. External UE applications and engine
calls are outside this fixture's verification scope.

Traceability: R09, R13, R18, C04, E05.
