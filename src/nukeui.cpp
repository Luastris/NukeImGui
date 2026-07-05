#include "nukeui.h"
#include "imgui.h"
#include "IconsLucide.h"      // ICON_LC_* + ICON_MIN_LC / ICON_MAX_LC range
#include "backends/imgui_impl_glfw.h"   // multi-viewport PLATFORM backend (native OS windows)
#include <render/irender.h>   // engine: iRender + NukeUIDrawData (neutral seam)
#include <vector>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>          // WM_SETICON for the secondary OS windows
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h> // glfwGetWin32Window (main window HWND -> icon source)
#endif

struct GLFWwindow;

using namespace nuke;   // iRender / NukeUIDrawData live in namespace nuke

static iRender*                            g_render = nullptr;
static std::vector<std::function<void()>>  g_callbacks;
static bool                                g_glfwPlatform = false;   // imgui_impl_glfw mounted (viewports on)
static void*                               g_mainHwnd = nullptr;     // main window (icon source for secondaries)
static void (*g_origCreateWindow)(ImGuiViewport*) = nullptr;

// Wrap the platform backend's window creation: a fresh secondary OS window inherits the
// MAIN window's icons (title bar + taskbar), instead of the blank system default.
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
				uint64_t h = g_render->createTexture2D(tex->GetPixels(), tex->Width, tex->Height);
				tex->SetTexID((ImTextureID)h);
				tex->SetStatus(ImTextureStatus_OK);
				break;
			}
			case ImTextureStatus_WantUpdates:
			{
				// The neutral seam has no partial update: recreate the whole texture.
				if (tex->GetTexID() != ImTextureID_Invalid)
					g_render->destroyTexture2D((uint64_t)tex->GetTexID());
				uint64_t h = g_render->createTexture2D(tex->GetPixels(), tex->Width, tex->Height);
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
			NukeUICmd nc{};
			nc.clipRect[0] = (dc.ClipRect.x - pos.x) * scale.x;
			nc.clipRect[1] = (dc.ClipRect.y - pos.y) * scale.y;
			nc.clipRect[2] = (dc.ClipRect.z - pos.x) * scale.x;
			nc.clipRect[3] = (dc.ClipRect.w - pos.y) * scale.y;
			nc.texId     = (uint64_t)dc.GetTexID();
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

void NukeUI::MergeIconFont(const char* ttfPath, float sizePx, float glyphOffsetY)
{
	ImGuiIO& io = ImGui::GetIO();
	static const ImWchar ranges[] = { ICON_MIN_LC, ICON_MAX_LC, 0 }; // must outlive the atlas build
	ImFontConfig cfg;
	cfg.MergeMode        = true;        // merge onto the previously-added main font
	cfg.PixelSnapH       = true;
	cfg.GlyphMinAdvanceX = sizePx;      // make icons monospaced so toolbar buttons align
	cfg.GlyphOffset.y    = glyphOffsetY; // nudge icons down to vertically centre them in the line
	io.Fonts->AddFontFromFileTTF(ttfPath, sizePx, &cfg, ranges);
}

void NukeUI::AddDrawCallback(const std::function<void()>& cb)
{
	g_callbacks.push_back(cb);
}

void NukeUI::Frame()
{
	if (!g_render)
		return;

	ImGuiIO& io = ImGui::GetIO();
	if (g_glfwPlatform)
	{
		// The GLFW platform backend owns DisplaySize / DeltaTime / mouse state (and the
		// secondary OS windows' input) — one source of truth for the whole viewport set.
		ImGui_ImplGlfw_NewFrame();
	}
	else
	{
		// Follow the renderer's current framebuffer size so the UI scales with the window.
		int w = (g_render->width  > 0) ? g_render->width  : s_dispW;
		int h = (g_render->height > 0) ? g_render->height : s_dispH;
		io.DisplaySize = ImVec2((float)w, (float)h);

		// Real per-frame delta — ImGui needs this every frame or its time-based logic
		// (key repeat, blink, tooltips, animations) misbehaves. A fixed 1/60 made the
		// internal clock outrun real time at high FPS, so backspace auto-repeated and
		// deleted several characters per press.
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

	// Secondary viewports: windows dragged out of (or forced off) the main window are
	// REAL OS windows. The GLFW backend creates/moves them; the renderer draws each
	// one through the seam into its own swap chain.
	if (g_glfwPlatform && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
	{
		ImGui::UpdatePlatformWindows();
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
			g_render->uiViewportRender(v->PlatformHandleRaw, vw, vh, vd);
		}
	}
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

void NukeUI::Init(iRender* renderer)
{
	g_render = renderer;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // imgui 1.92 dynamic textures
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;        // sticky / dockable panels
	// Windows move ONLY by their title bar (Unity/UE behavior). Content-area drags then
	// belong to the CONTENT — 3D previews orbit, gizmos grab — instead of dragging the window.
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	io.IniFilename = "imgui.ini"; // restore saved window + docking layout
	if (renderer->width > 0 && renderer->height > 0)
	{
		s_dispW = renderer->width;
		s_dispH = renderer->height;
	}
	io.DisplaySize = ImVec2((float)s_dispW, (float)s_dispH);

	// Multi-viewport: panels dragged out of (or forced off) the main window become
	// NATIVE OS windows. The platform side is imgui_impl_glfw mounted on the renderer's
	// own GLFW window — both modules link the SAME glfw3.dll, so the renderer's event
	// polling pumps the secondary windows too, and InitForOther(install_callbacks=true)
	// CHAINS the renderer's existing input callbacks instead of replacing them.
	if (GLFWwindow* mainWin = (GLFWwindow*)renderer->nativeWindow())
	{
		io.ConfigFlags  |= ImGuiConfigFlags_ViewportsEnable;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;   // we render via the seam
		ImGui_ImplGlfw_InitForOther(mainWin, true);
		g_glfwPlatform = true;
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		// The renderer owns one swap chain per secondary window — drop it with the window.
		pio.Renderer_DestroyWindow = [](ImGuiViewport* v)
		{
			if (g_render && v->PlatformHandleRaw)
				g_render->uiViewportDestroy(v->PlatformHandleRaw);
		};
#ifdef _WIN32
		// Secondary windows inherit the main window's icons (title bar + taskbar).
		g_mainHwnd = (void*)glfwGetWin32Window(mainWin);
		g_origCreateWindow = pio.Platform_CreateWindow;
		pio.Platform_CreateWindow = CreateWindowWithIcon;
#endif
	}
	else
	{
		// No windowing layer to mount on (headless / non-GLFW backend): single-window UI,
		// input fed through the renderer's neutral callbacks as before.
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
