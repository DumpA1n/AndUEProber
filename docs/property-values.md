# Declared Bool and FieldPath metadata

`AndUEProber::Probe` provides `probeBoolProperty` and `probeFieldPathProperty` through `PropertyValues.hpp`. They consume bounded `MemoryReader` observations and independent metadata declarations. R09, R13, R18, C04 and E05 apply. Neither operation calls engine code, reads a containing property value, dereferences a class pointer or establishes a complete Phase 5/UE SDK layout.

Both profiles contain the preceding `PropertyProbeProfile`, a separately declared occupied FProperty prefix and a scan extent of at most 4096 bytes. Three to sixteen distinct named metadata objects must have non-overlapping extents. All nine known FField/FProperty ranges must fit their independent prefixes and remain disjoint. Name evidence identifies the physical pool and generation; Owner and scalar evidence identify the complete preceding profiles. `PropertyContext` shares these checks and publication rules with opaque property-tail probing. Operation declarations and complete inherited provenance occupy separate Evidence records; neither string is truncated or concatenated into an oversized identity.

The required transitive evidence closure is limited to 4096 nodes, 16384 dependency edges and 4 MiB of metadata. Input and retained candidate/report metadata each have a separate 4 MiB budget. Names and identities require nonempty valid UTF-8 without NUL, at most 1024 bytes, including combined evidence identities. The supplied read budget, deadline and cancellation flag remain shared across all work. No interrupted call publishes a new automatic field value. Reprobing stales old automatic values and invalidates obsolete dependencies; explicit User values, origin and version remain preserved. A matching User value requires validated current evidence and every prerequisite version.

## Bool bytes

`BoolPropertySample` declares `BoolEncoding`, `FieldSize`, `ByteOffset`, `ByteMask`, `FieldMask`, an independent `storageExtent` and its identity. The supported declarations are:

| Encoding | Declaration |
| --- | --- |
| `NativeByte` | FieldSize = 1, ByteOffset = 0, ByteMask = 1, FieldMask = 255 |
| `SingleBit` | ByteMask contains exactly one nonzero bit; FieldMask = ByteMask; ByteOffset < FieldSize <= storageExtent |
| `Unknown` or other values | `Unsupported` |

`storageExtent` is 1–255 bytes. A repeated storage identity must declare the same extent. It describes the independently declared storage contract and is never used as a memory-read range. Each metadata field is a separate uint8 candidate scanned after the occupied prefix; field order and adjacency are unconstrained. All four fields require unique, disjoint candidates and complete final readback before atomic publication. A set containing only NativeByte declarations cannot distinguish FieldSize from ByteMask. A set containing only SingleBit declarations cannot distinguish ByteMask from FieldMask. These cases return `InvalidEvidence`; the provider does not choose an arbitrary order.

The frozen offsets are `FBoolProperty::FieldSize`, `FBoolProperty::ByteOffset`, `FBoolProperty::ByteMask` and `FBoolProperty::FieldMask`. They describe the declared metadata representation, without proving that a particular UE version uses it.

## Inline FieldPath name

`FieldPathRepresentation::InlineFName` is an explicit metadata representation. The current NameLayout and physical name pool decode the complete declared `expectedName`; at least two distinct full names are required. Candidates scan on 4-byte boundaries after the occupied prefix. Raw FName bytes are retained, checked around each decode, and compared during final readback. Invalid, unmapped or overflowing candidate names are recorded; short reads, permission failures and operation-level failures abort the operation.

The only published field is `FFieldPathProperty::PropertyClass`. It identifies an inline FName in this declared representation. Other representations return `Unsupported`. An upstream wrapper with the same field name does not establish this layout for arbitrary UE metadata.

## Agent ownership and validation

`AUEP_StartBoolPropertyProbe` and `AUEP_StartFieldPathPropertyProbe` copy their complete configuration and anchor strings before returning. The caller retains ownership and synchronization of referenced metadata memory through `AUEP_Stop`. The configured upstream property phases and selected final observation share the same execution owner, normal-linker module lease, 64 MiB read budget and 30-second deadline. Success publishes `bool-property-observations.txt` or `field-path-property-observations.txt` from one frozen snapshot. Any earlier phase failure prevents a completed export.

`tests/property_values.cpp` executes the production providers against owned native records with deliberately nonadjacent and reordered Bool fields, complete inline names, corruption, ambiguity, late changes, cancellation, dependency cycles, strict overrides and bounded report retention. `tests/android_property_values.cpp` executes the actual Agent APIs against owned metadata and validates copied inputs, frozen exports and worker accounting. `tests/verify_agent.py` records the complete configured Agent slice, including these cases and missing-Memory admission. [Component evidence](../tests/property_values/verification.json) distinguishes compilation, host execution and actual Android execution. The automatic Phase 5 collector independently derives NativeBool and pointer-tail relationships; other FieldPath representations remain unsupported.
