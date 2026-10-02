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
    Media/AirPlayVideoDemuxer.cpp
    Media/AirPlayVideoReceiver.cpp
    Media/MfVideoDecoder.cpp
    Media/FFmpegVideoDecoder.cpp
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
