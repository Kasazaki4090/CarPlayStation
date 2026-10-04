cmake_minimum_required(VERSION 3.20)

project(CarPlayStation LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All headers are included relative to the repository root ("Core/...", "Application/...")
if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    add_compile_options(/utf-8 /W4 /permissive- /Zi)
    add_link_options(/DEBUG)
else()
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

# fdk-aac (Fraunhofer FDK AAC) — git submodule under Lib/ for the CarPlay audio
# downlink (AAC-LC / AAC-ELD decode) and microphone uplink (AAC-ELD encode).
# Built as a static library so no extra runtime DLL is required (Lib/Readme.md rule 7).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(Lib/fdk-aac EXCLUDE_FROM_ALL)
# Third-party C sources: keep the warning level below the project's own /W4.
if(MSVC)
    target_compile_options(fdk-aac PRIVATE /W3)
endif()

add_subdirectory(Core)
add_subdirectory(Application)
add_subdirectory(UI/Win32)
add_subdirectory(Tests)
