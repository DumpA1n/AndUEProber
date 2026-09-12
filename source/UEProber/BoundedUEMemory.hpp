#pragma once

#include <cstddef>
#include <cstdint>

namespace UEMemory {
using BoundedRead = bool (*)(void*, std::uintptr_t, void*, std::size_t) noexcept;
void SetBoundedReader(void* context, BoundedRead reader) noexcept;
void ClearBoundedReader() noexcept;
}
