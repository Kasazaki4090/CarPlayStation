add_library(CarPlayCore STATIC
    Hardware/NetworkMfiClient.cpp
    Hardware/FileMfiSigner.cpp
    Hardware/Ch341I2cAdapter.cpp
    Hardware/Ch347I2cAdapter.cpp
    Hardware/FtdiI2cAdapter.cpp
    Hardware/I2cMfiSigner.cpp
    Hardware/RadioAdapterHelper.cpp
    Transport/MockTransportChannel.cpp
    Transport/BtSdpHelper.cpp
    Transport/BtRfcommTransport.cpp
    Transport/WifiSoftApManager.cpp
    Transport/MdnsPacketBuilder.cpp
    Transport/MdnsAdvertiser.cpp
    Protocol/Iap2LinkEngine.cpp
    Protocol/Iap2MessageCatalog.cpp
    Protocol/Iap2SessionManager.cpp
    Session/PairingStore.cpp
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
    Media/MfVideoDecoder.cpp
    Media/FFmpegVideoDecoder.cpp
    Media/MpvVideoPlayer.cpp
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
endif()

target_compile_features(CarPlayCore PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(CarPlayCore PUBLIC ws2_32 bcrypt windowsapp)
    # Media Foundation hardware video decoder + D3D11 shared surfaces (DEC-008).
    target_link_libraries(CarPlayCore PUBLIC mfplat mfuuid d3d11 dxgi)
    # WASAPI shared-mode audio pipeline + microphone uplink (DEC-008).
    target_link_libraries(CarPlayCore PUBLIC ole32 oleaut32 mmdevapi ksuser)
endif()

# Automated FFmpeg sync (x64 / ARM64) - zero-dependency fallback when absent.
include(${CMAKE_SOURCE_DIR}/cmake/FindOrDownloadFFmpeg.cmake)
