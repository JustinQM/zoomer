#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <systemd/sd-bus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include "capture_kwin.h"
#include "core_wayland.h"

#define PORTAL_BUS  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.ScreenCast"

static void kwin_grab(void* backend)
{
    assert(false && "kwin_grab");
}

static const void* kwin_output_pixels(void* backend, uint32_t index)
{
    assert(false && "kwin_output_pixels");
}

static void kwin_destroy(void* backend)
{
    assert(false && "kwin_destroy");
}

const CaptureImpl capture_kwin_impl =
{
    .grab = kwin_grab,
    .output_pixels = kwin_output_pixels,
    .destroy = kwin_destroy,
};

bool capture_kwin_available(void)
{
    assert(false && "kwin_capture_kwin_available");
}

void* capture_kwin_create(const OutputInfo* outputs, uint32_t output_count)
{
    assert(false && "kwin_capture_kwin_create");
}
