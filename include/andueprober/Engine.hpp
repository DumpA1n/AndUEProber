#pragma once
#include "Core.hpp"

namespace andueprober {

// CompiledUtf16V1 describes this header's native shim, not a UE engine signature.
enum class EngineSignature { Unknown, CompiledUtf16V1 };
struct FStringValue {
    char16_t* data = nullptr;
    std::int32_t count = 0; // Includes the terminating UTF-16 code unit.
    std::int32_t capacity = 0;
};
struct Utf16Sink {
    void* context = nullptr;
    // Exactly one successful allocation is permitted per invocation.
    char16_t* (*allocate)(void*, std::uint32_t capacity) noexcept = nullptr;
};
using CompiledTextFunction = Error (*)(void* object, const Utf16Sink*, FStringValue*);
using EngineAllocate = char16_t* (*)(std::uint32_t capacity) noexcept;
using EngineRelease = void (*)(char16_t*, std::uint32_t capacity) noexcept;
struct CompiledTextProvider {
    CompiledTextFunction invoke = nullptr;
    EngineAllocate allocate = nullptr;
    EngineRelease release = nullptr;
};
struct EngineIdentity {
    std::string module;
    std::string layout;
    std::uint64_t generation = 0;
};

// Acquires a normal linker reference to an already loaded module. All three typed
// functions must belong to that module and be compiled against CompiledUtf16V1.
// Multiple loaded instances sharing a path or file identity are rejected. Apple
// host integration must serialize binding with other module load/unload operations;
// its image enumeration does not supply the Android/Linux loader callback lock.
// Raw UE/FName/ProcessEvent addresses, manually mapped modules and pointer casts
// purporting to establish a signature are unsupported. The provider must not
// retain the sink, its allocation or the borrowed object after invoke returns.
// A binding records its first invocation owner and rejects calls from a different
// owner or recursive calls through another executor. Allocation and release must
// be non-throwing and accept exactly the requested UTF-16 capacity.
class EngineTextBinding final {
public:
    static Status bind(const std::string& modulePath, std::string layoutIdentity,
        CompiledTextProvider, std::shared_ptr<EngineTextBinding>&);
    ~EngineTextBinding();
    EngineTextBinding(const EngineTextBinding&) = delete;
    EngineTextBinding& operator=(const EngineTextBinding&) = delete;
    EngineIdentity identity() const;
    void invalidate() noexcept;
private:
    struct State;
    explicit EngineTextBinding(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend class GameThreadExecutor;
    friend Status engineTextAdmission(EngineSignature, const std::shared_ptr<EngineTextBinding>&,
        const EngineIdentity&);
};

// Missing bindings and unknown signatures remain Unsupported. Identity matching
// is necessary for admission; it does not establish arbitrary engine ABI support.
Status engineTextAdmission(EngineSignature, const std::shared_ptr<EngineTextBinding>&,
    const EngineIdentity&);

struct EngineTextResult {
    TaskState state = TaskState::Pending;
    Status status;
    EngineIdentity identity;
    std::string utf8;
};
class EngineTextTask final {
public:
    EngineTextTask() = default;
    void cancel() noexcept;
    EngineTextResult result() const;
    Status waitUntil(std::chrono::steady_clock::time_point deadline) const;
private:
    struct State;
    explicit EngineTextTask(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend class GameThreadExecutor;
};
struct EngineRequest {
    EngineSignature signature = EngineSignature::Unknown;
    std::shared_ptr<EngineTextBinding> binding;
    EngineIdentity identity;
    // A non-null lease pins the target object throughout invocation. Lease
    // destruction must be non-throwing and valid on any control thread.
    std::shared_ptr<void> object;
    std::size_t maximumUnits = 65536;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};
struct EngineExecutorStats {
    std::size_t pending = 0, active = 0;
    bool accepting = false;
};

// attach() must be called from an integration's established game-thread callback.
// This executor does not discover, create or claim a game thread. pump() executes
// only on that attached owner, without holding the publication mutex.
// cancel() closes admission, completes queued requests and requests cancellation
// of active work. An active native call cannot be interrupted; drain waits for
// its return and matching allocator release. Owner/callback waits never block.
// Successful drain is not a DSO-unload contract: external bindings still hold
// linker references. The owner must finish member calls before destruction.
class GameThreadExecutor final {
public:
    explicit GameThreadExecutor(std::size_t pendingCapacity = 16);
    ~GameThreadExecutor();
    GameThreadExecutor(const GameThreadExecutor&) = delete;
    GameThreadExecutor& operator=(const GameThreadExecutor&) = delete;
    Status attach();
    Status submit(EngineRequest, EngineTextTask&);
    Status pump(std::size_t maximumCalls = 1);
    void cancel() noexcept;
    Status drain(std::chrono::steady_clock::time_point deadline);
    EngineExecutorStats stats() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
