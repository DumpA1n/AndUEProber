# Bounded snapshot publication

`AndUEProber::Core` provides `publishExport` through
`<andueprober/Export.hpp>`. Analysis workflows pass a frozen, validated Session
snapshot and explicitly owned output buffers. The snapshot, buffers and options
remain immutable throughout the call. The optional atomic cancellation flag is
the cross-thread control.

| Option | Default | Scope |
|---|---:|---|
| `maximumFiles` | 4096 | Payload file count |
| `maximumFileBytes` | 16777216 | Bytes in one payload file |
| `maximumTotalBytes` | 67108864 | Total payload bytes |
| `maximumManifestBytes` | 4194304 | Metadata text input budget and actual encoded completion manifest size |
| `maximumMetadataEntries` | 16384 | Aggregate offsets, dependencies, evidence records, sample identities and sample addresses |
| `maximumPathBytes` | 1024 | Bytes in a relative filename or session identifier |
| `deadline` | Construction time plus 30 seconds | Checks between bounded CPU work and filesystem operations |

Limits must be nonzero. Empty payload files are valid. Metadata and filenames
must be Unicode scalar UTF-8; embedded NUL is permitted in JSON metadata and
escaped, but is rejected in filesystem paths. Payload contents may contain any
bytes. Invalid UTF-8 returns `InvalidArgument`. Exhausted limits return
`BudgetExceeded`. Path, file-size and metadata-input checks precede filesystem
operations; encoded manifest expansion can exhaust its separate bound after a
staging directory has been created.

Writes and checksum work use blocks of at most 65536 bytes. Cancellation and
deadline are checked after the caller's operation callback and after directory
flushes, including the final marker flush. A blocked kernel operation cannot be
interrupted by this API; its result is checked after it returns. There is no
bounded wall-clock guarantee for an unresponsive filesystem.

The caller owns an absolute output root and excludes untrusted concurrent
filesystem mutations. A root lock serializes cooperating publishers. Each
session writes to a separate `.staging-<session>-*` directory, flushes its files
and directories, and publishes to a new session directory. Existing session
directories are not overwritten. `completion.json` is finalized after directory
publication. A successful result has `status.code == None`, a nonempty
`publishedDirectory`, and `completionUncertain == false`.

Filesystem failures return `Io`; exceptions from `beforeOperation` and other
unexpected exceptions return `Internal`. Error messages can be empty when
reporting allocation or exception failures. Failed output remains at
`incompleteDirectory` when its path is available. Marker retraction is attempted
if finalization fails. If that rename, its directory flush or its directory close also fails,
`completionUncertain` is true. Such a result is a failure even when a marker is
still visible; it requires inspection and must not be reported as success.

`isCompletedExport` resolves directory aliases, rejects hidden/staging session
directories and symbolic-link markers, and checks for a regular completion
marker. It does not validate manifest content, file checksums or durability.
Its boolean result cannot override a failed `publishExport` result.

`tests/export_limits.cpp` executes the production exporter against owned
temporary directories. Cases cover exact limits, empty files, encoded manifest
growth, UTF-8 errors and chunk boundaries, operation exceptions, partial writes,
late cancellation/deadline, failed marker retraction and staging aliases. Its
`fsync` wrapper performs the real syscall before injecting late cancellation or
delayed return. Its `close` wrapper performs the real descriptor close before
injecting directory close errors. It does not substitute an in-memory filesystem. Existing Core
contract and generated SDK subset fixtures provide complementary coverage.
The [verification manifest](../tests/export/verification.json) identifies actual
host and Android configurations, source and artifact hashes, and unverified gates.

`tests/export_interruption.cpp` runs the production exporter in owned child
processes that call `_exit` during a partial write, before directory publication,
after directory publication and after the completion marker becomes visible.
No C++ cleanup runs in the interrupted child. The parent checks retained output,
the preceding successful export and a subsequent publication using the same
root lock. Before marker finalization, incomplete output is rejected. A marker
visible before its final directory flush still satisfies the presence check;
it cannot establish that the interrupted call returned successfully or that its
output is durable. These process-exit cases do not simulate power loss.

```sh
cmake -S . -B build/export-check -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DANDUEPROBER_BUILD_AGENT=OFF \
  -DANDUEPROBER_BUILD_INSPECTOR=OFF -DANDUEPROBER_SANITIZERS=ON
cmake --build build/export-check --target andueprober_export_limits
ctest --test-dir build/export-check -R '^andueprober.export_limits$' --output-on-failure
```

Frozen observation export is implemented. A complete bounded reflection-to-dumper
adapter and an owned UE application's full generated SDK remain separate
acceptance gates; a successful generic file publication does not close them.
