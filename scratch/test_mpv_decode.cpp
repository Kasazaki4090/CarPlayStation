#include <windows.h>
#include <iostream>
#include <vector>

typedef void* (*mpv_create_fn)();
typedef int (*mpv_initialize_fn)(void*);
typedef int (*mpv_set_option_string_fn)(void*, const char*, const char*);
typedef char* (*mpv_get_property_string_fn)(void*, const char*);
typedef void (*mpv_free_fn)(void*);
typedef void (*mpv_destroy_fn)(void*);
typedef int (*mpv_request_log_messages_fn)(void*, const char*);
typedef struct mpv_event* (*mpv_wait_event_fn)(void*, double);
typedef int (*mpv_render_context_create_fn)(void**, void*, void*);
typedef void (*mpv_render_context_free_fn)(void*);
typedef int (*mpv_command_string_fn)(void*, const char*);

struct mpv_event_log_message {
    const char* prefix;
    const char* level;
    const char* text;
    int log_level;
};

struct mpv_event {
    int event_id;
    int error;
    uint64_t reply_userdata;
    void* data;
};

struct mpv_render_param {
    int type;
    void* data;
};

int main() {
    HMODULE hMpv = LoadLibraryA("D:\\CarPlayStation\\build\\UI\\Win32\\Release\\libmpv-2.dll");
    if (!hMpv) {
        std::cout << "Failed to load libmpv-2.dll" << std::endl;
        return 1;
    }

    auto mpv_create = (mpv_create_fn)GetProcAddress(hMpv, "mpv_create");
    auto mpv_initialize = (mpv_initialize_fn)GetProcAddress(hMpv, "mpv_initialize");
    auto mpv_set_option_string = (mpv_set_option_string_fn)GetProcAddress(hMpv, "mpv_set_option_string");
    auto mpv_get_property_string = (mpv_get_property_string_fn)GetProcAddress(hMpv, "mpv_get_property_string");
    auto mpv_free = (mpv_free_fn)GetProcAddress(hMpv, "mpv_free");
    auto mpv_destroy = (mpv_destroy_fn)GetProcAddress(hMpv, "mpv_destroy");
    auto mpv_request_log_messages = (mpv_request_log_messages_fn)GetProcAddress(hMpv, "mpv_request_log_messages");
    auto mpv_wait_event = (mpv_wait_event_fn)GetProcAddress(hMpv, "mpv_wait_event");
    auto mpv_render_context_create = (mpv_render_context_create_fn)GetProcAddress(hMpv, "mpv_render_context_create");
    auto mpv_render_context_free = (mpv_render_context_free_fn)GetProcAddress(hMpv, "mpv_render_context_free");
    auto mpv_command_string = (mpv_command_string_fn)GetProcAddress(hMpv, "mpv_command_string");

    void* mpv = mpv_create();
    mpv_request_log_messages(mpv, "v");

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto-copy");
    mpv_set_option_string(mpv, "untimed", "yes");
    mpv_set_option_string(mpv, "audio", "no");

    int r = mpv_initialize(mpv);
    std::cout << "mpv_initialize: " << r << std::endl;

    const char* apiType = "sw";
    mpv_render_param params[2];
    params[0].type = 1; // MPV_RENDER_PARAM_API_TYPE
    params[0].data = (void*)apiType;
    params[1].type = 0;

    void* render_ctx = nullptr;
    r = mpv_render_context_create(&render_ctx, mpv, params);
    std::cout << "mpv_render_context_create: " << r << std::endl;

    // Check events / log messages
    while (true) {
        mpv_event* ev = mpv_wait_event(mpv, 0.0);
        if (!ev || ev->event_id == 0) break; // MPV_EVENT_NONE = 0
        if (ev->event_id == 17) { // MPV_EVENT_LOG_MESSAGE
            auto* msg = (mpv_event_log_message*)ev->data;
            std::cout << "[" << msg->prefix << "] (" << msg->level << ") " << msg->text;
        }
    }

    if (render_ctx) mpv_render_context_free(render_ctx);
    mpv_destroy(mpv);
    FreeLibrary(hMpv);
    return 0;
}

