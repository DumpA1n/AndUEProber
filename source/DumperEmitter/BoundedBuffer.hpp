#pragma once

#include "EmitterTypes.hpp"

#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

namespace andueprober::dumper_emitter {

struct Stop {
    Error error;
};

inline void checkpoint(const Limits& limits) {
    if (limits.cancelled && limits.cancelled(limits.cancellationContext))
        throw Stop{Error::Cancelled};
    if (std::chrono::steady_clock::now() >= limits.deadline)
        throw Stop{Error::DeadlineExceeded};
}

class BufferFmt {
    const Limits& limits_;
    std::unique_ptr<char[]> data_;
    std::size_t size_{};

    struct OutputIterator {
        using iterator_category = std::output_iterator_tag;
        using value_type = void;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = void;
        BufferFmt* buffer;
        OutputIterator& operator*() noexcept { return *this; }
        OutputIterator& operator++() noexcept { return *this; }
        OutputIterator operator++(int) noexcept { return *this; }
        OutputIterator& operator=(char value) {
            buffer->put(value);
            return *this;
        }
    };

    void put(char value) {
        if ((size_ & 255) == 0) checkpoint(limits_);
        if (size_ == limits_.maxOutputBytes) throw Stop{Error::OutputLimit};
        data_[size_++] = value;
    }

public:
    explicit BufferFmt(const Limits& limits) : limits_(limits) {
        checkpoint(limits_);
        data_ = std::make_unique<char[]>(limits_.maxOutputBytes);
    }
    const Limits& limits() const noexcept { return limits_; }
    bool empty() const noexcept { return size_ == 0; }
    std::string read() const { return {data_.get(), size_}; }

    template <typename... Args>
    void append(fmt::format_string<Args...> format, Args&&... args) {
        checkpoint(limits_);
        fmt::format_to(OutputIterator{this}, format, std::forward<Args>(args)...);
        checkpoint(limits_);
    }
};

} // namespace andueprober::dumper_emitter
