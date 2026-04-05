#pragma once

#include <cstdint>
#include <dlfcn.h>
#include <string>
#include <functional>

struct TraceContext {
    uintptr_t fp;  // Frame pointer (x29)
    uintptr_t lr;  // Link register (x30)
    Dl_info info;  // Called function info
    int32_t depth; // Current depth
};

namespace BackTracer {

    using tracer_callback_t = std::function<void(const TraceContext&)>;

    std::string Trim(const std::string& str);

    void Trace(int32_t max_depth, bool use_cache, const tracer_callback_t& callback = nullptr);

    void Trace(const tracer_callback_t& callback = nullptr);

}
