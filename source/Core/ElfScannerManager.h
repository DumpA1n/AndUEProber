#pragma once

#include <array>
#include <set>
#include <string>
#include <string_view>

#include "Utils/KittyEx.h"

// ============================================================
//  X-macro 库注册表 — 新增库只需在此添加一行
//  格式: ELF_LIB_ENTRY(枚举名, 访问方法名, "完整so文件名")
// ============================================================
#define ELF_LIB_LIST                                                    \
    ELF_LIB_ENTRY(C,               c,               "libc.so")               \
    ELF_LIB_ENTRY(CRYPTO,          crypto,          "libcrypto.so")          \
    ELF_LIB_ENTRY(UE4,             UE4,             "libUE4.so")             \
    ELF_LIB_ENTRY(UNITY,           unity,           "libUnity.so")           \
    ELF_LIB_ENTRY(IL2CPP,          il2cpp,          "libil2cpp.so")          \
    ELF_LIB_ENTRY(TERSAFE,         tersafe,         "libtersafe.so")         \
    ELF_LIB_ENTRY(GAME,            game,            "libgame.so")            \
    ELF_LIB_ENTRY(VULKAN,          vulkan,          "libvulkan.so")          \
    ELF_LIB_ENTRY(INPUT,           input,           "libinput.so")           \
    ELF_LIB_ENTRY(ART,             art,             "libart.so")             \
    ELF_LIB_ENTRY(ANDROID_RUNTIME, android_runtime, "libandroid_runtime.so")

class ElfScannerManager {
public:
    // 由 X-macro 自动生成枚举
    enum Lib : int {
#define ELF_LIB_ENTRY(ENUM, FUNC, SO) LIB_##ENUM,
        ELF_LIB_LIST
#undef ELF_LIB_ENTRY
        LIB_COUNT
    };

    static ElfScannerManager& GetInstance() {
        static ElfScannerManager g_Instance;
        return g_Instance;
    }

    ElfScannerManager(const ElfScannerManager&) = delete;
    ElfScannerManager& operator=(const ElfScannerManager&) = delete;

    /**
     * 异步批量扫描多个库
     * @param libraries 库名称列表（如 "libc.so", "libUE4.so"）
     * @return 是否全部成功扫描
     */
    bool scanAsync(const std::set<std::string>& libraries);

    // 由 X-macro 自动生成访问方法（O(1) 数组索引，无锁）
#define ELF_LIB_ENTRY(ENUM, FUNC, SO) \
    ElfScanner& FUNC() { return m_scanners[LIB_##ENUM]; }
    ELF_LIB_LIST
#undef ELF_LIB_ENTRY

private:
    ElfScannerManager() = default;
    ~ElfScannerManager() = default;

    static int libNameToIndex(std::string_view libraryName);

    std::array<ElfScanner, LIB_COUNT> m_scanners{};
};

// 全局访问别名
inline ElfScannerManager& Elf = ElfScannerManager::GetInstance();
