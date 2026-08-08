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
#include <cstdint>
#include <dlfcn.h>

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

#endif // !_WIN32 && !__APPLE__
