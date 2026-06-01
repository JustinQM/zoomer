#include <stdlib.h>

#include "capture.h"
#include "capture_wlr.h"

struct Capture
{
    const CaptureImpl* impl; // NULL until capture_init picks a backend
    void* backend;           // backend private data

    // globals collected during registry enumeration, consumed by capture_init
    CaptureWlrGlobals wlr;
};

Capture* capture_create(void)
{
    Capture* cap = calloc(1, sizeof(*cap));
    if (!cap) die("failed to allocate capture interface");
    return cap;
}

void capture_handle_global(Capture* cap, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!cap) return;
    capture_wlr_handle_global(&cap->wlr, registry, name, interface, version);
    // future: capture_kwin needs no wayland globals here; it goes through the
    // screencast portal + PipeWire, so there is nothing to forward to it.
}

void capture_init(Capture* cap, struct wl_display* display, struct wl_shm* shm, const OutputInfo* outputs, uint32_t output_count)
{
    if (capture_wlr_available(&cap->wlr))
    {
        cap->backend = capture_wlr_create(&cap->wlr, display, shm, outputs, output_count);
        cap->impl = &capture_wlr_impl;
        return;
    }

    // future: probe for org.freedesktop.portal.ScreenCast here and select the
    // KWin/PipeWire backend instead of dying.
    die("no supported screen-capture backend: this compositor does not implement\n"
        "ext-image-copy-capture (wlroots), and the KWin/PipeWire backend is not yet implemented");
}

void capture_grab(Capture* cap)
{
    cap->impl->grab(cap->backend);
}

const void* capture_output_pixels(Capture* cap, uint32_t index, int32_t* stride)
{
    return cap->impl->output_pixels(cap->backend, index, stride);
}

void capture_destroy(Capture* cap)
{
    if (!cap) return;
    if (cap->impl && cap->backend) cap->impl->destroy(cap->backend);
    free(cap);
}
