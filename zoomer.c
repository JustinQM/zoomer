#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <wayland-client-protocol.h>
#include "ext-image-copy-capture.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "ext-image-capture-source.h"
#include "xdg-output.h"

typedef struct
{
    struct wl_output* output;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    const char* make;
    const char* model;
    bool done;

    struct wl_buffer* buffer;
    uint32_t buffer_offset;

    struct ext_image_capture_source_v1* source;
    struct ext_image_copy_capture_session_v1* session;
    bool session_done;

    struct ext_image_copy_capture_frame_v1* frame;
    bool frame_done;

    struct zxdg_output_v1* xdg_output;
    const char* name;

} OutputInfo;

typedef struct
{
    struct wl_display* display;
    struct wl_registry* registry;

    struct wl_shm* shm;
    void* shm_data;
    size_t shm_size;
    struct wl_shm_pool* pool;

    struct ext_output_image_capture_source_manager_v1* image_source_capture_manager;
    struct ext_image_copy_capture_manager_v1* image_copy_capture_manager;
    struct zxdg_output_manager_v1* xdg_output_manager;

    OutputInfo outputs[10];
    uint32_t output_count;

    int32_t capture_height;
    int32_t capture_width;

    void* composite;
    int32_t composite_height;
    int32_t composite_width;
    size_t composite_size;
} State;

//outputs
static void output_handle_geometry(void* data, struct wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform)
{
    (void)output;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)transform;
    OutputInfo* info = data;
    info->x = x;
    info->y = y;
    info->make = make;
    info->model = model;
}

static void output_handle_mode(void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    (void)output;
    (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;

    OutputInfo* info = data;
    info->width = width;
    info->height = height;
}

static void output_handle_done(void* data, struct wl_output* output)
{
    (void)output;
    OutputInfo* info = data;
    info->done = true;
}

static void output_handle_scale(void* data, struct wl_output* output, int scale)
{
    (void)data;
    (void)output;
    (void)scale;
}

static const struct wl_output_listener output_listener =
{
    .geometry    = output_handle_geometry,
    .mode        = output_handle_mode,
    .done        = output_handle_done,
    .scale       = output_handle_scale,
};

//XDG Output
static void xdg_output_handle_logical_position(void* data, struct zxdg_output_v1* output, int32_t x, int32_t y)
{
    (void)output;
    OutputInfo* info = data;
    info->x = x;
    info->y = y;
}

static void xdg_output_handle_logical_size(void* data, struct zxdg_output_v1* output, int32_t width, int32_t height)
{
    (void)data;
    (void)output;
    (void)width;
    (void)height;
}

static void xdg_output_handle_done(void* data, struct zxdg_output_v1* output)
{
    (void)data;
    (void)output;
}

static void xdg_output_handle_name(void* data, struct zxdg_output_v1* output, const char* name)
{
    (void)output;
    OutputInfo* info = data;
    info->name = name;
}

static void xdg_output_handle_description(void* data, struct zxdg_output_v1* output, const char* description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct zxdg_output_v1_listener xdg_output_listener =
{
    .logical_position      = xdg_output_handle_logical_position,
    .logical_size          = xdg_output_handle_logical_size,
    .done                  = xdg_output_handle_done,
    .name                  = xdg_output_handle_name,
    .description           = xdg_output_handle_description,
};

//registry
static void registry_handle_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)version;
    State* state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        assert(state->output_count < 10);
        uint32_t idx = state->output_count;
        state->outputs[idx].output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(state->outputs[idx].output, &output_listener, &state->outputs[idx]);
        state->output_count++;
    }
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
    {
        state->image_source_capture_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    }
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
    {
        state->image_copy_capture_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        state->xdg_output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 3);
    }

}

static void registry_handle_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
    return;
}

static const struct wl_registry_listener registry_listener = 
{
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

int allocate_shm(size_t size)
{
    (void)size;
    int fd = memfd_create("shm_fd", MFD_CLOEXEC);

    //TODO: Correct Error Handling
    if (fd == -1) exit(1);

    ftruncate(fd, size);

    return fd;
}

//session
static void session_handle_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)data;
    (void)session;
    //TODO: Could verify buffer sizes match what is calculated via outputs
    (void)width;
    (void)height;
}

static void session_handle_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    //TODO: Verify that XRGB8888 is on the list of supported formats
    (void)data;
    (void)session;
    (void)format;
}

static void session_handle_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    OutputInfo* info = data;
    info->session_done = true;
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
    fprintf(stderr, "capture session stopped\n");
    exit(1);
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

static void frame_handle_presentation(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo,uint32_t tv_nsec)
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
    OutputInfo* info = data;
    info->frame_done = true;
}

static void frame_handle_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)frame;
    OutputInfo* info = data;
    fprintf(stderr, "capturing frame on output %s failed! Reason: %d\n", info->make, reason);
    exit(-1);
}

static struct ext_image_copy_capture_frame_v1_listener frame_listener =
{
    .transform = frame_handle_transform,
    .damage = frame_handle_damage,
    .presentation_time = frame_handle_presentation,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

//main
int main(void)
{
    State state = {0};

    state.display = wl_display_connect(NULL);
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    wl_display_roundtrip(state.display);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        assert(state.outputs[i].done);
        printf("Got Output: %s\n", state.outputs[i].make);
        state.capture_width += state.outputs[i].width;
        if (state.capture_height < state.outputs[i].height)
        {
            state.capture_height = state.outputs[i].height;
        }
    }
    printf("Capture Width: %d | Capture Height: %d\n", state.capture_width, state.capture_height);

    state.shm_size = state.capture_height * state.capture_width * 4;
    int shm_fd = allocate_shm(state.shm_size);
    printf("SHM FD: %d\n", shm_fd);

    state.shm_data = mmap(NULL, state.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    assert(state.shm_data != MAP_FAILED);
    printf("SHM Data: %p\n", state.shm_data);

    state.pool = wl_shm_create_pool(state.shm, shm_fd, state.shm_size);
    printf("Got Pool: %p\n", state.pool);

    uint32_t offset = 0;
    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].buffer = wl_shm_pool_create_buffer(state.pool, offset, state.outputs[i].width, state.outputs[i].height, state.outputs[i].width * 4, WL_SHM_FORMAT_XRGB8888);
        printf("Created Buffer: %p at offset %d\n", state.outputs[i].buffer, offset);
        state.outputs[i].buffer_offset = offset;
        offset += state.outputs[i].width * state.outputs[i].height * 4;
    }

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].source = ext_output_image_capture_source_manager_v1_create_source(state.image_source_capture_manager, state.outputs[i].output);
        printf("Created Source: %p for %s\n", state.outputs[i].source, state.outputs[i].make);
    }

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].session = ext_image_copy_capture_manager_v1_create_session(state.image_copy_capture_manager, state.outputs[i].source, 0);
        ext_image_copy_capture_session_v1_add_listener(state.outputs[i].session, &session_listener, &state.outputs[i]);
        printf("Created Session: %p for %s\n", state.outputs[i].session, state.outputs[i].make);
    }

    wl_display_roundtrip(state.display);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        assert(state.outputs[i].session_done == true);
        state.outputs[i].frame = ext_image_copy_capture_session_v1_create_frame(state.outputs[i].session);
        ext_image_copy_capture_frame_v1_add_listener(state.outputs[i].frame, &frame_listener, &state.outputs[i]);
        ext_image_copy_capture_frame_v1_attach_buffer(state.outputs[i].frame, state.outputs[i].buffer);
        ext_image_copy_capture_frame_v1_capture(state.outputs[i].frame);
    }

    while (true)
    {
        bool all_done = true;
        for (uint32_t i = 0; i < state.output_count; i++)
        {
            if (!state.outputs[i].frame_done)
            {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        wl_display_dispatch(state.display);
    }

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(state.xdg_output_manager, state.outputs[i].output);
        zxdg_output_v1_add_listener(state.outputs[i].xdg_output, &xdg_output_listener, &state.outputs[i]);
    }

    wl_display_roundtrip(state.display);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        assert(state.outputs[i].frame_done == true);
    }

    printf("frames are done :)\n");

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        int32_t right = state.outputs[i].x + state.outputs[i].width;
        int32_t bottom = state.outputs[i].y + state.outputs[i].height;
        if (right > state.composite_width) state.composite_width = right;
        if (bottom > state.composite_height) state.composite_height = bottom;
    }

    state.composite_size = state.composite_width * state.composite_height * 4;
    state.composite = calloc(state.composite_size / 4, 4);

    printf("composite: %dx%d\n", state.composite_width, state.composite_height);
    printf("composite ptr: %p\n", state.composite);
    for (uint32_t i = 0; i < state.output_count; i++)
    {
        printf("output %u: x=%d y=%d w=%d h=%d offset=%d\n",
            i,
            state.outputs[i].x,
            state.outputs[i].y,
            state.outputs[i].width,
            state.outputs[i].height,
            state.outputs[i].buffer_offset);
    }

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        for (int r = 0; r < state.outputs[i].height; r++)
        {
            uint8_t* src = (uint8_t*)state.shm_data + state.outputs[i].buffer_offset + (r * state.outputs[i].width * 4);
            uint8_t* dst = (uint8_t*)state.composite + (state.outputs[i].y + r) * state.composite_width * 4 + state.outputs[i].x * 4;
            memcpy(dst, src, state.outputs[i].width * 4);
        }
    }

    FILE* f = fopen("output.raw", "wb");
    fwrite(state.composite, 1, state.composite_size, f);
    fclose(f);

    printf("wrote output to output.raw\n");

    return 0;
}
