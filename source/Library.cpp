#include "andueprober/Agent.h"
#include "andueprober/Core.hpp"
#include "andueprober/Export.hpp"
#if ANDUEPROBER_HAS_PROCESS_MEMORY
#include "andueprober/ProcessMemory.hpp"
#endif
#include "UEProber/DumperBridge.h"
#include "UEProber/UEProber.h"
#include "UEProber/ConfiguredProbeBridge.h"
#include "UI/Inspector.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>

namespace {
struct Agent {
    std::mutex mutex;
    std::shared_ptr<andueprober::Session> session;
    std::shared_ptr<andueprober::CommandSession> commands;
    std::shared_ptr<UEProber> prober;
    std::mutex drawMutex;
    std::thread::id drawThread;
    andueprober::Inspector inspector;
    std::string package;
    std::string outputRoot;
    AUEP_State state = AUEP_CREATED;
    bool stopping = false;
};
Agent& agent() { static Agent value; return value; }
}

extern "C" AUEP_Error AUEP_Initialize(const AUEP_Options* options) try {
    if (!options || options->struct_size != sizeof(AUEP_Options) || !options->expected_package ||
        !*options->expected_package || !options->output_root || !std::filesystem::path(options->output_root).is_absolute())
        return AUEP_INVALID_ARGUMENT;
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.session || runtime.commands || runtime.stopping) return AUEP_BUSY;
    if (runtime.state == AUEP_STOPPED) return AUEP_BUSY;
    if (options->expected_package != std::string(getprogname())) return AUEP_TARGET_MISMATCH;
    runtime.package = options->expected_package;
    runtime.outputRoot = options->output_root;
    runtime.state = AUEP_READY;
    return AUEP_OK;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_Start() try {
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.state != AUEP_READY) return runtime.session || runtime.commands ? AUEP_BUSY : AUEP_NOT_INITIALIZED;
    if (runtime.package != getprogname()) return AUEP_TARGET_MISMATCH;
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return AUEP_MISSING_DEPENDENCY;
#endif
    andueprober::Snapshot initial;
    initial.sessionId = "android-agent";
    initial.moduleIdentity = runtime.package;
    runtime.prober = std::make_shared<UEProber>(runtime.outputRoot);
    runtime.session = std::make_shared<andueprober::Session>(std::move(initial));
    auto status = runtime.session->start([prober = runtime.prober](andueprober::Snapshot& snapshot, const std::atomic<bool>& cancelled) {
        ConfigureProbeOperation(&cancelled);
        prober->RunAutoDumpFlow();
        snapshot = prober->GetSnapshot();
        if (cancelled.load() || prober->GetDumpStatus() == EDumpStatus::Cancelled)
            return andueprober::Status{andueprober::Error::Cancelled, "Analysis cancelled"};
        if (prober->GetDumpStatus() != EDumpStatus::Success)
            return !snapshot.result ? snapshot.result : andueprober::Status{andueprober::Error::InvalidEvidence, prober->GetDumpError()};
        return andueprober::Status{};
    });
    runtime.state = status ? AUEP_RUNNING : AUEP_FAILURE;
    return status ? AUEP_OK : AUEP_FAILED;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartIndexProbe(const AUEP_ObjectIndexOptions* options) try {
    if (!options || options->struct_size != sizeof(*options) || !options->module_path ||
        !*options->module_path || !options->module_address || !options->object_array ||
        !options->profile_id || !*options->profile_id || !options->session_id || !*options->session_id ||
        (options->layout != 1 && options->layout != 2)) return AUEP_INVALID_ARGUMENT;
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.state != AUEP_READY) return runtime.session || runtime.commands ? AUEP_BUSY : AUEP_NOT_INITIALIZED;
    if (runtime.package != getprogname()) return AUEP_TARGET_MISMATCH;
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return AUEP_MISSING_DEPENDENCY;
#else
    andueprober::ObjectArrayProfile profile;
    profile.identity = options->profile_id;
    profile.objects = options->objects_offset; profile.count = options->count_offset;
    profile.capacity = options->capacity_offset; profile.itemStride = options->item_stride;
    profile.itemObject = options->item_object_offset;
    profile.chunkCount = options->chunk_count_offset; profile.chunkCapacity = options->chunk_capacity_offset;
    profile.elementsPerChunk = options->elements_per_chunk;
    profile.maximumObjects = options->maximum_objects; profile.maximumExamined = options->maximum_examined;
    profile.sampleLimit = options->sample_limit;
    const std::string modulePath = options->module_path;
    const auto moduleAddress = options->module_address, arrayAddress = options->object_array;
    const auto extent = options->object_extent;
    andueprober::Snapshot initial;
    initial.sessionId = options->session_id;
    initial.layoutIdentity = andueprober::objectArrayLayoutIdentity(profile);
    initial.layout = options->layout == 1 ? andueprober::Layout::UProperty : andueprober::Layout::FField;
    runtime.session = std::make_shared<andueprober::Session>(std::move(initial));
    auto status = runtime.session->start([profile = std::move(profile), modulePath, moduleAddress,
        arrayAddress, extent, root = runtime.outputRoot](auto& snapshot, const auto& cancelled) {
        andueprober::ReadBudget budget;
        budget.cancelled = &cancelled;
        budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        andueprober::ProcessMemory memory;
        if (auto status = memory.open(modulePath, moduleAddress, &budget); !status) return status;
        snapshot.moduleIdentity = memory.identity(); snapshot.generation = memory.generation();
        std::vector<andueprober::Offset> candidates;
        if (auto status = andueprober::probeObjectArrayIndices(memory, arrayAddress, profile, extent, budget, candidates); !status)
            return status;
        if (candidates.size() != 1 || candidates[0].validation != andueprober::Validation::Validated)
            return andueprober::Status{andueprober::Error::InvalidEvidence, "Object index field is ambiguous or unavailable"};
        if (auto status = andueprober::publishOffset(snapshot, "UObject::InternalIndex", std::move(candidates[0])); !status)
            return status;
        const auto frozen = snapshot;
        andueprober::ExportOptions publication;
        publication.root = root; publication.cancelled = &cancelled;
        publication.deadline = budget.deadline;
        publication.dependencyRevisions = {{"AndSwapChainHook.Memory", andueprober::ProcessMemory::providerIdentity()}};
        return andueprober::publishExport(frozen, {{"observations.txt", "UObject::InternalIndex = " +
            std::to_string(*frozen.offsets.at("UObject::InternalIndex").value) + "\n"}}, publication).status;
    });
    runtime.state = status ? AUEP_RUNNING : AUEP_FAILURE;
    return status ? AUEP_OK : AUEP_FAILED;
#endif
} catch (...) { return AUEP_FAILED; }
namespace {
AUEP_Error startConfiguredProbe(ConfiguredProbeSetup setup) {
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.state != AUEP_READY) return runtime.session || runtime.commands ? AUEP_BUSY : AUEP_NOT_INITIALIZED;
    if (runtime.package != getprogname()) return AUEP_TARGET_MISMATCH;
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return AUEP_MISSING_DEPENDENCY;
#else
    andueprober::Snapshot initial; initial.sessionId = setup.sessionId;
    runtime.prober = std::make_shared<UEProber>(runtime.outputRoot);
    runtime.session = std::make_shared<andueprober::Session>(std::move(initial));
    const auto status = runtime.session->start([setup = std::move(setup), prober = runtime.prober,
        root = runtime.outputRoot](auto& snapshot, const auto& cancelled) {
        ConfigureProbeOperation(&cancelled);
        return RunConfiguredProbe(setup, *prober, root, snapshot, cancelled);
    });
    runtime.state = status ? AUEP_RUNNING : AUEP_FAILURE;
    return status ? AUEP_OK : AUEP_FAILED;
#endif
}
}
extern "C" AUEP_Error AUEP_StartStructProbe(const AUEP_StructProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyStructProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartObjectFlagProbe(const AUEP_ObjectFlagProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyObjectFlagProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartClassProbe(const AUEP_ClassProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyClassProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartFunctionProbe(const AUEP_FunctionProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyFunctionProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartFieldBaseProbe(const AUEP_FieldBaseProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyFieldBaseProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartFunctionLayoutProbe(const AUEP_FunctionProbeOptions* options,
    const AUEP_LayoutSchema* schema) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyFunctionProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    if (const auto status = CopyFunctionLayoutSchema(schema, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
#if !ANDUEPROBER_HAS_DUMPER_ADAPTER
    return AUEP_MISSING_DEPENDENCY;
#else
    return startConfiguredProbe(std::move(setup));
#endif
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartPropertyProbe(const AUEP_PropertyProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyPropertyProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartPropertyTailProbe(const AUEP_PropertyTailProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyPropertyTailProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartBoolPropertyProbe(const AUEP_BoolPropertyProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyBoolPropertyProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartFieldPathPropertyProbe(const AUEP_FieldPathPropertyProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyFieldPathPropertyProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_StartEnumProbe(const AUEP_EnumProbeOptions* options) try {
    ConfiguredProbeSetup setup;
    if (const auto status = CopyEnumProbeOptions(options, setup); !status)
        return status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_INVALID_ARGUMENT;
    return startConfiguredProbe(std::move(setup));
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_Cancel() try {
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.session) runtime.session->cancel();
    if (runtime.commands) runtime.commands->cancel();
    return AUEP_OK;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_Stop() try {
    auto& runtime = agent();
    std::shared_ptr<andueprober::Session> session;
    std::shared_ptr<andueprober::CommandSession> commands;
    {
        std::lock_guard lock(runtime.mutex);
        if (runtime.stopping) return AUEP_BUSY;
        runtime.stopping = true;
        session = runtime.session;
        commands = runtime.commands;
    }
    auto status = session ? session->stop() : andueprober::Status{};
    if (commands) status = commands->stop();
    std::lock_guard lock(runtime.mutex);
    runtime.stopping = false;
    if (!status) return AUEP_BUSY;
    runtime.state = AUEP_STOPPED;
    return AUEP_OK;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_Query(AUEP_Result* result) try {
    if (!result) return AUEP_INVALID_ARGUMENT;
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    *result = {};
    result->state = runtime.state;
    if (runtime.commands && runtime.state != AUEP_STOPPED) {
        const auto view = runtime.commands->view();
        const auto& status = view.ownerResult ? view.snapshot->result : view.ownerResult;
        result->state = view.accepting ? AUEP_RUNNING :
            view.ownerResult.code == andueprober::Error::Internal ? AUEP_FAILURE : AUEP_CANCELLED;
        result->error = status ? AUEP_OK : status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_FAILED;
        std::snprintf(result->message, sizeof(result->message), "%s", status.message.c_str());
    }
    if (runtime.session && runtime.state != AUEP_STOPPED) {
        const auto snapshot = runtime.session->snapshot();
        if (snapshot->state == andueprober::TaskState::Succeeded) result->state = AUEP_SUCCEEDED;
        if (snapshot->state == andueprober::TaskState::Failed) result->state = AUEP_FAILURE;
        if (snapshot->state == andueprober::TaskState::Cancelled) result->state = AUEP_CANCELLED;
        result->error = snapshot->result ? AUEP_OK : snapshot->result.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_FAILED;
        std::snprintf(result->message, sizeof(result->message), "%s", snapshot->result.message.c_str());
    }
    return AUEP_OK;
} catch (...) { return AUEP_FAILED; }
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

extern "C" AUEP_Error AUEP_StartInteractive() try {
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (runtime.state != AUEP_READY) return runtime.session || runtime.commands ? AUEP_BUSY : AUEP_NOT_INITIALIZED;
    if (runtime.package != getprogname()) return AUEP_TARGET_MISMATCH;
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return AUEP_MISSING_DEPENDENCY;
#else
    andueprober::Snapshot initial; initial.sessionId = "interactive-agent";
    runtime.prober = std::make_shared<UEProber>(runtime.outputRoot);
    runtime.commands = std::make_shared<andueprober::CommandSession>(std::move(initial),
        [prober = runtime.prober](const auto& command, auto& observation, const auto& cancelled) {
            ConfigureProbeOperation(&cancelled);
            return prober->ExecuteCommand(command, observation);
        });
    const auto status = runtime.commands->start();
    runtime.state = status ? AUEP_RUNNING : AUEP_FAILURE;
    return status ? AUEP_OK : AUEP_FAILED;
#endif
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_Submit(const AUEP_Command* input, uint64_t* id) try {
    if (id) *id = 0;
    if (!input || !id || input->struct_size != sizeof(*input) || input->kind < AUEP_DETECT ||
        input->kind > AUEP_INSPECT_MEMORY || input->has_value > 1 ||
        (input->field && strnlen(input->field, 1025) > 1024))
        return AUEP_INVALID_ARGUMENT;
    andueprober::Command command;
    command.kind = static_cast<andueprober::CommandKind>(input->kind - AUEP_DETECT);
    command.phase = input->phase; command.generation = input->generation;
    if (input->field) command.field = input->field;
    if (input->has_value) command.value = input->value;
    command.address = input->address;
    command.size = input->size;
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (!runtime.commands) return AUEP_NOT_INITIALIZED;
    const auto status = runtime.commands->submit(std::move(command), *id);
    return status ? AUEP_OK : status.code == andueprober::Error::Busy ? AUEP_BUSY :
        status.code == andueprober::Error::InvalidArgument ? AUEP_INVALID_ARGUMENT : AUEP_FAILED;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_QueryCommands(AUEP_CommandResult* result) try {
    if (!result || result->struct_size != sizeof(*result)) return AUEP_INVALID_ARGUMENT;
    auto& runtime = agent();
    std::lock_guard lock(runtime.mutex);
    if (!runtime.commands) return AUEP_NOT_INITIALIZED;
    const auto view = runtime.commands->view();
    *result = {}; result->struct_size = sizeof(*result);
    result->completed = view.completed; result->pending = view.pending;
    result->generation = view.snapshot->generation; result->accepting = view.accepting; result->running = view.running;
    const auto& status = view.ownerResult ? view.snapshot->result : view.ownerResult;
    result->operation.state = runtime.state == AUEP_STOPPED ? AUEP_STOPPED : view.accepting ? AUEP_RUNNING :
        view.ownerResult.code == andueprober::Error::Internal ? AUEP_FAILURE : AUEP_CANCELLED;
    result->operation.error = status ? AUEP_OK : status.code == andueprober::Error::Unsupported ? AUEP_UNSUPPORTED : AUEP_FAILED;
    std::snprintf(result->operation.message, sizeof(result->operation.message), "%s", status.message.c_str());
    return AUEP_OK;
} catch (...) { return AUEP_FAILED; }
extern "C" AUEP_Error AUEP_DrawInspector(void* context) try {
    auto& runtime = agent();
    std::shared_ptr<andueprober::CommandSession> commands;
    std::shared_ptr<andueprober::Session> session;
    {
        std::lock_guard lock(runtime.mutex);
        if (!runtime.commands && !runtime.session) return AUEP_NOT_INITIALIZED;
        commands = runtime.commands;
        session = runtime.session;
    }
    std::unique_lock drawing(runtime.drawMutex, std::try_to_lock);
    if (!drawing.owns_lock()) return AUEP_BUSY;
    const auto current = std::this_thread::get_id();
    if (runtime.drawThread != std::thread::id{} && runtime.drawThread != current) return AUEP_BUSY;
    auto status = commands ? runtime.inspector.draw(*commands, static_cast<ImGuiContext*>(context), {true, true}) :
        runtime.inspector.draw(session->snapshot(), static_cast<ImGuiContext*>(context));
    if (status) runtime.drawThread = current;
    return status ? AUEP_OK : AUEP_INVALID_ARGUMENT;
} catch (...) { return AUEP_FAILED; }
