#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>

#include <wayland-client-protocol.h>

#include "capture_wlr.h"

typedef struct CaptureWlr CaptureWlr;

typedef struct
{
    const OutputInfo* info;
    CaptureWlr* w;

    void* shm_data;
    size_t shm_size;
    struct wl_shm_pool* pool;
    struct wl_buffer* buffer;

    struct ext_image_capture_source_v1* source;

    struct ext_image_copy_capture_session_v1* session;
    bool session_done;

    struct ext_image_copy_capture_frame_v1* frame;
    bool frame_done;

    struct zwlr_screencopy_frame_v1* zwlr_frame;
} WlrOutput;

struct CaptureWlr
{
    struct wl_display* display;
    struct wl_shm* shm;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* copy_manager;
    struct zwlr_screencopy_manager_v1* screencopy_manager;

    const OutputInfo* outputs;
    uint32_t output_count;

    WlrOutput wlr_outputs[MAX_OUTPUTS];
};

static int allocate_shm(size_t size)
{
    int fd = memfd_create("zoomer-shm", MFD_CLOEXEC);
    if (fd < 0) die("memfd_create failed: %s", strerror(errno));
    if (ftruncate(fd, (off_t)size) < 0)
    {
        close(fd);
        die("ftruncate(%zu) failed: %s", size, strerror(errno));
    }
    return fd;
}

static void session_handle_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)data;
    (void)session;
    (void)width;
    (void)height;
}

static void session_handle_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)data;
    (void)session;
    (void)format;
}

static void session_handle_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    WlrOutput* o = data;
    o->session_done = true;
}

static void session_handle_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}

static void session_handle_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}

static void session_handle_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
    die("capture session stopped by compositor");
}

static const struct ext_image_copy_capture_session_v1_listener session_listener =
{
    .buffer_size = session_handle_buffer_size,
    .shm_format = session_handle_shm_format,
    .dmabuf_device = session_handle_dmabuf_device,
    .dmabuf_format = session_handle_dmabuf_format,
    .done = session_handle_done,
    .stopped = session_handle_stopped,
};

static void frame_handle_transform(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t transform)
{
    (void)data;
    (void)frame;
    (void)transform;
}

static void frame_handle_damage(void* data, struct ext_image_copy_capture_frame_v1* frame, int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void frame_handle_presentation(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)data;
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
}

static void frame_handle_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    (void)frame;
    WlrOutput* o = data;
    o->frame_done = true;
}

static void frame_handle_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)frame;
    WlrOutput* o = data;
    die("capturing frame on output %s failed (reason %u)", o->info->make ? o->info->make : "?", reason);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener =
{
    .transform = frame_handle_transform,
    .damage = frame_handle_damage,
    .presentation_time = frame_handle_presentation,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

static void zwlr_frame_handle_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)frame;
    WlrOutput* o = data;
    if (o->buffer) return;
    o->shm_size = (size_t)stride * height;
    int fd = allocate_shm(o->shm_size);
    o->shm_data = mmap(NULL, o->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (o->shm_data == MAP_FAILED) die("mmap(%zu) failed: %s", o->shm_size, strerror(errno));
    o->pool = wl_shm_create_pool(o->w->shm, fd, (int32_t)o->shm_size);
    close(fd);
    o->buffer = wl_shm_pool_create_buffer(o->pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride, format);
}

static void zwlr_frame_handle_flags(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t flags)
{
    (void)data;
    (void)frame;
    (void)flags;
}

static void zwlr_frame_handle_ready(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
    WlrOutput* o = data;
    o->frame_done = true;
}

static void zwlr_frame_handle_failed(void* data, struct zwlr_screencopy_frame_v1* frame)
{
    (void)frame;
    WlrOutput* o = data;
    die("capturing frame on output %s failed", o->info->make ? o->info->make : "?");
}

static void zwlr_frame_handle_linux_dmabuf(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format, uint32_t width, uint32_t height)
{
    (void)data;
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
}

static void zwlr_frame_handle_buffer_done(void* data, struct zwlr_screencopy_frame_v1* frame)
{
    WlrOutput* o = data;
    zwlr_screencopy_frame_v1_copy(frame, o->buffer);
}

static const struct zwlr_screencopy_frame_v1_listener zwlr_frame_listener =
{
    .buffer = zwlr_frame_handle_buffer,
    .flags = zwlr_frame_handle_flags,
    .ready = zwlr_frame_handle_ready,
    .failed = zwlr_frame_handle_failed,
    .linux_dmabuf = zwlr_frame_handle_linux_dmabuf,
    .buffer_done = zwlr_frame_handle_buffer_done,
};

static void wlr_grab_ext(CaptureWlr* w)
{
    for (uint32_t i = 0; i < w->output_count; i++)
        w->wlr_outputs[i].source = ext_output_image_capture_source_manager_v1_create_source(w->source_manager, w->outputs[i].output);

    for (uint32_t i = 0; i < w->output_count; i++)
    {
        w->wlr_outputs[i].session = ext_image_copy_capture_manager_v1_create_session(w->copy_manager, w->wlr_outputs[i].source, 0);
        ext_image_copy_capture_session_v1_add_listener(w->wlr_outputs[i].session, &session_listener, &w->wlr_outputs[i]);
    }

    wl_display_roundtrip(w->display);

    for (uint32_t i = 0; i < w->output_count; i++)
    {
        if (!w->wlr_outputs[i].session_done) die("capture session for output %u never completed", i);
        w->wlr_outputs[i].frame = ext_image_copy_capture_session_v1_create_frame(w->wlr_outputs[i].session);
        ext_image_copy_capture_frame_v1_add_listener(w->wlr_outputs[i].frame, &frame_listener, &w->wlr_outputs[i]);
        ext_image_copy_capture_frame_v1_attach_buffer(w->wlr_outputs[i].frame, w->wlr_outputs[i].buffer);
        ext_image_copy_capture_frame_v1_capture(w->wlr_outputs[i].frame);
    }

    for (;;)
    {
        bool all_done = true;
        for (uint32_t i = 0; i < w->output_count; i++)
        {
            if (!w->wlr_outputs[i].frame_done) { all_done = false; break; }
        }
        if (all_done) break;
        if (wl_display_dispatch(w->display) < 0) die("wl_display_dispatch failed during capture");
    }
}

static void wlr_grab_zwlr(CaptureWlr* w)
{
    for (uint32_t i = 0; i < w->output_count; i++)
    {
        w->wlr_outputs[i].zwlr_frame = zwlr_screencopy_manager_v1_capture_output(w->screencopy_manager, 0, w->outputs[i].output);
        zwlr_screencopy_frame_v1_add_listener(w->wlr_outputs[i].zwlr_frame, &zwlr_frame_listener, &w->wlr_outputs[i]);
    }

    for (;;)
    {
        bool all_done = true;
        for (uint32_t i = 0; i < w->output_count; i++)
        {
            if (!w->wlr_outputs[i].frame_done) { all_done = false; break; }
        }
        if (all_done) break;
        if (wl_display_dispatch(w->display) < 0) die("wl_display_dispatch failed during capture");
    }
}

static void wlr_grab(void* backend)
{
    CaptureWlr* w = backend;

    if (w->copy_manager)
    {
        for (uint32_t i = 0; i < w->output_count; i++)
        {
            WlrOutput* o = &w->wlr_outputs[i];
            o->shm_size = (size_t)w->outputs[i].width * (size_t)w->outputs[i].height * 4;
            int fd = allocate_shm(o->shm_size);
            o->shm_data = mmap(NULL, o->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (o->shm_data == MAP_FAILED) die("mmap(%zu) failed: %s", o->shm_size, strerror(errno));
            o->pool = wl_shm_create_pool(w->shm, fd, (int32_t)o->shm_size);
            close(fd);
            o->buffer = wl_shm_pool_create_buffer(o->pool, 0, w->outputs[i].width, w->outputs[i].height, w->outputs[i].width * 4, WL_SHM_FORMAT_XRGB8888);
        }
        wlr_grab_ext(w);
    }
    else
    {
        wlr_grab_zwlr(w);
    }
}

static const void* wlr_output_pixels(void* backend, uint32_t index)
{
    CaptureWlr* w = backend;
    if (index >= w->output_count) return NULL;
    if (!w->wlr_outputs[index].frame_done) return NULL;
    return w->wlr_outputs[index].shm_data;
}

static void wlr_destroy(void* backend)
{
    CaptureWlr* w = backend;
    for (uint32_t i = 0; i < w->output_count; i++)
    {
        WlrOutput* o = &w->wlr_outputs[i];
        if (o->zwlr_frame) zwlr_screencopy_frame_v1_destroy(o->zwlr_frame);
        if (o->frame)      ext_image_copy_capture_frame_v1_destroy(o->frame);
        if (o->session)    ext_image_copy_capture_session_v1_destroy(o->session);
        if (o->source)     ext_image_capture_source_v1_destroy(o->source);
        if (o->buffer)     wl_buffer_destroy(o->buffer);
        if (o->pool)       wl_shm_pool_destroy(o->pool);
        if (o->shm_data && o->shm_data != MAP_FAILED) munmap(o->shm_data, o->shm_size);
    }
    free(w);
}

const CaptureImpl capture_wlr_impl =
{
    .grab = wlr_grab,
    .output_pixels = wlr_output_pixels,
    .destroy = wlr_destroy,
};

void capture_wlr_handle_global(CaptureWlrGlobals* g, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)version;
    if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g->source_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g->copy_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
        g->screencopy_manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 3);
}

bool capture_wlr_available(const CaptureWlrGlobals* g)
{
    return (g->source_manager && g->copy_manager) || g->screencopy_manager;
}

void* capture_wlr_create(const CaptureWlrGlobals* g, struct wl_display* display, struct wl_shm* shm, const OutputInfo* outputs, uint32_t output_count)
{
    CaptureWlr* w = calloc(1, sizeof(*w));
    if (!w) die("failed to allocate wlroots capture backend");

    w->display = display;
    w->shm = shm;
    w->source_manager = g->source_manager;
    w->copy_manager = g->copy_manager;
    w->screencopy_manager = g->screencopy_manager;
    w->outputs = outputs;
    w->output_count = output_count;

    for (uint32_t i = 0; i < output_count; i++)
    {
        w->wlr_outputs[i].info = &outputs[i];
        w->wlr_outputs[i].w = w;
    }

    return w;
}
