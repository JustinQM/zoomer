#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>

#include <wayland-client-protocol.h>

#include "capture_wlr.h"

typedef struct
{
    const OutputInfo* info; // borrowed; used for error messages and dimensions

    struct wl_buffer* buffer;
    uint32_t buffer_offset;

    struct ext_image_capture_source_v1* source;

    struct ext_image_copy_capture_session_v1* session;
    bool session_done;

    struct ext_image_copy_capture_frame_v1* frame;
    bool frame_done;
} WlrOutput;

typedef struct
{
    struct wl_display* display;
    struct wl_shm* shm;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* copy_manager;

    const OutputInfo* outputs;
    uint32_t output_count;

    void* shm_data;
    size_t shm_size;
    struct wl_shm_pool* pool;

    WlrOutput wlr_outputs[MAX_OUTPUTS];
} CaptureWlr;

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

//session
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

//frame
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

static void wlr_grab(void* backend)
{
    CaptureWlr* w = backend;

    // pack each output's buffer sequentially into one shm pool
    int32_t total_width = 0;
    int32_t max_height = 0;
    for (uint32_t i = 0; i < w->output_count; i++)
    {
        total_width += w->outputs[i].width;
        if (max_height < w->outputs[i].height) max_height = w->outputs[i].height;
    }

    w->shm_size = (size_t)total_width * (size_t)max_height * 4;
    int shm_fd = allocate_shm(w->shm_size);

    w->shm_data = mmap(NULL, w->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (w->shm_data == MAP_FAILED) die("mmap(%zu) failed: %s", w->shm_size, strerror(errno));

    w->pool = wl_shm_create_pool(w->shm, shm_fd, (int32_t)w->shm_size);
    close(shm_fd); // the pool keeps its own reference; we no longer need the fd

    uint32_t offset = 0;
    for (uint32_t i = 0; i < w->output_count; i++)
    {
        w->wlr_outputs[i].buffer = wl_shm_pool_create_buffer(w->pool, (int32_t)offset, w->outputs[i].width, w->outputs[i].height, w->outputs[i].width * 4, WL_SHM_FORMAT_XRGB8888);
        w->wlr_outputs[i].buffer_offset = offset;
        offset += (uint32_t)w->outputs[i].width * (uint32_t)w->outputs[i].height * 4;
    }

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

    // block until every output has delivered its frame
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

static const void* wlr_output_pixels(void* backend, uint32_t index)
{
    CaptureWlr* w = backend;
    if (index >= w->output_count) return NULL;
    if (!w->wlr_outputs[index].frame_done) return NULL;
    return (const uint8_t*)w->shm_data + w->wlr_outputs[index].buffer_offset;
}

static void wlr_destroy(void* backend)
{
    CaptureWlr* w = backend;
    for (uint32_t i = 0; i < w->output_count; i++)
    {
        WlrOutput* o = &w->wlr_outputs[i];
        if (o->frame)   ext_image_copy_capture_frame_v1_destroy(o->frame);
        if (o->session) ext_image_copy_capture_session_v1_destroy(o->session);
        if (o->source)  ext_image_capture_source_v1_destroy(o->source);
        if (o->buffer)  wl_buffer_destroy(o->buffer);
    }
    if (w->pool) wl_shm_pool_destroy(w->pool);
    if (w->shm_data && w->shm_data != MAP_FAILED) munmap(w->shm_data, w->shm_size);
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
}

bool capture_wlr_available(const CaptureWlrGlobals* g)
{
    return g->source_manager && g->copy_manager;
}

void* capture_wlr_create(const CaptureWlrGlobals* g, struct wl_display* display, struct wl_shm* shm, const OutputInfo* outputs, uint32_t output_count)
{
    CaptureWlr* w = calloc(1, sizeof(*w));
    if (!w) die("failed to allocate wlroots capture backend");

    w->display = display;
    w->shm = shm;
    w->source_manager = g->source_manager;
    w->copy_manager = g->copy_manager;
    w->outputs = outputs;
    w->output_count = output_count;

    for (uint32_t i = 0; i < output_count; i++)
        w->wlr_outputs[i].info = &outputs[i];

    return w;
}
