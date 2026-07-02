#pragma once
#include <functional>

#ifdef NUKEIMGUI_EXPORTS
#define NUKEUI_API __declspec(dllexport)
#else
#define NUKEUI_API __declspec(dllimport)
#endif

namespace nuke { class iRender; }

// NukeUI: the bridge between Dear ImGui and a renderer, living in the shared
// UI module. It owns the ImGui context and renders each frame THROUGH the
// renderer's neutral seam (iRender::createTexture2D / renderDrawLists), so the
// UI knows nothing about Diligent/bgfx and the renderer knows nothing about ImGui.
namespace NukeUI
{
	// Create the ImGui context and hook this UI into the renderer's per-frame
	// GUI callback. Call once after the renderer is initialized.
	NUKEUI_API void Init(nuke::iRender* renderer);
	NUKEUI_API void Shutdown();

	// Register a window/menu drawing callback (it calls ImGui:: inside). This is
	// where the editor and the game differ — same UI module, different menus.
	NUKEUI_API void AddDrawCallback(const std::function<void()>& cb);

	// Build and submit one UI frame through the renderer. Hooked to iRender's onGUI.
	NUKEUI_API void Frame();

	// Tell the UI the current framebuffer size (call on resize).
	NUKEUI_API void SetDisplaySize(int width, int height);

	// Merge an icon font (e.g. Lucide's lucide.ttf) into the atlas ON TOP of the
	// main font, so icon glyphs (ICON_LC_*) can be used inline in any label.
	// Call AFTER the app adds its main font, BEFORE the first frame.
	NUKEUI_API void MergeIconFont(const char* ttfPath, float sizePx, float glyphOffsetY = 0.0f);
}
