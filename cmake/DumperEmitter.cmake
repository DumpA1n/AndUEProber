find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(ANDUEDUMPER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/AndUEDumper" CACHE PATH "Pinned dumper dependency source directory")
set(dumper_emitter_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/dumper-emitter")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_dumper_emitter.py"
    "${ANDUEDUMPER_ROOT}/AndUEDumper/src/UPackageGenerator.hpp"
    "${ANDUEDUMPER_ROOT}/AndUEDumper/src/UPackageGenerator.cpp"
    "${ANDUEDUMPER_ROOT}/deps/fmt/base.h"
    "${ANDUEDUMPER_ROOT}/deps/fmt/format.h"
    "${ANDUEDUMPER_ROOT}/deps/fmt/format-inl.h"
    "${ANDUEDUMPER_ROOT}/LICENSE"
    "${ANDUEDUMPER_ROOT}/deps/fmt/LICENSE")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_dumper_emitter.py"
        --source "${ANDUEDUMPER_ROOT}" --output "${dumper_emitter_generated}"
    RESULT_VARIABLE dumper_emitter_result OUTPUT_VARIABLE dumper_emitter_output ERROR_VARIABLE dumper_emitter_error)
if(NOT dumper_emitter_result EQUAL 0)
    message(FATAL_ERROR "Pinned dumper emitter extraction failed: ${dumper_emitter_output}${dumper_emitter_error}")
endif()
add_library(AndUEProberDumperEmitterObjects OBJECT
    source/DumperEmitter/Emitter.cpp
    "${dumper_emitter_generated}/UpstreamEmitter.cpp")
set_target_properties(AndUEProberDumperEmitterObjects PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(AndUEProberDumperEmitterObjects PUBLIC cxx_std_20)
target_compile_definitions(AndUEProberDumperEmitterObjects PRIVATE FMT_HEADER_ONLY)
target_include_directories(AndUEProberDumperEmitterObjects
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source/DumperEmitter>"
           "$<BUILD_INTERFACE:${dumper_emitter_generated}>"
    PRIVATE "${ANDUEDUMPER_ROOT}/deps")
if(ANDUEPROBER_SANITIZERS)
    target_compile_options(AndUEProberDumperEmitterObjects PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(AndUEProberDumperEmitterObjects PUBLIC -fsanitize=address,undefined)
endif()
add_library(AndUEProberDumperEmitter STATIC $<TARGET_OBJECTS:AndUEProberDumperEmitterObjects>)
target_link_libraries(AndUEProberDumperEmitter PUBLIC AndUEProberDumperEmitterObjects)
install(FILES "${ANDUEDUMPER_ROOT}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/AndUEDumper")
install(FILES "${ANDUEDUMPER_ROOT}/deps/fmt/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/AndUEProber/fmt")
install(FILES "${dumper_emitter_generated}/provenance.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/AndUEProber/dumper-emitter")
if(ANDUEPROBER_BUILD_DUMPER_ADAPTER)
    add_library(AndUEProberDumperAdapter STATIC
        source/DumperEmitter/DumperAdapter.cpp $<TARGET_OBJECTS:AndUEProberDumperEmitterObjects>)
    add_library(AndUEProber::DumperAdapter ALIAS AndUEProberDumperAdapter)
    set_target_properties(AndUEProberDumperAdapter PROPERTIES EXPORT_NAME DumperAdapter POSITION_INDEPENDENT_CODE ON)
    target_include_directories(AndUEProberDumperAdapter PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source/DumperEmitter" "${dumper_emitter_generated}")
    target_link_libraries(AndUEProberDumperAdapter PUBLIC AndUEProber::Core)
    file(READ "${dumper_emitter_generated}/provenance.json" dumper_emitter_provenance)
    string(JSON dumper_emitter_revision GET "${dumper_emitter_provenance}" upstream_revision)
    string(JSON dumper_emitter_sha GET "${dumper_emitter_provenance}" generated_sha256 UpstreamEmitter.cpp)
    target_compile_definitions(AndUEProberDumperAdapter PRIVATE
        "ANDUEPROBER_DUMPER_IDENTITY=AndUEDumper:${dumper_emitter_revision}:emitter-sha256:${dumper_emitter_sha}")
    install(TARGETS AndUEProberDumperAdapter EXPORT AndUEProberDumperAdapterTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT AndUEProberDumperAdapterTargets NAMESPACE AndUEProber:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProber)
endif()
