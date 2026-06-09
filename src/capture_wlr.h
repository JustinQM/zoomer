#ifndef CAPTURE_WLR_H
#define CAPTURE_WLR_H

#include <stdbool.h>
#include <stdint.h>

#include "capture.h" 
#include "ext-image-copy-capture.h"
#include "ext-image-capture-source.h"
#include "wlr-screencopy-unstable-v1.h"

typedef struct
{
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct zwlr_screencopy_manager_v1* screencopy_manager;
    struct ext_image_copy_capture_manager_v1* copy_manager;
} CaptureWlrGlobals;

void capture_wlr_handle_global(CaptureWlrGlobals* g, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
bool capture_wlr_available(const CaptureWlrGlobals* g);

void* capture_wlr_create(const CaptureWlrGlobals* g, struct wl_display* display, struct wl_shm* shm, const OutputInfo* outputs, uint32_t output_count);

extern const CaptureImpl capture_wlr_impl;

#endif
