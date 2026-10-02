#pragma once

#include <cstddef>
#include <cstdint>

namespace UEMemory {
using BoundedRead = bool (*)(void*, std::uintptr_t, void*, std::size_t) noexcept;
// The probe answers IsPtrReadable through the same channel as the reader, but a
// failed probe is an expected negative answer rather than a read failure.
void SetBoundedReader(void* context, BoundedRead reader, BoundedRead probe = nullptr) noexcept;
void ClearBoundedReader() noexcept;
}
