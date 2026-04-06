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

file(GLOB_RECURSE COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/*.c
)

# Exclude android_native_app_glue.c (only the header is needed for struct definitions)
list(FILTER COMMON_SOURCES EXCLUDE REGEX "android_native_app_glue\\.c$")

# Exclude DumpIntegration.cpp — compiled in AndUEDumperLib with KittyMemoryEx include paths
list(FILTER COMMON_SOURCES EXCLUDE REGEX "DumpIntegration\\.cpp$")

file(GLOB IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_android.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_opengl3.cpp
)

file(GLOB KITTYMEMORY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/external/KittyMemory/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/KittyMemory/*.c
)

# Exclude files that conflict with KittyMemoryEx (different class layouts, same symbols)
list(FILTER KITTYMEMORY_SOURCES EXCLUDE REGEX "MemoryPatch\\.cpp$")
list(FILTER KITTYMEMORY_SOURCES EXCLUDE REGEX "MemoryBackup\\.cpp$")

set(LIBRARY_SOURCES
    ${COMMON_SOURCES}
    ${IMGUI_SOURCES}
    ${KITTYMEMORY_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/source/PointerHook/trampoline.s
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_SOURCE_DIR}/source/Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
)

# ============================================================
#  Apply patches to AndUEDumper before compiling
# ============================================================
execute_process(
    COMMAND git apply --check ${CMAKE_CURRENT_SOURCE_DIR}/misc/UEWrappers_case_insensitive.patch
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper
    RESULT_VARIABLE PATCH_RESULT
    ERROR_QUIET
)
if(PATCH_RESULT EQUAL 0)
    execute_process(
        COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/misc/UEWrappers_case_insensitive.patch
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper
    )
    message(STATUS "Applied UEWrappers_case_insensitive.patch")
else()
    message(STATUS "UEWrappers_case_insensitive.patch already applied or not applicable")
endif()

# ============================================================
#  AndUEDumper — compile core dumper as a static library
# ============================================================
set(ANDUEDUMPER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper/AndUEDumper)
set(ANDUEDUMPER_SRC_DIR ${ANDUEDUMPER_DIR}/src)
set(ANDUEDUMPER_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper/deps)
set(KITTYMEMORYEX_DIR ${ANDUEDUMPER_DEPS_DIR}/KittyMemoryEx/KittyMemoryEx)

file(GLOB ANDUEDUMPER_UE_SRC ${ANDUEDUMPER_SRC_DIR}/UE/*.cpp)
file(GLOB ANDUEDUMPER_UTILS_SRC ${ANDUEDUMPER_SRC_DIR}/Utils/*.cpp)
file(GLOB KITTYMEMORYEX_SRC ${KITTYMEMORYEX_DIR}/*.cpp)

add_library(AndUEDumperLib STATIC
    ${ANDUEDUMPER_UE_SRC}
    ${ANDUEDUMPER_UTILS_SRC}
    ${ANDUEDUMPER_SRC_DIR}/Dumper.cpp
    ${ANDUEDUMPER_SRC_DIR}/UPackageGenerator.cpp
    ${KITTYMEMORYEX_SRC}
    ${ANDUEDUMPER_DEPS_DIR}/fmt/format.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/source/UEProber/DumpIntegration.cpp
)

target_include_directories(AndUEDumperLib PRIVATE
    ${KITTYMEMORYEX_DIR}
    ${KITTYMEMORYEX_DIR}/Deps
    ${ANDUEDUMPER_DEPS_DIR}
    ${ANDUEDUMPER_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_SOURCE_DIR}/source/UEProber
)

target_compile_definitions(AndUEDumperLib PRIVATE
    kNO_KEYSTONE
)

target_compile_options(AndUEDumperLib PRIVATE -std=c++20 -fexceptions)
target_link_libraries(AndUEDumperLib log)
# ============================================================

set(LIBS
    android
    log
    vulkan
    GLESv3
    EGL
    ${CMAKE_CURRENT_SOURCE_DIR}/external/Dobby/arm64-v8a/libdobby.a
)

add_library(${PROJECT_NAME} SHARED 
    ${CMAKE_CURRENT_SOURCE_DIR}/source/Library.cpp
    ${LIBRARY_SOURCES}
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

target_link_libraries(${PROJECT_NAME} ${LIBS} AndUEDumperLib)
