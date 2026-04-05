#pragma once

#include "FileIO.h"
#include "KittyEx.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

inline std::string FormatedTime()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_r(&time_t_now, &local_tm);
    
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

inline std::string FormatedTimeShort()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_r(&time_t_now, &local_tm);
    
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%m-%d_%H-%M-%S");
    return oss.str();
}

inline FileIO* GetLogFile(const std::string& dir)
{
    namespace fs = std::filesystem;
    static std::unordered_map<std::string, std::unique_ptr<FileIO>> g_log_files;
    static std::mutex g_log_files_mutex;
    std::lock_guard<std::mutex> lock(g_log_files_mutex);
    auto [it, inserted] = g_log_files.try_emplace(dir, nullptr);
    if (inserted)
    {
        fs::path saved_path = fs::path(KittyUtils::getExternalStorage()) / "Android" / "data" / getprogname() / "cache";
        fs::path final_path = saved_path / kPROJECT_NAME / dir;
        fs::path file_name = dir + "_" + FormatedTimeShort() + ".log";
        it->second = std::make_unique<FileIO>(final_path / file_name);
        it->second->Open();
    }
    return it->second.get();
}
