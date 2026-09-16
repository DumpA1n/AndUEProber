#include "Core/Budget.hpp"

namespace andueprober {

std::string Interrupted::message() const {
    return subject ? std::string(subject) + detail : std::string(detail);
}
void fail(Error code, const char* detail) { throw Interrupted{code, nullptr, detail}; }
void fail(Error code, const char* subject, const char* detail) { throw Interrupted{code, subject, detail}; }

Budget::Budget(const char* subject, const std::atomic<bool>* cancelled,
    std::chrono::steady_clock::time_point deadline, std::size_t bytes, std::size_t items)
    : subject_(subject), cancelled_(cancelled), deadline_(deadline), bytes_(bytes), items_(items) {}
Budget::Budget(const char* subject, const ReadBudget& budget, std::size_t bytes, std::size_t items)
    : Budget(subject, budget.cancelled, budget.deadline, bytes, items) {}

void Budget::check() const {
    if (cancelled_ && cancelled_->load()) fail(Error::Cancelled, subject_, " cancelled");
    if (std::chrono::steady_clock::now() >= deadline_)
        fail(Error::DeadlineExceeded, subject_, " deadline exceeded");
}
void Budget::charge(std::size_t count, std::size_t width) {
    check();
    if (count > bytes_ / width) fail(Error::BudgetExceeded, subject_, " metadata budget exceeded");
    bytes_ -= count * width;
}
void Budget::item(std::size_t count) {
    check();
    if (count > items_) fail(Error::BudgetExceeded, subject_, " item budget exceeded");
    items_ -= count;
}
void Budget::text(std::string_view value, std::size_t maximumBytes, Error invalid) {
    charge(value.size());
    if (value.empty() || value.size() > maximumBytes || value.find('\0') != std::string_view::npos ||
        !validateUtf8(value))
        fail(invalid, subject_, " requires bounded nonempty UTF-8 text");
    check();
}
void Budget::evidence(const Evidence& value) {
    charge(1, sizeof(Evidence));
    charge(value.check.size());
    charge(value.source.size());
    charge(value.relativeAddresses.size(), sizeof(std::uintptr_t));
    charge(value.sampleIdentities.size(), sizeof(std::string));
    for (const auto& identity : value.sampleIdentities) charge(identity.size());
}

} // namespace andueprober
