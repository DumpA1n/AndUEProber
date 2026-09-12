if(ANDUEPROBER_IMGUI_TARGET)
    if(NOT TARGET "${ANDUEPROBER_IMGUI_TARGET}")
        message(FATAL_ERROR "ANDUEPROBER_IMGUI_TARGET must name an existing caller-owned ImGui target")
    endif()
    set(andueprober_imgui_target "${ANDUEPROBER_IMGUI_TARGET}")
else()
    if(TARGET AndUEProberImGui::Core)
        message(FATAL_ERROR "Select the existing ImGui target with ANDUEPROBER_IMGUI_TARGET")
    endif()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(andueprober_imgui
        URL https://codeload.github.com/ocornut/imgui/tar.gz/45acd5e0e82f4c954432533ae9985ff0e1aad6d5
        URL_HASH SHA256=97484925aec2f4d3e913d6644d46b234f8d6d8d98c6aa9c50109e0f0df772090)
    FetchContent_MakeAvailable(andueprober_imgui)
    add_library(AndUEProberImGuiDependency STATIC
        ${andueprober_imgui_SOURCE_DIR}/imgui.cpp
        ${andueprober_imgui_SOURCE_DIR}/imgui_draw.cpp
        ${andueprober_imgui_SOURCE_DIR}/imgui_tables.cpp
        ${andueprober_imgui_SOURCE_DIR}/imgui_widgets.cpp)
    add_library(AndUEProberImGui::Core ALIAS AndUEProberImGuiDependency)
    set_target_properties(AndUEProberImGuiDependency PROPERTIES EXPORT_NAME Core POSITION_INDEPENDENT_CODE ON)
    target_compile_features(AndUEProberImGuiDependency PUBLIC cxx_std_20)
    target_compile_definitions(AndUEProberImGuiDependency PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
    target_include_directories(AndUEProberImGuiDependency PUBLIC
        $<BUILD_INTERFACE:${andueprober_imgui_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/AndUEProberImGui>)
    if(ANDUEPROBER_SANITIZERS)
        target_compile_options(AndUEProberImGuiDependency PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(AndUEProberImGuiDependency PUBLIC -fsanitize=address,undefined)
    endif()
    install(TARGETS AndUEProberImGuiDependency EXPORT AndUEProberImGuiTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT AndUEProberImGuiTargets NAMESPACE AndUEProberImGui:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProberImGui)
    install(FILES
        ${andueprober_imgui_SOURCE_DIR}/imgui.h
        ${andueprober_imgui_SOURCE_DIR}/imconfig.h
        ${andueprober_imgui_SOURCE_DIR}/imgui_internal.h
        ${andueprober_imgui_SOURCE_DIR}/imstb_rectpack.h
        ${andueprober_imgui_SOURCE_DIR}/imstb_textedit.h
        ${andueprober_imgui_SOURCE_DIR}/imstb_truetype.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/AndUEProberImGui)
    install(FILES ${andueprober_imgui_SOURCE_DIR}/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AndUEProberImGui)
    configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/AndUEProberImGuiConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberImGuiConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProberImGui)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/AndUEProberImGuiConfigVersion.cmake
        VERSION 1.92.2 COMPATIBILITY ExactVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberImGuiConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/AndUEProberImGuiConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AndUEProberImGui)
    set(andueprober_imgui_target AndUEProberImGui::Core)
    set(ANDUEPROBER_INSPECTOR_USE_DEFAULT_IMGUI ON)
endif()

if(TARGET AndUEProberImGui::Binding)
    get_target_property(andueprober_existing_imgui_target AndUEProberImGui::Binding INTERFACE_LINK_LIBRARIES)
    if(NOT andueprober_existing_imgui_target STREQUAL andueprober_imgui_target)
        message(FATAL_ERROR "An incompatible ImGui binding already exists")
    endif()
else()
    add_library(AndUEProberImGui::Binding INTERFACE IMPORTED GLOBAL)
    set_target_properties(AndUEProberImGui::Binding PROPERTIES
        INTERFACE_LINK_LIBRARIES "${andueprober_imgui_target}"
        INTERFACE_COMPILE_FEATURES cxx_std_20
        INTERFACE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/CheckImGuiVersion.cpp")
endif()
