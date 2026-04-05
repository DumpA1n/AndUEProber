#include "ElfScannerManager.h"

#include <future>
#include <string_view>

#include "Utils/Logger.h"
#include "Utils/KittyEx.h"

// 由 X-macro 自动生成查找表
static constexpr struct { std::string_view soName; int index; } kLibTable[] = {
#define ELF_LIB_ENTRY(ENUM, FUNC, SO) { SO, ElfScannerManager::LIB_##ENUM },
    ELF_LIB_LIST
#undef ELF_LIB_ENTRY
};

int ElfScannerManager::libNameToIndex(std::string_view libraryName) {
    for (const auto& entry : kLibTable) {
        if (entry.soName == libraryName)
            return entry.index;
    }
    return -1;
}

bool ElfScannerManager::scanAsync(const std::set<std::string>& libraries) {
    if (libraries.empty())
        return true;

    LOGI("[ElfScannerManager] Starting async scan for %zu libraries...", libraries.size());
    auto start = std::chrono::high_resolution_clock::now();

    struct ScanTask {
        int index;
        std::string name;
    };

    // 收集需要扫描的任务
    std::vector<ScanTask> tasks;
    tasks.reserve(libraries.size());
    for (const auto& libName : libraries) {
        int idx = libNameToIndex(libName);
        if (idx < 0) {
            LOGE("[ElfScannerManager] Unknown library: %s (not in predefined list)", libName.c_str());
            continue;
        }
        if (m_scanners[idx].isValid()) {
            LOGW("[ElfScannerManager] Library already scanned: %s", libName.c_str());
            continue;
        }
        tasks.push_back({ idx, libName });
    }

    // 启动异步扫描
    std::vector<std::future<std::pair<int, ElfScanner>>> futures;
    futures.reserve(tasks.size());
    for (const auto& task : tasks) {
        futures.push_back(std::async(std::launch::async, [task]() -> std::pair<int, ElfScanner> {
            LOGI("[ElfScannerManager] Scanning library: %s", task.name.c_str());
            ElfScanner scanner;
            KT::elfScan(task.name, scanner);
            return { task.index, std::move(scanner) };
        }));
    }

    // 收集结果
    bool allSuccess = true;
    for (auto& future : futures) {
        auto [idx, scanner] = future.get();
        const auto& name = tasks[&future - futures.data()].name;

        if (!scanner.isValid()) {
            LOGE("[ElfScannerManager] Failed to scan library: %s", name.c_str());
            allSuccess = false;
            continue;
        }

        m_scanners[idx] = std::move(scanner);
        LOGI("[ElfScannerManager] %s base: 0x%llX", name.c_str(), m_scanners[idx].base());
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
    LOGI("[ElfScannerManager] Async scan completed in %f ms", elapsed);

    return allSuccess;
}
