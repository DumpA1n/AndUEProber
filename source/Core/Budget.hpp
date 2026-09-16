#pragma once

#include "andueprober/Core.hpp"

#include <string_view>

namespace andueprober {

// Thrown by fail() and by Budget, and converted back into a Status at the
// operation boundary by guard() or by an explicit catch. Every member is a
// literal so throwing never allocates; the text is composed only when reported.
struct Interrupted {
    Error code = Error::Internal;
    // Prefixed to detail when present, so one operation subject serves every
    // budget message: "Reflection freeze" + " deadline exceeded".
    const char* subject = nullptr;
    const char* detail = "";
    std::string message() const;
};
[[noreturn]] void fail(Error code, const char* detail);
[[noreturn]] void fail(Error code, const char* subject, const char* detail);

// A metered allowance over one cancellation source, one deadline, a byte budget
// and an item budget. Every accessor rechecks cancellation and the deadline
// before it spends, so an interrupted operation cannot consume further budget.
// Exhaustion raises Interrupted instead of returning a Status, which keeps the
// budget out of the signature of every helper that only meters metadata.
class Budget {
public:
    Budget(const char* subject, const std::atomic<bool>* cancelled,
        std::chrono::steady_clock::time_point deadline, std::size_t bytes, std::size_t items = 0);
    Budget(const char* subject, const ReadBudget&, std::size_t bytes, std::size_t items = 0);
    void check() const;
    void charge(std::size_t count, std::size_t width = 1);
    void item(std::size_t count = 1);
    // Charges the text and requires nonempty NUL-free UTF-8 within maximumBytes.
    void text(std::string_view value, std::size_t maximumBytes = 1024,
        Error invalid = Error::InvalidArgument);
    // Charges one evidence record: its own storage, both texts and every sample.
    void evidence(const Evidence&);

private:
    const char* subject_;
    const std::atomic<bool>* cancelled_;
    std::chrono::steady_clock::time_point deadline_;
    std::size_t bytes_, items_;
};

// Runs work and reports Interrupted, and any other exception, as a Status.
template<class Work>
Status guard(Work&& work) {
    try {
        return work();
    } catch (const Interrupted& stop) {
        try { return {stop.code, stop.message()}; } catch (...) { return {stop.code, {}}; }
    } catch (...) {
        return {Error::Internal, {}};
    }
}

} // namespace andueprober
