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
	// NATIVE imgui multi-viewport windows (Vulkan backend): panels and editors dragged
	// past the main window become real per-window swapchain OS windows, imgui-native,
	// with full dock previews. Call BEFORE Init. On D3D backends leave it off — the
	// DXGI secondary-swapchain races are why the GDI host path exists (the fallback).
	NUKEUI_API void EnableNativeViewports(bool on);
	NUKEUI_API bool NativeViewportsActive();

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

	// --- EDITOR-OWNED HOST WINDOWS (detached asset editors, the Godot model) -----------
	// The APP creates a real decorated OS window and draws imgui content into it through
	// its OWN ImGui context (shared font atlas, copied style). Window lifecycle, input
	// routing and swap-chain timing belong to the app — none of the imgui multi-viewport
	// platform-window churn that raced DXGI into device removal (task #133).
	// A host ticks automatically inside Frame() after the main context: the content
	// callback runs inside a fullscreen borderless imgui window filling the host.
	NUKEUI_API void* HostCreate(const char* title, int w, int h);
	NUKEUI_API void  HostSetContent(void* host, const std::function<void()>& draw);
	// Extra ImGuiWindowFlags for the host's content window (e.g. NoScrollbar for preview
	// editors, UnsavedDocument for the dirty dot). May be called every frame.
	NUKEUI_API void  HostSetContentFlags(void* host, int imguiWindowFlags);
	// NORMAL DOCKING (drag, no buttons): the host window follows the cursor (grab point =
	// hot, client coords) until the mouse button is released. Started automatically when
	// the user drags the content window's title bar, or by the app right after a tear-off
	// (drag continues seamlessly from the main window into the new OS window).
	NUKEUI_API void  HostBeginDrag(void* host, float hotX, float hotY);
	NUKEUI_API bool  HostDragging(void* host);
	// True ONCE when a drag ended with the cursor inside the DOCK TARGET (see below): the
	// user dropped the window back in. Outputs the cursor in main-window client coords.
	NUKEUI_API bool  HostDockDrop(void* host, float* mainX, float* mainY);
	// True while ANY host window is being drag-followed with the cursor over the main
	// window — the app draws the dock-target overlay while this holds.
	NUKEUI_API bool  HostDragActive();
	// The drop zone (main-window client coords) that accepts a host drop as "dock back".
	// The app sets it every frame it draws the overlay; releasing a dragged host INSIDE
	// the rect docks it, releasing anywhere else just PLACES the OS window there.
	NUKEUI_API void  SetDockTarget(float x, float y, float w, float h);
	NUKEUI_API bool  HostAlive(void* host);     // false once the user closed the OS window
	NUKEUI_API bool  HostFocused(void* host);   // the OS window has keyboard focus
	NUKEUI_API void  HostFocus(void* host);     // raise + focus the OS window
	NUKEUI_API void  HostCancelClose(void* host); // clear a pending OS close request (close-confirm Cancel)
	NUKEUI_API void  HostSetTitle(void* host, const char* title); // retitle (OS title bar + content tab)
	NUKEUI_API void  HostDestroy(void* host);   // close + release (safe on dead handles)

	// --- DETACHABLE DOCUMENT WINDOWS (for ANY panel or module editor) ------------------
	// One call per frame per open document — the whole docked/detached lifecycle in a box:
	//  * docked:   draw() runs immediately inside a normal imgui window (CURRENT context);
	//  * detached: draw() runs later this frame inside the document's own OS host window.
	// NORMAL DOCKING: dragging the docked window's title bar past the main-window edge
	// (or pinning the cursor against the screen edge) tears it off into an OS window that
	// rides the cursor; dropping the detached window's tab back onto the main window
	// re-docks it at the drop point. New documents follow DocDetachDefault.
	// p_open goes false when the user closes the window (imgui X / OS close). Calling
	// again with *p_open==true afterwards CANCELS the close (dirty-confirm "Cancel").
	// State is garbage-collected: stop calling for an id and its window is destroyed.
	NUKEUI_API void DocWindow(const char* id, const char* title, bool* p_open,
	                          int imguiWindowFlags, int width, int height,
	                          const std::function<void()>& draw);
	// Same lifecycle, but for PERSISTENT PANELS (Hierarchy, Console, ...): always starts
	// docked — the detach-default preference and the "apply to all" toggle never touch
	// panels; a panel leaves the main window only when the user drags it out.
	NUKEUI_API void DocPanel(const char* id, const char* title, bool* p_open,
	                         int imguiWindowFlags, int width, int height,
	                         const std::function<void()>& draw);
	NUKEUI_API void DocFocus(const char* id);        // raise/focus the document's window
	NUKEUI_API bool DocDetached(const char* id);     // currently an OS window?
	NUKEUI_API void DocDetachDefault(bool detached); // mode for NEW documents (editor preference)
	NUKEUI_API void DocDetachAll(bool detached);     // apply a mode to every open document NOW
}
