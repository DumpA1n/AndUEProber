cmake_minimum_required(VERSION 3.22.1)

project(AndUEProber VERSION 0.2.0 LANGUAGES C CXX)
include(GNUInstallDirs)

option(ANDUEPROBER_BUILD_AGENT "Build the Android agent, inspector and dumper adapter" ${ANDROID})
option(ANDUEPROBER_BUILD_TESTS "Build host regression fixtures" ${PROJECT_IS_TOP_LEVEL})
option(ANDUEPROBER_SANITIZERS "Enable host address and undefined behavior sanitizers" OFF)
option(ANDUEPROBER_BUILD_DUMPER_EMITTER "Build the private bounded pinned text emitter" OFF)
option(ANDUEPROBER_BUILD_DUMPER_ADAPTER "Build the optional frozen reflection text adapter" ${ANDUEPROBER_BUILD_AGENT})
set(ANDUEPROBER_MEMORY_SOURCE_DIR "" CACHE PATH "Explicit public Memory producer checkout; no sibling discovery")
set(ANDSWAPCHAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/AndSwapChainHook CACHE PATH "Pinned Android dependency source directory")
set(ANDUEDUMPER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper" CACHE PATH "Pinned dumper dependency source directory")
find_package(Threads REQUIRED)
find_package(Git QUIET)
function(andueprober_git_revision directory fallback output)
    set(revision "${fallback}")
    if(GIT_FOUND AND EXISTS "${directory}")
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${directory}" rev-parse HEAD
            OUTPUT_VARIABLE found_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(found_revision)
            set(revision "${found_revision}")
            execute_process(COMMAND ${GIT_EXECUTABLE} -C "${directory}" status --porcelain
                OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(changes)
                string(APPEND revision "-dirty")
            endif()
        endif()
    endif()
    set(${output} "${revision}" PARENT_SCOPE)
endfunction()
andueprober_git_revision("${CMAKE_CURRENT_SOURCE_DIR}" "unrecorded-source-archive" ANDUEPROBER_SOURCE_REVISION)
andueprober_git_revision("${ANDUEDUMPER_ROOT}" "unrecorded-dumper-source" ANDUEPROBER_DUMPER_REVISION)
if(ANDUEPROBER_MEMORY_SOURCE_DIR)
    andueprober_git_revision("${ANDUEPROBER_MEMORY_SOURCE_DIR}" "unrecorded-memory-source" ANDUEPROBER_MEMORY_REVISION)
else()
    set(ANDUEPROBER_MEMORY_REVISION "unrecorded-installed-memory-package")
endif()
andueprober_git_revision("${ANDSWAPCHAIN_DIR}/external/KittyMemoryEx" "unrecorded-kitty-source" ANDUEPROBER_KITTY_REVISION)
configure_file(cmake/BuildInfo.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.hpp @ONLY)
add_library(AndUEProberCore STATIC source/Core/Core.cpp source/Core/Export.cpp source/Core/Commands.cpp source/Core/Engine.cpp source/Core/Reflection.cpp source/Core/Evidence.cpp)
add_library(AndUEProber::Core ALIAS AndUEProberCore)
set_target_properties(AndUEProberCore PROPERTIES EXPORT_NAME Core POSITION_INDEPENDENT_CODE ON)
target_compile_features(AndUEProberCore PUBLIC cxx_std_20)
target_include_directories(AndUEProberCore PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(AndUEProberCore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(AndUEProberCore PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
add_library(AndUEProberProbe STATIC
    source/Probe/Probe.cpp
    source/Probe/Names.cpp
    source/Probe/Relations.cpp
    source/Probe/Discovery.cpp
    source/Probe/Structs.cpp
    source/Probe/Classes.cpp
    source/Probe/Functions.cpp
    source/Probe/Fields.cpp
    source/Probe/Properties.cpp
    source/Probe/PropertyBases.cpp
    source/Probe/ObjectFlags.cpp
    source/Probe/Enums.cpp
    source/Probe/PropertyTails.cpp
    source/Probe/PropertyContext.cpp
    source/Probe/PropertyValues.cpp)
add_library(AndUEProber::Probe ALIAS AndUEProberProbe)
set_target_properties(AndUEProberProbe PROPERTIES EXPORT_NAME Probe POSITION_INDEPENDENT_CODE ON)
target_link_libraries(AndUEProberProbe PUBLIC AndUEProberCore)
if(ANDUEPROBER_SANITIZERS)
    if(ANDROID OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Host Clang or GCC is required for this sanitizer configuration")
    endif()
    target_compile_options(AndUEProberCore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(AndUEProberCore PUBLIC -fsanitize=address,undefined)
endif()
include(CMakePackageConfigHelpers)
if(ANDUEPROBER_BUILD_DUMPER_EMITTER OR ANDUEPROBER_BUILD_DUMPER_ADAPTER)
    include(cmake/DumperEmitter.cmake)
endif()
install(TARGETS AndUEProberCore AndUEProberProbe EXPORT AndUEProberTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/andueprober DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT AndUEProberTargets NAMESPACE AndUEProber:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/AndUEProberConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
option(ANDUEPROBER_WITH_PROCESS_MEMORY "Build the process-memory adapter using AndSwapChainHook::Memory" OFF)
if(ANDUEPROBER_MEMORY_SOURCE_DIR)
    set(ANDUEPROBER_WITH_PROCESS_MEMORY ON)
endif()
if(ANDUEPROBER_WITH_PROCESS_MEMORY)
    if(ANDUEPROBER_MEMORY_SOURCE_DIR AND NOT TARGET AndSwapChainHook::Memory)
        function(andueprober_add_memory_producer)
            set(BUILD_TESTING OFF)
            set(ANDSWAPCHAIN_BUILD_ANDROID OFF)
            set(ANDSWAPCHAIN_BUILD_GRAPHICS OFF)
            set(ANDSWAPCHAIN_BUILD_OVERLAY OFF)
            set(ANDSWAPCHAIN_BUILD_POINTER_ADAPTER OFF)
            set(ANDSWAPCHAIN_BUILD_CRASH_RECORDER OFF)
            add_subdirectory("${ANDUEPROBER_MEMORY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/memory-producer")
        endfunction()
        andueprober_add_memory_producer()
    endif()
    if(NOT TARGET AndSwapChainHook::Memory)
        find_package(AndSwapChainHook 0.2 CONFIG REQUIRED COMPONENTS Memory)
    endif()
    set(ANDUEPROBER_MEMORY_PROVENANCE "unrecorded-installed-package" CACHE STRING "Memory producer revision or source fingerprint")
    if(ANDUEPROBER_MEMORY_SOURCE_DIR)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${ANDUEPROBER_MEMORY_SOURCE_DIR}/include/AndSwapChainHook/Memory.h"
            "${ANDUEPROBER_MEMORY_SOURCE_DIR}/source/Memory/Memory.cpp")
        file(SHA256 "${ANDUEPROBER_MEMORY_SOURCE_DIR}/include/AndSwapChainHook/Memory.h" memory_header_hash)
        file(SHA256 "${ANDUEPROBER_MEMORY_SOURCE_DIR}/source/Memory/Memory.cpp" memory_source_hash)
        string(SHA256 memory_hash "${memory_header_hash}:${memory_source_hash}")
        set(ANDUEPROBER_MEMORY_PROVENANCE "working-tree-memory-source-sha256:${memory_hash}")
    endif()
    add_library(AndUEProberProcessMemory STATIC source/Android/ProcessMemory.cpp)
    add_library(AndUEProber::ProcessMemory ALIAS AndUEProberProcessMemory)
    set_target_properties(AndUEProberProcessMemory PROPERTIES EXPORT_NAME ProcessMemory POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(AndUEProberProcessMemory PUBLIC AndUEProberCore PRIVATE AndSwapChainHook::Memory ${CMAKE_DL_LIBS})
    target_compile_definitions(AndUEProberProcessMemory PRIVATE ANDUEPROBER_MEMORY_PROVENANCE="${ANDUEPROBER_MEMORY_PROVENANCE}")
    install(TARGETS AndUEProberProcessMemory EXPORT AndUEProberProcessMemoryTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT AndUEProberProcessMemoryTargets NAMESPACE AndUEProber:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
endif()
set(inspector_default OFF)
if(ANDUEPROBER_BUILD_AGENT OR ANDUEPROBER_BUILD_TESTS)
    set(inspector_default ON)
endif()
option(ANDUEPROBER_BUILD_INSPECTOR "Build the optional ImGui inspector component" ${inspector_default})
set(ANDUEPROBER_IMGUI_TARGET "" CACHE STRING "Existing caller-owned ImGui target; empty builds the fixed default dependency")
set(ANDUEPROBER_INSPECTOR_USE_DEFAULT_IMGUI OFF)
if(ANDUEPROBER_BUILD_AGENT AND NOT ANDUEPROBER_BUILD_INSPECTOR)
    message(FATAL_ERROR "The agent requires ANDUEPROBER_BUILD_INSPECTOR=ON")
endif()
if(ANDUEPROBER_BUILD_INSPECTOR)
    include(cmake/ImGuiDependency.cmake)
    add_library(AndUEProberInspector STATIC source/UI/Inspector.cpp)
    add_library(AndUEProber::Inspector ALIAS AndUEProberInspector)
    set_target_properties(AndUEProberInspector PROPERTIES EXPORT_NAME Inspector POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(AndUEProberInspector PUBLIC AndUEProber::Core PRIVATE AndUEProberImGui::Binding)
    install(TARGETS AndUEProberInspector EXPORT AndUEProberInspectorTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT AndUEProberInspectorTargets NAMESPACE AndUEProber:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
    install(FILES cmake/CheckImGuiVersion.cpp DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
endif()
configure_package_config_file(cmake/AndUEProberConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
if(ANDUEPROBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(NOT ANDUEPROBER_BUILD_AGENT)
    return()
endif()
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The agent requires the Android AArch64 toolchain; use an android preset")
endif()
enable_language(ASM)

set(ANDUEDUMPER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper CACHE PATH "Pinned dumper dependency source directory")

# Private dependency required by the pinned dumper implementation.
set(KITTY_ROOT  ${ANDSWAPCHAIN_DIR}/external/KittyMemoryEx)
set(KITTY_INNER ${KITTY_ROOT}/KittyMemoryEx)

add_library(KittyMemoryExLib STATIC
    ${KITTY_INNER}/KittyAsm.cpp ${KITTY_INNER}/KittyIOFile.cpp
    ${KITTY_INNER}/KittyMemOp.cpp ${KITTY_INNER}/KittyMemoryEx.cpp
    ${KITTY_INNER}/KittyMemoryMgr.cpp ${KITTY_INNER}/KittyPerfEvent.cpp
    ${KITTY_INNER}/KittyPtrValidator.cpp ${KITTY_INNER}/KittyScanner.cpp
    ${KITTY_INNER}/KittyTrace.cpp ${KITTY_INNER}/KittyUtils.cpp)
target_compile_features(KittyMemoryExLib PRIVATE cxx_std_20)
set_target_properties(KittyMemoryExLib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(KittyMemoryExLib PUBLIC
    ${KITTY_ROOT}                # for "KittyMemoryEx/KittyMemoryEx.hpp"
    ${KITTY_INNER}               # for unprefixed <KittyMemoryMgr.hpp> etc.
    ${KITTY_INNER}/Deps
)
target_compile_definitions(KittyMemoryExLib PUBLIC
    kUSE_KITTYMEMORYEX
    kNO_KEYSTONE
)
target_link_libraries(KittyMemoryExLib PRIVATE log)

# These upstream types and globals are internal to the agent runtime.
set(ANDUEDUMPER_DIR  ${ANDUEDUMPER_ROOT}/AndUEDumper)
set(ANDUEDUMPER_SRC  ${ANDUEDUMPER_DIR}/src)
set(ANDUEDUMPER_DEPS ${ANDUEDUMPER_ROOT}/deps)

add_library(AndUEDumperLib STATIC
    ${ANDUEDUMPER_SRC}/UE/UEGameProfile.cpp source/UEProber/BoundedUEMemory.cpp
    ${ANDUEDUMPER_SRC}/UE/UEOffsets.cpp ${ANDUEDUMPER_SRC}/UE/UEWrappers.cpp
    ${ANDUEDUMPER_SRC}/Utils/BufferFmt.cpp ${ANDUEDUMPER_SRC}/Utils/ProgressUtils.cpp
    ${ANDUEDUMPER_SRC}/Dumper.cpp ${ANDUEDUMPER_SRC}/UPackageGenerator.cpp
    ${ANDUEDUMPER_DEPS}/fmt/format.cc)
target_compile_features(AndUEDumperLib PRIVATE cxx_std_20)
set_target_properties(AndUEDumperLib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(AndUEDumperLib PUBLIC
    ${ANDUEDUMPER_SRC}        # exposed UE/* and Utils/BufferFmt.hpp consumed by UEProber
    ${ANDUEDUMPER_DEPS}       # BufferFmt.hpp transitively needs fmt/; UEProber uses utfcpp/
    ${CMAKE_CURRENT_SOURCE_DIR}/source/UEProber
)

target_link_libraries(AndUEDumperLib PUBLIC KittyMemoryExLib)

# Runtime implementation has one composition owner; it is not an installed adapter API.
add_library(AndUEProberAgentRuntime STATIC
    source/UEProber/UEProber.cpp
    source/UEProber/DumperBridge.cpp
    source/UEProber/ConfiguredProbeBridge.cpp)
set_target_properties(AndUEProberAgentRuntime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(AndUEProberAgentRuntime PUBLIC cxx_std_20)
target_include_directories(AndUEProberAgentRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(AndUEProberAgentRuntime PRIVATE
    kANDROID_LOG
    kANDROID_LOG_TAG="INJECT"
    kPROJECT_NAME="${PROJECT_NAME}")
target_link_libraries(AndUEProberAgentRuntime
    PUBLIC AndUEProber::Probe
    PRIVATE AndUEDumperLib log)
if(ANDUEPROBER_BUILD_DUMPER_ADAPTER)
    target_link_libraries(AndUEProberAgentRuntime PRIVATE AndUEProber::DumperAdapter)
    target_compile_definitions(AndUEProberAgentRuntime PUBLIC ANDUEPROBER_HAS_DUMPER_ADAPTER=1)
else()
    target_compile_definitions(AndUEProberAgentRuntime PUBLIC ANDUEPROBER_HAS_DUMPER_ADAPTER=0)
endif()

add_library(AndUEProber SHARED source/Library.cpp)
add_library(AndUEProber::Agent ALIAS AndUEProber)
set_target_properties(AndUEProber PROPERTIES EXPORT_NAME Agent)
target_compile_features(AndUEProber PRIVATE cxx_std_20)
target_include_directories(AndUEProber PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(AndUEProber PRIVATE
    AndUEProberAgentRuntime AndUEProber::Inspector log)
if(ANDUEPROBER_WITH_PROCESS_MEMORY)
    target_link_libraries(AndUEProberAgentRuntime PUBLIC AndUEProber::ProcessMemory)
    target_compile_definitions(AndUEProberAgentRuntime PUBLIC ANDUEPROBER_HAS_PROCESS_MEMORY=1)
    add_executable(andueprober_target_runner source/TargetRunner.cpp)
    target_link_libraries(andueprober_target_runner PRIVATE AndUEProberAgentRuntime log)
    install(TARGETS andueprober_target_runner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS AndUEProber EXPORT AndUEProberAgentTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT AndUEProberAgentTargets NAMESPACE AndUEProber:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
install(FILES ${ANDUEDUMPER_ROOT}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/AndUEDumper)
install(FILES ${KITTY_ROOT}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/KittyMemoryEx)
install(FILES ${ANDUEDUMPER_DEPS}/fmt/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/fmt)
install(FILES ${ANDUEDUMPER_DEPS}/utfcpp/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/utfcpp)
install(FILES ${ANDUEDUMPER_DEPS}/nlohmann/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/nlohmann)
