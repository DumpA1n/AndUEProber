# Compiled text shim contracts

The production `AndUEProber::Core` Engine API supports one native calling shape:
`Error(void* object, const Utf16Sink*, FStringValue*)`. The fixture compiles that
exact function type into a separate owned DSO and calls it through the production
binding and executor. It does not invoke UE, guess a ProcessEvent or FName
signature, or treat profile metadata as ABI verification.

`EngineTextBinding::bind` requires an absolute path to an already loaded normal
linker module. It obtains a `RTLD_NOLOAD` reference and verifies that the typed
invocation, allocation and release wrappers belong to that module. A binding has
an immutable canonical module path, caller-supplied layout identity and monotonic
generation within the linked Core instance. Explicit invalidation prevents later
invocations and rejects results from an active invocation. The first invocation
records the binding's owner; another owner and recursive invocation are rejected.
Bindings retain their linker references until their last owner releases them.
Binding rejects multiple loaded instances with the same path or device/inode
identity, including Android `ANDROID_DLEXT_FORCE_LOAD` instances. Linux/Android
validation enumerates modules under the loader's `dl_iterate_phdr` callback.
Apple host integration must serialize binding with other module load/unload work;
its public image enumeration does not provide that callback lock.

An integration must call `GameThreadExecutor::attach` from its established game
thread and pump accepted work there. The executor neither discovers a game thread
nor creates one. Submission copies request metadata and retains a target object
lease; lease destruction must be non-throwing and safe on control threads. The
pending queue defaults to 16 requests and admits at most 1024. Each request limits
output to at most 65536 UTF-16 units and has a deadline. Cancellation closes
admission, completes pending requests and cooperatively cancels an active call.
An active native function must return before its allocation and object lease can
be released. `drain` exposes a timeout while that work remains; owner and callback
waits return `Busy` instead of waiting for themselves.

The sink permits exactly one successful allocation per invocation and records its
pointer and capacity. A returned FString must reference that allocation and report
matching capacity with a count in range. A nonempty count includes a terminator;
both unallocated and allocated empty strings are supported. The Core decoder
rejects unpaired UTF-16 surrogates. Returned pointers that do not match the recorded
allocation are never read or freed. The recorded allocation is released exactly
once, through its paired wrapper on the invocation owner, including native error,
C++ exception, cancellation, timeout, invalidated generation and corrupt header
paths. A result is published only after this release and contains owned UTF-8.

A UE integration requires a shim compiled against the owned engine's actual
headers, signatures, object lifetime and allocator contract. Raw engine FString
allocations cannot be adopted through this interface. A shim must release any
separate engine-returned temporary through that engine's allocator and copy text
into the supplied sink. Additional dynamically resolved engine modules require
their own residency references retained by the object lease. An ordinary linker
lease does not protect against manual unmapping or external modification. The
default DumperBridge refusal uses the same admission check and has no registered
UE shim. Successful draining does not establish that an Agent or executor DSO can
be unloaded.

The host fixture covers 135 checks, including normal and empty text, every allocation
failure path, malformed counts and capacities, foreign pointers, missing
terminators, malformed surrogates, queue limits, wrong owner, recursive pump,
callback cancellation, concurrent drain, cancellation during allocator release,
pending-object destruction, active invalidation, deadline crossing, loader
reference retention, actual module unloading and reload generation advancement.
It does not replace the production executor, binding or UTF-16 decoder.
Android adds nine actual duplicate-instance checks, for 144 total: two normal
loader instances of the same owned DSO have different function addresses, both
bindings are rejected while ambiguous, a unique instance can bind after the
duplicate closes, and rejection leaves no linker reference behind.

```sh
cmake --preset host-sanitizers
cmake --build --preset host-sanitizers --target andueprober_engine andueprober_engine_module
ctest --preset host-sanitizers -R '^andueprober.engine$' --no-tests=error --output-on-failure
```

For Android, the same targets are built with NDK r29, AArch64 and API 27. Both
artifacts must be copied to an explicitly owned device directory before execution:

```sh
adb -s SERIAL push BUILD/tests/andueprober_engine BUILD/tests/libandueprober_engine_module.so OWNED_DIRECTORY/
adb -s SERIAL shell OWNED_DIRECTORY/andueprober_engine OWNED_DIRECTORY/libandueprober_engine_module.so
```

`verification.json` records actual host Debug, Release and Debug ASan/UBSan runs,
Android Debug/Release runs, the complete command archive, source hashes before and
after execution, device page size, device artifact hashes and matching UUID/build
ID identifiers for both the executable and owned module. The macOS sanitizer run
disables LeakSanitizer and does not establish leak freedom. Other engine
signatures, real UE modules and allocators, engine-wide object consistency,
manual loaders, 16 KB devices and independently enabled BTI/PAC/CFI/MTE
configurations remain unverified.
