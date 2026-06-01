#ifndef CORE_WAYLAND_H
#define CORE_WAYLAND_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <wayland-client-protocol.h>
#include <wayland-cursor.h>

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

void die(const char* fmt, ...);

//config
typedef struct
{
    float scroll_speed;
    float drag_friction;
    float scale_friction;
} Config;

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

    struct zxdg_output_v1* xdg_output;
    char* name;
} OutputInfo;

struct Capture;

typedef struct
{
    struct wl_display* display;
    struct wl_registry* registry;

    struct wl_shm* shm;
    struct zxdg_output_manager_v1* xdg_output_manager;
    struct zwlr_layer_shell_v1* layer_shell;
    struct wl_compositor* compositor;

    // opaque capture backend; registry globals are forwarded to it
    struct Capture* capture;

    OutputInfo outputs[MAX_OUTPUTS];
    uint32_t output_count;

    // bounding box covering every output, in logical coordinates
    int32_t bounds_x;
    int32_t bounds_y;
    int32_t bounds_width;
    int32_t bounds_height;

    struct wl_surface* surface;
    struct zwlr_layer_surface_v1* layer_surface;
    bool layer_surface_configured;
    uint32_t surface_width;
    uint32_t surface_height;

    struct wl_cursor_theme* cursor_theme;
    struct wl_cursor* cursor_grab;
    struct wl_cursor* cursor_grabbing;
    struct wl_surface* cursor_surface;
    uint32_t pointer_serial;

    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_keyboard* keyboard;

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

    int32_t target_output_index;

    uint32_t last_key;
    uint32_t last_key_state;
    bool should_quit;
} CoreWayland;

Config load_config(void);

// Connect to the compositor, bind globals, enumerate outputs and their logical
// layout, and compute the desktop bounding box. Capture globals are forwarded to
// c->capture, so c->capture must be set before calling this.
void core_wayland_connect(CoreWayland* c);

// Create the overlay layer surface and load the cursor. Call this only after the
// capture backend has produced its frame(s), so the overlay never captures itself.
void core_wayland_create_surface(CoreWayland* c);

void core_wayland_cleanup(CoreWayland* c);

#endif
