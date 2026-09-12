# Class metadata contracts

`andueprober_classes` exercises production `probeClassFields`, the Phase 1/2
probes, and `Session` using independently declared native fixture types. The
fixture supplies the complete class extent, names, 64-bit cast flags, and default
object addresses. Default objects are deliberately outside the readable regions;
this phase compares their pointers without invoking or dereferencing them.

The public input requires 3-16 distinct named class anchors, at least two flag
values, and at least two distinct non-null default objects. Null remains valid for
other explicitly named default-object expectations. Field scans include offset
zero, retain rejected candidates, require one matching candidate per field, and
recheck both fields before publication. A native compact record separately tests
offset zero; it does not establish a real UObject layout.
One default-object identity cannot denote contradictory addresses.

Both outputs depend on validated Phase 1 Index/Name/Class/Outer and Phase 2
Next/SuperStruct/Children/PropertiesSize versions. FField also requires
ChildProperties; MinAlignment is optional and supplies no size or offset inference.
A new attempt makes prior automatic results stale. Results publish together, and
contradictory user overrides fail without replacement. Matching overrides retain
their original version and origin. They must already have passing validated
evidence and every required current dependency version. Matching Candidate/Stale
overrides or incomplete evidence fail without implicitly validating user data.

The regression fixture covers independent high-bit flags, optional null CDOs,
metadata bounds, duplicate anchors, missing/stale dependencies, corruption,
ambiguity, short reads, permission and mapping failures, byte budgets, deadlines,
cancellation, module-generation changes, final-read mutation, override conflicts,
and immutable Session publication. The reader and metadata containers require one
operation owner and externally synchronized storage; readback does not make
arbitrarily mutating engine memory an atomic snapshot.

Host configuration and execution:

```sh
cmake -S . -B build/classes-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDUEPROBER_BUILD_AGENT=OFF -DANDUEPROBER_BUILD_INSPECTOR=OFF \
  -DANDUEPROBER_BUILD_TESTS=ON
cmake --build build/classes-host --target andueprober_classes header_Classes_hpp
ctest --test-dir build/classes-host -R '^andueprober.classes$' --output-on-failure
```

`ANDUEPROBER_SANITIZERS=ON` enables the host AddressSanitizer/UndefinedBehaviorSanitizer
configuration. Android builds use the same executable with the NDK arm64-v8a
configuration and execute only in an owned fixture directory. Exact executed
configurations, source/artifact hashes, matching symbols, and logs are recorded in
`verification.json` and `evidence/`.

The fixture does not validate an actual UE application, engine ABI, allocator,
or profile metadata. Production adapter configuration and owned UE asset
acceptance remain separate requirements. No function calls, signal recovery,
permission changes, or object-array order assumptions are part of this phase.
