#include <windows.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

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
typedef int (*mpv_stream_cb_add_ro_fn)(void*, const char*, void*, void*);

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

struct mpv_stream_cb_info {
    void* cookie;
    int64_t (*read_fn)(void* cookie, char* buf, uint64_t nbytes);
    void (*close_fn)(void* cookie);
    int64_t (*seek_fn)(void* cookie, int64_t offset);
    int64_t (*size_fn)(void* cookie);
    void (*cancel_fn)(void* cookie);
};

static int64_t stream_read(void* cookie, char* buf, uint64_t nbytes) {
    // Return 0 for EOF or sleep
    return 0;
}
static void stream_close(void* cookie) {}

static int stream_open(void* user_data, char* uri, mpv_stream_cb_info* info) {
    info->cookie = user_data;
    info->read_fn = stream_read;
    info->close_fn = stream_close;
    info->seek_fn = nullptr;
    info->size_fn = nullptr;
    info->cancel_fn = nullptr;
    return 0;
}

int main() {
    HMODULE hMpv = LoadLibraryA("D:\\CarPlayStation\\build\\UI\\Win32\\Release\\libmpv-2.dll");
    if (!hMpv) {
        std::cout << "Failed to load libmpv-2.dll\n";
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
    auto mpv_stream_cb_add_ro = (mpv_stream_cb_add_ro_fn)GetProcAddress(hMpv, "mpv_stream_cb_add_ro");

    void* mpv = mpv_create();
    mpv_request_log_messages(mpv, "v");

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto-copy");
    mpv_set_option_string(mpv, "demuxer-lavf-format", "h264");
    mpv_set_option_string(mpv, "untimed", "yes");
    mpv_set_option_string(mpv, "cache", "no");
    mpv_set_option_string(mpv, "audio", "no");

    int r = mpv_initialize(mpv);
    std::cout << "mpv_initialize: " << r << std::endl;

    r = mpv_stream_cb_add_ro(mpv, "test-stream", nullptr, (void*)stream_open);
    std::cout << "stream_cb_add_ro: " << r << std::endl;

    const char* apiType = "sw";
    mpv_render_param params[2];
    params[0].type = 1;
    params[0].data = (void*)apiType;
    params[1].type = 0;

    void* render_ctx = nullptr;
    r = mpv_render_context_create(&render_ctx, mpv, params);
    std::cout << "render_context_create: " << r << std::endl;

    mpv_command_string(mpv, "loadfile test-stream://live");

    for (int i = 0; i < 20; ++i) {
        while (true) {
            mpv_event* ev = mpv_wait_event(mpv, 0.05);
            if (!ev || ev->event_id == 0) break;
            if (ev->event_id == 17) {
                auto* msg = (mpv_event_log_message*)ev->data;
                std::cout << "[" << msg->prefix << "] (" << msg->level << ") " << msg->text;
            }
        }
    }

    char* hwdec_cur = mpv_get_property_string(mpv, "hwdec-current");
    std::cout << "Final hwdec-current: " << (hwdec_cur ? hwdec_cur : "null") << std::endl;
    if (hwdec_cur) mpv_free(hwdec_cur);

    char* dec = mpv_get_property_string(mpv, "decoder");
    std::cout << "Final decoder: " << (dec ? dec : "null") << std::endl;
    if (dec) mpv_free(dec);

    if (render_ctx) mpv_render_context_free(render_ctx);
    mpv_destroy(mpv);
    FreeLibrary(hMpv);
    return 0;
}

