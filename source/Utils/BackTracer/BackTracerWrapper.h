#pragma once

#include "BackTracer.h"
#include "Logger.h"

#define PrintCallStack()                                                                 \
    BackTracer::Trace(128, true, [](const TraceContext& ctx) {                           \
        LOGI(std::format(                                                                \
                "    #{:02d}    {:010X}    {}\t{}",                                      \
                ctx.depth,                                                               \
                ctx.lr - (uintptr_t)ctx.info.dli_fbase,                                  \
                ctx.info.dli_fname ? (BackTracer::Trim(ctx.info.dli_fname)) : "UNKNOWN", \
                ctx.info.dli_sname ? ctx.info.dli_sname : ""                             \
            ).c_str()                                                                    \
        );                                                                               \
    })

#define LogToFileCallStack(logFileName)                                                  \
    BackTracer::Trace(128, true, [&](const TraceContext& ctx) {                          \
        GetLogFile(logFileName)->Append(                                                 \
            std::format(                                                                 \
                "{}    #{:02d}    {:010X}    {}\t{}\n",                                  \
                FormatedTime(),                                                          \
                ctx.depth,                                                               \
                ctx.lr - (uintptr_t)ctx.info.dli_fbase,                                  \
                ctx.info.dli_fname ? (BackTracer::Trim(ctx.info.dli_fname)) : "UNKNOWN", \
                ctx.info.dli_sname ? ctx.info.dli_sname : ""                             \
            )                                                                            \
        );                                                                               \
    })
