cmake_minimum_required(VERSION 3.20)
project(sdk_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDK_A_DIR "" CACHE PATH "Path to generated SDK_A from an owned, versioned fixture")
if(NOT EXISTS "${SDK_A_DIR}/SDK.hpp")
    message(FATAL_ERROR "SDK smoke requires an explicit SDK_A_DIR containing SDK.hpp; no generated SDK was compiled")
endif()

# Every generated translation unit is compiled. General SDKs remain object-only
# because their engine symbols require a separately verified runtime contract.
file(GLOB_RECURSE SDK_SRCS CONFIGURE_DEPENDS "${SDK_A_DIR}/*.cpp")

if(NOT SDK_SRCS)
    message(FATAL_ERROR "SDK smoke requires generated source files")
endif()

add_library(sdk_smoke_objs OBJECT
    ${SDK_SRCS}
    "${CMAKE_CURRENT_SOURCE_DIR}/Demo.cpp"
)

target_include_directories(sdk_smoke_objs PRIVATE "${SDK_A_DIR}")

# Generated UE headers use non-standard-layout offsetof and engine literal conventions.
target_compile_options(sdk_smoke_objs PRIVATE
    -Wno-invalid-offsetof    # offsetof() on non-standard-layout (UE classes)
    -Wno-class-conversion    # FAllocatedString -> FString conversion ops
    -Wno-return-stack-address
    -Wno-unused-value
    -Wno-multichar           # UE FName fragment 4-char literals
    -Wno-invalid-source-encoding
)

# The source count records the actual generated compilation scope.
list(LENGTH SDK_SRCS SDK_SRCS_COUNT)
message(STATUS "sdk_smoke: compiling ${SDK_SRCS_COUNT} dumped .cpp files from ${SDK_A_DIR}")

option(SDK_SMOKE_RUN "Link an owned fixture containing its own main function" OFF)
if(SDK_SMOKE_RUN)
    add_executable(sdk_smoke_exec $<TARGET_OBJECTS:sdk_smoke_objs>)
endif()
