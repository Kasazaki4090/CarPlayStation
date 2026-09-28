cmake_minimum_required(VERSION 3.20)

project(CarPlayStation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All headers are included relative to the repository root ("Core/...", "Application/...")
if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

add_subdirectory(Core)
add_subdirectory(Application)
add_subdirectory(UI/Win32)
add_subdirectory(Tests)
