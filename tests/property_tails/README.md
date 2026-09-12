# Property pointer-tail contracts

`probePropertyTails` observes eleven pointer offsets for nine explicitly selected
property kinds. Each invocation consumes metadata for one kind and publishes its
one or two fields together.

| Kind | First field | Second field |
| --- | --- | --- |
| Enum | `FEnumProperty::UnderlyingType` | `FEnumProperty::Enum` |
| Array | `FArrayProperty::Inner` | — |
| Set | `FSetProperty::ElementProp` | — |
| Map | `FMapProperty::KeyProp` | `FMapProperty::ValueProp` |
| Object | `FObjectPropertyBase::PropertyClass` | — |
| Struct | `FStructProperty::Struct` | — |
| Byte | `FByteProperty::Enum` | — |
| Class | `FObjectPropertyBase::PropertyClass` | `FClassProperty::MetaClass` |
| Interface | `FInterfaceProperty::InterfaceClass` | — |

`Unknown` and `Bool` return `Unsupported`. This component does not infer bool
FieldSize/ByteOffset/ByteMask/FieldMask, `sizeof(FProperty)`, a shared
`SubPropertyBase`, or a neighboring pointer's location. FFieldPathProperty name
metadata and other legacy wrapper semantics remain separate migration boundaries. It does not establish complete container
layouts, element ownership, or complete Phase 5 acceptance.

Three to sixteen independently named metadata objects have disjoint ranges.
Each pointer column requires at least two distinct non-null named targets.
Other samples may explicitly declare null; those observations compare zero only.
Single-field kinds require no second field. Dual-field kinds require an explicit
second value, with independent scanning, a distinct pointer slot and complete
final readback. Pointers are never dereferenced, normalized, type-checked or
invoked. A target name cannot denote contradictory addresses.

`PropertyTailProfile::property` supplies the preceding scalar configuration.
The exact Name/Owner/scalar evidence sources must match canonical pool address,
generation, metadata profile and declared layouts. `propertyObservationIdentity`
identifies the scalar observation without conflating scan extent with layout.
`propertyBaseExtent` independently declares the occupied prefix in this subclass.
All five FField ranges and four FProperty scalar ranges must fit their declared
prefixes, remain disjoint, and precede every tail candidate. Neither a candidate
nor an observed pointer determines that prefix.

Required base fields and tentative outputs use production iterative evidence
validation with 4096 nodes, 16384 dependencies and 4 MiB metadata. Input and
retained field-report metadata also have separate 4 MiB bounds. Reads share the
caller byte budget, deadline and cancellation. Failures leave automatic results
stale and retain available reports. Matching User overrides require current
validated evidence and every prerequisite; their value, origin, validation and
version remain unchanged. Unrelated stale results are outside this operation's
dependency closure.

`OwnedPropertyTails.hpp` embeds complete compiled property members and supplies
independent targets for every kind. Its Enum and Map pointer members use reversed,
non-adjacent placement. Its Class record embeds the complete ObjectProperty
record, preserving PropertyClass at the compiled base offset while MetaClass
occupies an independently separated slot. The fixture executes actual Phase 1, Phase 2, Phase 3,
Phase 4, FField-base and FProperty-scalar operations before probing tails. The
opaque target records are excluded from its readable ranges, and target reads
and native calls remain zero. Zero expectation, each field's ambiguity and
final-read mutation, shared-slot rejection, identity/prefix/provenance errors,
provider failures, budgets, cancellation, stale/cyclic dependency closure,
strict overrides and immutable Session publication are exercised. Sixteen real
large native records with repeated pointer slots exercise report-budget failure
without publishing partial results.

```sh
cmake -S . -B build/property-tails-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDUEPROBER_BUILD_AGENT=OFF -DANDUEPROBER_BUILD_INSPECTOR=OFF \
  -DANDUEPROBER_BUILD_TESTS=ON
cmake --build build/property-tails-host --target andueprober_property_tails header_PropertyTails_hpp
ctest --test-dir build/property-tails-host -R '^andueprober.property_tails$' --output-on-failure
```

`verification.json` records executed host/Android configurations and exact input,
artifact and matching-symbol identities. Actual owned UE assets, bool metadata,
complete property/container graphs, engine calls, other devices and independently
configured hardening remain separate acceptance requirements.
