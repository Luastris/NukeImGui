// macOS absolutes for the host-window gestures (drag-to-dock, edge resize). GLFW's
// per-window cursor is EVENT-DRIVEN on macOS — between deliveries it is stale, and
// window-pos + stale-client-cursor feeds back into itself (the window then drifts on its
// own). These two query the OS directly, independent of any event delivery.
#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>

// Global cursor in GLFW screen space (top-left origin of the primary display, points) —
// the same space glfwSetWindowPos/glfwGetWindowPos speak (GLFW flips Cocoa's bottom-left
// origin against the primary display height; mirror that exactly).
extern "C" void NukeUICocoaGlobalCursor(int* x, int* y)
{
    @autoreleasepool
    {
        const NSPoint p = [NSEvent mouseLocation];
        const CGFloat h = CGDisplayBounds(CGMainDisplayID()).size.height;
        *x = (int)p.x;
        *y = (int)(h - p.y);
    }
}

// Physical left-button state, straight from the OS (bit 0 of pressedMouseButtons).
extern "C" bool NukeUICocoaMouseLeftDown(void)
{
    return ([NSEvent pressedMouseButtons] & 1) != 0;
}

#endif // __APPLE__
