#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
#include "wlr-layer-shell-unstable-v1.h"
#include "xdg-output.h"

#include "core_wayland.h"
#include "capture.h"

void die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

//config
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

Config load_config(void)
{
    Config c = { .scroll_speed = 2.0f, .drag_friction = 5.0f, .scale_friction = 10.0f };

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
    CoreWayland* state = data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL || key == KEY_LEFTSHIFT)
    {
        state->ctrl_held = (state_val == WL_KEYBOARD_KEY_STATE_PRESSED);
        return;
    }
    state->last_key = key;
    state->last_key_state = state_val;
    state->redraw = true;
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

static void set_cursor(CoreWayland* state, struct wl_cursor* cursor)
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
    CoreWayland* state = data;
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
        state->redraw = true;
    }

    if (state->flashlight)
    {
        state->redraw = true;
    }

    state->cursor_x = new_x;
    state->cursor_y = new_y;
}

static void pointer_handle_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    CoreWayland* state = data;
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
        state->zoom_vel -= (float)v * state->config.scroll_speed * 0.1f;
    }
    state->redraw = true;
}

static void pointer_handle_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)surface;
    (void)pointer;

    CoreWayland* state = data;

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
    CoreWayland* state = data;
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
        state->redraw = true;
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
    CoreWayland* state = data;
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
    CoreWayland* state = data;
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
    else
    {
        // hand anything we do not own to the capture backend so it can bind its
        // own globals (wlroots image-copy-capture, etc.)
        capture_handle_global(state->capture, registry, name, interface, version);
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

//surface
static void surface_handle_enter(void* data, struct wl_surface* surface, struct wl_output* output)
{
    (void)surface;
    CoreWayland* state = data;
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
    CoreWayland* state = data;
    state->surface_width = width;
    state->surface_height = height;
    state->layer_surface_configured = true;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

static void layer_surface_handle_closed(void* data, struct zwlr_layer_surface_v1* layer_surface)
{
    (void)layer_surface;
    CoreWayland* state = data;
    state->should_quit = true;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener =
{
    .configure = layer_surface_handle_configure,
    .closed    = layer_surface_handle_closed,
};

static void require_globals(CoreWayland* s)
{
    if (!s->shm)                die("compositor does not expose wl_shm");
    if (!s->compositor)         die("compositor does not expose wl_compositor");
    if (!s->seat)               die("compositor does not expose wl_seat");
    if (!s->layer_shell)        die("compositor does not expose zwlr_layer_shell_v1 (wlr-layer-shell)");
    if (!s->xdg_output_manager) die("compositor does not expose zxdg_output_manager_v1");
    if (s->output_count == 0)   die("no outputs available to capture");
}

void core_wayland_connect(CoreWayland* c)
{
    c->display = wl_display_connect(NULL);
    if (!c->display) die("failed to connect to Wayland display (is WAYLAND_DISPLAY set?)");

    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wl_display_roundtrip(c->display); // bind globals
    wl_display_roundtrip(c->display); // collect output geometry/mode events

    require_globals(c);

    for (uint32_t i = 0; i < c->output_count; i++)
        assert(c->outputs[i].done); // invariant: two roundtrips above must have delivered wl_output.done

    for (uint32_t i = 0; i < c->output_count; i++)
    {
        c->outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(c->xdg_output_manager, c->outputs[i].output);
        zxdg_output_v1_add_listener(c->outputs[i].xdg_output, &xdg_output_listener, &c->outputs[i]);
    }
    wl_display_roundtrip(c->display); // collect logical positions

    int32_t start_x = INT_MAX;
    int32_t start_y = INT_MAX;
    int32_t end_x   = INT_MIN;
    int32_t end_y   = INT_MIN;

    for (uint32_t i = 0; i < c->output_count; i++)
    {
        start_x = min(c->outputs[i].x, start_x);
        start_y = min(c->outputs[i].y, start_y);
        end_x = max(c->outputs[i].x + c->outputs[i].width, end_x);
        end_y = max(c->outputs[i].y + c->outputs[i].height, end_y);
    }

    c->bounds_x = start_x;
    c->bounds_y = start_y;
    c->bounds_width = end_x - start_x;
    c->bounds_height = end_y - start_y;
}

void core_wayland_create_surface(CoreWayland* c)
{
    c->surface = wl_compositor_create_surface(c->compositor);
    wl_surface_add_listener(c->surface, &surface_listener, c);

    c->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        c->layer_shell,
        c->surface,
        NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "zoomer"
    );

    zwlr_layer_surface_v1_set_size(c->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(c->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
    );
    zwlr_layer_surface_v1_set_exclusive_zone(c->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(c->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

    zwlr_layer_surface_v1_add_listener(c->layer_surface, &layer_surface_listener, c);
    wl_surface_commit(c->surface);
    wl_display_roundtrip(c->display);
    if (!c->layer_surface_configured) die("layer surface was never configured");

    c->cursor_theme = wl_cursor_theme_load(NULL, 24, c->shm);
    if (c->cursor_theme)
    {
        c->cursor_grab = wl_cursor_theme_get_cursor(c->cursor_theme, "grab");
        c->cursor_grabbing = wl_cursor_theme_get_cursor(c->cursor_theme, "grabbing");
        if (c->cursor_grab && c->cursor_grabbing)
            c->cursor_surface = wl_compositor_create_surface(c->compositor);
    }
    c->redraw = true;
}

void core_wayland_cleanup(CoreWayland* c)
{
    for (uint32_t i = 0; i < c->output_count; i++)
    {
        OutputInfo* o = &c->outputs[i];
        if (o->xdg_output) zxdg_output_v1_destroy(o->xdg_output);
        free(o->make);
        free(o->model);
        free(o->name);
    }

    if (c->cursor_surface) wl_surface_destroy(c->cursor_surface);
    if (c->cursor_theme)   wl_cursor_theme_destroy(c->cursor_theme);

    if (c->layer_surface) zwlr_layer_surface_v1_destroy(c->layer_surface);
    if (c->surface)       wl_surface_destroy(c->surface);

    if (c->display) wl_display_disconnect(c->display);
}
