cmake_minimum_required(VERSION 3.22.1)

if(DEFINED ENV{NDK_HOME})
    set(ANDROID_PLATFORM 27)
    set(ANDROID_ABI arm64-v8a)
    file(TO_CMAKE_PATH "$ENV{NDK_HOME}" NDK_HOME_CMAKE)
    set(CMAKE_TOOLCHAIN_FILE "${NDK_HOME_CMAKE}/build/cmake/android.toolchain.cmake")
else()
    message(FATAL_ERROR "NDK_HOME environment variable not set. Please set it to the path of your Android NDK.")
endif()

project(AndUEProber C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for smaller binary size
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-fvisibility=hidden -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL)
endif()

# ============================================================
#  AndSwapChainHook — compile as a static library
# ============================================================
set(ANDSWAPCHAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/AndSwapChainHook)

file(GLOB_RECURSE ANDSWAPCHAIN_SOURCES
    ${ANDSWAPCHAIN_DIR}/source/*.cpp
    ${ANDSWAPCHAIN_DIR}/source/*.c
)
list(FILTER ANDSWAPCHAIN_SOURCES EXCLUDE REGEX "android_native_app_glue\\.c$")
list(FILTER ANDSWAPCHAIN_SOURCES EXCLUDE REGEX "Library\\.cpp$")

file(GLOB IMGUI_SOURCES
    ${ANDSWAPCHAIN_DIR}/external/imgui/imgui.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/imgui_demo.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/imgui_draw.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/imgui_tables.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/imgui_widgets.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/backends/imgui_impl_android.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
    ${ANDSWAPCHAIN_DIR}/external/imgui/backends/imgui_impl_opengl3.cpp
)

file(GLOB KITTYMEMORY_SOURCES
    ${ANDSWAPCHAIN_DIR}/external/KittyMemory/*.cpp
    ${ANDSWAPCHAIN_DIR}/external/KittyMemory/*.c
)
list(FILTER KITTYMEMORY_SOURCES EXCLUDE REGEX "MemoryPatch\\.cpp$")
list(FILTER KITTYMEMORY_SOURCES EXCLUDE REGEX "MemoryBackup\\.cpp$")

add_library(AndSwapChainHookLib STATIC
    ${ANDSWAPCHAIN_SOURCES}
    ${IMGUI_SOURCES}
    ${KITTYMEMORY_SOURCES}
    ${ANDSWAPCHAIN_DIR}/source/PointerHook/trampoline.s
)

target_include_directories(AndSwapChainHookLib PUBLIC
    ${ANDSWAPCHAIN_DIR}/source
    ${ANDSWAPCHAIN_DIR}/source/Utils
    ${ANDSWAPCHAIN_DIR}/external
    ${ANDSWAPCHAIN_DIR}/external/imgui
)

target_compile_definitions(AndSwapChainHookLib PRIVATE
    kBUILD_LIBRARY
    kNO_KEYSTONE
    kANDROID_LOG
    kANDROID_LOG_TAG="INJECT"
    kPROJECT_NAME="${PROJECT_NAME}"
)

target_compile_options(AndSwapChainHookLib PRIVATE -std=c++20 -fexceptions)
target_link_libraries(AndSwapChainHookLib android log vulkan GLESv3 EGL)
# ============================================================

# ============================================================
#  Apply patches to AndUEDumper before compiling
# ============================================================
set(ANDUEDUMPER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper)

function(apply_anduedumper_patch patch_name)
    execute_process(
        COMMAND git apply --check ${CMAKE_CURRENT_SOURCE_DIR}/misc/${patch_name}
        WORKING_DIRECTORY ${ANDUEDUMPER_ROOT}
        RESULT_VARIABLE PATCH_RESULT
        ERROR_QUIET
    )
    if(PATCH_RESULT EQUAL 0)
        execute_process(
            COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/misc/${patch_name}
            WORKING_DIRECTORY ${ANDUEDUMPER_ROOT}
        )
        message(STATUS "Applied ${patch_name}")
    else()
        message(STATUS "${patch_name} already applied or not applicable")
    endif()
endfunction()

apply_anduedumper_patch(UEWrappers_case_insensitive.patch)
apply_anduedumper_patch(AIOHeader_sort_packages.patch)

# ============================================================
#  AndUEDumper — compile core dumper as a static library
# ============================================================
set(ANDUEDUMPER_DIR  ${ANDUEDUMPER_ROOT}/AndUEDumper)
set(ANDUEDUMPER_SRC  ${ANDUEDUMPER_DIR}/src)
set(ANDUEDUMPER_DEPS ${ANDUEDUMPER_ROOT}/deps)
set(KITTYMEMORYEX_DIR ${ANDUEDUMPER_DEPS}/KittyMemoryEx/KittyMemoryEx)
file(GLOB ANDUEDUMPER_UE_SRC ${ANDUEDUMPER_SRC}/UE/*.cpp)
file(GLOB ANDUEDUMPER_UTILS_SRC ${ANDUEDUMPER_SRC}/Utils/*.cpp)
file(GLOB KITTYMEMORYEX_SRC ${KITTYMEMORYEX_DIR}/*.cpp)

add_library(AndUEDumperLib STATIC
    ${ANDUEDUMPER_UE_SRC}
    ${ANDUEDUMPER_UTILS_SRC}
    ${ANDUEDUMPER_SRC}/Dumper.cpp
    ${ANDUEDUMPER_SRC}/UPackageGenerator.cpp
    ${KITTYMEMORYEX_SRC}
    ${ANDUEDUMPER_DEPS}/fmt/format.cc
)

target_include_directories(AndUEDumperLib PRIVATE
    ${KITTYMEMORYEX_DIR}
    ${KITTYMEMORYEX_DIR}/Deps
    ${ANDUEDUMPER_DEPS}
    ${ANDUEDUMPER_SRC}
)

target_compile_definitions(AndUEDumperLib PRIVATE
    kNO_KEYSTONE
)

target_compile_options(AndUEDumperLib PRIVATE -std=c++20 -fexceptions)
target_link_libraries(AndUEDumperLib log)
# ============================================================

set(LIBS
    ${ANDSWAPCHAIN_DIR}/external/Dobby/arm64-v8a/libdobby.a
)

# UEProber-specific sources
file(GLOB_RECURSE UEPROBER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/UEProber/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/UEProber/*.c
)
add_library(${PROJECT_NAME} SHARED 
    ${CMAKE_CURRENT_SOURCE_DIR}/source/Library.cpp
    ${UEPROBER_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${KITTYMEMORYEX_DIR}
    ${KITTYMEMORYEX_DIR}/Deps
    ${ANDUEDUMPER_DEPS}
    ${ANDUEDUMPER_SRC}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    kBUILD_LIBRARY
    kNO_KEYSTONE # KittyMemory
    kENABLE_FORMAT_LOG
    kANDROID_LOG
    kANDROID_LOG_TAG="INJECT"
    kPROJECT_NAME="${PROJECT_NAME}"
)

# Allow multiple definitions: KittyMemory (in-process) and KittyMemoryEx (cross-process)
# share some identical function implementations in KittyUtils namespace.
# The linker picks the first definition found; all duplicates have identical implementations.
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)

target_link_libraries(${PROJECT_NAME} ${LIBS} AndSwapChainHookLib AndUEDumperLib)
