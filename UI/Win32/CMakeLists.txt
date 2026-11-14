add_library(CarPlayUI STATIC
    VideoAdaptationHelper.cpp
    D3D11Renderer.cpp
    MainWindow.cpp
    SystemPermissionHelper.cpp
    ThemeHelper.cpp
    SetupWizard.cpp
    OverlayRenderer.cpp
    Win32InputRouter.cpp
    AppController.cpp
)

target_include_directories(CarPlayUI PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayUI PUBLIC
    CarPlayApp
    d3d11
    dxgi
    d3dcompiler
    d2d1
    dwrite
    dwmapi
)

target_compile_features(CarPlayUI PUBLIC cxx_std_20)

add_executable(CarPlayStation
    main.cpp
    CarPlayStation.rc
)

target_include_directories(CarPlayStation PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation PRIVATE
    CarPlayUI
    dbghelp
)

target_compile_features(CarPlayStation PRIVATE cxx_std_20)

if(WIN32)
    # Win32 subsystem configuration (no console window pop-up).
    # Disable linker auto-generated manifest so it does not collide with CarPlayStation.rc (ID=1).
    set_target_properties(CarPlayStation PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    if(MSVC)
        target_link_options(CarPlayStation PRIVATE /MANIFEST:NO)
    endif()
endif()
