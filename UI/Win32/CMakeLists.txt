add_library(CarPlayUI STATIC
    VideoAdaptationHelper.cpp
    D3D11Renderer.cpp
    MainWindow.cpp
    SystemPermissionHelper.cpp
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
)

target_include_directories(CarPlayStation PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation PRIVATE
    CarPlayUI
)

target_compile_features(CarPlayStation PRIVATE cxx_std_20)

# CMake auto-embeds its own default manifest (resource ID 1) at link time.
# Replace it with the UAC manifest (requireAdministrator) so the AF_BTH RFCOMM
# Bluetooth server socket can bind. This is done post-build with mt.exe, which
# avoids the duplicate-resource (CVT1100) error that a .rc RT_MANIFEST would
# cause alongside CMake's embedded manifest.
add_custom_command(TARGET CarPlayStation POST_BUILD
    COMMAND ${CMAKE_MT} -manifest ${CMAKE_CURRENT_SOURCE_DIR}/CarPlayStation.manifest
            -outputresource:$<TARGET_FILE:CarPlayStation>\;1
    COMMENT "Embedding UAC manifest (requireAdministrator) into CarPlayStation.exe"
)

if(WIN32)
    # Win32 subsystem configuration (no console window pop-up).
    # ENABLE_EMBED_MANIFESTS OFF stops CMake from auto-embedding its own
    # manifest (ID=1); the UAC manifest is provided by CarPlayStation.rc
    # instead, otherwise the two collide (CVT1100 duplicate resource).
    set_target_properties(CarPlayStation PROPERTIES
        WIN32_EXECUTABLE TRUE
        ENABLE_EMBED_MANIFESTS OFF
    )
endif()
