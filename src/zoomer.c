#define _GNU_SOURCE
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include "glad.h"

#include "core_wayland.h"
#include "capture.h"

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

static void apply_zoom(CoreWayland* c, float factor)
{
    c->pan_x += ((float)c->cursor_x - c->zoom_center_x) * (1.0f - 1.0f / c->zoom_level);
    c->pan_y += ((float)c->cursor_y - c->zoom_center_y) * (1.0f - 1.0f / c->zoom_level);
    c->zoom_center_x = (float)c->cursor_x;
    c->zoom_center_y = (float)c->cursor_y;
    c->zoom_level *= factor;
}

static void reset_view(CoreWayland* c)
{
    c->zoom_level = 1.0f;
    c->zoom_center_x = 0.5f;
    c->zoom_center_y = 0.5f;
    c->pan_x = 0.0f;
    c->pan_y = 0.0f;
    c->zoom_vel = 0.0f;
    c->pan_vel_x = 0.0f;
    c->pan_vel_y = 0.0f;
}

int main(void)
{
    Capture* capture = capture_create();

    CoreWayland core = {0};
    core.capture = capture;
    core.config = load_config();
    core.flashlight_radius = 0.15f;
    core.cursor_x = 0.5;
    core.cursor_y = 0.5;
    reset_view(&core);

    core_wayland_connect(&core);

    capture_init(capture, core.display, core.shm, core.outputs, core.output_count);
    capture_grab(capture);

    core_wayland_create_surface(&core);

    struct wl_egl_window* egl_window = wl_egl_window_create(core.surface, (int)core.surface_width, (int)core.surface_height);
    if (!egl_window) die("wl_egl_window_create failed");

    EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)core.display);
    if (egl_display == EGL_NO_DISPLAY) die("eglGetDisplay failed");

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) die("eglInitialize failed");

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
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0)
        die("no suitable EGL config found");

    if (!eglBindAPI(EGL_OPENGL_API)) die("eglBindAPI(EGL_OPENGL_API) failed");

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    EGLContext egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) die("eglCreateContext failed");

    EGLSurface egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)egl_window, NULL);
    if (egl_surface == EGL_NO_SURFACE) die("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
        die("eglMakeCurrent failed");

    if (!gladLoadGL()) die("gladLoadGL failed");

    glViewport(0, 0, (GLsizei)core.surface_width, (GLsizei)core.surface_height);

    // stitch each output's captured pixels into one desktop-sized composite
    uint8_t* composite = calloc((size_t)core.bounds_width * (size_t)core.bounds_height, 4);
    if (!composite) die("failed to allocate composite buffer");

    for (uint32_t i = 0; i < core.output_count; i++)
    {
        int32_t stride = 0;
        const uint8_t* src_base = capture_output_pixels(capture, i, &stride);
        if (!src_base) continue;

        for (int r = 0; r < core.outputs[i].height; r++)
        {
            const uint8_t* src = src_base + ((size_t)r * stride);
            uint8_t* dst = composite + ((size_t)(core.outputs[i].y - core.bounds_y + r) * core.bounds_width * 4) + (((size_t)core.outputs[i].x - core.bounds_x) * 4);
            memcpy(dst, src, (size_t)core.outputs[i].width * 4);
        }
    }

    GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);

    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
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

    GLuint vao;
    GLuint vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, core.bounds_width, core.bounds_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, composite);

    free(composite);

    glUseProgram(program);
    GLint loc_uv_offset        = glGetUniformLocation(program, "u_uv_offset");
    GLint loc_uv_scale         = glGetUniformLocation(program, "u_uv_scale");
    GLint loc_zoom_level       = glGetUniformLocation(program, "u_zoom_level");
    GLint loc_zoom_center      = glGetUniformLocation(program, "u_zoom_center");
    GLint loc_pan              = glGetUniformLocation(program, "u_pan");
    GLint loc_flashlight       = glGetUniformLocation(program, "u_flashlight");
    GLint loc_flashlight_pos   = glGetUniformLocation(program, "u_flashlight_pos");
    GLint loc_flashlight_radius= glGetUniformLocation(program, "u_flashlight_radius");
    GLint loc_aspect_ratio     = glGetUniformLocation(program, "u_aspect_ratio");

    int wl_fd = wl_display_get_fd(core.display);

    struct timespec last_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_ts);

    while (!core.should_quit)
    {
        while (!core.should_quit && wl_display_prepare_read(core.display) != 0)
            wl_display_dispatch_pending(core.display);

        if (core.should_quit) { wl_display_cancel_read(core.display); break; }

        wl_display_flush(core.display);

        struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };
        poll(&pfd, 1, 1);

        if (pfd.revents & POLLIN) wl_display_read_events(core.display);
        else wl_display_cancel_read(core.display);

        wl_display_dispatch_pending(core.display);

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        float dt = (now_ts.tv_sec - last_ts.tv_sec) + (now_ts.tv_nsec - last_ts.tv_nsec) / 1e9f;
        last_ts = now_ts;
        if (dt > 0.1f) dt = 0.1f;

        if (core.last_key_state == WL_KEYBOARD_KEY_STATE_PRESSED)
        {
            switch (core.last_key)
            {
                case KEY_ESC:
                    core.should_quit = true;
                    break;
                case KEY_MINUS: // zoom out
                    core.zoom_vel -= core.config.scroll_speed;
                    break;
                case KEY_EQUAL: // zoom in
                    core.zoom_vel += core.config.scroll_speed;
                    break;
                case KEY_R:
                    reset_view(&core);
                    break;
                case KEY_F:
                    core.flashlight = !core.flashlight;
                    break;
            }
            core.last_key_state = WL_KEYBOARD_KEY_STATE_RELEASED;
        }

        if (core.zoom_vel != 0.0f)
        {
            apply_zoom(&core, expf(core.zoom_vel * dt));
            core.zoom_vel *= expf(-core.config.scale_friction * dt);
            if (fabsf(core.zoom_vel) < 1e-3f) core.zoom_vel = 0.0f;
        }

        if (core.drag_active)
        {
            if (dt > 0.0f)
            {
                core.pan_vel_x = (float)(core.drag_accum_x / dt);
                core.pan_vel_y = (float)(core.drag_accum_y / dt);
            }
            core.drag_accum_x = 0.0;
            core.drag_accum_y = 0.0;
        }
        else if (core.pan_vel_x != 0.0f || core.pan_vel_y != 0.0f)
        {
            core.pan_x += core.pan_vel_x * dt;
            core.pan_y += core.pan_vel_y * dt;
            float decay = expf(-core.config.drag_friction * dt);
            core.pan_vel_x *= decay;
            core.pan_vel_y *= decay;
            if (fabsf(core.pan_vel_x) < 1e-4f) core.pan_vel_x = 0.0f;
            if (fabsf(core.pan_vel_y) < 1e-4f) core.pan_vel_y = 0.0f;
        }

        OutputInfo* target = &core.outputs[core.target_output_index];
        glUniform2f(loc_uv_offset,
            (float)(target->x - core.bounds_x) / (float)core.bounds_width,
            (float)(target->y - core.bounds_y) / (float)core.bounds_height);
        glUniform2f(loc_uv_scale,
            (float)target->width / (float)core.bounds_width,
            (float)target->height / (float)core.bounds_height);

        glUniform1f(loc_zoom_level, core.zoom_level);
        glUniform2f(loc_zoom_center, core.zoom_center_x, core.zoom_center_y);
        glUniform2f(loc_pan, core.pan_x, core.pan_y);
        glUniform1i(loc_flashlight, core.flashlight ? 1 : 0);
        glUniform2f(loc_flashlight_pos, (float)core.cursor_x, (float)core.cursor_y);
        glUniform1f(loc_flashlight_radius, core.flashlight_radius);
        glUniform1f(loc_aspect_ratio, (float)core.surface_width / (float)core.surface_height);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        eglSwapBuffers(egl_display, egl_surface);
    }

    glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surface);
    eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
    wl_egl_window_destroy(egl_window);

    capture_destroy(capture);
    core_wayland_cleanup(&core);
    return EXIT_SUCCESS;
}
