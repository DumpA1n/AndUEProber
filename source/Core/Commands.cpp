#include "andueprober/Commands.hpp"
#include <condition_variable>
#include <deque>
#include <limits>

namespace andueprober {
struct CommandSession::State {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::pair<std::uint64_t, Command>> commands;
    std::shared_ptr<const Snapshot> snapshot;
    Handler handler;
    std::size_t capacity;
    std::uint64_t next = 1, completed = 0;
    bool started = false, closed = false, running = false;
    State(Snapshot initial, Handler work, std::size_t limit)
        : snapshot(std::make_shared<const Snapshot>(initial)), handler(std::move(work)), capacity(limit) {}
};
CommandSession::CommandSession(Snapshot initial, Handler handler, std::size_t capacity)
    : state_(std::make_shared<State>(initial, std::move(handler), capacity)), worker_(std::move(initial)) {}
CommandSession::~CommandSession() { stop(); }
Status CommandSession::start() {
    std::lock_guard lock(state_->mutex);
    if (state_->started || state_->closed) return {Error::Busy, "A command session cannot restart"};
    if (!state_->handler || !state_->capacity || state_->capacity > 1024)
        return {Error::InvalidArgument, "A handler and bounded pending capacity are required"};
    state_->started = true;
    const auto status = worker_.start([state = state_](Snapshot&, const std::atomic<bool>& cancelled) {
        struct Close {
            std::shared_ptr<State> state;
            ~Close() { std::lock_guard lock(state->mutex); state->closed = true; state->running = false; state->commands.clear(); }
        } close{state};
        for (;;) {
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return state->closed || cancelled.load() || !state->commands.empty(); });
            if (state->closed || cancelled.load()) return Status{Error::Cancelled, "Command session cancelled"};
            auto [id, command] = std::move(state->commands.front());
            state->commands.pop_front();
            auto observation = *state->snapshot;
            state->running = true;
            lock.unlock();
            observation.state = TaskState::Running;
            observation.result = {};
            observation.result = state->handler(command, observation, cancelled);
            observation.state = cancelled.load() || observation.result.code == Error::Cancelled ? TaskState::Cancelled :
                observation.result ? TaskState::Succeeded : TaskState::Failed;
            auto published = std::make_shared<const Snapshot>(observation);
            lock.lock();
            state->snapshot = std::move(published);
            state->completed = id;
            state->running = false;
        }
    });
    if (!status) state_->closed = true;
    return status;
}
Status CommandSession::submit(Command command, std::uint64_t& id) {
    id = 0;
    if (command.kind < CommandKind::Detect || command.kind > CommandKind::InspectMemory || command.field.size() > 1024 ||
        (command.kind == CommandKind::ProbePhase && (command.phase < 1 || command.phase > 6)) ||
        ((command.kind == CommandKind::SetOverride || command.kind == CommandKind::ClearOverride) && command.field.empty()) ||
        (command.kind == CommandKind::SetOverride && (!command.value || *command.value > INT32_MAX)) ||
        (command.kind == CommandKind::InspectMemory && (!command.address || !command.size || command.size > 512)))
        return {Error::InvalidArgument, "Invalid inspector command"};
    std::lock_guard lock(state_->mutex);
    if (!state_->started || state_->closed) return {Error::Busy, "The command session is not accepting work"};
    if (state_->commands.size() >= state_->capacity) return {Error::Busy, "The pending command capacity is exhausted"};
    if (state_->next == std::numeric_limits<std::uint64_t>::max()) return {Error::Overflow, "Command identifier capacity is exhausted"};
    state_->commands.emplace_back(state_->next, std::move(command));
    id = state_->next++;
    state_->ready.notify_one();
    return {};
}
void CommandSession::cancel() noexcept {
    worker_.cancel();
    { std::lock_guard lock(state_->mutex); state_->closed = true; state_->commands.clear(); }
    state_->ready.notify_all();
}
Status CommandSession::stop() { cancel(); return worker_.stop(); }
CommandView CommandSession::view() const {
    CommandView result;
    {
        std::lock_guard lock(state_->mutex);
        result.snapshot = state_->snapshot; result.completed = state_->completed;
        result.pending = state_->commands.size(); result.accepting = state_->started && !state_->closed;
        result.running = state_->running;
    }
    result.ownerResult = worker_.snapshot()->result;
    return result;
}
}
