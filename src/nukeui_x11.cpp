// Linux native-handle shim (the sibling of nukeui_cocoa.mm): Xlib.h's macro pollution
// (#define Status int, None, Bool, ...) must not leak into nukeui.cpp — this TU is the only
// place the native GLFW header is included. Serves BOTH backends: X11 (Window id, icon
// property mirroring) and native Wayland (wl_surface*). The Wayland entry points resolve
// via dlsym so linking never depends on which backends this GLFW build carries.
#if !defined(_WIN32) && !defined(__APPLE__)

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>   // XA_CARDINAL (_NET_WM_ICON payload type)
#include <X11/extensions/Xfixes.h>   // CLIPBOARD owner-change events (clipboard change counter)
#include <cstdint>
#include <dlfcn.h>
#include <time.h>

// Runtime platform check — GLFW 3.4+ always exports these regardless of built backends.
extern "C" bool NukeUINativeIsWayland(void)
{
#ifdef GLFW_PLATFORM_WAYLAND
	return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else
	return false;
#endif
}

// The renderer keys per-window swap chains on a NATIVE handle: the X11 Window id on X11,
// the wl_surface* on Wayland — both travel in a void*.
extern "C" void* NukeUIX11WindowHandle(GLFWwindow* w)
{
	if (!w) return nullptr;
	if (NukeUINativeIsWayland())
	{
		static void* (*getWlWindow)(GLFWwindow*) =
			(void* (*)(GLFWwindow*))dlsym(RTLD_DEFAULT, "glfwGetWaylandWindow");
		return getWlWindow ? getWlWindow(w) : nullptr;
	}
	return (void*)(uintptr_t)glfwGetX11Window(w);
}

// Mirror the main window's _NET_WM_ICON onto a secondary window — the X11 counterpart of
// the WM_SETICON copy on Windows. The renderer sets the source icon via glfwSetWindowIcon;
// this keeps every viewport/host window on the same taskbar identity without NukeImGui
// having to decode image files itself. Wayland: no-op (icons come from the .desktop entry).
extern "C" void NukeUIX11CopyIcon(GLFWwindow* src, GLFWwindow* dst)
{
	if (NukeUINativeIsWayland()) return;
	Display* dpy = glfwGetX11Display();
	if (!dpy || !src || !dst) return;
	const Window s = glfwGetX11Window(src), d = glfwGetX11Window(dst);
	if (!s || !d) return;
	const Atom icon = XInternAtom(dpy, "_NET_WM_ICON", False);
	Atom type = None; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
	if (XGetWindowProperty(dpy, s, icon, 0, 1 << 22, False, XA_CARDINAL,
	                       &type, &fmt, &n, &after, &data) == Success && data)
	{
		if (n) XChangeProperty(dpy, d, icon, XA_CARDINAL, 32, PropModeReplace, data, (int)n);
		XFree(data);
	}
}

// ---- clipboard change tracking (NukeUI::ClipboardText) ----------------------------------------
// A private display connection subscribed through XFixes to CLIPBOARD owner changes, pumped on
// every query: a change counter that never touches GLFW's event loop. Wayland / no XFixes:
// version 0 = "no counter", the caller polls by time.
struct NukeUIX11Clip
{
	Display* dpy = nullptr; Window win = 0;
	Atom clipboard = 0, targets = 0, prop = 0, utf8 = 0, text = 0, plain = 0;
	int evBase = 0; unsigned version = 1; bool tried = false;
};
static NukeUIX11Clip g_clip;
static bool NukeUIX11ClipInit()
{
	if (g_clip.tried) return g_clip.dpy != nullptr;
	g_clip.tried = true;
	if (NukeUINativeIsWayland()) return false;
	Display* d = XOpenDisplay(nullptr);
	if (!d) return false;
	int errBase = 0;
	if (!XFixesQueryExtension(d, &g_clip.evBase, &errBase)) { XCloseDisplay(d); return false; }
	g_clip.dpy = d;
	g_clip.win = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	g_clip.clipboard = XInternAtom(d, "CLIPBOARD", False);
	g_clip.targets   = XInternAtom(d, "TARGETS", False);
	g_clip.prop      = XInternAtom(d, "NUKEUI_CLIP", False);
	g_clip.utf8      = XInternAtom(d, "UTF8_STRING", False);
	g_clip.text      = XInternAtom(d, "TEXT", False);
	g_clip.plain     = XInternAtom(d, "text/plain", False);
	XFixesSelectSelectionInput(d, g_clip.win, g_clip.clipboard,
	                           XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask);
	XFlush(d);
	return true;
}
static void NukeUIX11ClipPump(XEvent* out, bool wantSelection)
{
	XEvent ev;
	while (XPending(g_clip.dpy))
	{
		XNextEvent(g_clip.dpy, &ev);
		if (ev.type == g_clip.evBase + XFixesSelectionNotify) ++g_clip.version;
		else if (wantSelection && out && ev.type == SelectionNotify && ev.xselection.selection == g_clip.clipboard) { *out = ev; out = nullptr; }
	}
}
extern "C" unsigned NukeUIX11ClipboardVersion(void)
{
	if (!NukeUIX11ClipInit()) return 0;
	NukeUIX11ClipPump(nullptr, false);
	return g_clip.version;
}
// Is text offered? Our own window: yes (GLFW stores strings only, and answering a TARGETS
// request needs the event loop this very thread runs). Another owner: ask it for TARGETS, wait
// up to ~50 ms; text when UTF8_STRING / STRING / TEXT / text/plain is among them.
extern "C" bool NukeUIX11ClipboardHasText(GLFWwindow* main)
{
	if (!NukeUIX11ClipInit()) return true;   // cannot tell: let the read decide
	Display* d = g_clip.dpy;
	const Window owner = XGetSelectionOwner(d, g_clip.clipboard);
	if (!owner) return false;
	if (main && owner == glfwGetX11Window(main)) return true;
	XDeleteProperty(d, g_clip.win, g_clip.prop);
	XConvertSelection(d, g_clip.clipboard, g_clip.targets, g_clip.prop, g_clip.win, CurrentTime);
	XFlush(d);
	for (int i = 0; i < 50; ++i)
	{
		XEvent sel; sel.type = 0;
		NukeUIX11ClipPump(&sel, true);
		if (sel.type == SelectionNotify)
		{
			if (sel.xselection.property == None) return false;
			Atom type = None; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
			bool text = false;
			if (XGetWindowProperty(d, g_clip.win, g_clip.prop, 0, 1024, True, XA_ATOM, &type, &fmt, &n, &after, &data) == Success && data)
			{
				const Atom* atoms = (const Atom*)data;
				for (unsigned long k = 0; k < n; ++k)
					if (atoms[k] == g_clip.utf8 || atoms[k] == XA_STRING || atoms[k] == g_clip.text || atoms[k] == g_clip.plain) { text = true; break; }
				XFree(data);
			}
			return text;
		}
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, nullptr);
	}
	return false;   // the owner did not answer: not readable as text
}

#endif // !_WIN32 && !__APPLE__
