#include "engine/Fixture.hpp"
#include <chrono>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/dlext.h>
#endif

using namespace andueprober;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void checkStatus(Status status, const char* message) { check(static_cast<bool>(status), message); }
EngineRequest request(const std::shared_ptr<EngineTextBinding>& binding,
    const std::shared_ptr<EngineFixtureObject>& object) {
    return {EngineSignature::CompiledUtf16V1, binding, binding->identity(), object, 65536, Clock::now() + 5s};
}
void* openModule(const char* module) {
    auto* handle = dlopen(module, RTLD_NOW | RTLD_LOCAL);
    check(handle != nullptr, "owned module loading failed");
    return handle;
}
CompiledTextProvider provider(void* handle) {
    auto fn = reinterpret_cast<FixtureProviderFunction>(dlsym(handle, "ownedEngineProvider"));
    check(fn != nullptr, "owned typed provider export is missing");
    CompiledTextProvider result;
    fn(&result);
    return result;
}
char16_t* foreignAllocate(std::uint32_t) noexcept { return nullptr; }
struct ActiveControl {
    GameThreadExecutor* executor;
    EngineTextTask* task;
    std::atomic<bool> entered{false}, leave{false};
    Error recursive = Error::None, drain = Error::None, wait = Error::None;
};
void blockCall(void* value) {
    auto& control = *static_cast<ActiveControl*>(value);
    control.recursive = control.executor->pump().code;
    control.wait = control.task->waitUntil(Clock::now() + 1s).code;
    control.entered = true;
    while (!control.leave) std::this_thread::yield();
}
void cancelInCall(void* value) {
    auto& control = *static_cast<ActiveControl*>(value);
    control.executor->cancel();
    control.drain = control.executor->drain(Clock::now() + 1s).code;
}
void basicContracts(const char* path) {
    auto* module = openModule(path);
    auto functions = provider(module);
    std::shared_ptr<EngineTextBinding> binding;
    check(EngineTextBinding::bind(path, "", functions, binding).code == Error::InvalidArgument, "empty layout admitted");
    check(EngineTextBinding::bind(std::string(path) + std::string("\0ignored", 8), "owned-v1", functions, binding).code == Error::InvalidArgument,
        "embedded-NUL module path admitted");
    auto foreign = functions; foreign.allocate = foreignAllocate;
    check(EngineTextBinding::bind(path, "owned-v1", foreign, binding).code == Error::InvalidEvidence, "foreign allocator admitted");
    checkStatus(EngineTextBinding::bind(path, "owned-v1", functions, binding), "typed module binding failed");
    const auto firstIdentity = binding->identity();
    check(firstIdentity.generation != 0, "module generation is missing");
    dlclose(module);
    auto* pinned = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    check(pinned != nullptr, "binding did not retain its module"); dlclose(pinned);

    auto object = std::make_shared<EngineFixtureObject>();
    GameThreadExecutor executor(2);
    EngineTextTask task;
    check(executor.submit(request(binding, object), task).code == Error::Unsupported, "missing game-thread owner admitted");
    checkStatus(executor.attach(), "owner attach failed");
    check(executor.attach().code == Error::Busy, "duplicate owner attached");
    auto unknown = request(binding, object); unknown.signature = EngineSignature::Unknown;
    check(executor.submit(unknown, task).code == Error::Unsupported, "unknown signature admitted");
    unknown = request(binding, object); ++unknown.identity.generation;
    check(executor.submit(unknown, task).code == Error::StaleIdentity, "stale generation admitted");
    unknown = request(binding, object); unknown.identity.layout = "different-layout";
    check(executor.submit(unknown, task).code == Error::StaleIdentity, "stale layout admitted");
    unknown = request(binding, object); unknown.maximumUnits = 65537;
    check(executor.submit(unknown, task).code == Error::InvalidArgument, "unbounded output admitted");
    unknown = request(binding, object); unknown.deadline = Clock::now() - 1ms;
    check(executor.submit(unknown, task).code == Error::DeadlineExceeded, "expired request admitted");
    check(object->calls == 0, "admission errors invoked native code");

    checkStatus(executor.submit(request(binding, object), task), "first call submission failed");
    check(task.waitUntil(Clock::now() + 1s).code == Error::Busy, "owner waited on its own pending task");
    Error foreignPump = Error::None;
    std::thread other([&] { foreignPump = executor.pump().code; }); other.join();
    check(foreignPump == Error::Unsupported && object->calls == 0, "foreign thread invoked native code");
    checkStatus(executor.pump(), "owner pump failed");
    const auto result = task.result();
    check(result.state == TaskState::Succeeded && result.utf8 == "A\xF0\x9F\x98\x80Z", "UTF-16 conversion failed");
    check(object->calls == 1 && object->allocations == 1 && object->releases == 1 && object->outstanding == 0 &&
        object->wrongThread == 0, "allocation, release or invocation owner mismatch");
    auto copy = result; copy.utf8 = "changed";
    check(task.result().utf8 == result.utf8, "task result is externally mutable");

    const Error errors[] = {Error::None, Error::InvalidArgument, Error::InvalidArgument, Error::InvalidArgument,
        Error::InvalidArgument, Error::InvalidArgument, Error::Internal, Error::InvalidArgument,
        Error::InvalidArgument, Error::Internal, Error::None, Error::PermissionDenied, Error::InvalidArgument, Error::None};
    for (int mode = 1; mode < 14; ++mode) {
        auto test = std::make_shared<EngineFixtureObject>(); test->mode = mode;
        checkStatus(executor.submit(request(binding, test), task), "failure-mode submission failed");
        checkStatus(executor.pump(), "failure-mode pump failed");
        auto value = task.result();
        check(value.status.code == errors[mode] && value.utf8.empty(), "failure-mode status or partial output mismatch");
        check(test->allocations == test->releases && !test->outstanding && !test->wrongThread,
            "failure path did not release through the same owner and allocator");
    }
    Error foreignBinding = Error::None;
    std::thread alternate([&] {
        GameThreadExecutor otherExecutor;
        otherExecutor.attach();
        EngineTextTask otherTask;
        otherExecutor.submit(request(binding, object), otherTask);
        otherExecutor.pump();
        foreignBinding = otherTask.result().status.code;
    }); alternate.join();
    check(foreignBinding == Error::Unsupported && object->calls == 1,
        "another executor transferred a compiled binding to a different owner");

    EngineTextTask first, second, overflow;
    checkStatus(executor.submit(request(binding, object), first), "queue first submission failed");
    checkStatus(executor.submit(request(binding, object), second), "queue second submission failed");
    check(executor.submit(request(binding, object), overflow).code == Error::Busy, "bounded queue overflow admitted");
    const int beforeCancel = object->calls;
    first.cancel();
    checkStatus(executor.pump(), "cancelled queued task pump failed");
    check(first.result().status.code == Error::Cancelled && object->calls == beforeCancel, "cancelled queued call entered provider");
    executor.cancel();
    check(second.result().state == TaskState::Cancelled && !executor.stats().pending, "executor cancellation did not complete pending tasks");
    checkStatus(executor.drain(Clock::now() + 1s), "cancelled executor did not drain");
    check(executor.submit(request(binding, object), task).code == Error::Unsupported, "closed executor accepted work");

    GameThreadExecutor stale;
    checkStatus(stale.attach(), "stale owner attach failed");
    checkStatus(stale.submit(request(binding, object), task), "stale task submission failed");
    binding->invalidate();
    checkStatus(stale.pump(), "stale task pump failed");
    check(task.result().status.code == Error::StaleIdentity && object->calls == beforeCancel, "invalidated module entered provider");
    stale.cancel(); checkStatus(stale.drain(Clock::now() + 1s), "stale executor did not drain");
    // Completed tasks retain values, not the code lease or borrowed object.
    unknown = {}; binding.reset();
    auto* unpinned = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    check(unpinned == nullptr, "completed tasks retained the module lease");
    if (unpinned) dlclose(unpinned);
    module = openModule(path); functions = provider(module);
    checkStatus(EngineTextBinding::bind(path, "owned-v1", functions, binding), "reload binding failed");
    check(binding->identity().generation > firstIdentity.generation, "reloaded module reused a generation");
    dlclose(module); binding.reset();
}
void activeContracts(const char* path) {
    auto* module = openModule(path);
    std::shared_ptr<EngineTextBinding> binding;
    checkStatus(EngineTextBinding::bind(path, "owned-v1", provider(module), binding), "active binding failed");
    dlclose(module);
    GameThreadExecutor executor;
    checkStatus(executor.attach(), "active owner attach failed");
    auto object = std::make_shared<EngineFixtureObject>();
    EngineTextTask task;
    ActiveControl control{&executor, &task}; object->control = &control; object->onInvoke = blockCall;
    checkStatus(executor.submit(request(binding, object), task), "active task submission failed");
    Error earlyDrain = Error::None, lateDrain = Error::Internal;
    std::thread canceller([&] {
        while (!control.entered) std::this_thread::yield();
        executor.cancel();
        earlyDrain = executor.drain(Clock::now() + 20ms).code;
        control.leave = true;
        lateDrain = executor.drain(Clock::now() + 2s).code;
    });
    checkStatus(executor.pump(), "active owner pump failed"); canceller.join();
    check(control.recursive == Error::Busy && control.wait == Error::Busy, "callback recursion or self-wait was admitted");
    check(earlyDrain == Error::DeadlineExceeded && lateDrain == Error::None, "concurrent drain ignored active code lifetime");
    check(task.result().status.code == Error::Cancelled && task.result().utf8.empty(), "late completion published after cancellation");
    check(object->allocations == 1 && object->releases == 1 && !object->outstanding && !object->wrongThread,
        "active cancellation skipped owner allocator release");

    GameThreadExecutor self;
    checkStatus(self.attach(), "self owner attach failed");
    object = std::make_shared<EngineFixtureObject>(); ActiveControl selfControl{&self, &task};
    object->control = &selfControl; object->onInvoke = cancelInCall;
    checkStatus(self.submit(request(binding, object), task), "self cancellation submission failed");
    checkStatus(self.pump(), "self cancellation pump failed");
    check(selfControl.drain == Error::Busy && task.result().status.code == Error::Cancelled, "callback drain blocked or cancellation was lost");
    checkStatus(self.drain(Clock::now() + 1s), "self executor did not drain after callback return");

    GameThreadExecutor afterRelease;
    checkStatus(afterRelease.attach(), "release owner attach failed");
    object = std::make_shared<EngineFixtureObject>(); ActiveControl releaseControl{&afterRelease, &task};
    object->control = &releaseControl; object->onRelease = cancelInCall;
    checkStatus(afterRelease.submit(request(binding, object), task), "release cancellation submission failed");
    checkStatus(afterRelease.pump(), "release cancellation pump failed");
    check(task.result().status.code == Error::Cancelled && object->releases == 1, "release-side cancellation was lost");
    checkStatus(afterRelease.drain(Clock::now() + 1s), "release executor did not drain");

    GameThreadExecutor deadline;
    checkStatus(deadline.attach(), "deadline owner attach failed");
    object = std::make_shared<EngineFixtureObject>();
    object->onInvoke = [](void*) { std::this_thread::sleep_for(20ms); };
    auto timed = request(binding, object); timed.deadline = Clock::now() + 10ms;
    checkStatus(deadline.submit(std::move(timed), task), "deadline submission failed");
    checkStatus(deadline.pump(), "deadline pump failed");
    check(task.result().status.code == Error::DeadlineExceeded && task.result().utf8.empty() && object->releases == 1,
        "provider crossing its deadline published output or retained allocation");
    deadline.cancel(); checkStatus(deadline.drain(Clock::now() + 1s), "deadline executor did not drain");

    GameThreadExecutor invalidated;
    checkStatus(invalidated.attach(), "invalidation owner attach failed");
    object = std::make_shared<EngineFixtureObject>(); object->control = binding.get();
    object->onInvoke = [](void* value) { static_cast<EngineTextBinding*>(value)->invalidate(); };
    checkStatus(invalidated.submit(request(binding, object), task), "invalidation submission failed");
    checkStatus(invalidated.pump(), "invalidation pump failed");
    check(task.result().status.code == Error::StaleIdentity && task.result().utf8.empty() && object->releases == 1,
        "active invalidation published output or released its allocation incorrectly");
    invalidated.cancel(); checkStatus(invalidated.drain(Clock::now() + 1s), "invalidated executor did not drain");
}
void retirementContracts(const char* path) {
    auto* module = openModule(path);
    std::shared_ptr<EngineTextBinding> binding;
    checkStatus(EngineTextBinding::bind(path, "owned-v1", provider(module), binding), "retirement binding failed");
    dlclose(module);
    GameThreadExecutor executor;
    checkStatus(executor.attach(), "retirement owner attach failed");
    std::atomic<bool> entered{false}, leave{false};
    Error selfDrain = Error::None;
    auto object = std::shared_ptr<EngineFixtureObject>(new EngineFixtureObject, [&](EngineFixtureObject* value) {
        selfDrain = executor.drain(Clock::now() + 1s).code;
        entered = true;
        while (!leave) std::this_thread::yield();
        delete value;
    });
    EngineTextTask task;
    checkStatus(executor.submit(request(binding, object), task), "retirement submission failed");
    object.reset();
    std::thread canceller([&] { executor.cancel(); });
    while (!entered) std::this_thread::yield();
    check(executor.stats().active == 1 && executor.drain(Clock::now() + 1ms).code == Error::Busy,
        "owner retirement accounting or nonblocking owner drain failed");
    Error concurrentDrain = Error::None;
    std::thread waiter([&] { concurrentDrain = executor.drain(Clock::now() + 10ms).code; }); waiter.join();
    check(concurrentDrain == Error::DeadlineExceeded, "drain returned before accepted object lease destruction");
    leave = true; canceller.join();
    check(selfDrain == Error::Busy && task.result().state == TaskState::Cancelled,
        "lease destructor reentrant drain blocked or pending cancellation was lost");
    checkStatus(executor.drain(Clock::now() + 1s), "retired object lease did not drain");
}
#if defined(__ANDROID__)
void duplicateInstanceContracts(const char* path) {
    auto* first = openModule(path);
    auto firstProvider = provider(first);
    android_dlextinfo extension{};
    extension.flags = ANDROID_DLEXT_FORCE_LOAD;
    auto* second = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &extension);
    check(second != nullptr, "owned forced second module instance was not loaded");
    auto secondProvider = provider(second);
    check(firstProvider.invoke != secondProvider.invoke, "forced module instances share an invocation address");
    std::shared_ptr<EngineTextBinding> binding;
    check(EngineTextBinding::bind(path, "owned-v1", firstProvider, binding).code == Error::InvalidEvidence && !binding,
        "ambiguous first module instance received a lease");
    check(EngineTextBinding::bind(path, "owned-v1", secondProvider, binding).code == Error::InvalidEvidence && !binding,
        "NOLOAD reference to another instance admitted dangling second-instance functions");
    dlclose(second);
    checkStatus(EngineTextBinding::bind(path, "owned-v1", firstProvider, binding), "unique instance did not recover after duplicate unload");
    dlclose(first); binding.reset();
    auto* remaining = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    check(remaining == nullptr, "duplicate rejection retained a linker reference");
    if (remaining) dlclose(remaining);
}
#endif
}
int main(int argc, char** argv) {
    alarm(20);
    try {
        check(argc == 2, "the owned compiled module path is required");
        basicContracts(argv[1]); activeContracts(argv[1]); retirementContracts(argv[1]);
#if defined(__ANDROID__)
        duplicateInstanceContracts(argv[1]);
#endif
        std::cout << "PASS: " << checks << " engine shim/executor/FString checks; real owned DSO calls and allocator release; no UE engine invocation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
