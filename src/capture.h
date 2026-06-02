#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

#include "core_wayland.h" // OutputInfo, die

typedef struct Capture Capture;

// Per-backend vtable. `backend` is the backend's private data.
typedef struct
{
    void        (*grab)(void* backend);
    const void* (*output_pixels)(void* backend, uint32_t index);
    void        (*destroy)(void* backend);
} CaptureImpl;

// Allocate the capture interface. Call this before binding globals so that
// capture_handle_global can collect whatever each candidate backend needs.
Capture* capture_create(void);

// Forward a registry global so the capture backends can bind their interfaces.
// Call from the wl_registry global handler.
void capture_handle_global(Capture* cap, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);

// Pick a backend based on what was advertised and prepare it. Call after the
// registry roundtrip and after outputs are enumerated. Dies if nothing supported.
void capture_init(Capture* cap, struct wl_display* display, struct wl_shm* shm, const OutputInfo* outputs, uint32_t output_count);

// Capture one frame from every output. Blocks until all outputs are captured.
// wlroots round-trips the image-copy-capture requests; a PipeWire backend would
// block until the first frame has arrived on every stream. Dies on failure.
void capture_grab(Capture* cap);

// Pixels for output `index` (parallel to the array passed to capture_init), or
// NULL if that output was not captured. The buffer is tightly packed, top-left
// origin, XRGB8888 little-endian; the row stride is outputs[index].width * 4.
// Ready for a GL_BGRA upload.
const void* capture_output_pixels(Capture* cap, uint32_t index);

void capture_destroy(Capture* cap);

#endif
