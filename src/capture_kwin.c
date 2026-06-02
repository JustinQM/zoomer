#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include "capture_kwin.h"
#include "core_wayland.h"

// opaque types — we never dereference these directly
typedef struct sd_bus            sd_bus;
typedef struct sd_bus_message    sd_bus_message;
typedef struct sd_bus_slot       sd_bus_slot;
typedef struct sd_bus_error      sd_bus_error;
typedef int (*sd_bus_message_handler_t)(sd_bus_message*, void*, sd_bus_error*);

// spa/pipewire forward declarations
typedef struct spa_pod           spa_pod;
typedef struct spa_pod_builder   spa_pod_builder;
typedef struct spa_hook          spa_hook;
typedef struct pw_thread_loop    pw_thread_loop;
typedef struct pw_context        pw_context;
typedef struct pw_core           pw_core;
typedef struct pw_stream         pw_stream;
typedef struct pw_buffer         pw_buffer;
typedef struct spa_buffer        spa_buffer;

// we need the real spa/pw structs for the inline macros and callbacks
// so we include only the headers that don't pull in sd-bus or pipewire link deps
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <pipewire/pipewire.h>

#define PORTAL_BUS  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.ScreenCast"
#define TOKEN_PATH "/.config/zoomer/restore_token"

// sd-bus function pointers
static int      (*p_sd_bus_open_user)             (sd_bus**);
static int      (*p_sd_bus_unref)                 (sd_bus*);
static int      (*p_sd_bus_get_unique_name)        (sd_bus*, const char**);
static int      (*p_sd_bus_get_property_trivial)   (sd_bus*, const char*, const char*, const char*, const char*, sd_bus_error*, char, void*);
static int      (*p_sd_bus_add_match)              (sd_bus*, sd_bus_slot**, const char*, sd_bus_message_handler_t, void*);
static int      (*p_sd_bus_call)                   (sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*, sd_bus_message**);
static int      (*p_sd_bus_wait)                   (sd_bus*, uint64_t);
static int      (*p_sd_bus_process)                (sd_bus*, sd_bus_message**);
static int      (*p_sd_bus_slot_unref)             (sd_bus_slot*);
static int      (*p_sd_bus_message_new_method_call)(sd_bus*, sd_bus_message**, const char*, const char*, const char*, const char*);
static int      (*p_sd_bus_message_unref)          (sd_bus_message*);
static int      (*p_sd_bus_message_read)           (sd_bus_message*, const char*, ...);
static int      (*p_sd_bus_message_read_basic)     (sd_bus_message*, char, void*);
static int      (*p_sd_bus_message_skip)           (sd_bus_message*, const char*);
static int      (*p_sd_bus_message_enter_container)(sd_bus_message*, char, const char*);
static int      (*p_sd_bus_message_exit_container) (sd_bus_message*);
static int      (*p_sd_bus_message_open_container) (sd_bus_message*, char, const char*);
static int      (*p_sd_bus_message_close_container)(sd_bus_message*);
static int      (*p_sd_bus_message_append_basic)   (sd_bus_message*, char, const void*);

// pipewire function pointers
static void     (*p_pw_init)                       (int*, char***);
static struct pw_thread_loop* (*p_pw_thread_loop_new)       (const char*, const struct spa_dict*);
static void     (*p_pw_thread_loop_destroy)        (struct pw_thread_loop*);
static int      (*p_pw_thread_loop_start)          (struct pw_thread_loop*);
static void     (*p_pw_thread_loop_stop)           (struct pw_thread_loop*);
static void     (*p_pw_thread_loop_lock)           (struct pw_thread_loop*);
static void     (*p_pw_thread_loop_unlock)         (struct pw_thread_loop*);
static void     (*p_pw_thread_loop_wait)           (struct pw_thread_loop*);
static void     (*p_pw_thread_loop_signal)         (struct pw_thread_loop*, bool);
static struct pw_loop* (*p_pw_thread_loop_get_loop)(struct pw_thread_loop*);
static struct pw_context* (*p_pw_context_new)      (struct pw_loop*, const struct pw_properties*, size_t);
static struct pw_core* (*p_pw_context_connect_fd)  (struct pw_context*, int, const struct pw_properties*, size_t);
static int      (*p_pw_core_disconnect)            (struct pw_core*);
static struct pw_stream* (*p_pw_stream_new)        (struct pw_core*, const char*, struct pw_properties*);
static void     (*p_pw_stream_destroy)             (struct pw_stream*);
static int      (*p_pw_stream_connect)             (struct pw_stream*, enum pw_direction, uint32_t, enum pw_stream_flags, const struct spa_pod**, uint32_t);
static void     (*p_pw_stream_add_listener)        (struct pw_stream*, struct spa_hook*, const struct pw_stream_events*, void*);
static struct pw_buffer* (*p_pw_stream_dequeue_buffer)(struct pw_stream*);
static int      (*p_pw_stream_queue_buffer)        (struct pw_stream*, struct pw_buffer*);
static int      (*p_pw_stream_update_params)       (struct pw_stream*, const struct spa_pod**, uint32_t);

static bool kwin_libs_load(void)
{
    void* sbus = dlopen("libsystemd.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!sbus) { fprintf(stderr, "kwin: dlopen libsystemd: %s\n", dlerror()); return false; }

    void* pw = dlopen("libpipewire-0.3.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!pw) { fprintf(stderr, "kwin: dlopen libpipewire: %s\n", dlerror()); return false; }

#define LOAD_SBUS(name) p_##name = dlsym(sbus, #name); if (!p_##name) return false;
#define LOAD_PW(name)   p_##name = dlsym(pw,   #name); if (!p_##name) return false;

    LOAD_SBUS(sd_bus_open_user)
    LOAD_SBUS(sd_bus_unref)
    LOAD_SBUS(sd_bus_get_unique_name)
    LOAD_SBUS(sd_bus_get_property_trivial)
    LOAD_SBUS(sd_bus_add_match)
    LOAD_SBUS(sd_bus_call)
    LOAD_SBUS(sd_bus_wait)
    LOAD_SBUS(sd_bus_process)
    LOAD_SBUS(sd_bus_slot_unref)
    LOAD_SBUS(sd_bus_message_new_method_call)
    LOAD_SBUS(sd_bus_message_unref)
    LOAD_SBUS(sd_bus_message_read)
    LOAD_SBUS(sd_bus_message_read_basic)
    LOAD_SBUS(sd_bus_message_skip)
    LOAD_SBUS(sd_bus_message_enter_container)
    LOAD_SBUS(sd_bus_message_exit_container)
    LOAD_SBUS(sd_bus_message_open_container)
    LOAD_SBUS(sd_bus_message_close_container)
    LOAD_SBUS(sd_bus_message_append_basic)

    LOAD_PW(pw_init)
    LOAD_PW(pw_thread_loop_new)
    LOAD_PW(pw_thread_loop_destroy)
    LOAD_PW(pw_thread_loop_start)
    LOAD_PW(pw_thread_loop_stop)
    LOAD_PW(pw_thread_loop_lock)
    LOAD_PW(pw_thread_loop_unlock)
    LOAD_PW(pw_thread_loop_wait)
    LOAD_PW(pw_thread_loop_signal)
    LOAD_PW(pw_thread_loop_get_loop)
    LOAD_PW(pw_context_new)
    LOAD_PW(pw_context_connect_fd)
    LOAD_PW(pw_core_disconnect)
    LOAD_PW(pw_stream_new)
    LOAD_PW(pw_stream_destroy)
    LOAD_PW(pw_stream_connect)
    LOAD_PW(pw_stream_add_listener)
    LOAD_PW(pw_stream_dequeue_buffer)
    LOAD_PW(pw_stream_queue_buffer)
    LOAD_PW(pw_stream_update_params)

#undef LOAD_SBUS
#undef LOAD_PW

    return true;
}

typedef struct
{
    sd_bus* bus;

    const OutputInfo* outputs;
    uint32_t output_count;

    void* pixels[MAX_OUTPUTS];

    const char* session_handle;

    struct pw_thread_loop* pw_loop;
    struct pw_context* pw_context;
    struct pw_core* pw_core;
    struct pw_stream* pw_streams[MAX_OUTPUTS];
    struct spa_hook pw_listeners[MAX_OUTPUTS];
    uint32_t frames_done;
    int32_t strides[MAX_OUTPUTS];
} CaptureKwin;

typedef struct
{
    CaptureKwin* cap;
    uint32_t     index;
} StreamData;

static void stream_param_changed(void* userdata, uint32_t id, const struct spa_pod* param)
{
    StreamData* sd = userdata;
    CaptureKwin* cap = sd->cap;

    if (param == NULL || id != SPA_PARAM_Format) return;

    struct spa_video_info_raw info = {0};
    spa_format_video_raw_parse(param, &info);

    struct pw_stream* stream = cap->pw_streams[sd->index];
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
    p_pw_stream_update_params(stream, params, 1);
}

static void stream_process(void* userdata)
{
    StreamData* sd = userdata;
    CaptureKwin* cap = sd->cap;
    uint32_t index = sd->index;

    struct pw_buffer* pw_buf = p_pw_stream_dequeue_buffer(cap->pw_streams[index]);
    if (!pw_buf) return;

    struct spa_buffer* spa_buf = pw_buf->buffer;
    if (spa_buf->datas[0].data)
    {
        uint32_t stride = spa_buf->datas[0].chunk->stride;
        uint32_t size   = spa_buf->datas[0].chunk->size;
        void* pixels = malloc(size);
        if (pixels)
        {
            memcpy(pixels, spa_buf->datas[0].data, size);
            cap->pixels[index]  = pixels;
            cap->strides[index] = (int32_t)stride;
        }
    }

    p_pw_stream_queue_buffer(cap->pw_streams[index], pw_buf);

    cap->frames_done++;
    if (cap->frames_done >= cap->output_count)
        p_pw_thread_loop_signal(cap->pw_loop, false);
}

static const struct pw_stream_events stream_events =
{
    PW_VERSION_STREAM_EVENTS,
    .param_changed = stream_param_changed,
    .process       = stream_process,
};

typedef struct
{
    char* session_handle;
    bool  done;
} SessionResponse;

static int session_response_callback(sd_bus_message* m, void* userdata, sd_bus_error* err)
{
    (void)err;
    SessionResponse* response = userdata;

    uint32_t response_code = 0;
    p_sd_bus_message_read(m, "u", &response_code);
    if (response_code != 0)
    {
        response->done = true;
        return 1;
    }

    p_sd_bus_message_enter_container(m, 'a', "{sv}");
    while (p_sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char* key = NULL;
        p_sd_bus_message_read_basic(m, 's', &key);
        if (strcmp(key, "session_handle") == 0)
        {
            p_sd_bus_message_enter_container(m, 'v', "s");
            const char* handle = NULL;
            p_sd_bus_message_read_basic(m, 's', &handle);
            response->session_handle = strdup(handle);
            p_sd_bus_message_exit_container(m);
        }
        else
        {
            p_sd_bus_message_skip(m, "v");
        }
        p_sd_bus_message_exit_container(m);
    }
    p_sd_bus_message_exit_container(m);

    response->done = true;
    return 1;
}

typedef struct
{
    bool done;
    bool success;
} SimpleResponse;

static int simple_response_callback(sd_bus_message* m, void* userdata, sd_bus_error* err)
{
    (void)err;
    SimpleResponse* response = userdata;
    uint32_t response_code = 0;
    p_sd_bus_message_read(m, "u", &response_code);
    response->success = (response_code == 0);
    response->done = true;
    return 1;
}

typedef struct
{
    uint32_t node_ids[MAX_OUTPUTS];
    uint32_t node_count;
    char* restore_token;
    bool     done;
    bool     success;
} StartResponse;

static int start_response_callback(sd_bus_message* m, void* userdata, sd_bus_error* err)
{
    (void)err;
    StartResponse* response = userdata;

    uint32_t response_code = 0;
    p_sd_bus_message_read(m, "u", &response_code);
    if (response_code != 0)
    {
        response->done = true;
        return 1;
    }

    p_sd_bus_message_enter_container(m, 'a', "{sv}");
    while (p_sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char* key = NULL;
        p_sd_bus_message_read_basic(m, 's', &key);
        if (strcmp(key, "streams") == 0)
        {
            p_sd_bus_message_enter_container(m, 'v', "a(ua{sv})");
            p_sd_bus_message_enter_container(m, 'a', "(ua{sv})");
            while (p_sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0)
            {
                uint32_t node_id = 0;
                p_sd_bus_message_read_basic(m, 'u', &node_id);
                p_sd_bus_message_skip(m, "a{sv}");
                if (response->node_count < MAX_OUTPUTS)
                    response->node_ids[response->node_count++] = node_id;
                p_sd_bus_message_exit_container(m);
            }
            p_sd_bus_message_exit_container(m); // a
            p_sd_bus_message_exit_container(m); // v
        }
        else if (strcmp(key, "restore_token") == 0)
        {
            p_sd_bus_message_enter_container(m, 'v', "s");
            const char* token = NULL;
            p_sd_bus_message_read_basic(m, 's', &token);
            response->restore_token = strdup(token);
            p_sd_bus_message_exit_container(m);
        }
        else
        {
            p_sd_bus_message_skip(m, "v");
        }
        p_sd_bus_message_exit_container(m);
    }
    p_sd_bus_message_exit_container(m);

    response->success = true;
    response->done = true;
    return 1;
}

//Helpers

static char* read_restore_token(void)
{
    const char* home = getenv("HOME");
    if (!home) return NULL;

    char path[512] = {0};
    snprintf(path, sizeof(path), "%s%s", home, TOKEN_PATH);

    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    char token[256] = {0};
    if (!fgets(token, sizeof(token), f)) { fclose(f); return NULL; }
    fclose(f);

    size_t len = strlen(token);
    if (len > 0 && token[len - 1] == '\n') token[len - 1] = '\0';

    return strdup(token);
}

static void write_restore_token(const char* token)
{
    const char* home = getenv("HOME");
    if (!home) return;

    char path[512] = {0};
    snprintf(path, sizeof(path), "%s%s", home, TOKEN_PATH);

    char dir[512] = {0};
    snprintf(dir, sizeof(dir), "%s/.config/zoomer", home);
    mkdir(dir, 0755);

    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", token);
    fclose(f);
}

static char* parse_unique_name(const char* raw_name)
{
    if (raw_name == NULL) return NULL;
    uint32_t raw_name_size = strlen(raw_name);
    if (raw_name_size <= 4) return NULL;
    char buffer[256] = {0};
    if (raw_name[0] != ':') return NULL;

    uint32_t j = 0;
    for (uint32_t i = 1; i < raw_name_size; i++, j++)
    {
        if (raw_name[i] == '.')
        {
            buffer[j] = '_';
            continue;
        }
        buffer[j] = raw_name[i];
    }

    return strdup(buffer);
}

static char* create_request_path(sd_bus* bus)
{
    const char* request_path_prefix = "/org/freedesktop/portal/desktop/request/";
    const char* unique_name;
    int32_t ok = p_sd_bus_get_unique_name(bus, &unique_name);
    if (ok < 0) die("could not get unique sd bus name");
    char* parsed_name = parse_unique_name(unique_name);
    if (parsed_name == NULL) die("could not parse unique dbus name");

    char buffer[256] = {0};
    snprintf(buffer, sizeof(buffer), "%s%s/1", request_path_prefix, parsed_name);
    free(parsed_name);
    return strdup(buffer);
}

static char* create_match_rule(char* request_path)
{
    const char* match_rule_template = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path=";
    char buffer[1024] = {0};
    size_t wrote = snprintf(buffer, sizeof(buffer), "%s\'%s\'", match_rule_template, request_path);
    if (wrote != strlen(match_rule_template) + strlen(request_path) + 2)
        die("could not create match rule for dbus");
    return strdup(buffer);
}

static int32_t call_with_token(sd_bus* bus, const char* method, const char* session)
{
    sd_bus_message* msg = NULL;
    p_sd_bus_message_new_method_call(bus, &msg, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, method);
    if (session)
        p_sd_bus_message_append_basic(msg, 'o', session);
    p_sd_bus_message_open_container(msg, 'a', "{sv}");
    // entry 1
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "handle_token");
    p_sd_bus_message_open_container(msg, 'v', "s");
    p_sd_bus_message_append_basic(msg, 's', "1");
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    // entry 2
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "session_handle_token");
    p_sd_bus_message_open_container(msg, 'v', "s");
    p_sd_bus_message_append_basic(msg, 's', "1");
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    int32_t ok = p_sd_bus_call(bus, msg, 0, NULL, NULL);
    p_sd_bus_message_unref(msg);
    return ok;
}

static char* portal_create_session(sd_bus* bus)
{
    char* request_path = create_request_path(bus);
    char* match_rule = create_match_rule(request_path);

    sd_bus_slot* slot = NULL;
    SessionResponse response = {0};
    p_sd_bus_add_match(bus, &slot, match_rule, session_response_callback, &response);
    int32_t ok = call_with_token(bus, "CreateSession", NULL);
    if (ok < 0) die("CreateSession failed: %s", strerror(-ok));

    while (!response.done)
    {
        p_sd_bus_wait(bus, UINT64_MAX);
        p_sd_bus_process(bus, NULL);
    }

    if (response.session_handle == NULL) die("could not get dbus session handle");

    free(request_path);
    free(match_rule);
    p_sd_bus_slot_unref(slot);

    return response.session_handle;
}

static void portal_select_sources(sd_bus* bus, const char* session_handle)
{
    char* request_path = create_request_path(bus);
    char* match_rule = create_match_rule(request_path);
    char* restore_token = read_restore_token();

    sd_bus_slot* slot = NULL;
    SimpleResponse response = {0};
    p_sd_bus_add_match(bus, &slot, match_rule, simple_response_callback, &response);

    sd_bus_message* msg = NULL;
    p_sd_bus_message_new_method_call(bus, &msg, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, "SelectSources");
    p_sd_bus_message_append_basic(msg, 'o', session_handle);
    p_sd_bus_message_open_container(msg, 'a', "{sv}");
    // handle_token
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "handle_token");
    p_sd_bus_message_open_container(msg, 'v', "s");
    p_sd_bus_message_append_basic(msg, 's', "1");
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    // session_handle_token
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "session_handle_token");
    p_sd_bus_message_open_container(msg, 'v', "s");
    p_sd_bus_message_append_basic(msg, 's', "1");
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    // types = MONITOR (1)
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "types");
    p_sd_bus_message_open_container(msg, 'v', "u");
    p_sd_bus_message_append_basic(msg, 'u', &(uint32_t){1});
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    // multiple = true
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "multiple");
    p_sd_bus_message_open_container(msg, 'v', "b");
    p_sd_bus_message_append_basic(msg, 'b', &(int){1});
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    // restore_token
    if (restore_token)
    {
        p_sd_bus_message_open_container(msg, 'e', "sv");
        p_sd_bus_message_append_basic(msg, 's', "restore_token");
        p_sd_bus_message_open_container(msg, 'v', "s");
        p_sd_bus_message_append_basic(msg, 's', restore_token);
        p_sd_bus_message_close_container(msg);
        p_sd_bus_message_close_container(msg);

        p_sd_bus_message_open_container(msg, 'e', "sv");
        p_sd_bus_message_append_basic(msg, 's', "persist_mode");
        p_sd_bus_message_open_container(msg, 'v', "u");
        p_sd_bus_message_append_basic(msg, 'u', &(uint32_t){2});
        p_sd_bus_message_close_container(msg);
        p_sd_bus_message_close_container(msg);

        free(restore_token);
    }
    else
    {
        p_sd_bus_message_open_container(msg, 'e', "sv");
        p_sd_bus_message_append_basic(msg, 's', "persist_mode");
        p_sd_bus_message_open_container(msg, 'v', "u");
        p_sd_bus_message_append_basic(msg, 'u', &(uint32_t){2});
        p_sd_bus_message_close_container(msg);
        p_sd_bus_message_close_container(msg);
    }
    p_sd_bus_message_close_container(msg);

    int32_t ok = p_sd_bus_call(bus, msg, 0, NULL, NULL);
    p_sd_bus_message_unref(msg);
    if (ok < 0) die("SelectSources failed: %s", strerror(-ok));

    while (!response.done)
    {
        p_sd_bus_wait(bus, UINT64_MAX);
        p_sd_bus_process(bus, NULL);
    }

    free(request_path);
    free(match_rule);
    p_sd_bus_slot_unref(slot);

    if (!response.success) die("SelectSources rejected by portal");
}

static void portal_start(sd_bus* bus, const char* session_handle, uint32_t* node_ids, uint32_t* node_count)
{
    char* request_path = create_request_path(bus);
    char* match_rule = create_match_rule(request_path);

    sd_bus_slot* slot = NULL;
    StartResponse response = {0};
    p_sd_bus_add_match(bus, &slot, match_rule, start_response_callback, &response);

    sd_bus_message* msg = NULL;
    p_sd_bus_message_new_method_call(bus, &msg, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, "Start");
    p_sd_bus_message_append_basic(msg, 'o', session_handle);
    p_sd_bus_message_append_basic(msg, 's', "");
    p_sd_bus_message_open_container(msg, 'a', "{sv}");
    // handle_token
    p_sd_bus_message_open_container(msg, 'e', "sv");
    p_sd_bus_message_append_basic(msg, 's', "handle_token");
    p_sd_bus_message_open_container(msg, 'v', "s");
    p_sd_bus_message_append_basic(msg, 's', "1");
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);
    p_sd_bus_message_close_container(msg);

    int32_t ok = p_sd_bus_call(bus, msg, 0, NULL, NULL);
    p_sd_bus_message_unref(msg);
    if (ok < 0) die("Start failed: %s", strerror(-ok));

    while (!response.done)
    {
        p_sd_bus_wait(bus, UINT64_MAX);
        p_sd_bus_process(bus, NULL);
    }

    if (response.restore_token)
    {
        write_restore_token(response.restore_token);
        free(response.restore_token);
    }

    free(request_path);
    free(match_rule);
    p_sd_bus_slot_unref(slot);

    if (!response.success) die("Start rejected by portal");

    memcpy(node_ids, response.node_ids, response.node_count * sizeof(uint32_t));
    *node_count = response.node_count;
}

static int portal_open_pipewire_remote(sd_bus* bus, const char* session_handle)
{
    sd_bus_message* msg = NULL;
    sd_bus_message* reply = NULL;

    p_sd_bus_message_new_method_call(bus, &msg, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, "OpenPipeWireRemote");
    p_sd_bus_message_append_basic(msg, 'o', session_handle);
    p_sd_bus_message_open_container(msg, 'a', "{sv}");
    p_sd_bus_message_close_container(msg);

    int32_t ok = p_sd_bus_call(bus, msg, 0, NULL, &reply);
    p_sd_bus_message_unref(msg);
    if (ok < 0) die("OpenPipeWireRemote failed: %s", strerror(-ok));

    int fd = -1;
    p_sd_bus_message_read(reply, "h", &fd);
    fd = dup(fd);
    p_sd_bus_message_unref(reply);

    if (fd < 0) die("OpenPipeWireRemote returned invalid fd");
    return fd;
}

static void pipewire_init(CaptureKwin* cap, int pw_fd)
{
    p_pw_init(NULL, NULL);

    cap->pw_loop = p_pw_thread_loop_new(NULL, NULL);
    if (!cap->pw_loop) die("failed to create pipewire thread loop");

    struct pw_context* ctx = p_pw_context_new(p_pw_thread_loop_get_loop(cap->pw_loop), NULL, 0);
    if (!ctx) die("failed to create pipewire context");

    p_pw_thread_loop_start(cap->pw_loop);

    p_pw_thread_loop_lock(cap->pw_loop);
    cap->pw_core = p_pw_context_connect_fd(ctx, pw_fd, NULL, 0);
    if (!cap->pw_core) die("failed to connect pipewire core");
    p_pw_thread_loop_unlock(cap->pw_loop);
}

static void pipewire_create_streams(CaptureKwin* cap, uint32_t* node_ids, uint32_t node_count)
{
    p_pw_thread_loop_lock(cap->pw_loop);

    for (uint32_t i = 0; i < node_count; i++)
    {
        StreamData* sd = calloc(1, sizeof(StreamData));
        sd->cap   = cap;
        sd->index = i;

        cap->pw_streams[i] = p_pw_stream_new(cap->pw_core, "zoomer", NULL);
        if (!cap->pw_streams[i]) die("failed to create pipewire stream %u", i);

        p_pw_stream_add_listener(cap->pw_streams[i], &cap->pw_listeners[i], &stream_events, sd);

        uint8_t buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* params[1];
        params[0] = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx));

        p_pw_stream_connect(cap->pw_streams[i],
            PW_DIRECTION_INPUT,
            node_ids[i],
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
            params, 1);
    }

    p_pw_thread_loop_unlock(cap->pw_loop);
}

//Interface
static void kwin_grab(void* backend)
{
    CaptureKwin* cap = backend;
    char* session = portal_create_session(cap->bus);
    portal_select_sources(cap->bus, session);

    uint32_t node_ids[MAX_OUTPUTS] = {0};
    uint32_t node_count = 0;
    portal_start(cap->bus, session, node_ids, &node_count);

    cap->session_handle = session;

    int pw_fd = portal_open_pipewire_remote(cap->bus, session);
    pipewire_init(cap, pw_fd);

    pipewire_create_streams(cap, node_ids, node_count);

    p_pw_thread_loop_lock(cap->pw_loop);
    while (cap->frames_done < node_count)
        p_pw_thread_loop_wait(cap->pw_loop);
    p_pw_thread_loop_unlock(cap->pw_loop);
}

static const void* kwin_output_pixels(void* backend, uint32_t index)
{
    CaptureKwin* cap = backend;
    if (index >= cap->output_count) return NULL;
    if (!cap->pixels[index]) return NULL;
    return cap->pixels[index];
}

static void kwin_destroy(void* backend)
{
    CaptureKwin* cap = backend;

    if (cap->pw_loop) p_pw_thread_loop_stop(cap->pw_loop);

    for (uint32_t i = 0; i < cap->output_count; i++)
    {
        if (cap->pw_streams[i])
            p_pw_stream_destroy(cap->pw_streams[i]);
    }

    if (cap->pw_core) p_pw_core_disconnect(cap->pw_core);
    if (cap->pw_loop) p_pw_thread_loop_destroy(cap->pw_loop);

    for (uint32_t i = 0; i < cap->output_count; i++)
        if (cap->pixels[i]) free(cap->pixels[i]);

    if (cap->session_handle) free((void*)cap->session_handle);
    if (cap->bus) p_sd_bus_unref(cap->bus);

    free(cap);
}

const CaptureImpl capture_kwin_impl =
{
    .grab = kwin_grab,
    .output_pixels = kwin_output_pixels,
    .destroy = kwin_destroy,
};

bool capture_kwin_available(void)
{
    if (!kwin_libs_load()) return false;

    sd_bus* bus = NULL;
    int32_t ok = p_sd_bus_open_user(&bus);
    if (ok < 0) return false;

    uint32_t flags = 0;
    ok = p_sd_bus_get_property_trivial(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, "AvailableSourceTypes", NULL, 'u', (void*)(&flags));
    p_sd_bus_unref(bus);
    if (ok < 0) return false;

    return (flags & 1) != 0;
}

void* capture_kwin_create(const OutputInfo* outputs, uint32_t output_count)
{
    CaptureKwin* cap = calloc(1, sizeof(CaptureKwin));
    cap->outputs = outputs;
    cap->output_count = output_count;

    sd_bus* bus = NULL;
    int32_t ok = p_sd_bus_open_user(&bus);
    if (ok < 0) die("failed to open session bus for kwin capture");
    cap->bus = bus;

    return cap;
}
