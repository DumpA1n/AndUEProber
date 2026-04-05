#pragma once

#include <string>
#include <cstdarg>
#include <cstdio>

#ifdef kANDROID_LOG
#include <android/log.h>
#ifndef kNO_DEBUG_LOG
#define kNO_DEBUG_LOG
#endif
#endif

namespace LogHelper {
    enum class Level : uint8_t { Debug = 0, Info, Warn, Error, Fatal };

    // Single platform dispatch for printf-style logging
    inline void emitRaw(Level level, const char* fmt, ...)
#ifdef __GNUC__
        __attribute__((format(printf, 2, 3)))
#endif
    {
        va_list args;
        va_start(args, fmt);
#ifdef kANDROID_LOG
        static constexpr int kPrio[] = {
            ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
            ANDROID_LOG_ERROR, ANDROID_LOG_FATAL
        };
        __android_log_vprint(kPrio[static_cast<uint8_t>(level)], kANDROID_LOG_TAG, fmt, args);
#else
        vprintf(fmt, args);
        putchar('\n');
#endif
        va_end(args);
    }
}

#ifndef kNO_DEBUG_LOG
#define LOGD(fmt, ...) LogHelper::emitRaw(LogHelper::Level::Debug, "[%s(%d) %s] " fmt, GetNameFromFullPath(__FILE__).c_str(), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) do {} while(0)
#endif

#define LOGI(fmt, ...) LogHelper::emitRaw(LogHelper::Level::Info, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LogHelper::emitRaw(LogHelper::Level::Warn, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LogHelper::emitRaw(LogHelper::Level::Error, fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) LogHelper::emitRaw(LogHelper::Level::Fatal, fmt, ##__VA_ARGS__)

#ifdef kENABLE_FORMAT_LOG
#include <format>

template<typename T>
concept StringLike = std::convertible_to<T, std::string> || std::convertible_to<T, std::string_view>;

namespace LogHelper {
    inline void emitStr(Level level, const char* msg) {
#ifdef kANDROID_LOG
        static constexpr int kPrio[] = {
            ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
            ANDROID_LOG_ERROR, ANDROID_LOG_FATAL
        };
        __android_log_print(kPrio[static_cast<uint8_t>(level)], kANDROID_LOG_TAG, "%s", msg);
#else
        static constexpr const char* kPrefix[] = {"D: ", "I: ", "W: ", "E: ", "F: "};
        printf("%s%s\n", kPrefix[static_cast<uint8_t>(level)], msg);
#endif
    }

    template<StringLike T>
    inline std::string formatLog(const T& msg) {
        return std::string(msg);
    }

    template<typename... Args>
    inline std::string formatLog(std::format_string<Args...> fmt, Args&&... args) {
        return std::format(fmt, std::forward<Args>(args)...);
    }
}

#ifndef kNO_DEBUG_LOG
#define FLOGD(fmt, ...) LogHelper::emitStr(LogHelper::Level::Debug, LogHelper::formatLog(fmt, ##__VA_ARGS__).c_str())
#else
#define FLOGD(fmt, ...) do {} while(0)
#endif

#define FLOGI(fmt, ...) LogHelper::emitStr(LogHelper::Level::Info, LogHelper::formatLog(fmt, ##__VA_ARGS__).c_str())
#define FLOGW(fmt, ...) LogHelper::emitStr(LogHelper::Level::Warn, LogHelper::formatLog(fmt, ##__VA_ARGS__).c_str())
#define FLOGE(fmt, ...) LogHelper::emitStr(LogHelper::Level::Error, LogHelper::formatLog(fmt, ##__VA_ARGS__).c_str())
#define FLOGF(fmt, ...) LogHelper::emitStr(LogHelper::Level::Fatal, LogHelper::formatLog(fmt, ##__VA_ARGS__).c_str())

#else // !kENABLE_FORMAT_LOG

#define FLOGD(fmt, ...) do {} while(0)
#define FLOGI(fmt, ...) do {} while(0)
#define FLOGW(fmt, ...) do {} while(0)
#define FLOGE(fmt, ...) do {} while(0)
#define FLOGF(fmt, ...) do {} while(0)

#endif // kENABLE_FORMAT_LOG

inline std::string GetNameFromFullPath(const std::string& dir)
{
    auto slashPos = dir.rfind('\\');
    if (slashPos == std::string::npos) {
        slashPos = dir.rfind('/');
    }
    if (slashPos == std::string::npos) {
        return dir;
    }
    return dir.substr(slashPos + 1);
}
