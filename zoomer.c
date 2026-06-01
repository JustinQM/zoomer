#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/memfd.h>
#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
#include <EGL/egl.h>
#include <wayland-egl.h>
#include "glad.h"
#include "ext-image-copy-capture.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "ext-image-capture-source.h"
#include "xdg-output.h"

#define MAX_OUTPUTS 16

static inline int32_t min(int32_t x, int32_t y)
{
    if (x > y) return y;
    return x;
}

static inline int32_t max(int32_t x, int32_t y)
{
    if (x < y) return y;
    return x;
}

static void die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

const char* vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec2 uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = uv;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

const char* frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_uv_offset;\n"
    "uniform vec2 u_uv_scale;\n"
    "uniform float u_zoom_level;\n"
    "uniform vec2 u_zoom_center;\n"
    "uniform vec2 u_pan;\n"
    "uniform int u_flashlight;\n"
    "uniform vec2 u_flashlight_pos;\n"
    "uniform float u_flashlight_radius;\n"
    "uniform float u_aspect_ratio;\n"
    "void main() {\n"
    "    vec2 zoomed_uv = u_zoom_center + (v_uv - u_zoom_center) / u_zoom_level;\n"
    "    vec2 uv = u_uv_offset + (zoomed_uv - u_pan) * u_uv_scale;\n"
    "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
    "        frag_color = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    frag_color.rgb = texture(u_texture, uv).rgb;\n"
    "    frag_color.a = 1.0;\n"
    "    if (u_flashlight == 1) {\n"
    "        vec2 diff = v_uv - u_flashlight_pos;\n"
    "        diff.x *= u_aspect_ratio;\n"
    "        float dist = length(diff);\n"
    "        float effective_radius = u_flashlight_radius * u_zoom_level;\n"
    "        float brightness = 1.0 - smoothstep(effective_radius * 0.85, effective_radius, dist);\n"
    "        frag_color.rgb *= mix(0.15, 1.0, brightness);\n"
    "    }\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        die("shader compile error: %s", log);
    }
    return shader;
}

//config
typedef struct
{
    float scroll_speed;
    float drag_friction;
    float scale_friction;
} Config;

static void mkdir_p(const char* path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static Config load_config(void)
{
    Config c = { .scroll_speed = 5.0f, .drag_friction = 5.0f, .scale_friction = 10.0f };

    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    char dir[256];
    char path[512];
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/zoomer", xdg);
    else if (home) snprintf(dir, sizeof(dir), "%s/.config/zoomer", home);
    else return c;
    snprintf(path, sizeof(path), "%s/config", dir);

    if (access(path, F_OK) != 0)
    {
        mkdir_p(dir);
        FILE* f = fopen(path, "w");
        if (f)
        {
            fprintf(f, "# how quickly scrolling zooms in/out\n");
            fprintf(f, "scroll_speed = %.3f\n", c.scroll_speed);
            fprintf(f, "# how quickly panning slows down after dragging\n");
            fprintf(f, "drag_friction = %.3f\n", c.drag_friction);
            fprintf(f, "# how quickly zooming slows down after scrolling\n");
            fprintf(f, "scale_friction = %.3f\n", c.scale_friction);
            fclose(f);
        }
    }

    FILE* f = fopen(path, "r");
    if (!f) return c;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64];
        float val;
        if (sscanf(line, " %63[^= ] = %f", key, &val) == 2)
        {
            if (strcmp(key, "scroll_speed") == 0) c.scroll_speed = val;
            else if (strcmp(key, "drag_friction") == 0) c.drag_friction = val;
            else if (strcmp(key, "scale_friction") == 0) c.scale_friction = val;
        }
    }
    fclose(f);
    return c;
}

typedef struct
{
    struct wl_output* output;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    char* make;
    char* model;
    bool done;

    struct wl_buffer* buffer;
    uint32_t buffer_offset;

    struct ext_image_capture_source_v1* source;
    struct ext_image_copy_capture_session_v1* session;
    bool session_done;

    struct ext_image_copy_capture_frame_v1* frame;
    bool frame_done;

    struct zxdg_output_v1* xdg_output;
    char* name;

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

    OutputInfo outputs[MAX_OUTPUTS];
    uint32_t output_count;

    int32_t capture_height;
    int32_t capture_width;

    void* composite;
    int32_t composite_height;
    int32_t composite_width;
    size_t composite_size;

    struct zwlr_layer_shell_v1* layer_shell;
    struct wl_compositor* compositor;
    struct wl_surface* surface;
    struct zwlr_layer_surface_v1* layer_surface;
    bool layer_surface_configured;
    uint32_t surface_width;
    uint32_t surface_height;

    Config config;

    float zoom_level;
    double cursor_x;
    double cursor_y;
    float zoom_center_x;
    float zoom_center_y;
    float pan_x;
    float pan_y;
    float zoom_vel;
    float pan_vel_x;
    float pan_vel_y;
    bool drag_active;
    double drag_prev_x;
    double drag_prev_y;
    double drag_accum_x;
    double drag_accum_y;
    bool flashlight;
    float flashlight_radius;
    bool ctrl_held;

    struct wl_cursor_theme* cursor_theme;
    struct wl_cursor* cursor_grab;
    struct wl_cursor* cursor_grabbing;
    struct wl_surface* cursor_surface;
    uint32_t pointer_serial;

    int32_t target_output_index;

    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_keyboard* keyboard;
    uint32_t last_key;
    uint32_t last_key_state;
    bool should_quit;

    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLuint texture;

    struct wl_egl_window* egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
} State;

static void apply_zoom(State* s, float factor)
{
    s->pan_x += ((float)s->cursor_x - s->zoom_center_x) * (1.0f - 1.0f / s->zoom_level);
    s->pan_y += ((float)s->cursor_y - s->zoom_center_y) * (1.0f - 1.0f / s->zoom_level);
    s->zoom_center_x = (float)s->cursor_x;
    s->zoom_center_y = (float)s->cursor_y;
    s->zoom_level *= factor;
}

static void reset_view(State* s)
{
    s->zoom_level = 1.0f;
    s->zoom_center_x = 0.5f;
    s->zoom_center_y = 0.5f;
    s->pan_x = 0.0f;
    s->pan_y = 0.0f;
    s->zoom_vel = 0.0f;
    s->pan_vel_x = 0.0f;
    s->pan_vel_y = 0.0f;
}

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
    info->make = strdup(make);
    info->model = strdup(model);
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
    info->name = strdup(name);
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

//keyboard
static void keyboard_handle_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_handle_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_handle_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_handle_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state_val)
{
    (void)keyboard; (void)serial; (void)time;
    State* state = data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL || key == KEY_LEFTSHIFT)
    {
        state->ctrl_held = (state_val == WL_KEYBOARD_KEY_STATE_PRESSED);
        return;
    }
    state->last_key = key;
    state->last_key_state = state_val;
}

static void keyboard_handle_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void keyboard_handle_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener =
{
    .keymap      = keyboard_handle_keymap,
    .enter       = keyboard_handle_enter,
    .leave       = keyboard_handle_leave,
    .key         = keyboard_handle_key,
    .modifiers   = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void set_cursor(State* state, struct wl_cursor* cursor)
{
    if (!cursor) return;
    if (!state->cursor_surface) return;

    struct wl_cursor_image* image = cursor->images[0];

    struct wl_buffer* buffer =
        wl_cursor_image_get_buffer(image);

    wl_surface_attach(
        state->cursor_surface,
        buffer,
        0,
        0
    );

    wl_surface_damage_buffer(
        state->cursor_surface,
        0,
        0,
        image->width,
        image->height
    );

    wl_surface_commit(state->cursor_surface);

    wl_pointer_set_cursor(
        state->pointer,
        state->pointer_serial,
        state->cursor_surface,
        image->hotspot_x,
        image->hotspot_y
    );
}


static void pointer_handle_motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer; (void)time;
    State* state = data;
    double new_x = wl_fixed_to_double(x) / state->surface_width;
    double new_y = wl_fixed_to_double(y) / state->surface_height;

    if (state->drag_active)
    {
        double dx = (new_x - state->drag_prev_x) / state->zoom_level;
        double dy = (new_y - state->drag_prev_y) / state->zoom_level;
        state->pan_x += (float)dx;
        state->pan_y += (float)dy;
        state->drag_accum_x += dx;
        state->drag_accum_y += dy;
        state->drag_prev_x = new_x;
        state->drag_prev_y = new_y;
    }

    state->cursor_x = new_x;
    state->cursor_y = new_y;
}

static void pointer_handle_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    State* state = data;
    double v = wl_fixed_to_double(value);

    if (state->ctrl_held)
    {
        float step = state->flashlight_radius * 0.1f;
        if (v > 0) state->flashlight_radius += step;
        else state->flashlight_radius -= step;
        if (state->flashlight_radius < 0.01f) state->flashlight_radius = 0.01f;
    }
    else
    {
        state->zoom_vel -= copysignf(state->config.scroll_speed, (float)v);
    }
}

static void pointer_handle_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)surface;
    (void)pointer;

    State* state = data;

    state->pointer_serial = serial;

    state->cursor_x = wl_fixed_to_double(x) / state->surface_width;
    state->cursor_y = wl_fixed_to_double(y) / state->surface_height;

    set_cursor(state, state->cursor_grab);
}

static void pointer_handle_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
}

static void pointer_handle_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state_val)
{
    (void)pointer;
    (void)serial;
    (void)time;
    State* state = data;
    if (button == BTN_LEFT || button == BTN_MIDDLE)
    {
        state->drag_active =
            (state_val == WL_POINTER_BUTTON_STATE_PRESSED);

        if (state->drag_active)
        {
            set_cursor(state, state->cursor_grabbing);

            state->drag_prev_x = state->cursor_x;
            state->drag_prev_y = state->cursor_y;
            state->drag_accum_x = 0.0;
            state->drag_accum_y = 0.0;
            state->pan_vel_x = 0.0f;
            state->pan_vel_y = 0.0f;
        }
        else
        {
            set_cursor(state, state->cursor_grab);
        }
    }
}

static void pointer_handle_frame(void* data, struct wl_pointer* pointer)
{
    (void)data; (void)pointer;
}

static void pointer_handle_axis_source(void* data, struct wl_pointer* pointer, uint32_t axis_source)
{
    (void)data; (void)pointer; (void)axis_source;
}

static void pointer_handle_axis_stop(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_handle_axis_discrete(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener =
{
    .enter         = pointer_handle_enter,
    .leave         = pointer_handle_leave,
    .motion        = pointer_handle_motion,
    .button        = pointer_handle_button,
    .axis          = pointer_handle_axis,
    .frame         = pointer_handle_frame,
    .axis_source   = pointer_handle_axis_source,
    .axis_stop     = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
};

//seat
static void seat_handle_capabilities(void* data, struct wl_seat* seat, uint32_t capabilities)
{
    State* state = data;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
    {
        state->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER)
    {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    }
}

static void seat_handle_name(void* data, struct wl_seat* seat, const char* name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener =
{
    .capabilities = seat_handle_capabilities,
    .name         = seat_handle_name,
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
        if (state->output_count >= MAX_OUTPUTS)
        {
            fprintf(stderr, "warning: more than %d outputs, ignoring extras\n", MAX_OUTPUTS);
            return;
        }
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
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
    {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    }
    else if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}

static void registry_handle_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener =
{
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
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
    OutputInfo* info = data;
    info->frame_done = true;
}

static void frame_handle_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)frame;
    OutputInfo* info = data;
    die("capturing frame on output %s failed (reason %u)", info->make ? info->make : "?", reason);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener =
{
    .transform = frame_handle_transform,
    .damage = frame_handle_damage,
    .presentation_time = frame_handle_presentation,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

//surface
static void surface_handle_enter(void* data, struct wl_surface* surface, struct wl_output* output)
{
    (void)surface;
    State* state = data;
    for (uint32_t i = 0; i < state->output_count; i++)
    {
        if (state->outputs[i].output == output)
        {
            state->target_output_index = (int32_t)i;
            break;
        }
    }
}

static void surface_handle_leave(void* data, struct wl_surface* surface, struct wl_output* output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static const struct wl_surface_listener surface_listener =
{
    .enter = surface_handle_enter,
    .leave = surface_handle_leave,
};

//layer surface
static void layer_surface_handle_configure(void* data, struct zwlr_layer_surface_v1* layer_surface, uint32_t serial, uint32_t width, uint32_t height)
{
    State* state = data;
    state->surface_width = width;
    state->surface_height = height;
    state->layer_surface_configured = true;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

static void layer_surface_handle_closed(void* data, struct zwlr_layer_surface_v1* layer_surface)
{
    (void)layer_surface;
    State* state = data;
    state->should_quit = true;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener =
{
    .configure = layer_surface_handle_configure,
    .closed    = layer_surface_handle_closed,
};

static void require_globals(State* s)
{
    if (!s->shm)                          die("compositor does not expose wl_shm");
    if (!s->compositor)                   die("compositor does not expose wl_compositor");
    if (!s->seat)                         die("compositor does not expose wl_seat");
    if (!s->layer_shell)                  die("compositor does not expose zwlr_layer_shell_v1 (wlr-layer-shell)");
    if (!s->image_source_capture_manager) die("compositor does not expose ext_output_image_capture_source_manager_v1");
    if (!s->image_copy_capture_manager)   die("compositor does not expose ext_image_copy_capture_manager_v1");
    if (!s->xdg_output_manager)           die("compositor does not expose zxdg_output_manager_v1");
    if (s->output_count == 0)             die("no outputs available to capture");
}

static void cleanup(State* s)
{
    if (s->egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(s->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s->egl_surface != EGL_NO_SURFACE) eglDestroySurface(s->egl_display, s->egl_surface);
        if (s->egl_context != EGL_NO_CONTEXT) eglDestroyContext(s->egl_display, s->egl_context);
        eglTerminate(s->egl_display);
    }
    if (s->egl_window) wl_egl_window_destroy(s->egl_window);

    for (uint32_t i = 0; i < s->output_count; i++)
    {
        OutputInfo* o = &s->outputs[i];
        if (o->frame)      ext_image_copy_capture_frame_v1_destroy(o->frame);
        if (o->session)    ext_image_copy_capture_session_v1_destroy(o->session);
        if (o->source)     ext_image_capture_source_v1_destroy(o->source);
        if (o->xdg_output) zxdg_output_v1_destroy(o->xdg_output);
        if (o->buffer)     wl_buffer_destroy(o->buffer);
        free(o->make);
        free(o->model);
        free(o->name);
    }

    if (s->layer_surface) zwlr_layer_surface_v1_destroy(s->layer_surface);
    if (s->surface)       wl_surface_destroy(s->surface);
    if (s->pool)          wl_shm_pool_destroy(s->pool);

    if (s->shm_data && s->shm_data != MAP_FAILED) munmap(s->shm_data, s->shm_size);
    free(s->composite);

    if (s->display) wl_display_disconnect(s->display);
}

//main
int main(void)
{
    State state = {0};

    state.config = load_config();
    state.flashlight_radius = 0.15f;
    state.cursor_x = 0.5;
    state.cursor_y = 0.5;
    reset_view(&state);

    state.egl_display = EGL_NO_DISPLAY;
    state.egl_context = EGL_NO_CONTEXT;
    state.egl_surface = EGL_NO_SURFACE;

    state.display = wl_display_connect(NULL);
    if (!state.display) die("failed to connect to Wayland display (is WAYLAND_DISPLAY set?)");

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display); // bind globals
    wl_display_roundtrip(state.display); // collect output geometry/mode events

    require_globals(&state);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        assert(state.outputs[i].done); // invariant: two roundtrips above must have delivered wl_output.done
        state.capture_width += state.outputs[i].width;
        if (state.capture_height < state.outputs[i].height)
            state.capture_height = state.outputs[i].height;
    }

    state.shm_size = (size_t)state.capture_width * (size_t)state.capture_height * 4;
    int shm_fd = allocate_shm(state.shm_size);

    state.shm_data = mmap(NULL, state.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (state.shm_data == MAP_FAILED) die("mmap(%zu) failed: %s", state.shm_size, strerror(errno));

    state.pool = wl_shm_create_pool(state.shm, shm_fd, (int32_t)state.shm_size);
    close(shm_fd); // the pool keeps its own reference; we no longer need the fd

    uint32_t offset = 0;
    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].buffer = wl_shm_pool_create_buffer(state.pool, (int32_t)offset, state.outputs[i].width, state.outputs[i].height, state.outputs[i].width * 4, WL_SHM_FORMAT_XRGB8888);
        state.outputs[i].buffer_offset = offset;
        offset += (uint32_t)state.outputs[i].width * (uint32_t)state.outputs[i].height * 4;
    }

    for (uint32_t i = 0; i < state.output_count; i++)
        state.outputs[i].source = ext_output_image_capture_source_manager_v1_create_source(state.image_source_capture_manager, state.outputs[i].output);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].session = ext_image_copy_capture_manager_v1_create_session(state.image_copy_capture_manager, state.outputs[i].source, 0);
        ext_image_copy_capture_session_v1_add_listener(state.outputs[i].session, &session_listener, &state.outputs[i]);
    }

    wl_display_roundtrip(state.display);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        if (!state.outputs[i].session_done) die("capture session for output %u never completed", i);
        state.outputs[i].frame = ext_image_copy_capture_session_v1_create_frame(state.outputs[i].session);
        ext_image_copy_capture_frame_v1_add_listener(state.outputs[i].frame, &frame_listener, &state.outputs[i]);
        ext_image_copy_capture_frame_v1_attach_buffer(state.outputs[i].frame, state.outputs[i].buffer);
        ext_image_copy_capture_frame_v1_capture(state.outputs[i].frame);
    }

    for (;;)
    {
        bool all_done = true;
        for (uint32_t i = 0; i < state.output_count; i++)
        {
            if (!state.outputs[i].frame_done) { all_done = false; break; }
        }
        if (all_done) break;
        if (wl_display_dispatch(state.display) < 0) die("wl_display_dispatch failed during capture");
    }

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        state.outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(state.xdg_output_manager, state.outputs[i].output);
        zxdg_output_v1_add_listener(state.outputs[i].xdg_output, &xdg_output_listener, &state.outputs[i]);
    }

    wl_display_roundtrip(state.display);

    state.surface = wl_compositor_create_surface(state.compositor);
    wl_surface_add_listener(state.surface, &surface_listener, &state);

    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell,
        state.surface,
        NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "zoomer"
    );

    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
    );
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

    zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);
    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);
    if (!state.layer_surface_configured) die("layer surface was never configured");

    state.cursor_theme = wl_cursor_theme_load(NULL, 24, state.shm);
    if (state.cursor_theme)
    {
        state.cursor_grab = wl_cursor_theme_get_cursor(state.cursor_theme, "grab");
        state.cursor_grabbing = wl_cursor_theme_get_cursor(state.cursor_theme, "grabbing");
        if (state.cursor_grab && state.cursor_grabbing) 
            state.cursor_surface = wl_compositor_create_surface(state.compositor);
    }

    state.egl_window = wl_egl_window_create(state.surface, (int)state.surface_width, (int)state.surface_height);
    if (!state.egl_window) die("wl_egl_window_create failed");

    state.egl_display = eglGetDisplay((EGLNativeDisplayType)state.display);
    if (state.egl_display == EGL_NO_DISPLAY) die("eglGetDisplay failed");

    EGLint major, minor;
    if (!eglInitialize(state.egl_display, &major, &minor)) die("eglInitialize failed");

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig egl_config;
    EGLint num_configs;
    if (!eglChooseConfig(state.egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0)
        die("no suitable EGL config found");

    if (!eglBindAPI(EGL_OPENGL_API)) die("eglBindAPI(EGL_OPENGL_API) failed");

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    state.egl_context = eglCreateContext(state.egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (state.egl_context == EGL_NO_CONTEXT) die("eglCreateContext failed");

    state.egl_surface = eglCreateWindowSurface(state.egl_display, egl_config, (EGLNativeWindowType)state.egl_window, NULL);
    if (state.egl_surface == EGL_NO_SURFACE) die("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(state.egl_display, state.egl_surface, state.egl_surface, state.egl_context))
        die("eglMakeCurrent failed");

    if (!gladLoadGL()) die("gladLoadGL failed");

    glViewport(0, 0, (GLsizei)state.surface_width, (GLsizei)state.surface_height);

    int32_t start_x = INT_MAX;
    int32_t start_y = INT_MAX;
    int32_t end_x   = INT_MIN;
    int32_t end_y   = INT_MIN;

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        start_x = min(state.outputs[i].x, start_x);
        start_y = min(state.outputs[i].y, start_y);
        end_x = max(state.outputs[i].x + state.outputs[i].width, end_x);
        end_y = max(state.outputs[i].y + state.outputs[i].height, end_y);
    }

    state.composite_width = end_x - start_x;
    state.composite_height = end_y - start_y;

    state.composite_size = (size_t)state.composite_width * (size_t)state.composite_height * 4;
    state.composite = calloc((size_t)state.composite_width * (size_t)state.composite_height, 4);
    if (!state.composite) die("failed to allocate %zu-byte composite buffer", state.composite_size);

    for (uint32_t i = 0; i < state.output_count; i++)
    {
        for (int r = 0; r < state.outputs[i].height; r++)
        {
            uint8_t* src = (uint8_t*)state.shm_data + state.outputs[i].buffer_offset + ((size_t)r * state.outputs[i].width * 4);
            uint8_t* dst = (uint8_t*)state.composite + ((size_t)(state.outputs[i].y - start_y + r) * state.composite_width * 4) + (((size_t)state.outputs[i].x  - start_x)* 4);
            memcpy(dst, src, (size_t)state.outputs[i].width * 4);
        }
    }

    GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);

    state.program = glCreateProgram();
    glAttachShader(state.program, vert);
    glAttachShader(state.program, frag);
    glLinkProgram(state.program);

    GLint ok;
    glGetProgramiv(state.program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(state.program, sizeof(log), NULL, log);
        die("shader link error: %s", log);
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    float vertices[] = 
    {
        -1.0f,  1.0f,    0.0f,  0.0f,
         1.0f,  1.0f,    1.0f,  0.0f,
        -1.0f, -1.0f,    0.0f,  1.0f,
         1.0f, -1.0f,    1.0f,  1.0f,
    };

    glGenVertexArrays(1, &state.vao);
    glGenBuffers(1, &state.vbo);

    glBindVertexArray(state.vao);
    glBindBuffer(GL_ARRAY_BUFFER, state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &state.texture);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, state.composite_width, state.composite_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, state.composite);

    glUseProgram(state.program);
    GLint loc_uv_offset        = glGetUniformLocation(state.program, "u_uv_offset");
    GLint loc_uv_scale         = glGetUniformLocation(state.program, "u_uv_scale");
    GLint loc_zoom_level       = glGetUniformLocation(state.program, "u_zoom_level");
    GLint loc_zoom_center      = glGetUniformLocation(state.program, "u_zoom_center");
    GLint loc_pan              = glGetUniformLocation(state.program, "u_pan");
    GLint loc_flashlight       = glGetUniformLocation(state.program, "u_flashlight");
    GLint loc_flashlight_pos   = glGetUniformLocation(state.program, "u_flashlight_pos");
    GLint loc_flashlight_radius= glGetUniformLocation(state.program, "u_flashlight_radius");
    GLint loc_aspect_ratio     = glGetUniformLocation(state.program, "u_aspect_ratio");

    int wl_fd = wl_display_get_fd(state.display);

    struct timespec last_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_ts);

    while (!state.should_quit)
    {
        while (!state.should_quit && wl_display_prepare_read(state.display) != 0)
            wl_display_dispatch_pending(state.display);

        if (state.should_quit) { wl_display_cancel_read(state.display); break; }

        wl_display_flush(state.display);

        struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };
        poll(&pfd, 1, 1);

        if (pfd.revents & POLLIN) wl_display_read_events(state.display);
        else wl_display_cancel_read(state.display);

        wl_display_dispatch_pending(state.display);

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        float dt = (now_ts.tv_sec - last_ts.tv_sec) + (now_ts.tv_nsec - last_ts.tv_nsec) / 1e9f;
        last_ts = now_ts;
        if (dt > 0.1f) dt = 0.1f;

        if (state.last_key_state == WL_KEYBOARD_KEY_STATE_PRESSED)
        {
            switch (state.last_key)
            {
                case KEY_ESC:
                    state.should_quit = true;
                    break;
                case KEY_MINUS: // zoom out
                    state.zoom_vel -= state.config.scroll_speed;
                    break;
                case KEY_EQUAL: // zoom in
                    state.zoom_vel += state.config.scroll_speed;
                    break;
                case KEY_R:
                    reset_view(&state);
                    break;
                case KEY_F:
                    state.flashlight = !state.flashlight;
                    break;
            }
            state.last_key_state = WL_KEYBOARD_KEY_STATE_RELEASED;
        }

        if (state.zoom_vel != 0.0f)
        {
            apply_zoom(&state, expf(state.zoom_vel * dt));
            state.zoom_vel *= expf(-state.config.scale_friction * dt);
            if (fabsf(state.zoom_vel) < 1e-3f) state.zoom_vel = 0.0f;
        }

        if (state.drag_active)
        {
            if (dt > 0.0f)
            {
                state.pan_vel_x = (float)(state.drag_accum_x / dt);
                state.pan_vel_y = (float)(state.drag_accum_y / dt);
            }
            state.drag_accum_x = 0.0;
            state.drag_accum_y = 0.0;
        }
        else if (state.pan_vel_x != 0.0f || state.pan_vel_y != 0.0f)
        {
            state.pan_x += state.pan_vel_x * dt;
            state.pan_y += state.pan_vel_y * dt;
            float decay = expf(-state.config.drag_friction * dt);
            state.pan_vel_x *= decay;
            state.pan_vel_y *= decay;
            if (fabsf(state.pan_vel_x) < 1e-4f) state.pan_vel_x = 0.0f;
            if (fabsf(state.pan_vel_y) < 1e-4f) state.pan_vel_y = 0.0f;
        }

        OutputInfo* target = &state.outputs[state.target_output_index];
        glUniform2f(loc_uv_offset,
            (float)(target->x - start_x) / (float)state.composite_width,
            (float)(target->y - start_y) / (float)state.composite_height);
        glUniform2f(loc_uv_scale,
            (float)target->width / (float)state.composite_width,
            (float)target->height / (float)state.composite_height);

        glUniform1f(loc_zoom_level, state.zoom_level);
        glUniform2f(loc_zoom_center, state.zoom_center_x, state.zoom_center_y);
        glUniform2f(loc_pan, state.pan_x, state.pan_y);
        glUniform1i(loc_flashlight, state.flashlight ? 1 : 0);
        glUniform2f(loc_flashlight_pos, (float)state.cursor_x, (float)state.cursor_y);
        glUniform1f(loc_flashlight_radius, state.flashlight_radius);
        glUniform1f(loc_aspect_ratio, (float)state.surface_width / (float)state.surface_height);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(state.program);
        glBindTexture(GL_TEXTURE_2D, state.texture);
        glBindVertexArray(state.vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        eglSwapBuffers(state.egl_display, state.egl_surface);
    }

    glDeleteTextures(1, &state.texture);
    glDeleteBuffers(1, &state.vbo);
    glDeleteVertexArrays(1, &state.vao);
    glDeleteProgram(state.program);

    cleanup(&state);
    return EXIT_SUCCESS;
}
