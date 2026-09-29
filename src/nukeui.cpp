#include "nukeui.h"
#include "imgui.h"
#include "imgui_internal.h"   // MovingWindow/ClearActiveID — host drag-to-dock (task #135)
#include "IconsLucide.h"      // ICON_LC_* + ICON_MIN_LC / ICON_MAX_LC range
#include <array>
#include <string>
#include "backends/imgui_impl_glfw.h"   // multi-viewport PLATFORM backend (native OS windows)
#include <render/irender.h>   // engine: iRender + NukeUIDrawData (neutral seam)
#include <vector>
#include <cstring>
#include <chrono>
#include <map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>          // WM_SETICON for the secondary OS windows
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h> // glfwGetWin32Window (main window HWND -> icon source)
#else
#include <GLFW/glfw3.h>       // window/input plumbing is GLFW on every platform
struct POINT { long x = 0, y = 0; };   // NukeUIHost::rsCur — only fed by the Windows chrome path
#if !defined(__APPLE__)
// nukeui_x11.cpp — Xlib.h's macro pollution (#define Status int, ...) stays out of this TU.
extern "C" void* NukeUIX11WindowHandle(GLFWwindow* w);   // X11 Window id / wl_surface*
extern "C" void  NukeUIX11CopyIcon(GLFWwindow* src, GLFWwindow* dst);   // _NET_WM_ICON mirror
extern "C" bool  NukeUINativeIsWayland(void);
#endif
#endif

struct GLFWwindow;

using namespace nuke;   // iRender / NukeUIDrawData live in namespace nuke

static iRender*                            g_render = nullptr;

// The renderer keys secondary swap chains on a NATIVE handle. imgui_impl_glfw fills
// PlatformHandleRaw on Win32 (HWND) and Cocoa (NSWindow) but leaves it null on X11 —
// resolve the X11 Window id from the viewport's GLFWwindow there.
static void* ViewportNativeHandle(ImGuiViewport* v)
{
#if defined(_WIN32) || defined(__APPLE__)
	return v ? v->PlatformHandleRaw : nullptr;
#else
	return (v && v->PlatformHandle)
	     ? NukeUIX11WindowHandle((GLFWwindow*)v->PlatformHandle) : nullptr;
#endif
}
static std::vector<std::function<void()>>  g_callbacks;
static void TickContentScale();   // per-frame DPI watch (defined with the UI-scale block)
static bool                                g_glfwPlatform = false;   // imgui_impl_glfw mounted (viewports on)
static void*                               g_mainHwnd = nullptr;     // main window (icon source for secondaries)
static void (*g_origCreateWindow)(ImGuiViewport*) = nullptr;

#ifdef __APPLE__
static void (*g_origSetWindowPos)(ImGuiViewport*, ImVec2) = nullptr;

// Place-verify-correct window positioning (see the install site for the macOS rationale).
static void SetWindowPosVerified(ImGuiViewport* v, ImVec2 pos)
{
	g_origSetWindowPos(v, pos);
	GLFWwindow* w = (GLFWwindow*)v->PlatformHandle;
	if (!w) return;
	// A HIDDEN window is where the mis-set lives in the first place — readback there is
	// not trustworthy either. Defer: the show-wrapper re-asserts the position once visible.
	if (!glfwGetWindowAttrib(w, GLFW_VISIBLE)) return;
	int ax = 0, ay = 0;
	glfwGetWindowPos(w, &ax, &ay);
	const float e1x = (float)ax - pos.x, e1y = (float)ay - pos.y;
	if (fabsf(e1x) <= 2.0f && fabsf(e1y) <= 2.0f) return;   // honest platform: done

	// Second sample: shift-compensated attempt (solves a pure offset outright).
	g_origSetWindowPos(v, ImVec2(pos.x - e1x, pos.y - e1y));
	int bx = 0, by = 0;
	glfwGetWindowPos(w, &bx, &by);
	if (fabsf((float)bx - pos.x) <= 2.0f && fabsf((float)by - pos.y) <= 2.0f) return;

	// Affine per-axis: actual = a*set + b from the two samples; invert for the exact target.
	auto solve = [](float s1, float a1, float s2, float a2, float want) -> float
	{
		const float ds = s2 - s1;
		if (fabsf(ds) < 0.5f) return want;
		const float a = (a2 - a1) / ds;
		if (fabsf(a) < 0.25f) return want;     // degenerate (platform ignoring us): give up
		const float b = a1 - a * s1;
		return (want - b) / a;
	};
	g_origSetWindowPos(v, ImVec2(solve(pos.x, (float)ax, pos.x - e1x, (float)bx, pos.x),
	                             solve(pos.y, (float)ay, pos.y - e1y, (float)by, pos.y)));
}

// GLFW/macOS has a history of size-sets REPOSITIONING the window (glfw#1553, refixed and
// re-broken across versions): lock the position across every size change.
static void (*g_origSetWindowSize)(ImGuiViewport*, ImVec2) = nullptr;
static void SetWindowSizePosLocked(ImGuiViewport* v, ImVec2 size)
{
	g_origSetWindowSize(v, size);
	if (g_origSetWindowPos) SetWindowPosVerified(v, v->Pos);
}

// The broken placement happens at CREATION: the glfw backend positions the freshly created
// (still HIDDEN) window with a direct glfwSetWindowPos — off-target on macOS/GLFW 3.5 — and
// then suppresses its own pos event, so imgui never learns the window is 600px away from
// its hitboxes. Re-assert the viewport position right after the window becomes visible.
static void (*g_origShowWindow)(ImGuiViewport*) = nullptr;
static void ShowWindowVerified(ImGuiViewport* v)
{
	g_origShowWindow(v);
	SetWindowPosVerified(v, v->Pos);
}
#endif

// Platform-backend window creation wrapper: a new secondary OS window inherits the main window's icons.
static GLFWwindow* g_mainGlfwIcon = nullptr;   // main window (X11 _NET_WM_ICON source)
static void CreateWindowWithIcon(ImGuiViewport* v)
{
	if (g_origCreateWindow) g_origCreateWindow(v);
#ifdef _WIN32
	if (v->PlatformHandleRaw && g_mainHwnd)
	{
		HWND src = (HWND)g_mainHwnd, dst = (HWND)v->PlatformHandleRaw;
		HICON big = (HICON)SendMessageW(src, WM_GETICON, ICON_BIG, 0);
		if (!big) big = (HICON)GetClassLongPtrW(src, GCLP_HICON);
		HICON sml = (HICON)SendMessageW(src, WM_GETICON, ICON_SMALL, 0);
		if (!sml) sml = (HICON)GetClassLongPtrW(src, GCLP_HICONSM);
		if (big) SendMessageW(dst, WM_SETICON, ICON_BIG,   (LPARAM)big);
		if (sml) SendMessageW(dst, WM_SETICON, ICON_SMALL, (LPARAM)sml);
	}
#elif !defined(__APPLE__)
	if (v->PlatformHandle && g_mainGlfwIcon)
		NukeUIX11CopyIcon(g_mainGlfwIcon, (GLFWwindow*)v->PlatformHandle);
#endif
}

// Scratch buffers reused each frame (kept alive across the renderDrawLists call).
static std::vector<NukeUIDrawList>          s_lists;
static std::vector<std::vector<NukeUICmd>>  s_cmds;
static int s_dispW = 1280, s_dispH = 720;

namespace
{
// ImGui 1.92 dynamic texture lifecycle, routed through the renderer's seam.
void UpdateTextures(ImDrawData* dd)
{
	if (dd->Textures == nullptr)
		return;
	for (ImTextureData* tex : *dd->Textures)
	{
		switch (tex->Status)
		{
			case ImTextureStatus_WantCreate:
			{
				// Upload can fail transiently: never store a null id as OK — stay WantCreate, retry next frame.
				uint64_t h = g_render->createTexture2D(tex->GetPixels(), tex->Width, tex->Height);
				if (!h)
					break;
				tex->SetTexID((ImTextureID)h);
				tex->SetStatus(ImTextureStatus_OK);
				break;
			}
			case ImTextureStatus_WantUpdates:
			{
				// No partial update in the seam: recreate whole, replacement FIRST so a failure keeps the stale texture.
				uint64_t h = g_render->createTexture2D(tex->GetPixels(), tex->Width, tex->Height);
				if (!h)
					break;
				if (tex->GetTexID() != ImTextureID_Invalid)
					g_render->destroyTexture2D((uint64_t)tex->GetTexID());
				tex->SetTexID((ImTextureID)h);
				tex->SetStatus(ImTextureStatus_OK);
				break;
			}
			case ImTextureStatus_WantDestroy:
			{
				if (tex->UnusedFrames > 0)
				{
					if (tex->GetTexID() != ImTextureID_Invalid)
						g_render->destroyTexture2D((uint64_t)tex->GetTexID());
					tex->SetTexID(ImTextureID_Invalid);
					tex->SetStatus(ImTextureStatus_Destroyed);
				}
				break;
			}
			default:
				break;
		}
	}
}

// ImDrawData -> the neutral seam's draw-list layout. `lists`/`cmds` are caller scratch
// that must outlive the render call consuming `out`.
void BuildDrawData(ImDrawData* dd, std::vector<NukeUIDrawList>& lists,
                   std::vector<std::vector<NukeUICmd>>& cmds, NukeUIDrawData& out)
{
	lists.clear();
	cmds.clear();
	cmds.resize(dd->CmdListsCount);

	const ImVec2 pos   = dd->DisplayPos;
	const ImVec2 scale = dd->FramebufferScale;

	for (int i = 0; i < dd->CmdListsCount; ++i)
	{
		const ImDrawList* dl = dd->CmdLists[i];
		std::vector<NukeUICmd>& cl = cmds[i];
		cl.reserve(dl->CmdBuffer.Size);
		for (int c = 0; c < dl->CmdBuffer.Size; ++c)
		{
			const ImDrawCmd& dc = dl->CmdBuffer[c];
			if (dc.UserCallback != nullptr || dc.ElemCount == 0)
				continue;
			// Not GetTexID(): its IM_ASSERT aborts on a texture that failed to upload. Skip the cmd instead.
			const ImTextureData* texData = dc.TexRef._TexData;
			const ImTextureID    texId   = texData ? texData->TexID : dc.TexRef._TexID;
			if (texId == ImTextureID_Invalid)
				continue;
			NukeUICmd nc{};
			nc.clipRect[0] = (dc.ClipRect.x - pos.x) * scale.x;
			nc.clipRect[1] = (dc.ClipRect.y - pos.y) * scale.y;
			nc.clipRect[2] = (dc.ClipRect.z - pos.x) * scale.x;
			nc.clipRect[3] = (dc.ClipRect.w - pos.y) * scale.y;
			nc.texId     = (uint64_t)texId;
			nc.elemCount = dc.ElemCount;
			nc.idxOffset = dc.IdxOffset;
			nc.vtxOffset = dc.VtxOffset;
			cl.push_back(nc);
		}

		NukeUIDrawList nl{};
		nl.vtx      = reinterpret_cast<const NukeUIVert*>(dl->VtxBuffer.Data); // ImDrawVert == NukeUIVert layout
		nl.vtxCount = dl->VtxBuffer.Size;
		nl.idx      = reinterpret_cast<const uint16_t*>(dl->IdxBuffer.Data);
		nl.idxCount = dl->IdxBuffer.Size;
		nl.cmds     = cl.data();
		nl.cmdCount = (int)cl.size();
		lists.push_back(nl);
	}

	out.lists       = lists.data();
	out.listCount   = (int)lists.size();
	out.dispPos[0]  = pos.x;
	out.dispPos[1]  = pos.y;
	out.dispSize[0] = dd->DisplaySize.x;
	out.dispSize[1] = dd->DisplaySize.y;
}
} // namespace

void NukeUI::SetDisplaySize(int width, int height)
{
	if (width > 0 && height > 0) { s_dispW = width; s_dispH = height; }
}

void NukeUI::MergeIconFont(const char* ttfPath, float sizePx, float glyphOffsetY,
                           unsigned int rangeMin, unsigned int rangeMax)
{
	ImGuiIO& io = ImGui::GetIO();
	// Each icon font brings its OWN codepoints; the ranges must outlive the atlas build, so they
	// are kept in a list that only grows.
	static std::vector<std::array<ImWchar, 3>> kept;
	if (!rangeMin || !rangeMax) { rangeMin = ICON_MIN_LC; rangeMax = ICON_MAX_LC; }
	kept.push_back({ (ImWchar)rangeMin, (ImWchar)rangeMax, 0 });
	const ImWchar* ranges = kept.back().data();
	ImFontConfig cfg;
	cfg.MergeMode        = true;        // merge onto the previously-added main font
	cfg.PixelSnapH       = true;
	cfg.GlyphMinAdvanceX = sizePx;      // make icons monospaced so toolbar buttons align
	cfg.GlyphOffset.y    = glyphOffsetY; // nudge icons down to vertically centre them in the line
	io.Fonts->AddFontFromFileTTF(ttfPath, sizePx, &cfg, ranges);
}

void NukeUI::MergeFallbackFont(const char* ttfPath, float sizePx)
{
	ImGuiIO& io = ImGui::GetIO();
	ImFontConfig cfg;
	cfg.MergeMode  = true;
	cfg.PixelSnapH = true;
	io.Fonts->AddFontFromFileTTF(ttfPath, sizePx, &cfg);
}

void NukeUI::AddDrawCallback(const std::function<void()>& cb)
{
	g_callbacks.push_back(cb);
}

static void TickHosts();      // editor-owned host windows (defined below)
static void DocFrameBegin();  // detachable document windows: frame stamp (defined below)
static void CollectDocs();    // ...and end-of-frame garbage collection

void NukeUI::Frame()
{
	if (!g_render)
		return;

	TickContentScale();   // window moved to a different-DPI monitor -> rescale the UI
	DocFrameBegin();   // stamp the frame for the detachable-document GC
	ImGuiIO& io = ImGui::GetIO();
	if (g_glfwPlatform)
	{
		// The GLFW backend owns DisplaySize / DeltaTime / mouse state for the whole viewport set.
		ImGui_ImplGlfw_NewFrame();
		// Headless/remote sessions can enumerate ZERO displays (CGDisplay/X denied) and imgui
		// asserts on an empty monitor list with viewports on — synthesize one over the main
		// window so the frame proceeds; a real session repopulates the list next change.
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) && pio.Monitors.Size == 0)
		{
			ImGuiPlatformMonitor mon;
			mon.MainPos  = mon.WorkPos  = ImVec2(0, 0);
			mon.MainSize = mon.WorkSize = (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
			                              ? io.DisplaySize : ImVec2(1280, 720);
			pio.Monitors.push_back(mon);
			// A display-less session must never PERSIST layout: window placements computed
			// against the synthetic monitor would poison imgui.ini for real sessions.
			io.IniFilename = nullptr;
		}
	}
	else
	{
		// Follow the renderer's current framebuffer size so the UI scales with the window.
		int w = (g_render->width  > 0) ? g_render->width  : s_dispW;
		int h = (g_render->height > 0) ? g_render->height : s_dispH;
		io.DisplaySize = ImVec2((float)w, (float)h);

		// Real per-frame delta: a fixed 1/60 outruns real time and breaks key repeat/blink.
		static std::chrono::steady_clock::time_point last;
		static bool haveLast = false;
		auto now = std::chrono::steady_clock::now();
		io.DeltaTime = haveLast ? std::chrono::duration<float>(now - last).count() : (1.0f / 60.0f);
		last = now; haveLast = true;
		if (io.DeltaTime <= 0.0f)
			io.DeltaTime = 1.0f / 60.0f;
	}

	ImGui::NewFrame();
	for (auto& cb : g_callbacks)
		cb();
	ImGui::Render();

	ImDrawData* dd = ImGui::GetDrawData();
	if (!dd)
		return;

	UpdateTextures(dd); // must run before reading cmd tex ids

	NukeUIDrawData nd{};
	BuildDrawData(dd, s_lists, s_cmds, nd);
	g_render->renderDrawLists(nd);

	// Secondary viewports: GLFW owns the OS windows, the renderer draws each into its own swap chain.
	if (g_glfwPlatform && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
	{
		ImGui::UpdatePlatformWindows();
#ifdef __APPLE__
		// Position coherence: hitboxes live at imgui's viewport Pos, pixels at the OS window.
		// Diverged (AppKit constraints, GLFW quirks) → adopt the OS position ONE-WAY via
		// PlatformRequestMove. Never re-assert in a loop: an async mover turns that into a
		// 60fps tug-of-war and the window visibly oscillates.
		{
			ImGuiPlatformIO& gpio = ImGui::GetPlatformIO();
			ImGuiContext* gctx = ImGui::GetCurrentContext();
			for (int i = 1; i < gpio.Viewports.Size; ++i)
			{
				ImGuiViewport* v = gpio.Viewports[i];
				GLFWwindow* w = (GLFWwindow*)v->PlatformHandle;
				if (!w || !glfwGetWindowAttrib(w, GLFW_VISIBLE)) continue;
				if (gctx->MovingWindow) continue;   // never fight a live imgui drag
				int ox = 0, oy = 0;
				glfwGetWindowPos(w, &ox, &oy);
				if (fabsf((float)ox - v->Pos.x) <= 2.0f && fabsf((float)oy - v->Pos.y) <= 2.0f) continue;
				v->PlatformRequestMove = true;
			}
		}
#endif
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		static std::vector<NukeUIDrawList>         vpLists;   // scratch reused per viewport
		static std::vector<std::vector<NukeUICmd>> vpCmds;
		for (int i = 1; i < pio.Viewports.Size; ++i)   // 0 = the main viewport, drawn above
		{
			ImGuiViewport* v = pio.Viewports[i];
			if ((v->Flags & ImGuiViewportFlags_IsMinimized) || !v->DrawData)
				continue;
			UpdateTextures(v->DrawData);
			NukeUIDrawData vd{};
			BuildDrawData(v->DrawData, vpLists, vpCmds, vd);
			const int vw = (int)(v->DrawData->DisplaySize.x * v->DrawData->FramebufferScale.x);
			const int vh = (int)(v->DrawData->DisplaySize.y * v->DrawData->FramebufferScale.y);
			g_render->uiViewportRender(ViewportNativeHandle(v), vw, vh, vd);
		}
	}

	TickHosts();     // editor-owned host windows (detached asset editors) — after the main pass
	CollectDocs();   // documents not drawn this frame are closed — free their hosts
}

// GLFW key code -> ImGuiKey (modifier state comes from the mods bitmask instead).
static ImGuiKey GlfwToImGuiKey(int key)
{
	switch (key)
	{
		case 32:  return ImGuiKey_Space;
		case 39:  return ImGuiKey_Apostrophe;
		case 44:  return ImGuiKey_Comma;
		case 45:  return ImGuiKey_Minus;
		case 46:  return ImGuiKey_Period;
		case 47:  return ImGuiKey_Slash;
		case 59:  return ImGuiKey_Semicolon;
		case 61:  return ImGuiKey_Equal;
		case 256: return ImGuiKey_Escape;
		case 257: return ImGuiKey_Enter;
		case 258: return ImGuiKey_Tab;
		case 259: return ImGuiKey_Backspace;
		case 260: return ImGuiKey_Insert;
		case 261: return ImGuiKey_Delete;
		case 262: return ImGuiKey_RightArrow;
		case 263: return ImGuiKey_LeftArrow;
		case 264: return ImGuiKey_DownArrow;
		case 265: return ImGuiKey_UpArrow;
		case 266: return ImGuiKey_PageUp;
		case 267: return ImGuiKey_PageDown;
		case 268: return ImGuiKey_Home;
		case 269: return ImGuiKey_End;
		case 335: return ImGuiKey_KeypadEnter;
		default:  break;
	}
	if (key >= 48 && key <= 57)   return (ImGuiKey)(ImGuiKey_0 + (key - 48));
	if (key >= 65 && key <= 90)   return (ImGuiKey)(ImGuiKey_A + (key - 65));
	if (key >= 320 && key <= 329) return (ImGuiKey)(ImGuiKey_Keypad0 + (key - 320));
	return ImGuiKey_None;
}


// A host = a GLFW window we create + its own ImGui context (shared font atlas, copied style).
// Our GLFW callbacks feed that context; drawing goes through iRender::uiViewportRender.
struct NukeUIHost
{
	GLFWwindow*           win = nullptr;
	ImGuiContext*         ctx = nullptr;
	std::function<void()> content;
	std::string           title;
	bool                  alive = true;
	int                   contentFlags = 0;   // extra ImGuiWindowFlags for the content window
	// Drag-to-dock state (the window follows the cursor; release over the main window = dock).
	bool  dragging = false;
	float hotX = 0, hotY = 0;    // grab point in client coords — stays under the cursor
	bool  dockDrop = false;      // consumed by HostDockDrop
	float dropX = 0, dropY = 0;  // cursor at drop, main-window client coords
	// Borderless resize (OUR chrome): edge mask 1=L 2=R 4=T 8=B + gesture start state.
	int   rsEdge = 0;
	int   rsStartW = 0, rsStartH = 0, rsStartX = 0, rsStartY = 0;
	POINT rsCur{};
};
static std::vector<NukeUIHost*> g_hosts;
static GLFWwindow* g_mainGlfw = nullptr;   // the renderer's main window (portable host math)


#ifdef __APPLE__
// nukeui_cocoa.mm: ABSOLUTE cursor/button state from the OS. glfw's per-window cursor is
// event-driven on macOS — stale between deliveries, and window-pos + stale-client-cursor
// feeds back into itself: the window then drifts entirely on its own.
extern "C" void NukeUICocoaGlobalCursor(int* x, int* y);
extern "C" bool NukeUICocoaMouseLeftDown(void);
#endif

// Global cursor in GLFW screen coordinates — the same space glfwSetWindowPos speaks.
// macOS: an absolute OS query (never stale, never a feedback loop). Elsewhere: the
// button-owning window's origin + client cursor (GLFW keeps reporting to that window).
static void HostCursorGlobal(GLFWwindow* win, int& gx, int& gy)
{
#ifdef __APPLE__
	(void)win;
	NukeUICocoaGlobalCursor(&gx, &gy);
#else
	double cx = 0, cy = 0;
	int wx = 0, wy = 0;
	glfwGetCursorPos(win, &cx, &cy);
	glfwGetWindowPos(win, &wx, &wy);
	gx = wx + (int)cx;
	gy = wy + (int)cy;
#endif
}

// Physical left-button state for gesture release detection — must not depend on which
// window the release event lands in.
static bool HostMouseLeftDown(GLFWwindow* winA, GLFWwindow* winB)
{
#ifdef __APPLE__
	(void)winA; (void)winB;
	return NukeUICocoaMouseLeftDown();
#else
	return (winA && glfwGetMouseButton(winA, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
	    || (winB && glfwGetMouseButton(winB, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
#endif
}
static bool  g_nativeViewports = false;    // Vulkan: imgui multi-viewport ON (hosts = D3D fallback)
static bool  g_hostDragOverMain = false;   // a host is drag-following with the cursor over main
static float g_dockTarget[4] = {};         // dock drop zone, main-window client coords (x,y,w,h)
static bool  g_dockTargetValid = false;

void NukeUI::EnableNativeViewports(bool on) { g_nativeViewports = on; }
bool NukeUI::NativeViewportsActive()        { return g_nativeViewports; }

// ---- UI scale ------------------------------------------------------------------------------
static ImGuiStyle g_uiStyleBase;          // the theme at 100%, captured after ApplyStyle
static bool       g_uiStyleBaseValid = false;
static float      g_uiScale = 1.0f;

void NukeUI::CaptureStyleBaseline()
{
	if (!ImGui::GetCurrentContext()) return;
	g_uiStyleBase      = ImGui::GetStyle();
	g_uiStyleBaseValid = true;
}

void NukeUI::SetUIScale(float factor)
{
	if (!ImGui::GetCurrentContext()) return;
	if (factor < 0.25f) factor = 0.25f;
	if (factor > 4.0f)  factor = 4.0f;
	if (!g_uiStyleBaseValid) CaptureStyleBaseline();
	g_uiScale = factor;
	// Sizes re-derive from the baseline (repeat calls never compound); the font stack scales
	// through FontScaleMain — imgui 1.92 dynamic fonts re-rasterize crisply, icons included.
	ImGuiStyle scaled = g_uiStyleBase;
	scaled.ScaleAllSizes(factor);
	scaled.FontScaleMain = factor;
	ImGui::GetStyle() = scaled;
	// Open host windows carry their own context with a style COPY — keep them in step
	// (freshly created hosts snapshot the main style and inherit the scale for free).
	ImGuiContext* mainCtx = ImGui::GetCurrentContext();
	for (NukeUIHost* h : g_hosts)
		if (h && h->ctx)
		{
			ImGui::SetCurrentContext(h->ctx);
			ImGui::GetStyle() = scaled;
			ImGui::SetCurrentContext(mainCtx);
		}
}

float NukeUI::SystemContentScale()
{
	float sx = 1.0f, sy = 1.0f;
	if (g_mainGlfw) glfwGetWindowContentScale(g_mainGlfw, &sx, &sy);
	return sx > 0.1f ? sx : 1.0f;
}

// User factor (Preferences slider) with LIVE content-scale tracking: Frame() polls the OS
// scale — one glfw call — and re-applies when the window lands on a different-DPI monitor.
static float g_userUIScale        = 0.0f;   // 0 = tracking inactive (raw SetUIScale only)
static float g_lastContentScale   = 1.0f;

void NukeUI::SetUserUIScale(float userFactor)
{
	if (userFactor <= 0.0f) userFactor = 1.0f;
	g_userUIScale      = userFactor;
	g_lastContentScale = SystemContentScale();
	SetUIScale(g_userUIScale * g_lastContentScale);
}

static void TickContentScale()
{
	if (g_userUIScale <= 0.0f || !g_mainGlfw) return;
	const float cs = NukeUI::SystemContentScale();
	if (cs > 0.1f && (cs > g_lastContentScale * 1.01f || cs < g_lastContentScale * 0.99f))
	{
		printf("[NukeUI]\t\tmonitor content scale %.2f -> %.2f — rescaling UI\n",
		       g_lastContentScale, cs);
		g_lastContentScale = cs;
		NukeUI::SetUIScale(g_userUIScale * cs);
	}
}

bool NukeUI::HostDragActive() { return g_hostDragOverMain; }
void NukeUI::SetDockTarget(float x, float y, float w, float h)
{
	g_dockTarget[0] = x; g_dockTarget[1] = y; g_dockTarget[2] = w; g_dockTarget[3] = h;
	g_dockTargetValid = w > 0 && h > 0;
}

static NukeUIHost* HostOf(GLFWwindow* w) { return (NukeUIHost*)glfwGetWindowUserPointer(w); }

// Every callback swaps to the host's context, feeds the event, and swaps back.
#define NUKE_HOST_EVENT(body) \
	NukeUIHost* h = HostOf(w); if (!h) return; \
	ImGuiContext* prev = ImGui::GetCurrentContext(); \
	ImGui::SetCurrentContext(h->ctx); \
	body; \
	ImGui::SetCurrentContext(prev);

static void HostCursorPos(GLFWwindow* w, double x, double y)
{ NUKE_HOST_EVENT(ImGui::GetIO().AddMousePosEvent((float)x, (float)y)) }
static void HostMouseBtn(GLFWwindow* w, int button, int action, int)
{
	if (button < 0 || button > 2) return;
	NUKE_HOST_EVENT(ImGui::GetIO().AddMouseButtonEvent(button, action == GLFW_PRESS))
}
static void HostScroll(GLFWwindow* w, double dx, double dy)
{ NUKE_HOST_EVENT(ImGui::GetIO().AddMouseWheelEvent((float)dx, (float)dy)) }
static void HostChar(GLFWwindow* w, unsigned int c)
{ NUKE_HOST_EVENT(ImGui::GetIO().AddInputCharacter(c)) }
static void HostFocusCb(GLFWwindow* w, int focused)
{ NUKE_HOST_EVENT(ImGui::GetIO().AddFocusEvent(focused != 0)) }
static void HostKey(GLFWwindow* w, int key, int, int action, int mods)
{
	NukeUIHost* h = HostOf(w); if (!h) return;
	ImGuiContext* prev = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(h->ctx);
	ImGuiIO& hio = ImGui::GetIO();
	hio.AddKeyEvent(ImGuiMod_Ctrl,  (mods & GLFW_MOD_CONTROL) != 0);
	hio.AddKeyEvent(ImGuiMod_Shift, (mods & GLFW_MOD_SHIFT) != 0);
	hio.AddKeyEvent(ImGuiMod_Alt,   (mods & GLFW_MOD_ALT) != 0);
	const ImGuiKey k = GlfwToImGuiKey(key);
	if (k != ImGuiKey_None && action != GLFW_REPEAT)
		hio.AddKeyEvent(k, action == GLFW_PRESS);
	ImGui::SetCurrentContext(prev);
}

void* NukeUI::HostCreate(const char* title, int w, int h)
{
	if (!g_render) return nullptr;
	ImGuiContext* mainCtx = ImGui::GetCurrentContext();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	// Borderless: title bar, borders and resize edges are drawn by imgui in TickHosts.
	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	glfwWindowHint(GLFW_VISIBLE,   GLFW_TRUE);
	GLFWwindow* win = glfwCreateWindow(w, h, title ? title : "NukeEngine", nullptr, nullptr);
	if (!win) return nullptr;
	// Spawn CENTERED over the main window — the WM's default placement can land a fresh
	// host almost off-screen. A tear-off re-positions under the cursor this same frame;
	// on native Wayland this is a no-op and the compositor's own placement applies.
	if (g_mainGlfw)
	{
		int mx = 0, my = 0, mw = 0, mh = 0;
		glfwGetWindowPos(g_mainGlfw, &mx, &my);
		glfwGetWindowSize(g_mainGlfw, &mw, &mh);
		glfwSetWindowPos(win, mx + (mw - w) / 2, my + (mh - h) / 2);
	}

	NukeUIHost* host = new NukeUIHost();
	host->win = win;
	host->title = title ? title : "NukeEngine";
	std::cout << "[NukeUI]\thost CREATE '" << host->title << "' (" << w << "x" << h << ")" << std::endl;
	const ImGuiStyle mainStyle = ImGui::GetStyle();           // snapshot while MAIN is current
	host->ctx = ImGui::CreateContext(ImGui::GetIO().Fonts);   // SHARE the font atlas
	ImGui::SetCurrentContext(host->ctx);
	ImGui::GetIO().IniFilename = nullptr;                     // hosts don't persist imgui layout
	ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	ImGui::GetStyle() = mainStyle;                            // same look as the editor
	ImGui::SetCurrentContext(mainCtx);

	glfwSetWindowUserPointer(win, host);
	glfwSetCursorPosCallback(win, HostCursorPos);
	glfwSetMouseButtonCallback(win, HostMouseBtn);
	glfwSetScrollCallback(win, HostScroll);
	glfwSetKeyCallback(win, HostKey);
	glfwSetCharCallback(win, HostChar);
	glfwSetWindowFocusCallback(win, HostFocusCb);
#ifdef _WIN32
	// Dark class background: a fresh window must not flash white before the first blit.
	if (HWND hb = glfwGetWin32Window(win))
	{
		static HBRUSH darkBrush = CreateSolidBrush(RGB(15, 15, 17));
		SetClassLongPtrW(hb, GCLP_HBRBACKGROUND, (LONG_PTR)darkBrush);
	}
	// Inherit the editor's window icon (title bar + taskbar).
	if (g_mainHwnd)
	{
		HWND src = (HWND)g_mainHwnd, dst = glfwGetWin32Window(win);
		HICON big = (HICON)SendMessageW(src, WM_GETICON, ICON_BIG, 0);
		if (!big) big = (HICON)GetClassLongPtrW(src, GCLP_HICON);
		HICON sml = (HICON)SendMessageW(src, WM_GETICON, ICON_SMALL, 0);
		if (!sml) sml = (HICON)GetClassLongPtrW(src, GCLP_HICONSM);
		if (big) SendMessageW(dst, WM_SETICON, ICON_BIG,   (LPARAM)big);
		if (sml) SendMessageW(dst, WM_SETICON, ICON_SMALL, (LPARAM)sml);
	}
#elif !defined(__APPLE__)
	// Inherit the editor's window icon (X11 _NET_WM_ICON, set by the renderer on the main window).
	if (g_mainGlfw)
		NukeUIX11CopyIcon(g_mainGlfw, win);
#endif
	g_hosts.push_back(host);
	return host;
}

void NukeUI::HostSetContent(void* hostPtr, const std::function<void()>& draw)
{
	if (NukeUIHost* h = (NukeUIHost*)hostPtr) h->content = draw;
}

bool NukeUI::HostAlive(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	return h && h->alive && h->win && !glfwWindowShouldClose(h->win);
}

bool NukeUI::HostFocused(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	return h && h->win && glfwGetWindowAttrib(h->win, GLFW_FOCUSED);
}

// Minimize + maximize/restore for the CURRENT imgui window (call right after Begin). The OS
// window is either the editor-owned host or, with native viewports, the window's OWN platform
// viewport — a docked window has neither and draws nothing. Painted on the window's foreground
// list with a manual hit test: a real widget in the title bar would fight imgui's own close
// button and the title drag, so the click is consumed here and the move is cancelled.
void NukeUI::WindowCaptionButtons(void* hostPtr)
{
	ImGuiWindow* win = ImGui::GetCurrentWindow();
	if (!win || (win->Flags & ImGuiWindowFlags_NoTitleBar)) return;
	GLFWwindow* os = nullptr;
	ImGuiViewport* vp = win->Viewport;
	const bool imguiOwned = !hostPtr;   // imgui drives this platform window's pos/size
	if (hostPtr) os = ((NukeUIHost*)hostPtr)->win;
	else if (vp && vp != ImGui::GetMainViewport() && (vp->Flags & ImGuiViewportFlags_IsPlatformWindow))
		os = (GLFWwindow*)vp->PlatformHandle;
	// Caption buttons belong to windows with an OS window OF THEIR OWN. A docked tab or a
	// floating panel inside the main window keeps imgui's plain close X and nothing else —
	// custom minimize/maximize chrome there is noise (and overlaps the tab strip when docked).
	if (!os || win->DockIsActive) return;
	const bool canMinimize = os != nullptr;

	// Maximize state. An imgui-OWNED window must NOT be maximized through the OS: imgui pushes
	// its own size every frame, Windows refuses it while maximized, and the pair fight forever
	// (the swap chain resizes each frame). So "maximize" means: remember pos/size and stretch
	// the imgui window over the monitor work area; restore puts them back. Only editor-owned
	// hosts — whose size imgui does not drive — use the real OS maximize.
	struct CapState { bool maxed = false; ImVec2 pos, size; };
	static std::map<ImGuiID, CapState> capState;
	CapState& cap = capState[win->ID];
#ifndef __APPLE__
	// Windows-only heal: on macOS GLFW_MAXIMIZED ([NSWindow isZoomed]) lies for borderless
	// windows and a per-frame restore animates the window up the screen ([window zoom:]).
	if (imguiOwned && os && glfwGetWindowAttrib(os, GLFW_MAXIMIZED))
		glfwRestoreWindow(os);   // heal a window an earlier build left OS-maximized
#endif

	ImGuiContext& g = *ImGui::GetCurrentContext();
	const float bh = win->TitleBarHeight;
	const float slotW = bh + 4.0f;
	const float y0 = win->Pos.y, y1 = y0 + bh;
	const ImVec2 mp = ImGui::GetIO().MousePos;
	const bool maxed = imguiOwned ? cap.maxed : (glfwGetWindowAttrib(os, GLFW_MAXIMIZED) != 0);
	// Stretch over / restore from the monitor holding the window's centre.
	auto toggleMax = [&]()
	{
		if (!imguiOwned)
		{
			if (maxed) glfwRestoreWindow(os); else glfwMaximizeWindow(os);
			return;
		}
		if (cap.maxed)
		{
			ImGui::SetWindowPos(cap.pos);
			ImGui::SetWindowSize(cap.size);
			cap.maxed = false;
			return;
		}
		cap.pos = win->Pos;
		cap.size = win->Size;
		const ImVec2 c(win->Pos.x + win->Size.x * 0.5f, win->Pos.y + win->Size.y * 0.5f);
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		const ImGuiPlatformMonitor* best = nullptr;
		for (int mi = 0; mi < pio.Monitors.Size; ++mi)
		{
			const ImGuiPlatformMonitor& m = pio.Monitors[mi];
			if (c.x >= m.WorkPos.x && c.x < m.WorkPos.x + m.WorkSize.x
			    && c.y >= m.WorkPos.y && c.y < m.WorkPos.y + m.WorkSize.y) { best = &m; break; }
			if (!best) best = &m;
		}
		if (best)
		{
			ImGui::SetWindowPos(best->WorkPos);
			ImGui::SetWindowSize(best->WorkSize);
			cap.maxed = true;
		}
	};
	// Begin() clips the window's draw list to its INNER rect — the title bar is outside it, so
	// anything drawn there is invisible. Replace the clip with the title bar itself (no
	// intersect) for the buttons, then restore.
	ImDrawList* dl = win->DrawList;   // the window's OWN list: always part of its draw data
	dl->PushClipRect(ImVec2(win->Pos.x, y0), ImVec2(win->Pos.x + win->Size.x, y1), false);
	struct ClipPop { ImDrawList* d; ~ClipPop() { d->PopClipRect(); } } clipPop{ dl };
	bool overBtn = false;
	// Native Wayland: drag-to-dock cannot exist (no global cursor, no window positioning) —
	// a torn-off window's ONLY way home is this explicit re-dock button, one slot left of
	// minimize. X11/Windows/macOS keep the drag gesture and skip the extra button.
	bool showDock = false;
#if !defined(_WIN32) && !defined(__APPLE__)
	showDock = hostPtr && NukeUINativeIsWayland();
#endif
	if (showDock)
	{
		const float x1 = win->Pos.x + win->Size.x - slotW * 4 + 2.0f;
		const float x2 = x1 + slotW - 4.0f;
		const bool over = mp.x >= x1 && mp.x < x2 && mp.y >= y0 && mp.y < y1;
		overBtn = overBtn || over;
		if (over)
			dl->AddRectFilled(ImVec2(x1, y0 + 1), ImVec2(x2, y1 - 1),
			                  ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
		const ImU32 gc = ImGui::GetColorU32(over ? ImGuiCol_Text : ImGuiCol_TextDisabled);
		const float cx = (x1 + x2) * 0.5f, cy = (y0 + y1) * 0.5f;
		// dock-back glyph: a frame with an arrow pointing into it
		dl->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), gc, 0, 0, 1.5f);
		dl->AddLine(ImVec2(cx + 4, cy - 4), ImVec2(cx - 2, cy + 2), gc, 1.5f);
		dl->AddLine(ImVec2(cx - 2, cy - 1), ImVec2(cx - 2, cy + 2), gc, 1.5f);
		dl->AddLine(ImVec2(cx - 2, cy + 2), ImVec2(cx + 1, cy + 2), gc, 1.5f);
		if (over && ImGui::IsMouseClicked(0))
		{
			ImGui::ClearActiveID();
			g.MovingWindow = nullptr;
			NukeUIHost* h = (NukeUIHost*)hostPtr;
			h->dragging = false;
			h->dockDrop = true;   // consumed by HostDockDrop -> the regular re-dock flow
			if (g_dockTargetValid)
			{ h->dropX = g_dockTarget[0] + g_dockTarget[2] * 0.5f; h->dropY = g_dockTarget[1] + g_dockTarget[3] * 0.5f; }
			else if (g_mainGlfw)
			{ int mw = 0, mh = 0; glfwGetWindowSize(g_mainGlfw, &mw, &mh); h->dropX = mw * 0.5f; h->dropY = mh * 0.5f; }
		}
	}
	// slot 0 = minimize, slot 1 = maximize/restore; imgui's close X owns the rightmost slot.
	// Without an OS window of its own there is nothing to iconify — start at the maximize slot.
	for (int slot = canMinimize ? 0 : 1; slot < 2; ++slot)
	{
		const float x1 = win->Pos.x + win->Size.x - slotW * (3 - slot) + 2.0f;
		const float x2 = x1 + slotW - 4.0f;
		const bool over = mp.x >= x1 && mp.x < x2 && mp.y >= y0 && mp.y < y1;
		overBtn = overBtn || over;
		if (over)
			dl->AddRectFilled(ImVec2(x1, y0 + 1), ImVec2(x2, y1 - 1),
			                  ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
		const ImU32 gc = ImGui::GetColorU32(over ? ImGuiCol_Text : ImGuiCol_TextDisabled);
		const float cx = (x1 + x2) * 0.5f, cy = (y0 + y1) * 0.5f;
		if (slot == 0)        // minimize: a dash
			dl->AddLine(ImVec2(cx - 5, cy + 3), ImVec2(cx + 5, cy + 3), gc, 1.5f);
		else if (!maxed)      // maximize: a box
			dl->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), gc, 0, 0, 1.5f);
		else                  // restore: two offset boxes
		{
			dl->AddRect(ImVec2(cx - 5, cy - 2), ImVec2(cx + 2, cy + 5), gc, 0, 0, 1.5f);
			dl->AddLine(ImVec2(cx - 2, cy - 5), ImVec2(cx + 5, cy - 5), gc, 1.5f);
			dl->AddLine(ImVec2(cx + 5, cy - 5), ImVec2(cx + 5, cy + 2), gc, 1.5f);
		}
		if (over && ImGui::IsMouseClicked(0))
		{
			ImGui::ClearActiveID();
			g.MovingWindow = nullptr;   // the click is the button's, not a title drag
			if (slot == 0) { if (os) glfwIconifyWindow(os); }
			else           toggleMax();
		}
	}
	// double-click on the free title area = maximize toggle (Windows convention, never a roll-up)
	if (!overBtn && mp.y >= y0 && mp.y < y1
	    && mp.x >= win->Pos.x && mp.x < win->Pos.x + win->Size.x - slotW * (showDock ? 4 : 3)
	    && ImGui::IsMouseDoubleClicked(0))
	{
		ImGui::ClearActiveID();
		g.MovingWindow = nullptr;
		toggleMax();
	}
}

void NukeUI::HostFocus(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (h && h->win) glfwFocusWindow(h->win);
}

void NukeUI::HostSetContentFlags(void* hostPtr, int imguiWindowFlags)
{
	if (NukeUIHost* h = (NukeUIHost*)hostPtr) h->contentFlags = imguiWindowFlags;
}

void NukeUI::HostBeginDrag(void* hostPtr, float hotX, float hotY)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (!h || !h->win) return;
	h->dragging = true; h->hotX = hotX; h->hotY = hotY;
#ifdef _WIN32
	// Snap under the cursor right away — a tear-off must not flash at a stale position.
	POINT cp; GetCursorPos(&cp);
	glfwSetWindowPos(h->win, cp.x - (int)hotX, cp.y - (int)hotY);
#else
	// Tear-off starts with the button held on the MAIN window — sample the cursor there.
	if (g_mainGlfw)
	{
		int gx = 0, gy = 0;
		HostCursorGlobal(g_mainGlfw, gx, gy);
		glfwSetWindowPos(h->win, gx - (int)hotX, gy - (int)hotY);
	}
	// The click that started the gesture landed on the MAIN window, so the WM's focus-
	// stealing prevention ignores a plain raise for this one — ride ABOVE (always-on-top)
	// for the drag's duration instead; the drop site clears it. Exactly the Windows feel:
	// the dragged window is never under anything.
	glfwSetWindowAttrib(h->win, GLFW_FLOATING, GLFW_TRUE);
#endif
	glfwFocusWindow(h->win);
}

bool NukeUI::HostDragging(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	return h && h->dragging;
}

bool NukeUI::HostDockDrop(void* hostPtr, float* mainX, float* mainY)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (!h || !h->dockDrop) return false;
	h->dockDrop = false;
	if (mainX) *mainX = h->dropX;
	if (mainY) *mainY = h->dropY;
	return true;
}

void NukeUI::HostCancelClose(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (h && h->win) { glfwSetWindowShouldClose(h->win, GLFW_FALSE); h->alive = true; }
}

void NukeUI::HostSetTitle(void* hostPtr, const char* title)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (!h || !title || h->title == title) return;
	h->title = title;                       // content tab text (imgui strips "##..." itself)
	if (h->win)
	{
		// The OS/taskbar title must not show the imgui "###id" suffix.
		std::string os = h->title;
		const size_t hash = os.find("##");
		if (hash != std::string::npos) os.resize(hash);
		glfwSetWindowTitle(h->win, os.c_str());
	}
}

void NukeUI::HostDestroy(void* hostPtr)
{
	NukeUIHost* h = (NukeUIHost*)hostPtr;
	if (!h) return;
	for (size_t i = 0; i < g_hosts.size(); ++i)
		if (g_hosts[i] == h) { g_hosts.erase(g_hosts.begin() + i); break; }
	if (h->win && g_render)
	{
#ifdef _WIN32
		g_render->uiViewportDestroy((void*)glfwGetWin32Window(h->win));   // park its swap chain
#elif !defined(__APPLE__)
		g_render->uiViewportDestroy(NukeUIX11WindowHandle(h->win));
#endif
		glfwDestroyWindow(h->win);
	}
	if (h->ctx) ImGui::DestroyContext(h->ctx);
	delete h;
}

// One frame for every live host: own-context NewFrame -> fullscreen content window ->
// Render -> the renderer seam. Runs inside NukeUI::Frame AFTER the main context's pass.
static void TickHosts()
{
	if (g_hosts.empty() || !g_render) return;
	ImGuiContext* mainCtx = ImGui::GetCurrentContext();
	const float dt = ImGui::GetIO().DeltaTime > 0 ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f;
	static std::vector<NukeUIDrawList>          hostLists;
	static std::vector<std::vector<NukeUICmd>>  hostCmds;
	static uint64_t tickNo = 0;
	++tickNo;
	for (NukeUIHost* h : g_hosts)
	{
		if (!h->win) continue;
		if (glfwWindowShouldClose(h->win)) { h->alive = false; continue; }   // the app decides (HostAlive)
		// Idle unfocused hosts tick every 4th frame; focus, drags and resizes run full rate.
		const bool interacting = h->dragging || h->rsEdge != 0 || glfwGetWindowAttrib(h->win, GLFW_FOCUSED);
		if (!interacting && ((tickNo + ((uintptr_t)h >> 4)) & 3) != 0) continue;

#ifdef _WIN32
		// Drag-to-dock follow: only a release inside the dock target docks back. Button state is
		// the OR of both contexts — a tear-off holds capture in MAIN, a title-bar drag in the HOST.
		if (h->dragging)
		{
			POINT cp; GetCursorPos(&cp);
			glfwSetWindowPos(h->win, cp.x - (int)h->hotX, cp.y - (int)h->hotY);
			bool overMain = false, overTarget = false;
			POINT mp = cp;
			if (g_mainHwnd && !IsIconic((HWND)g_mainHwnd) && ScreenToClient((HWND)g_mainHwnd, &mp))
			{
				RECT rc; GetClientRect((HWND)g_mainHwnd, &rc);
				overMain = PtInRect(&rc, mp) != 0;
				overTarget = overMain && g_dockTargetValid &&
				             mp.x >= g_dockTarget[0] && mp.y >= g_dockTarget[1] &&
				             mp.x <  g_dockTarget[0] + g_dockTarget[2] &&
				             mp.y <  g_dockTarget[1] + g_dockTarget[3];
			}
			g_hostDragOverMain = overMain;
			glfwSetWindowOpacity(h->win, overTarget ? 0.45f : 0.85f);   // "will dock" hint
			const bool down = mainCtx->IO.MouseDown[0] || h->ctx->IO.MouseDown[0];
			if (!down)
			{
				h->dragging = false;
				g_hostDragOverMain = false;
				glfwSetWindowOpacity(h->win, 1.0f);
				if (overTarget) { h->dockDrop = true; h->dropX = (float)mp.x; h->dropY = (float)mp.y; }
			}
		}
#else
		// Portable drag-to-dock follow. Cursor and button come from ABSOLUTE OS state
		// (HostCursorGlobal/HostMouseLeftDown) — the gesture must depend on neither event
		// delivery nor the window's own position, or it feeds back into itself.
		if (h->dragging)
		{
			// The physical button is THE gesture: released -> the drag is over, this frame.
			if (!HostMouseLeftDown(g_mainGlfw, h->win))
			{
				int gx = 0, gy = 0;
				HostCursorGlobal(h->win, gx, gy);
				bool overMain = false, overTarget = false;
				float mx = 0, my = 0;
				if (g_mainGlfw && !glfwGetWindowAttrib(g_mainGlfw, GLFW_ICONIFIED))
				{
					int mwx = 0, mwy = 0, mww = 0, mwh = 0;
					glfwGetWindowPos(g_mainGlfw, &mwx, &mwy);
					glfwGetWindowSize(g_mainGlfw, &mww, &mwh);
					mx = (float)(gx - mwx); my = (float)(gy - mwy);
					overMain = mx >= 0 && my >= 0 && mx < (float)mww && my < (float)mwh;
					overTarget = overMain && g_dockTargetValid &&
					             mx >= g_dockTarget[0] && my >= g_dockTarget[1] &&
					             mx <  g_dockTarget[0] + g_dockTarget[2] &&
					             my <  g_dockTarget[1] + g_dockTarget[3];
				}
				h->dragging = false;
				g_hostDragOverMain = false;
				glfwSetWindowOpacity(h->win, 1.0f);
				glfwSetWindowAttrib(h->win, GLFW_FLOATING, GLFW_FALSE);   // drop the drag-time always-on-top
				if (overTarget) { h->dockDrop = true; h->dropX = mx; h->dropY = my; }
			}
			else
			{
				int gx = 0, gy = 0;
				HostCursorGlobal(h->win, gx, gy);
				glfwSetWindowPos(h->win, gx - (int)h->hotX, gy - (int)h->hotY);
				bool overMain = false, overTarget = false;
				if (g_mainGlfw && !glfwGetWindowAttrib(g_mainGlfw, GLFW_ICONIFIED))
				{
					int mwx = 0, mwy = 0, mww = 0, mwh = 0;
					glfwGetWindowPos(g_mainGlfw, &mwx, &mwy);
					glfwGetWindowSize(g_mainGlfw, &mww, &mwh);
					const float mx = (float)(gx - mwx), my = (float)(gy - mwy);
					overMain = mx >= 0 && my >= 0 && mx < (float)mww && my < (float)mwh;
					overTarget = overMain && g_dockTargetValid &&
					             mx >= g_dockTarget[0] && my >= g_dockTarget[1] &&
					             mx <  g_dockTarget[0] + g_dockTarget[2] &&
					             my <  g_dockTarget[1] + g_dockTarget[3];
				}
				g_hostDragOverMain = overMain;
				glfwSetWindowOpacity(h->win, overTarget ? 0.45f : 0.85f);   // "will dock" hint
			}
		}
#endif

		int fw = 0, fh = 0, ww = 0, wh = 0;
		glfwGetFramebufferSize(h->win, &fw, &fh);
		glfwGetWindowSize(h->win, &ww, &wh);
		if (fw < 8 || fh < 8) continue;   // minimized: sit the frame out
		if (ww < 1 || wh < 1) { ww = fw; wh = fh; }

		ImGui::SetCurrentContext(h->ctx);
		ImGuiIO& hio = ImGui::GetIO();
		// LOGICAL size for imgui — the cursor callbacks speak logical coordinates. On a scaled
		// native-Wayland monitor the framebuffer is logical×scale; feeding pixels here put
		// every hitbox at 1/scale of its visual position (title bar unclickable, everything
		// offset). BuildDrawData scales geometry/clips back up via DisplayFramebufferScale.
		hio.DisplaySize             = ImVec2((float)ww, (float)wh);
		hio.DisplayFramebufferScale = ImVec2((float)fw / (float)ww, (float)fh / (float)wh);
		hio.DeltaTime   = dt;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));   // pinned: imgui never moves it INSIDE the host
		ImGui::SetNextWindowSize(hio.DisplaySize);
		ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);   // the host never "rolls up"
		// This title bar is the whole chrome: drag = follow drag, X = OS close, border = frame.
		bool hostOpen = true;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		ImGui::Begin((h->title + "###hostcontent").c_str(), &hostOpen,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
		             h->contentFlags);
		NukeUI::WindowCaptionButtons(h);   // minimize / maximize next to imgui's close X
		if (h->content) h->content();
		ImGui::End();
		ImGui::PopStyleVar();
		if (!hostOpen) glfwSetWindowShouldClose(h->win, GLFW_TRUE);   // imgui X == OS close

		// Title bar grabbed: cancel imgui's window move and turn it into the OS-window follow drag.
		if (!h->dragging && h->rsEdge == 0)
		{
			ImGuiContext* hg = ImGui::GetCurrentContext();
			if (hg->MovingWindow && hg->MovingWindow->RootWindow &&
			    strstr(hg->MovingWindow->RootWindow->Name, "###hostcontent") &&
			    ImGui::IsMouseDragging(0, 4.0f))
			{
				const ImVec2 m = ImGui::GetMousePos();
				ImGui::ClearActiveID();
				hg->MovingWindow = nullptr;
				NukeUI::HostBeginDrag(h, m.x, m.y);
			}
		}
		// Borderless resize: the 6px edges belong to the frame (left/top edges also move the
		// window). Cursor math is portable (GLFW screen space); only the raw cursor read and
		// the capture-loss safety check are per-platform.
		if (!h->dragging)
		{
			static GLFWcursor* curEW   = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
			static GLFWcursor* curNS   = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
			static GLFWcursor* curMove = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);
			const float edge = 6.0f;
			if (h->rsEdge == 0)
			{
				int e = 0;
				const ImVec2 mp = hio.MousePos;
				if (mp.x >= 0 && mp.y >= 0 && mp.x < hio.DisplaySize.x && mp.y < hio.DisplaySize.y)
				{
					if (mp.x < edge) e |= 1; else if (mp.x >= hio.DisplaySize.x - edge) e |= 2;
					if (mp.y < edge) e |= 4; else if (mp.y >= hio.DisplaySize.y - edge) e |= 8;
				}
				if (e)
				{
					glfwSetCursor(h->win, ((e & 3) && (e & 12)) ? curMove : (e & 3) ? curEW : curNS);
					if (ImGui::IsMouseClicked(0))
					{
						h->rsEdge = e;
						glfwGetWindowSize(h->win, &h->rsStartW, &h->rsStartH);
						glfwGetWindowPos(h->win, &h->rsStartX, &h->rsStartY);
#ifdef _WIN32
						GetCursorPos(&h->rsCur);
#else
						int gx = 0, gy = 0;
						HostCursorGlobal(h->win, gx, gy);
						h->rsCur.x = gx; h->rsCur.y = gy;
#endif
						ImGui::ClearActiveID();   // the frame owns this gesture, not a widget
					}
				}
				else glfwSetCursor(h->win, nullptr);
			}
			else
			{
#ifdef _WIN32
				POINT cp; GetCursorPos(&cp);
				const int dx = cp.x - (int)h->rsCur.x, dy = cp.y - (int)h->rsCur.y;
#else
				int cgx = 0, cgy = 0;
				HostCursorGlobal(h->win, cgx, cgy);   // the resize button was pressed on the host
				const int dx = cgx - (int)h->rsCur.x, dy = cgy - (int)h->rsCur.y;
#endif
				int nw = h->rsStartW, nh = h->rsStartH, nx = h->rsStartX, ny = h->rsStartY;
				if (h->rsEdge & 2) nw += dx;
				if (h->rsEdge & 1) { nw -= dx; nx += dx; }
				if (h->rsEdge & 8) nh += dy;
				if (h->rsEdge & 4) { nh -= dy; ny += dy; }
				if (nw < 220) { if (h->rsEdge & 1) nx -= 220 - nw; nw = 220; }
				if (nh < 140) { if (h->rsEdge & 4) ny -= 140 - nh; nh = 140; }
				glfwSetWindowPos(h->win, nx, ny);
				glfwSetWindowSize(h->win, nw, nh);
#ifdef _WIN32
				const bool rsHeld = hio.MouseDown[0] || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
#else
				const bool rsHeld = HostMouseLeftDown(h->win, nullptr);   // absolute on macOS
#endif
				if (!rsHeld)
				{
					h->rsEdge = 0;
					glfwSetCursor(h->win, nullptr);
				}
			}
		}
		ImGui::Render();

		ImDrawData* dd = ImGui::GetDrawData();
		if (dd)
		{
			UpdateTextures(dd);
			NukeUIDrawData nd{};
			BuildDrawData(dd, hostLists, hostCmds, nd);
#ifdef _WIN32
			g_render->uiViewportRender((void*)glfwGetWin32Window(h->win), fw, fh, nd);
#elif !defined(__APPLE__)
			g_render->uiViewportRender(NukeUIX11WindowHandle(h->win), fw, fh, nd);
#endif
		}
	}
	ImGui::SetCurrentContext(mainCtx);
}

// Detachable document windows: one DocWindow/DocPanel call per frame drives the whole
// docked/detached lifecycle for any panel or module editor.
struct NukeUIDoc
{
	std::function<void()> draw;      // refreshed every DocWindow call
	std::string title;
	void* host = nullptr;
	int   flags = 0, w = 720, h = 520;
	bool  detached = false;
	bool  dragOut = false;           // torn off mid-drag: the new host picks the drag up
	bool  wantDock = false;          // host dropped onto the main window
	bool  hasDrop = false; float dropX = 0, dropY = 0;
	bool  wantFocus = false;
	bool  closeReported = false;     // OS close reported once; re-assert open = cancel it
	bool  panel = false;             // persistent panel: never auto-detached
	unsigned lastSeen = 0;           // frame stamp for garbage collection
};
static std::map<std::string, NukeUIDoc> g_uiDocs;
static bool     g_docDetachDefault = false;
static unsigned g_uiFrameNo = 0;

static void DocFrameBegin() { ++g_uiFrameNo; }

void NukeUI::DocDetachDefault(bool detached) { g_docDetachDefault = detached; }

bool NukeUI::DocDetached(const char* id)
{
	auto it = g_uiDocs.find(id);
	return it != g_uiDocs.end() && it->second.host != nullptr;
}

void NukeUI::DocFocus(const char* id)
{
	auto it = g_uiDocs.find(id);
	if (it != g_uiDocs.end()) it->second.wantFocus = true;
}

void NukeUI::DocDetachAll(bool detached)
{
	for (auto& kv : g_uiDocs)
	{
		if (kv.second.panel) continue;   // panels move only by the user's own drag
		if (detached) { if (!kv.second.host) kv.second.detached = true; }
		else if (kv.second.host) kv.second.wantDock = true;
	}
}

static void DocWindowImpl(const char* id, const char* title, bool* p_open,
                          int imguiWindowFlags, int width, int height,
                          const std::function<void()>& draw, bool isPanel);

void NukeUI::DocWindow(const char* id, const char* title, bool* p_open,
                       int imguiWindowFlags, int width, int height,
                       const std::function<void()>& draw)
{
	DocWindowImpl(id, title, p_open, imguiWindowFlags, width, height, draw, false);
}

void NukeUI::DocPanel(const char* id, const char* title, bool* p_open,
                      int imguiWindowFlags, int width, int height,
                      const std::function<void()>& draw)
{
	DocWindowImpl(id, title, p_open, imguiWindowFlags, width, height, draw, true);
}

static void DocWindowImpl(const char* id, const char* title, bool* p_open,
                          int imguiWindowFlags, int width, int height,
                          const std::function<void()>& draw, bool isPanel)
{
	if (!id || !p_open || !*p_open) return;
	auto ins = g_uiDocs.emplace(id, NukeUIDoc{});
	NukeUIDoc& d = ins.first->second;
	if (ins.second)
	{
		d.panel = isPanel;
		d.detached = isPanel ? false : g_docDetachDefault;   // panels ALWAYS start docked
	}
	d.draw = draw; d.title = title ? title : id;
	d.flags = imguiWindowFlags; d.w = width; d.h = height;
	d.lastSeen = g_uiFrameNo;

	if (g_nativeViewports)
	{
		// Native viewports: imgui owns the whole lifecycle — hosts and gestures stay dormant.
		if (d.wantDock)  { d.detached = false; d.wantDock = false; }
		if (d.wantFocus) { ImGui::SetNextWindowFocus(); d.wantFocus = false; }
		// First appearance: dead CENTER of the main window. imgui's default placement can
		// land a fresh window at the far edge of the viewport — sometimes practically
		// off-screen. imgui.ini-restored positions still win (FirstUseEver).
		{
			ImGuiViewport* mv = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(mv->Pos.x + mv->Size.x * 0.5f, mv->Pos.y + mv->Size.y * 0.5f),
			                        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2((float)d.w, (float)d.h), ImGuiCond_FirstUseEver);
		// NoCollapse: the title double-click maximizes (WindowCaptionButtons) instead of
		// rolling the window up — imgui consumes that double-click inside Begin otherwise.
		if (ImGui::Begin(d.title.c_str(), p_open, d.flags | ImGuiWindowFlags_NoCollapse))
		{
			NukeUI::WindowCaptionButtons();   // minimize/maximize once imgui gives it an OS window
			if (d.draw) d.draw();
		}
		ImGui::End();
		return;
	}

	// Dropped back / preference turned off: leave the host (never from inside its tick).
	if (d.wantDock)
	{
		if (d.host) { NukeUI::HostDestroy(d.host); d.host = nullptr; }
		d.detached = false; d.wantDock = false; d.wantFocus = true;
	}

	if (d.detached)
	{
		if (!d.host)
		{
			std::string osTitle = d.title;   // OS title without the imgui "###id" suffix
			const size_t hash = osTitle.find("##");
			if (hash != std::string::npos) osTitle.resize(hash);
			d.host = NukeUI::HostCreate(osTitle.c_str(), d.w, d.h);
			if (d.host) NukeUI::HostSetTitle(d.host, d.title.c_str());   // tab keeps the FULL label
			if (!d.host) { d.detached = false; }   // window creation failed: fall back to docked
			else
			{
				const std::string key = id;        // capture the KEY — the map can rehash
				NukeUI::HostSetContent(d.host, [key]()
				{
					auto it = g_uiDocs.find(key);
					if (it != g_uiDocs.end() && it->second.draw) it->second.draw();
				});
			}
		}
		if (d.host)
		{
			if (d.dragOut) { NukeUI::HostBeginDrag(d.host, d.w * 0.5f, 12.0f); d.dragOut = false; }
			NukeUI::HostSetTitle(d.host, d.title.c_str());
			NukeUI::HostSetContentFlags(d.host, d.flags);
			float dx = 0, dy = 0;
			if (NukeUI::HostDockDrop(d.host, &dx, &dy)) { d.wantDock = true; d.hasDrop = true; d.dropX = dx; d.dropY = dy; }
			if (d.wantFocus) { NukeUI::HostFocus(d.host); d.wantFocus = false; }
			if (!NukeUI::HostAlive(d.host))
			{
				if (d.closeReported && *p_open) { NukeUI::HostCancelClose(d.host); d.closeReported = false; }
				else { *p_open = false; d.closeReported = true; }
			}
			return;                                // content is drawn by the host tick
		}
	}

	// ---- docked: a normal imgui window in the CURRENT (main) context ----
	if (d.wantFocus) { ImGui::SetNextWindowFocus(); d.wantFocus = false; }
	if (d.hasDrop)
	{
		// Re-docked by drag: appear where the user dropped it (title bar under the cursor).
		d.hasDrop = false;
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos(ImVec2(ImMax(0.0f, d.dropX - 220.0f), ImMax(0.0f, d.dropY - 10.0f)), ImGuiCond_Always);
	}
	ImGui::SetNextWindowSize(ImVec2((float)d.w, (float)d.h), ImGuiCond_FirstUseEver);
	// The title IS the imgui identity: panels pass a stable name so imgui.ini layouts keep
	// working; documents with changing titles embed their own "###<id>" suffix.
	const std::string label = d.title;
	ImGuiWindow* self = nullptr;
	if (ImGui::Begin(label.c_str(), p_open, d.flags))
	{
		self = ImGui::GetCurrentWindow();
		if (d.draw) d.draw();
	}
	else self = ImGui::GetCurrentWindow();
	ImGui::End();

	// Tear-off: title bar dragged past the main-window edge, or cursor pinned to the screen edge.
	ImGuiContext* g = ImGui::GetCurrentContext();
	if (self && g->MovingWindow && g->MovingWindow->RootWindow == self->RootWindow)
	{
		const ImVec2 m  = ImGui::GetMousePos();
		const ImVec2 ds = ImGui::GetIO().DisplaySize;
		const float out = 12.0f;
		const bool leftWin = m.x < -out || m.y < -out || m.x >= ds.x + out || m.y >= ds.y + out;
		bool clamped = false;
#ifdef _WIN32
		{
			POINT cp; GetCursorPos(&cp);
			const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
			const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
			clamped = cp.x <= vx || cp.y <= vy || cp.x >= vx + vw - 1 || cp.y >= vy + vh - 1;
		}
#endif
		if (leftWin || clamped)
		{
			d.detached = true; d.dragOut = true;
			ImGui::ClearActiveID();                // hand the drag over to the host window
			g->MovingWindow = nullptr;
		}
	}
}

// Documents whose owner stopped calling DocWindow this frame are gone — free their hosts.
static void CollectDocs()
{
	for (auto it = g_uiDocs.begin(); it != g_uiDocs.end(); )
	{
		if (it->second.lastSeen != g_uiFrameNo)
		{
			if (it->second.host) NukeUI::HostDestroy(it->second.host);
			it = g_uiDocs.erase(it);
		}
		else ++it;
	}
}

void NukeUI::Init(iRender* renderer)
{
	g_render = renderer;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // imgui 1.92 dynamic textures
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;        // sticky / dockable panels
	io.ConfigWindowsMoveFromTitleBarOnly = true;   // content-area drags belong to the content
	io.IniFilename = "imgui.ini"; // restore saved window + docking layout
	if (renderer->width > 0 && renderer->height > 0)
	{
		s_dispW = renderer->width;
		s_dispH = renderer->height;
	}
	io.DisplaySize = ImVec2((float)s_dispW, (float)s_dispH);

	// Vulkan uses native imgui multi-viewport; D3D uses NukeUI hosts instead (DXGI per-window races).
	// Either way imgui_impl_glfw mounts on the renderer's window: InitForOther(true) CHAINS its callbacks.
	if (GLFWwindow* mainWin = (GLFWwindow*)renderer->nativeWindow())
	{
		g_mainGlfw = mainWin;   // portable host math (drag-to-dock in GLFW screen space)
#if !defined(_WIN32) && !defined(__APPLE__)
		// Native Wayland has no global coordinates and no client-side window positioning —
		// imgui multi-viewport would place windows blind and fight the compositor. Panels
		// stay docked in the main window; NUKE_DISPLAY_BACKEND=x11 restores tear-off.
		if (g_nativeViewports && NukeUINativeIsWayland())
		{
			printf("[NukeUI]\t\tnative Wayland: imgui multi-viewport OFF (no window positioning) — "
			       "run with NUKE_DISPLAY_BACKEND=x11 for detachable panels\n");
			g_nativeViewports = false;
		}
#endif
		if (g_nativeViewports)
		{
			io.ConfigFlags  |= ImGuiConfigFlags_ViewportsEnable;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;   // rendered via the seam
			io.ConfigViewportsNoDecoration = true;       // chrome = imgui title bar, so every drag is imgui's
			io.ConfigDockingTransparentPayload = true;   // dock anchors stay visible under the dragged window
			io.ConfigViewportsNoAutoMerge = true;        // floating = always its own OS window; re-dock via anchors
		}
		ImGui_ImplGlfw_InitForOther(mainWin, true);
		g_glfwPlatform = true;
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		// The renderer owns one swap chain per secondary window — drop it with the window.
		pio.Renderer_DestroyWindow = [](ImGuiViewport* v)
		{
			void* nh = ViewportNativeHandle(v);
			if (g_render && nh)
				g_render->uiViewportDestroy(nh);
		};
#ifdef _WIN32
		// Secondary windows inherit the main window's icons (title bar + taskbar).
		g_mainHwnd = (void*)glfwGetWin32Window(mainWin);
#elif !defined(__APPLE__)
		g_mainGlfwIcon = mainWin;   // X11: _NET_WM_ICON source for secondary windows
#endif
		// With multi-viewport off Platform_CreateWindow is null — never wrap a null original.
		g_origCreateWindow = pio.Platform_CreateWindow;
		if (g_origCreateWindow) pio.Platform_CreateWindow = CreateWindowWithIcon;
#ifdef __APPLE__
		// SELF-VERIFYING window placement: on macOS the GLFW 3.5 set/get pair disagreed on Y
		// for borderless viewport windows (set lands flipped; imgui pos 637 -> OS pos 38,
		// same X). imgui then walks the window a step per sync. Wrap the setter: place,
		// read back, and if the OS answered off-target, solve actual = a*set + b per axis
		// from two samples and hit exactly on the third call. An honest GLFW short-circuits
		// at the first check. macOS-ONLY: on X11/Wayland these extra set/get round-trips are
		// pure noise (and Wayland can't position windows at all).
		g_origSetWindowPos = pio.Platform_SetWindowPos;
		if (g_origSetWindowPos) pio.Platform_SetWindowPos = SetWindowPosVerified;
		g_origShowWindow = pio.Platform_ShowWindow;
		if (g_origShowWindow) pio.Platform_ShowWindow = ShowWindowVerified;
		g_origSetWindowSize = pio.Platform_SetWindowSize;
		if (g_origSetWindowSize) pio.Platform_SetWindowSize = SetWindowSizePosLocked;
#endif
	}
	else
	{
		// No GLFW to mount on: single-window UI fed through the renderer's neutral callbacks.
		renderer->_UImove = [](int x, int y) {
			ImGui::GetIO().AddMousePosEvent((float)x, (float)y);
		};
		renderer->_UImouse = [](int button, int state, int x, int y) {
			ImGuiIO& gio = ImGui::GetIO();
			gio.AddMousePosEvent((float)x, (float)y);
			if (button >= 0 && button < 5)
				gio.AddMouseButtonEvent(button, state != 0);
		};
		renderer->_UImouseWheel = [](int /*button*/, int dir, int /*x*/, int /*y*/) {
			ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)dir);
		};
		renderer->_UIchar = [](unsigned int c) { ImGui::GetIO().AddInputCharacter(c); };
		renderer->_UIkey = [](int key, int action, int mods) {
			ImGuiIO& gio = ImGui::GetIO();
			gio.AddKeyEvent(ImGuiMod_Ctrl,  (mods & 0x0002) != 0); // GLFW_MOD_CONTROL
			gio.AddKeyEvent(ImGuiMod_Shift, (mods & 0x0001) != 0); // GLFW_MOD_SHIFT
			gio.AddKeyEvent(ImGuiMod_Alt,   (mods & 0x0004) != 0); // GLFW_MOD_ALT
			ImGuiKey k = GlfwToImGuiKey(key);
			if (k != ImGuiKey_None)
				gio.AddKeyEvent(k, action != 0); // GLFW_RELEASE == 0
		};
	}

	// Drive the UI from the renderer's per-frame GUI hook.
	renderer->setOnGUI([]() { NukeUI::Frame(); });
}

void NukeUI::Shutdown()
{
	if (ImGui::GetCurrentContext() != nullptr)
	{
		if (g_glfwPlatform)
		{
			ImGui::DestroyPlatformWindows();   // closes secondary OS windows (seam destroy per window)
			ImGui_ImplGlfw_Shutdown();
			g_glfwPlatform = false;
		}
		ImGui::DestroyContext();
	}
	g_render = nullptr;
	g_callbacks.clear();
}
