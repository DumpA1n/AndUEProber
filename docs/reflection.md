# Frozen reflection layouts

`AndUEProber::Core` provides `freezeReflection()` through
`<andueprober/Reflection.hpp>`. The function returns an owned, immutable
`FrozenReflection` containing a data-layout subset and the analysis evidence
used to resolve its offsets. It performs no memory reads or engine calls.

Every field names an existing `Snapshot::offsets` entry through `offsetSource`.
The entry must be validated, have passed evidence with named samples, and have
current dependency versions. Offset zero is valid. The frozen field records both
the value and its version. Failed, cancelled, stale, cyclic or incomplete analysis
is rejected. All analysis offsets must satisfy these requirements, including
offsets outside the selected layout subset.

Record size, alignment, field type, array count, enum values and their provenance
are independent caller declarations. A probe's readable extent is not a record
size. Validation establishes internal consistency with those declarations; it
does not verify the complete layout of an external UE type. A generated header
describes only the selected fields, with explicit padding for omitted bytes.

## Representation

- Little-endian, 64-bit addresses, IEEE binary32 and binary64.
- Signed and unsigned 8-, 16-, 32- and 64-bit integers, floating-point scalars,
  address values, fixed arrays, enums and records embedded by value.
- Enum values contain the exact underlying-width bits. Signed values use
  two's-complement representation; values are not host `long` values.
- Fields use natural alignment, fit within the declared record size and do not
  overlap. Record alignment is a power of two up to 4096. Record sizes are a
  multiple of their alignment. Dependencies determine output record order.
- Type IDs are nonzero and unique across records and enums. Type names are
  unique C++ identifiers. Field and enum names are distinct within their owner.

Recursive records embedded by value, inheritance, bitfields, unions, packed
fields, callable methods, and ownership of referenced engine allocations have
no representation. Address values are numeric observations, not callable or
dereferenceable pointers. This model does not establish full UE reflection or
full SDK support.

## Bounds and ownership

Default limits are 1024 types, 16384 fields, 16384 enum values, 4 MiB of accounted
metadata, 16 MiB per record or array, 65536 array elements, and a 30-second
deadline. Nonempty text is valid UTF-8 without NUL and at most 1024 bytes;
identifiers use a restricted ASCII C++ identifier form of at most 127 bytes.
Reserved identifiers and C++ keywords are rejected. The metadata budget counts
input entry sizes and text bytes; it is not a bound on allocator overhead or
total process heap usage.

Cancellation and the deadline are checked while validating and copying entries,
and before returning a snapshot. Allocation failure returns `Internal` without
a partial result. Dependency and record ordering use iterative traversal.
Inputs must remain immutable during the call; the returned snapshot is
independent of subsequent input mutation. UI messages and field reports are
excluded from the frozen analysis, while offset evidence is retained.

Schema and provenance strings are data. They are not C++ source and must not be
inserted unescaped into generated comments or string literals. The optional
`AndUEProber::DumperAdapter` consumes the frozen model through a separate target.

## Verification scope

The owned fixture builds an actual Phase 1–4 observation through the production
probe providers, freezes its five native function metadata fields, and checks
versions, immutable ownership and downstream invalidation. Separate declared
layouts exercise every scalar representation, signed enum boundaries, nested
arrays, cycles, invalid evidence and configured limits. Allocation injection
exercises successive allocations in the production freeze operation. The Core
contract fixture also checks a 16384-node dependency chain, cycles and shared
dependencies.

Actual configuration, runtime results and artifact hashes are recorded in
`tests/reflection/verification.json`. Generated C++ compilation is recorded by
the DumperAdapter consumer fixture; freezing a model alone is not a compilation
or engine-layout verification. Traceability: R09, R12, R13, C04, E05, E06.
