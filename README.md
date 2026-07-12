# NukeImGui

The EDITOR's shared ImGui DLL for [NukeEngine](https://github.com/Luastris/NukeEngine-Eco):
one vendored copy of [Dear ImGui](https://github.com/ocornut/imgui) 1.92 (+ ImGuizmo,
Lucide icon font) that the editor and every editor-facing plugin link against — so ImGui
state, contexts and allocators are shared across DLL boundaries.

- **Never ships with a game.** Packaging auto-excludes any module whose import table
  names `NukeImGui.dll` (that's the "editor-only module" test). Runtime/game GUI is the
  separate [NukeGUI](https://github.com/Luastris/NukeGUI) module with its own ImGui copy.
- Exposes the neutral `NukeUI` seam the renderer consumes (draw-list conversion, the
  ImGui 1.92 dynamic-texture lifecycle, multi-viewport OS windows) — the renderer never
  sees ImGui types.
- Deploys itself + `fonts/lucide.ttf` next to the editor post-build.

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild
(builds FIRST — the editor links `NukeImGui.lib`), or standalone:
`cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`).
