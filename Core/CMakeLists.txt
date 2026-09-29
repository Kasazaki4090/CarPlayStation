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
)

target_include_directories(CarPlayCore PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(CarPlayCore PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(CarPlayCore PUBLIC ws2_32 bcrypt)
endif()
