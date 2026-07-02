cmake_minimum_required(VERSION 3.20)
project(NukeImGui CXX)

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
    imguizmo/ImGuizmo.cpp   # transform gizmo (compiled against OUR imgui, shares the context)
    src/nukeui.cpp          # the bridge: ImGui <-> iRender neutral seam
)

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
target_compile_definitions(NukeImGui
    PRIVATE   "IMGUI_API=__declspec(dllexport)"
              NUKEIMGUI_EXPORTS
              GLM_ENABLE_EXPERIMENTAL
              _CRT_SECURE_NO_WARNINGS
    INTERFACE "IMGUI_API=__declspec(dllimport)"
)

# Deploy the DLL next to the editor exe so every binary loads the same instance.
set(DEPLOY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/x64/$<CONFIG>")
add_custom_command(TARGET NukeImGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:NukeImGui>" "${DEPLOY_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/lucide.ttf" "${DEPLOY_DIR}/fonts/"
    COMMENT "Deploying NukeImGui.dll + fonts/lucide.ttf next to the editor"
)
