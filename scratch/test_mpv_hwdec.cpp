#include <windows.h>
#include <iostream>

typedef void* (*mpv_create_fn)();
typedef int (*mpv_initialize_fn)(void*);
typedef int (*mpv_set_option_string_fn)(void*, const char*, const char*);
typedef char* (*mpv_get_property_string_fn)(void*, const char*);
typedef void (*mpv_free_fn)(void*);
typedef void (*mpv_destroy_fn)(void*);

int main() {
    HMODULE hMpv = LoadLibraryA("D:\\CarPlayStation\\build\\UI\\Win32\\Release\\libmpv-2.dll");
    if (!hMpv) {
        std::cout << "Failed to load libmpv-2.dll, error: " << GetLastError() << std::endl;
        return 1;
    }

    auto mpv_create = (mpv_create_fn)GetProcAddress(hMpv, "mpv_create");
    auto mpv_initialize = (mpv_initialize_fn)GetProcAddress(hMpv, "mpv_initialize");
    auto mpv_set_option_string = (mpv_set_option_string_fn)GetProcAddress(hMpv, "mpv_set_option_string");
    auto mpv_get_property_string = (mpv_get_property_string_fn)GetProcAddress(hMpv, "mpv_get_property_string");
    auto mpv_free = (mpv_free_fn)GetProcAddress(hMpv, "mpv_free");
    auto mpv_destroy = (mpv_destroy_fn)GetProcAddress(hMpv, "mpv_destroy");

    void* mpv = mpv_create();
    std::cout << "mpv_create: " << mpv << std::endl;

    std::cout << "Setting vo=libmpv: " << mpv_set_option_string(mpv, "vo", "libmpv") << std::endl;
    std::cout << "Setting hwdec=auto-copy: " << mpv_set_option_string(mpv, "hwdec", "auto-copy") << std::endl;
    std::cout << "Setting hwdec=d3d11va-copy: " << mpv_set_option_string(mpv, "hwdec", "d3d11va-copy") << std::endl;

    int r = mpv_initialize(mpv);
    std::cout << "mpv_initialize: " << r << std::endl;

    char* hwdec = mpv_get_property_string(mpv, "hwdec");
    std::cout << "property hwdec: " << (hwdec ? hwdec : "null") << std::endl;
    if (hwdec) mpv_free(hwdec);

    char* hwdec_current = mpv_get_property_string(mpv, "hwdec-current");
    std::cout << "property hwdec-current: " << (hwdec_current ? hwdec_current : "null") << std::endl;
    if (hwdec_current) mpv_free(hwdec_current);

    char* hwdec_interop = mpv_get_property_string(mpv, "hwdec-interop");
    std::cout << "property hwdec-interop: " << (hwdec_interop ? hwdec_interop : "null") << std::endl;
    if (hwdec_interop) mpv_free(hwdec_interop);

    mpv_destroy(mpv);
    FreeLibrary(hMpv);
    return 0;
}

