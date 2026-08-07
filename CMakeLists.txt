cmake_minimum_required(VERSION 3.20)
project(NukeImGui CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/cmake/NukeRunDir.cmake")   # NUKE_RUN_SUBDIR: x64 / macos / linux

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Dear ImGui sources — the DOCKING branch (1.92.9), giving dockable/sticky panels.
# We don't use Diligent's ImGuiImplDiligent (we render through the neutral iRender
# seam), so the UI module's ImGui version is free to differ from Diligent's.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

add_library(NukeImGui SHARED
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp   # ImGui::ShowDemoWindow (validation/reference)
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp   # multi-viewport PLATFORM backend (native OS windows)
    imguizmo/ImGuizmo.cpp   # transform gizmo (compiled against OUR imgui, shares the context)
    src/nukeui.cpp          # the bridge: ImGui <-> iRender neutral seam
)

# GLFW (vcpkg, DYNAMIC -- the same glfw3.dll the renderer loads, so both modules share
# ONE GLFW instance: the renderer's event polling pumps the UI's secondary windows).
if(WIN32)
    target_link_directories(NukeImGui PRIVATE "${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib")
    target_link_libraries(NukeImGui PRIVATE glfw3dll)
else()
    # The superbuild hands every window-touching module ONE shared GLFW (objc GLFW* classes
    # must exist once per process); standalone configures fall back to vcpkg's static glfw3.
    set(_nuke_ig_rpath "@loader_path;$ORIGIN")
    if(NUKE_GLFW_DYLIB)
        target_link_libraries(NukeImGui PRIVATE "${NUKE_GLFW_DYLIB}")
        target_include_directories(NukeImGui BEFORE PRIVATE "${NUKE_GLFW_INCLUDE}")
        list(APPEND _nuke_ig_rpath "${NUKE_GLFW_LIBDIR}")
    else()
        find_package(glfw3 CONFIG REQUIRED)
        target_link_libraries(NukeImGui PRIVATE glfw)
    endif()
    find_package(Boost REQUIRED)   # engine headers (irender.h) pull boost/function.hpp
    target_link_libraries(NukeImGui PRIVATE Boost::headers)
    if(APPLE)
        # Absolute cursor/button queries for host gestures (see nukeui_cocoa.mm).
        target_sources(NukeImGui PRIVATE src/nukeui_cocoa.mm)
        target_link_libraries(NukeImGui PRIVATE "-framework AppKit" "-framework CoreGraphics")
    endif()
    target_compile_options(NukeImGui PRIVATE -Wno-c++11-narrowing -Wno-unknown-attributes)
    set_target_properties(NukeImGui PROPERTIES BUILD_RPATH "${_nuke_ig_rpath}")
endif()

# Export the ImGui API from this DLL; consumers import it. One shared binary ->
# one ImGui context across the editor, the render module and plugins.
target_include_directories(NukeImGui
    PUBLIC  ${IMGUI_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/imguizmo
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/include   # iRender + UIDrawData
            ${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine
            ${VCPKG_INSTALLED}/include                          # boost / assimp / glm (via engine headers)
)
if(WIN32)
    target_compile_definitions(NukeImGui
        PRIVATE   "IMGUI_API=__declspec(dllexport)"
                  NUKEIMGUI_EXPORTS
                  GLM_ENABLE_EXPERIMENTAL
                  _CRT_SECURE_NO_WARNINGS
        INTERFACE "IMGUI_API=__declspec(dllimport)"
    )
else()
    # Mach-O/ELF: default visibility already exports the ImGui API from the dylib;
    # consumers import it without decoration.
    target_compile_definitions(NukeImGui
        PRIVATE   NUKEIMGUI_EXPORTS
                  GLM_ENABLE_EXPERIMENTAL
    )
endif()

# Deploy the DLL next to the editor exe so every binary loads the same instance.
set(DEPLOY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>")
add_custom_command(TARGET NukeImGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:NukeImGui>" "${DEPLOY_DIR}/"
    # Import lib to the LEGACY link path (the hand-maintained editor vcxproj + companion
    # modules link NukeImGui/build/<config>; the superbuild's binary dir is elsewhere).
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/build/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_LINKER_FILE:NukeImGui>" "${CMAKE_CURRENT_SOURCE_DIR}/build/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/lucide.ttf" "${DEPLOY_DIR}/fonts/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/nukefileicons.ttf" "${DEPLOY_DIR}/fonts/"
    COMMENT "Deploying NukeImGui.dll + icon fonts next to the editor"
)
