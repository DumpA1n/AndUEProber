#include "andueprober/Core.hpp"

#include <limits>
#include <utility>

namespace andueprober {
Status validateUtf8(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        auto value = static_cast<unsigned char>(bytes[i++]);
        if (value < 128) continue;
        unsigned count = 0, minimum = 0, cp = 0;
        if (value >= 0xc2 && value <= 0xdf) { count = 1; minimum = 0x80; cp = value & 31; }
        else if (value >= 0xe0 && value <= 0xef) { count = 2; minimum = 0x800; cp = value & 15; }
        else if (value >= 0xf0 && value <= 0xf4) { count = 3; minimum = 0x10000; cp = value & 7; }
        else return {Error::InvalidArgument, "Text has invalid UTF-8"};
        if (count > bytes.size() - i) return {Error::InvalidArgument, "Text has truncated UTF-8"};
        while (count--) {
            value = static_cast<unsigned char>(bytes[i++]);
            if ((value & 0xc0) != 0x80) return {Error::InvalidArgument, "Text has invalid UTF-8 continuation"};
            cp = (cp << 6) | (value & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return {Error::InvalidArgument, "Text has an invalid Unicode scalar"};
    }
    return {};
}

Status readExact(MemoryReader& reader, std::uintptr_t address, std::span<std::byte> out, ReadBudget& budget) {
    if (out.size() > std::numeric_limits<std::uintptr_t>::max() - address)
        return {Error::Overflow, "Read range overflows the address space"};
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Read cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Read deadline exceeded"};
    if (reader.generation() != budget.generation) return {Error::StaleIdentity, "Module generation changed"};
    if (out.size() > budget.remainingBytes) return {Error::BudgetExceeded, "Read budget exhausted"};
    budget.remainingBytes -= out.size();
    auto result = reader.read(address, out);
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Read cancelled during provider execution"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Read deadline exceeded during provider execution"};
    if (reader.generation() != budget.generation) return {Error::StaleIdentity, "Module generation changed during read"};
    if (result.error != Error::None) return {result.error, "Memory provider rejected the read; system error " + std::to_string(result.systemError)};
    if (result.transferred != out.size()) return {Error::ShortRead, "Memory provider returned a short read"};
    return {};
}

Status readUtf16(MemoryReader& reader, std::uintptr_t data, std::int32_t count,
    std::int32_t capacity, std::size_t maximumUnits, ReadBudget& budget, std::string& out) {
    out.clear();
    if (count < 0 || capacity < count || static_cast<std::size_t>(capacity) > maximumUnits ||
        maximumUnits > std::numeric_limits<std::size_t>::max() / sizeof(char16_t))
        return {Error::InvalidArgument, "FString length or capacity is outside the permitted range"};
    if (count == 0) return {};
    if (!data) return {Error::InvalidArgument, "FString data is null"};
    std::vector<char16_t> units(static_cast<std::size_t>(count));
    auto status = readExact(reader, data, std::as_writable_bytes(std::span(units)), budget);
    if (!status) return status;
    if (units.back() != 0) return {Error::InvalidArgument, "FString is not terminated within its length"};
    return decodeUtf16(std::span(units).first(units.size() - 1), out);
}

Status decodeUtf16(std::span<const char16_t> units, std::string& out) {
    out.clear();
    std::string result;
    for (std::size_t i = 0; i < units.size(); ++i) {
        std::uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= units.size() || units[i + 1] < 0xDC00 || units[i + 1] > 0xDFFF)
                return {Error::InvalidArgument, "Text contains an unpaired UTF-16 surrogate"};
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return {Error::InvalidArgument, "Text contains an unpaired UTF-16 surrogate"};
        }
        if (cp < 0x80) result.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    out = std::move(result);
    return {};
}

Status publishOffset(Snapshot& snapshot, std::string name, Offset value) {
    if (name.empty()) return {Error::InvalidArgument, "An offset name is required"};
    auto old = snapshot.offsets.find(name);
    if (old != snapshot.offsets.end() && old->second.version == std::numeric_limits<std::uint64_t>::max())
        return {Error::Overflow, "Offset version capacity exhausted"};
    if (old != snapshot.offsets.end() && old->second.origin == Origin::User && value.origin != Origin::User)
        return {Error::InvalidEvidence, "An automatic result cannot replace an explicit user override"};
    value.version = old == snapshot.offsets.end() ? 1 : old->second.version + 1;
    snapshot.offsets[std::move(name)] = std::move(value);
    bool changed;
    do {
        changed = false;
        for (auto& [key, offset] : snapshot.offsets) {
            if (offset.validation == Validation::Stale) continue;
            for (const auto& [dependency, version] : offset.dependencies) {
                auto source = snapshot.offsets.find(dependency);
                if (source == snapshot.offsets.end() || source->second.version != version || source->second.validation == Validation::Stale) {
                    offset.validation = Validation::Stale;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
    return {};
}
Status validateSnapshot(const Snapshot& snapshot) {
    if (snapshot.state == TaskState::Failed || snapshot.state == TaskState::Cancelled || !snapshot.result)
        return {Error::InvalidEvidence, "A failed or cancelled observation cannot be published"};
    if (snapshot.schemaVersion != 1 || snapshot.sessionId.empty() || snapshot.moduleIdentity.empty() || snapshot.layout == Layout::Unknown || snapshot.layoutIdentity.empty() || snapshot.offsets.empty())
        return {Error::InvalidEvidence, "Snapshot identity, layout and offsets are required"};
    for (const auto& [name, offset] : snapshot.offsets) {
        if (name.empty() || !offset.value || !offset.version || offset.validation != Validation::Validated || offset.evidence.empty())
            return {Error::InvalidEvidence, "Export requires validated offsets with evidence"};
        for (const auto& item : offset.evidence)
            if (item.check.empty() || !item.passed || !item.samples) return {Error::InvalidEvidence, "Snapshot contains failed or empty evidence"};
        for (const auto& [key, version] : offset.dependencies) {
            auto source = snapshot.offsets.find(key);
            if (source == snapshot.offsets.end() || source->second.version != version)
                return {Error::InvalidEvidence, "Snapshot contains a stale dependency"};
        }
    }
    std::map<std::string, unsigned> visited;
    struct Frame {
        const Offset* offset;
        decltype(Offset{}.dependencies)::const_iterator next;
        unsigned* state;
    };
    std::vector<Frame> stack;
    for (const auto& [name, offset] : snapshot.offsets) {
        auto& state = visited[name];
        if (state == 2) continue;
        state = 1;
        stack.push_back({&offset, offset.dependencies.begin(), &state});
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == frame.offset->dependencies.end()) {
                *frame.state = 2;
                stack.pop_back();
                continue;
            }
            const auto& dependency = (frame.next++)->first;
            auto& dependencyState = visited[dependency];
            if (dependencyState == 1) return {Error::InvalidEvidence, "Snapshot dependencies contain a cycle"};
            if (dependencyState == 2) continue;
            dependencyState = 1;
            const auto& source = snapshot.offsets.at(dependency);
            stack.push_back({&source, source.dependencies.begin(), &dependencyState});
        }
    }
    return {};
}
Session::Session(Snapshot initial) : snapshot_(std::make_shared<const Snapshot>(initial)) {}
Session::~Session() { stop(); }
Status Session::start(Work work) {
    std::lock_guard lock(mutex_);
    if (!work) return {Error::InvalidArgument, "Session work is required"};
    if (started_ || stopped_) return {Error::Busy, "A session has one execution owner; create a new session to restart"};
    auto running = *snapshot_;
    running.state = TaskState::Running;
    snapshot_ = std::make_shared<const Snapshot>(running);
    started_ = true;
    try {
        working_ = std::make_shared<Snapshot>(running);
        auto result = std::make_shared<Snapshot>(std::move(running));
        worker_ = std::thread([this, work = std::move(work), result = std::move(result), working = working_]() mutable {
            try { working->result = work(*working, cancelled_); }
            catch (...) { working->result.code = Error::Internal; working->result.message.clear(); }
            try { *result = *working; }
            catch (...) { result->result.code = Error::Internal; result->result.message.clear(); }
            result->state = cancelled_.load() || result->result.code == Error::Cancelled ? TaskState::Cancelled :
                result->result ? TaskState::Succeeded : TaskState::Failed;
            std::lock_guard completed(mutex_);
            snapshot_ = std::move(result);
        });
        workerId_ = worker_.get_id();
    } catch (const std::exception& error) {
        auto failed = *snapshot_;
        failed.state = TaskState::Failed;
        failed.result = {Error::Internal, error.what()};
        snapshot_ = std::make_shared<const Snapshot>(std::move(failed));
        return snapshot_->result;
    }
    return {};
}
void Session::cancel() noexcept { cancelled_.store(true); }
Status Session::stop() {
    cancel();
    {
        std::lock_guard lock(mutex_);
        if (!started_ && !stopped_) {
            auto cancelled = std::make_shared<Snapshot>(*snapshot_);
            cancelled->state = TaskState::Cancelled;
            cancelled->result = {Error::Cancelled, "Session stopped before execution"};
            snapshot_ = std::move(cancelled);
        }
        stopped_ = true;
        if (workerId_ == std::this_thread::get_id())
            return {Error::Busy, "Worker requested cancellation; an external owner must join"};
    }
    std::lock_guard joining(joinMutex_);
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    { std::lock_guard lock(mutex_); workerId_ = {}; }
    return {};
}
std::shared_ptr<const Snapshot> Session::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}
}
