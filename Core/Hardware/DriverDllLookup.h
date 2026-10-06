#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CarPlayStation::Core::Hardware {

// Build the ordered list of candidate paths for a vendor driver DLL.
//
// Vendor installers (WCH, FTDI) register the kernel driver but leave the user
// mode DLL either in System32, in the executable's directory, or in
// DriverStore\FileRepository\<inf>_*, which is NOT part of the default DLL
// search path. A bare LoadLibraryA() therefore fails on a clean install, so
// callers probe: bare name (default search path), the executable directory,
// System32, and any DriverStore folder matching the given INF prefix.
//
// `names` must be ordered by preference (e.g. the 64-bit build first on x64,
// since a 32-bit DLL cannot be loaded into a 64-bit process).
inline std::vector<std::string> BuildDriverDllCandidates(
        const std::vector<std::string>& names,
        const std::vector<std::string>& driver_store_inf_prefixes = {}) {
    std::vector<std::string> candidates;
    if (names.empty()) return candidates;

#ifdef _WIN32
    auto wideToUtf8 = [](const wchar_t* w) {
        if (!w || !*w) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
        return out;
    };

    std::vector<std::string> dirs;
    dirs.push_back(""); // bare name -> default search path (exe dir, System32, ...)

    // Directory of the running executable (where the build copies vendored DLLs).
    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            std::string full = wideToUtf8(exePath);
            auto pos = full.find_last_of("/\\");
            if (pos != std::string::npos) {
                dirs.push_back(full.substr(0, pos));
            }
        }
    }

    // System32 (the FTDI D2XX installer puts ftd2xx.dll here).
    {
        wchar_t sysPath[MAX_PATH]{};
        if (GetSystemDirectoryW(sysPath, MAX_PATH) > 0) {
            dirs.push_back(wideToUtf8(sysPath));
        }
    }

    // DriverStore FileRepository folders for the given INF names.
    {
        static const char kFileRepository[] = "C:\\Windows\\System32\\DriverStore\\FileRepository";
        for (const auto& inf : driver_store_inf_prefixes) {
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA((std::string(kFileRepository) + "\\" + inf + ".*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (fd.cFileName[0] == '.') continue;
                dirs.push_back(std::string(kFileRepository) + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    for (const auto& dir : dirs) {
        for (const auto& name : names) {
            if (dir.empty()) {
                candidates.push_back(name);
            } else {
                candidates.push_back(dir + "\\" + name);
            }
        }
    }
#else
    candidates = names;
#endif
    return candidates;
}

} // namespace CarPlayStation::Core::Hardware
