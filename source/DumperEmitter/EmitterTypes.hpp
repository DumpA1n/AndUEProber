#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace andueprober::dumper_emitter {

enum class Error {
    None,
    InvalidInput,
    Unsupported,
    InputLimit,
    OutputLimit,
    Cancelled,
    DeadlineExceeded,
    AllocationFailure,
    FormatFailure,
};

struct Limits {
    std::size_t maxInputBytes{1024 * 1024};
    std::size_t maxItems{4096};
    std::size_t maxOutputBytes{1024 * 1024};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
    bool (*cancelled)(void*) noexcept{nullptr};
    void* cancellationContext{nullptr};
};

struct Result {
    Error error{Error::None};
    std::string output;
    explicit operator bool() const noexcept { return error == Error::None; }
};

std::string formatEnumValue(std::string_view underlying, std::uint64_t bits);

} // namespace andueprober::dumper_emitter
