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

target_link_libraries(${PROJECT_NAME} ${LIBS})
