#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace andueprober {

enum class Error { None, InvalidArgument, Overflow, ShortRead, PermissionDenied,
    Unmapped, StaleIdentity, BudgetExceeded, DeadlineExceeded, Cancelled, Busy,
    Unsupported, Io, InvalidEvidence, Internal };
struct Status {
    Error code = Error::None;
    std::string message;
    explicit operator bool() const { return code == Error::None; }
};
struct ReadResult { std::size_t transferred = 0; Error error = Error::None; int systemError = 0; };
class MemoryReader {
public:
    virtual ~MemoryReader() = default;
    virtual ReadResult read(std::uintptr_t address, std::span<std::byte> destination) = 0;
    virtual std::uint64_t generation() const = 0;
};
struct ReadBudget {
    std::size_t remainingBytes = 1024 * 1024;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    const std::atomic<bool>* cancelled = nullptr;
    std::uint64_t generation = 0;
};
Status readExact(MemoryReader&, std::uintptr_t, std::span<std::byte>, ReadBudget&);
Status decodeUtf16(std::span<const char16_t>, std::string& utf8);
// Accepts Unicode scalar UTF-8, including NUL; callers apply identifier-specific restrictions.
Status validateUtf8(std::string_view);
Status readUtf16(MemoryReader&, std::uintptr_t data, std::int32_t count,
    std::int32_t capacity, std::size_t maximumUnits, ReadBudget&, std::string& utf8);

enum class Layout { Unknown, UProperty, FField };
enum class Origin { Profile, Probe, User };
enum class Validation { Candidate, Validated, Rejected, Stale };
enum class TaskState { Pending, Running, Succeeded, Failed, Cancelled };
struct Evidence {
    std::string check;
    bool passed = false;
    std::size_t samples = 0;
    std::vector<std::uintptr_t> relativeAddresses;
    std::string source;
    std::vector<std::string> sampleIdentities;
};
struct Offset {
    std::optional<std::uint32_t> value;
    Origin origin = Origin::Probe;
    Validation validation = Validation::Candidate;
    std::uint64_t version = 0;
    std::map<std::string, std::uint64_t> dependencies;
    std::vector<Evidence> evidence;
};
struct CandidateRejection {
    std::uint32_t offset = 0;
    Error error = Error::None;
    std::string sampleIdentity, reason;
};
struct FieldProbeReport {
    std::uint64_t generation = 0;
    std::size_t examinedOffsets = 0;
    std::vector<Offset> candidates;
    std::vector<CandidateRejection> rejected;
};
struct Snapshot {
    std::uint32_t schemaVersion = 1;
    std::string sessionId;
    std::string moduleIdentity;
    std::uint64_t generation = 0;
    Layout layout = Layout::Unknown;
    std::string layoutIdentity;
    TaskState state = TaskState::Pending;
    std::map<std::string, Offset> offsets;
    std::map<std::string, FieldProbeReport> fieldReports;
    std::vector<std::string> messages;
    Status result;
};
Status publishOffset(Snapshot&, std::string name, Offset);
Status validateSnapshot(const Snapshot&);

// Each session owns its worker. Callbacks must honor cancellation and must not destroy the session.
// Publication deep-copies working data. A working Snapshot remains allocated until session destruction.
class Session {
public:
    using Work = std::function<Status(Snapshot&, const std::atomic<bool>&)>;
    explicit Session(Snapshot initial);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Status start(Work);
    void cancel() noexcept;
    Status stop();
    std::shared_ptr<const Snapshot> snapshot() const;
private:
    mutable std::mutex mutex_;
    std::mutex joinMutex_;
    std::thread worker_;
    std::thread::id workerId_;
    std::atomic<bool> cancelled_{false};
    std::shared_ptr<const Snapshot> snapshot_;
    std::shared_ptr<Snapshot> working_;
    bool started_ = false;
    bool stopped_ = false;
};
}
