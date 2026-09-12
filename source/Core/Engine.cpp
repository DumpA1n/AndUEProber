#include "andueprober/Engine.hpp"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#if defined(__linux__)
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace andueprober {
namespace {
std::atomic<std::uint64_t> nextGeneration{1};
thread_local const void* executingExecutor = nullptr;
thread_local const void* completingTask = nullptr;
struct ExecutionScope {
    const void*& slot;
    const void* previous;
    ExecutionScope(const void*& target, const void* value) : slot(target), previous(target) { slot = value; }
    ~ExecutionScope() { slot = previous; }
};
using Clock = std::chrono::steady_clock;
bool finished(TaskState state) {
    return state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Cancelled;
}
Status checkRequest(const EngineRequest& request, const std::atomic<bool>& cancelled) {
    if (cancelled.load()) return {Error::Cancelled, "Engine request cancelled"};
    if (Clock::now() >= request.deadline) return {Error::DeadlineExceeded, "Engine request deadline exceeded"};
    return engineTextAdmission(request.signature, request.binding, request.identity);
}
struct Allocation {
    CompiledTextProvider provider;
    std::size_t maximum;
    char16_t* data = nullptr;
    std::uint32_t capacity = 0;
    Error error = Error::None;
    ~Allocation() { if (data) provider.release(data, capacity); }
    static char16_t* allocate(void* context, std::uint32_t capacity) noexcept {
        auto& value = *static_cast<Allocation*>(context);
        if (!capacity || capacity > value.maximum || value.data) {
            value.error = Error::InvalidArgument;
            return nullptr;
        }
        auto* data = value.provider.allocate(capacity);
        if (!data) { value.error = Error::Internal; return nullptr; }
        value.data = data;
        value.capacity = capacity;
        if (reinterpret_cast<std::uintptr_t>(data) % alignof(char16_t)) {
            value.error = Error::InvalidArgument;
            return nullptr;
        }
        std::memset(data, 0, static_cast<std::size_t>(capacity) * sizeof(char16_t));
        return data;
    }
};
#if defined(__unix__) || defined(__APPLE__)
struct LoadedInstance {
    const char* path;
    struct stat file;
    void* expectedBase;
    std::size_t visited = 0, matches = 0;
    bool expectedFound = false, failed = false;
    void observe(const char* name, void* base) noexcept {
        if (++visited > 4096) { failed = true; return; }
        if (!name || !*name) return;
        if (strnlen(name, 4096) == 4096) { failed = true; return; }
        struct stat candidate{};
        const bool samePath = std::strcmp(path, name) == 0;
        const bool sameFile = stat(name, &candidate) == 0 && candidate.st_dev == file.st_dev && candidate.st_ino == file.st_ino;
        if (!samePath && !sameFile) return;
        ++matches;
        if (base == expectedBase) expectedFound = true;
    }
};
Status uniqueLoadedInstance(const char* path, void* expectedBase) {
    LoadedInstance instance{path, {}, expectedBase};
    if (stat(path, &instance.file) != 0) return {Error::InvalidEvidence, "The compiled module file identity is unavailable"};
#if defined(__linux__)
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* context) {
        auto& state = *static_cast<LoadedInstance*>(context);
        state.observe(info->dlpi_name, reinterpret_cast<void*>(info->dlpi_addr));
        return state.failed ? 1 : 0;
    }, &instance);
#elif defined(__APPLE__)
    const auto count = _dyld_image_count();
    if (count > 4096) return {Error::Unsupported, "The loaded module inspection capacity is exhausted"};
    for (std::uint32_t index = 0; index < count; ++index)
        instance.observe(_dyld_get_image_name(index), const_cast<mach_header*>(_dyld_get_image_header(index)));
    if (count != _dyld_image_count()) instance.failed = true;
#else
    return {Error::Unsupported, "Loaded module instance validation is unavailable on this platform"};
#endif
    if (instance.failed || instance.matches != 1 || !instance.expectedFound)
        return {Error::InvalidEvidence, "The compiled module lease requires one matching loaded instance"};
    return {};
}
#endif
}

struct EngineTextBinding::State {
    std::mutex mutex;
    CompiledTextProvider provider;
    EngineIdentity identity;
    std::atomic<bool> valid{true};
    std::thread::id owner;
    bool invoking = false;
    void* module = nullptr;
    ~State() {
#if defined(__unix__) || defined(__APPLE__)
        if (module) dlclose(module);
#endif
    }
};
EngineTextBinding::EngineTextBinding(std::shared_ptr<State> state) : state_(std::move(state)) {}
EngineTextBinding::~EngineTextBinding() = default;
EngineIdentity EngineTextBinding::identity() const { return state_->identity; }
void EngineTextBinding::invalidate() noexcept { state_->valid = false; }
Status EngineTextBinding::bind(const std::string& modulePath, std::string layout,
    CompiledTextProvider provider, std::shared_ptr<EngineTextBinding>& result) {
    result.reset();
    if (modulePath.empty() || modulePath.size() > 4095 || modulePath.find('\0') != std::string::npos ||
        modulePath.front() != '/' || layout.empty() || layout.size() > 1024 || layout.find('\0') != std::string::npos ||
        !provider.invoke || !provider.allocate || !provider.release)
        return {Error::InvalidArgument, "An absolute module path, layout identity and complete typed provider are required"};
#if (defined(__unix__) || defined(__APPLE__)) && defined(RTLD_NOLOAD)
    try {
        auto state = std::make_shared<State>();
        state->module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!state->module) return {Error::Unsupported, "The compiled shim module is not already loaded"};
        Dl_info entry{}, allocate{}, release{};
        if (!dladdr(reinterpret_cast<void*>(provider.invoke), &entry) || !entry.dli_fname || !entry.dli_fbase ||
            !dladdr(reinterpret_cast<void*>(provider.allocate), &allocate) ||
            !dladdr(reinterpret_cast<void*>(provider.release), &release) ||
            entry.dli_fbase != allocate.dli_fbase || entry.dli_fbase != release.dli_fbase)
            return {Error::InvalidEvidence, "The invocation and allocator functions must belong to one normal linker module"};
        std::unique_ptr<char, decltype(&std::free)> requested(realpath(modulePath.c_str(), nullptr), &std::free);
        std::unique_ptr<char, decltype(&std::free)> actual(realpath(entry.dli_fname, nullptr), &std::free);
        if (!requested || !actual || std::strcmp(requested.get(), actual.get()) != 0)
            return {Error::StaleIdentity, "The compiled shim does not belong to the requested module path"};
        if (auto status = uniqueLoadedInstance(actual.get(), entry.dli_fbase); !status) return status;
        auto generation = nextGeneration.load();
        do {
            if (generation == std::numeric_limits<std::uint64_t>::max())
                return {Error::Overflow, "Engine module generation capacity exhausted"};
        } while (!nextGeneration.compare_exchange_weak(generation, generation + 1));
        state->identity = {actual.get(), std::move(layout), generation};
        state->provider = provider;
        result = std::shared_ptr<EngineTextBinding>(new EngineTextBinding(std::move(state)));
        return {};
    } catch (...) { return {Error::Internal, {}}; }
#else
    return {Error::Unsupported, "A normal linker lease is unavailable on this platform"};
#endif
}
Status engineTextAdmission(EngineSignature signature, const std::shared_ptr<EngineTextBinding>& binding,
    const EngineIdentity& identity) {
    if (signature != EngineSignature::CompiledUtf16V1 || !binding)
        return {Error::Unsupported, "A compiled UTF-16 shim, game-thread executor and allocator contract are required"};
    const auto& state = *binding->state_;
    if (!state.valid || identity.module != state.identity.module || identity.layout != state.identity.layout ||
        !identity.generation || identity.generation != state.identity.generation)
        return {Error::StaleIdentity, "Engine module or layout generation changed"};
    return {};
}

struct EngineTextTask::State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> cancelled{false};
    EngineRequest request;
    EngineTextResult result;
    std::thread::id owner;
};
EngineTextTask::EngineTextTask(std::shared_ptr<State> state) : state_(std::move(state)) {}
void EngineTextTask::cancel() noexcept { if (state_) state_->cancelled = true; }
EngineTextResult EngineTextTask::result() const {
    if (!state_) return {TaskState::Failed, {Error::InvalidArgument, "No engine task is present"}, {}, {}};
    std::lock_guard lock(state_->mutex);
    return state_->result;
}
Status EngineTextTask::waitUntil(Clock::time_point deadline) const {
    if (!state_) return {Error::InvalidArgument, "No engine task is present"};
    std::unique_lock lock(state_->mutex);
    if (!finished(state_->result.state) && (std::this_thread::get_id() == state_->owner || completingTask == state_.get()))
        return {Error::Busy, "The game-thread owner cannot wait for its own queued or active task"};
    if (!state_->changed.wait_until(lock, deadline, [&] { return finished(state_->result.state); }))
        return {Error::DeadlineExceeded, "Engine task completion wait expired"};
    return state_->result.status;
}

struct GameThreadExecutor::State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<EngineTextTask::State>> pending;
    std::shared_ptr<EngineTextTask::State> active;
    std::thread::id owner;
    std::size_t capacity, retiring = 0;
    bool attached = false, closed = false;
    explicit State(std::size_t size) : capacity(size) {}
    static void finish(const std::shared_ptr<EngineTextTask::State>& task, EngineTextResult result) {
        ExecutionScope completing(completingTask, task.get());
        EngineRequest retired;
        {
            std::lock_guard lock(task->mutex);
            retired = std::move(task->request);
        }
        // Object leases can run user destructors. No publication lock is held.
        retired.object.reset();
        retired.binding.reset();
        {
            std::lock_guard lock(task->mutex);
            if (result.status && task->cancelled.load()) {
                result.state = TaskState::Cancelled;
                result.status.code = Error::Cancelled;
                result.utf8.clear();
            }
            task->result = std::move(result);
        }
        task->changed.notify_all();
    }
    static EngineTextResult run(const std::shared_ptr<EngineTextTask::State>& task) {
        EngineTextResult result;
        try {
            result.identity = task->request.identity;
            result.status = checkRequest(task->request, task->cancelled);
            if (result.status) {
                const auto binding = task->request.binding->state_;
                {
                    std::lock_guard lock(binding->mutex);
                    if (binding->owner == std::thread::id{}) binding->owner = std::this_thread::get_id();
                    if (binding->owner != std::this_thread::get_id())
                        result.status = {Error::Unsupported, "The compiled binding belongs to another game-thread owner"};
                    else if (binding->invoking)
                        result.status = {Error::Busy, "Recursive invocation of one compiled binding is not permitted"};
                    else binding->invoking = true;
                }
                if (!result.status) { result.state = TaskState::Failed; return result; }
                struct Invocation {
                    std::shared_ptr<EngineTextBinding::State> binding;
                    ~Invocation() { std::lock_guard lock(binding->mutex); binding->invoking = false; }
                } invocation{binding};
                const auto provider = binding->provider;
                Allocation allocation{provider, task->request.maximumUnits};
                const Utf16Sink sink{&allocation, Allocation::allocate};
                FStringValue string;
                const auto call = provider.invoke(task->request.object.get(), &sink, &string);
                result.status = checkRequest(task->request, task->cancelled);
                if (result.status && allocation.error != Error::None)
                    result.status = {allocation.error, "The compiled shim violated or exhausted its UTF-16 allocation contract"};
                if (result.status && call != Error::None)
                    result.status = {call, "The compiled shim reported an invocation failure"};
                if (result.status && (string.data != allocation.data || string.capacity < 0 ||
                    static_cast<std::uint32_t>(string.capacity) != allocation.capacity || string.count < 0 ||
                    string.count > string.capacity))
                    result.status = {Error::InvalidArgument, "FString storage, count or capacity does not match the recorded allocation"};
                if (result.status && string.count) {
                    if (string.data[string.count - 1] != 0)
                        result.status = {Error::InvalidArgument, "FString is not terminated within its count"};
                    else result.status = decodeUtf16({string.data, static_cast<std::size_t>(string.count - 1)}, result.utf8);
                }
                if (result.status) result.status = checkRequest(task->request, task->cancelled);
            }
            // A release callback can request cancellation or invalidate the lease.
            if (result.status) result.status = checkRequest(task->request, task->cancelled);
        } catch (...) { result.status = {Error::Internal, {}}; }
        if (!result.status) result.utf8.clear();
        result.state = result.status.code == Error::Cancelled ? TaskState::Cancelled :
            result.status ? TaskState::Succeeded : TaskState::Failed;
        return result;
    }
};
GameThreadExecutor::GameThreadExecutor(std::size_t capacity) : state_(std::make_shared<State>(capacity)) {}
GameThreadExecutor::~GameThreadExecutor() { cancel(); }
Status GameThreadExecutor::attach() {
    std::lock_guard lock(state_->mutex);
    if (state_->attached || state_->closed) return {Error::Busy, "The game-thread executor cannot attach or restart twice"};
    if (!state_->capacity || state_->capacity > 1024) return {Error::InvalidArgument, "A pending capacity between 1 and 1024 is required"};
    state_->owner = std::this_thread::get_id();
    state_->attached = true;
    return {};
}
Status GameThreadExecutor::submit(EngineRequest request, EngineTextTask& task) {
    task = {};
    if (!request.object || !request.maximumUnits || request.maximumUnits > 65536)
        return {Error::InvalidArgument, "An object lease and bounded UTF-16 capacity are required"};
    const std::atomic<bool> notCancelled{false};
    if (auto status = checkRequest(request, notCancelled); !status) return status;
    try {
        auto work = std::make_shared<EngineTextTask::State>();
        work->result.identity = request.identity;
        work->request = std::move(request);
        std::lock_guard lock(state_->mutex);
        if (!state_->attached || state_->closed) return {Error::Unsupported, "No accepting game-thread executor is attached"};
        if (state_->pending.size() == state_->capacity) return {Error::Busy, "The pending engine request capacity is exhausted"};
        work->owner = state_->owner;
        state_->pending.push_back(work);
        task = EngineTextTask(std::move(work));
        return {};
    } catch (...) { return {Error::Internal, {}}; }
}
Status GameThreadExecutor::pump(std::size_t maximumCalls) {
    if (!maximumCalls || maximumCalls > 1024) return {Error::InvalidArgument, "A bounded pump count is required"};
    auto state = state_;
    for (std::size_t i = 0; i < maximumCalls; ++i) {
        std::shared_ptr<EngineTextTask::State> task;
        {
            std::lock_guard lock(state->mutex);
            if (!state->attached || state->owner != std::this_thread::get_id())
                return {Error::Unsupported, "Only the attached game-thread owner may execute calls"};
            if (state->active) return {Error::Busy, "Recursive pumping is not permitted"};
            if (state->pending.empty()) return {};
            task = state->pending.front();
            state->pending.erase(state->pending.begin());
            state->active = task;
        }
        { std::lock_guard lock(task->mutex); task->result.state = TaskState::Running; }
        ExecutionScope executing(executingExecutor, state.get());
        State::finish(task, State::run(task));
        { std::lock_guard lock(state->mutex); state->active.reset(); }
        state->changed.notify_all();
    }
    return {};
}
void GameThreadExecutor::cancel() noexcept {
    std::vector<std::shared_ptr<EngineTextTask::State>> retired;
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        if (state_->active) state_->active->cancelled = true;
        retired.swap(state_->pending);
        state_->retiring += retired.size();
    }
    ExecutionScope executing(executingExecutor, state_.get());
    for (auto& task : retired) {
        EngineTextResult result;
        // Move existing identity to keep cancellation independent of allocation.
        { std::lock_guard lock(task->mutex); result.identity = std::move(task->result.identity); }
        result.state = TaskState::Cancelled;
        result.status.code = Error::Cancelled;
        State::finish(task, std::move(result));
        { std::lock_guard lock(state_->mutex); --state_->retiring; }
    }
    state_->changed.notify_all();
}
Status GameThreadExecutor::drain(Clock::time_point deadline) {
    std::unique_lock lock(state_->mutex);
    if (!state_->closed) return {Error::Busy, "Cancel the executor before draining accepted work"};
    if ((state_->active || state_->retiring) && (state_->owner == std::this_thread::get_id() || executingExecutor == state_.get()))
        return {Error::Busy, "An active game-thread callback cannot drain itself"};
    if (!state_->changed.wait_until(lock, deadline, [&] { return !state_->active && state_->pending.empty() && !state_->retiring; }))
        return {Error::DeadlineExceeded, "Engine drain is waiting for an active native call to return"};
    return {};
}
EngineExecutorStats GameThreadExecutor::stats() const {
    std::lock_guard lock(state_->mutex);
    return {state_->pending.size(), (state_->active ? 1u : 0u) + state_->retiring, state_->attached && !state_->closed};
}
}
