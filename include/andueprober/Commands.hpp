#pragma once
#include "Core.hpp"

namespace andueprober {
enum class CommandKind { Detect, ProbePhase, ProbeAll, SetOverride, ClearOverride, ClearResults, Export };
struct Command {
    CommandKind kind = CommandKind::Detect;
    std::uint32_t phase = 0;
    std::string field;
    std::optional<std::uint32_t> value;
    std::uint64_t generation = 0;
};
struct CommandView {
    std::shared_ptr<const Snapshot> snapshot;
    std::uint64_t completed = 0;
    std::size_t pending = 0;
    bool accepting = false, running = false;
    Status ownerResult;
};
// Commands execute on one persistent worker. The handler owns its thread-bound providers.
// Cancellation closes admission, drops pending work and requests cancellation of the active command.
// The handler must honor cancellation and must not destroy its CommandSession owner.
class CommandSession {
public:
    using Handler = std::function<Status(const Command&, Snapshot&, const std::atomic<bool>&)>;
    CommandSession(Snapshot initial, Handler, std::size_t pendingCapacity = 16);
    ~CommandSession();
    CommandSession(const CommandSession&) = delete;
    CommandSession& operator=(const CommandSession&) = delete;
    Status start();
    Status submit(Command, std::uint64_t& id);
    void cancel() noexcept;
    Status stop();
    CommandView view() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    Session worker_;
};
}
