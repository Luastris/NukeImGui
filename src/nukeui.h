#pragma once
#include <functional>

#ifdef NUKEIMGUI_EXPORTS
#define NUKEUI_API __declspec(dllexport)
#else
#define NUKEUI_API __declspec(dllimport)
#endif

namespace nuke { class iRender; }

// Bridge between Dear ImGui and a renderer. Owns the ImGui context and draws
// through the renderer's neutral seam (iRender::createTexture2D / renderDrawLists).
namespace NukeUI
{
	// Enable native imgui multi-viewport OS windows (Vulkan only). Call BEFORE Init.
	NUKEUI_API void EnableNativeViewports(bool on);
	NUKEUI_API bool NativeViewportsActive();

	// Create the ImGui context and hook this UI into the renderer's per-frame
	// GUI callback. Call once after the renderer is initialized.
	NUKEUI_API void Init(nuke::iRender* renderer);
	NUKEUI_API void Shutdown();

	// Register a window/menu drawing callback (it calls ImGui:: inside).
	NUKEUI_API void AddDrawCallback(const std::function<void()>& cb);

	// Build and submit one UI frame through the renderer. Hooked to iRender's onGUI.
	NUKEUI_API void Frame();

	// Tell the UI the current framebuffer size (call on resize).
	NUKEUI_API void SetDisplaySize(int width, int height);

	// Merge an icon font into the atlas on top of the main font (ICON_LC_* glyphs).
	// Call AFTER the app adds its main font, BEFORE the first frame.
	NUKEUI_API void MergeIconFont(const char* ttfPath, float sizePx, float glyphOffsetY = 0.0f);

	// Host windows: a decorated OS window drawn through its OWN ImGui context (shared
	// font atlas). Hosts tick inside Frame() after the main context.
	NUKEUI_API void* HostCreate(const char* title, int w, int h);
	NUKEUI_API void  HostSetContent(void* host, const std::function<void()>& draw);
	// Extra ImGuiWindowFlags for the host's content window. May be called every frame.
	NUKEUI_API void  HostSetContentFlags(void* host, int imguiWindowFlags);
	// Make the host window follow the cursor until mouse release (hot = grab point, client coords).
	NUKEUI_API void  HostBeginDrag(void* host, float hotX, float hotY);
	NUKEUI_API bool  HostDragging(void* host);
	// True ONCE when a drag ended inside the dock target; outputs cursor in main-window client coords.
	NUKEUI_API bool  HostDockDrop(void* host, float* mainX, float* mainY);
	// True while any host is drag-followed with the cursor over the main window.
	NUKEUI_API bool  HostDragActive();
	// Set the drop zone (main-window client coords) that accepts a host drop as "dock back".
	NUKEUI_API void  SetDockTarget(float x, float y, float w, float h);
	NUKEUI_API bool  HostAlive(void* host);     // false once the user closed the OS window
	NUKEUI_API bool  HostFocused(void* host);   // the OS window has keyboard focus
	NUKEUI_API void  HostFocus(void* host);     // raise + focus the OS window
	NUKEUI_API void  HostCancelClose(void* host); // clear a pending OS close request (close-confirm Cancel)
	NUKEUI_API void  HostSetTitle(void* host, const char* title); // retitle (OS title bar + content tab)
	NUKEUI_API void  HostDestroy(void* host);   // close + release (safe on dead handles)

	// Detachable document window; call once per frame per open document (docked: draw() runs
	// now; detached: later in its own OS host). p_open goes false on close, and calling again
	// with *p_open==true CANCELS it; stop calling for an id and its window is destroyed.
	NUKEUI_API void DocWindow(const char* id, const char* title, bool* p_open,
	                          int imguiWindowFlags, int width, int height,
	                          const std::function<void()>& draw);
	// Same as DocWindow but for persistent panels: always starts docked and ignores
	// DocDetachDefault / DocDetachAll; detaches only on an explicit user drag.
	NUKEUI_API void DocPanel(const char* id, const char* title, bool* p_open,
	                         int imguiWindowFlags, int width, int height,
	                         const std::function<void()>& draw);
	NUKEUI_API void DocFocus(const char* id);        // raise/focus the document's window
	NUKEUI_API bool DocDetached(const char* id);     // currently an OS window?
	NUKEUI_API void DocDetachDefault(bool detached); // mode for NEW documents (editor preference)
	NUKEUI_API void DocDetachAll(bool detached);     // apply a mode to every open document NOW
}
