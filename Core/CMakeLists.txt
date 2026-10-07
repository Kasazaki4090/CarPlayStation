add_library(CarPlayCore STATIC
    Hardware/RemoteMfiHttpClient.cpp
    Hardware/FileMfiSigner.cpp
    Hardware/Ch341I2cAdapter.cpp
    Hardware/Ch347I2cAdapter.cpp
    Hardware/FtdiI2cAdapter.cpp
    Hardware/I2cMfiSigner.cpp
    Hardware/RadioAdapterHelper.cpp
    Hardware/TouchscreenCapabilities.cpp
    Transport/MockTransportChannel.cpp
    Transport/BtSdpHelper.cpp
    Transport/BtRfcommTransport.cpp
    Transport/WifiSoftApManager.cpp
    Transport/MdnsPacketBuilder.cpp
    Transport/MdnsAdvertiser.cpp
    Protocol/Iap2LinkEngine.cpp
    Protocol/Iap2MessageCatalog.cpp
    Protocol/Iap2ControlPlane.cpp
    Protocol/Iap2SessionManager.cpp
    Session/PairingStore.cpp
    Session/AccessoryIdentityStore.cpp
    Session/AirPlayRtspCodec.cpp
    Session/AirPlayRtspServer.cpp
    Session/CarPlaySessionStateMachine.cpp
    Session/Pairing/ed25519/fe.c
    Session/Pairing/ed25519/ge.c
    Session/Pairing/ed25519/sc.c
    Session/Pairing/ed25519/keypair.c
    Session/Pairing/ed25519/seed.c
    Session/Pairing/ed25519/sign.c
    Session/Pairing/ed25519/verify.c
    Session/Pairing/ed25519/sha512.c
    Media/AirPlayVideoDemuxer.cpp
    Media/AirPlayVideoReceiver.cpp
    Media/MpvVideoPlayer.cpp
    Media/AacCodecAdapter.cpp
    Media/AirPlayAudioDemuxer.cpp
    Media/AirPlayAudioReceiver.cpp
    Media/AudioDuckingManager.cpp
    Media/WasapiAudioPipeline.cpp
    Media/WasapiMicrophoneUplink.cpp
    Media/MicrophoneRtpSender.cpp
    Input/MouseTouchTranslator.cpp
    Input/KeyboardKnobTranslator.cpp
    Input/MultiTouchInjector.cpp
    Service/CoreEventBus.cpp
    Service/CarPlayCoreService.cpp
)

target_include_directories(CarPlayCore PUBLIC
    ${CMAKE_SOURCE_DIR}
)

# libmpv (mpv's embeddable C library) — loaded dynamically via LoadLibrary at
# runtime; only the headers are needed at compile time.
if(WIN32)
    target_include_directories(CarPlayCore PUBLIC
        ${CMAKE_SOURCE_DIR}/Lib/mpv/include
    )
    # Copy the libmpv DLL next to the executable so LoadLibrary can find it.
    add_custom_command(TARGET CarPlayCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/Lib/mpv/libmpv-2.dll
            $<TARGET_FILE_DIR:CarPlayStation>
        COMMENT "Copying libmpv-2.dll to output directory"
    )

    # Vendor driver DLLs (Lib/Readme.md: wch-ch341, ftdi-d2xx). The I2C adapters
    # load these with LoadLibrary; DriverStore folders are not on the default DLL
    # search path, so the copies next to the executable are what get picked up.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CPS_DRIVER_ARCH "x64")
    else()
        set(CPS_DRIVER_ARCH "Win32")
    endif()

    foreach(_cps_dll CH341DLLA64.DLL CH341DLL.DLL CH347DLLA64.DLL CH347DLL.DLL)
        add_custom_command(TARGET CarPlayCore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/Lib/wch-ch341/${_cps_dll}
                $<TARGET_FILE_DIR:CarPlayStation>
            COMMENT "Copying ${_cps_dll} to output directory"
        )
    endforeach()

    foreach(_cps_dll libmpsse.dll ftd2xx.dll)
        add_custom_command(TARGET CarPlayCore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/Lib/ftdi-d2xx/${CPS_DRIVER_ARCH}/${_cps_dll}
                $<TARGET_FILE_DIR:CarPlayStation>
            COMMENT "Copying ${_cps_dll} (${CPS_DRIVER_ARCH}) to output directory"
        )
    endforeach()
endif()

target_compile_features(CarPlayCore PUBLIC cxx_std_20)

# fdk-aac: AAC-LC decode (main media audio) + AAC-ELD decode/encode (Siri/telephony
# downlink and microphone uplink). Hidden behind Core/Media/AacCodecAdapter.{h,cpp}
# so no fdk-aac header reaches the rest of Core (Lib/Readme.md rule 9).
target_link_libraries(CarPlayCore PUBLIC FDK-AAC::fdk-aac)

if(WIN32)
    target_link_libraries(CarPlayCore PUBLIC ws2_32 bcrypt windowsapp)
    # D3D11 shared surface used to hand decoded frames to the renderer (DEC-009).
    target_link_libraries(CarPlayCore PUBLIC d3d11 dxgi)
    # WASAPI shared-mode audio pipeline + microphone uplink (DEC-008).
    target_link_libraries(CarPlayCore PUBLIC ole32 oleaut32 mmdevapi ksuser)
endif()
