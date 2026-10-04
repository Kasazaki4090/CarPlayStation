add_executable(CarPlayStation_Tests
    CommonTests.cpp
)

target_include_directories(CarPlayStation_Tests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_Tests PRIVATE
    CarPlayApp
)

target_compile_features(CarPlayStation_Tests PRIVATE cxx_std_20)

add_test(NAME CommonTests COMMAND CarPlayStation_Tests)

# Hardware and MFi tests
add_executable(CarPlayStation_HardwareTests
    HardwareTests.cpp
)

target_include_directories(CarPlayStation_HardwareTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_HardwareTests PRIVATE
    CarPlayCore
)

add_test(NAME HardwareTests COMMAND CarPlayStation_HardwareTests)

# Transport layer tests
add_executable(CarPlayStation_TransportTests
    TransportTests.cpp
)

target_include_directories(CarPlayStation_TransportTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_TransportTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_TransportTests PRIVATE cxx_std_20)

add_test(NAME TransportTests COMMAND CarPlayStation_TransportTests)

# Protocol layer tests (iAP2 framing, TLV catalog, session state machine)
add_executable(CarPlayStation_ProtocolTests
    ProtocolTests.cpp
)

target_include_directories(CarPlayStation_ProtocolTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_ProtocolTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_ProtocolTests PRIVATE cxx_std_20)

add_test(NAME ProtocolTests COMMAND CarPlayStation_ProtocolTests)

# Session & AirPlay layer tests (PairingStore, RTSP, CarPlay state machine)
add_executable(CarPlayStation_SessionTests
    SessionTests.cpp
)

target_include_directories(CarPlayStation_SessionTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_SessionTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_SessionTests PRIVATE cxx_std_20)

add_test(NAME SessionTests COMMAND CarPlayStation_SessionTests)

# Media layer tests (video demuxer, decoders, audio pipeline, mic uplink)
add_executable(CarPlayStation_MediaTests
    MediaTests.cpp
)

target_include_directories(CarPlayStation_MediaTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_MediaTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_MediaTests PRIVATE cxx_std_20)

add_test(NAME MediaTests COMMAND CarPlayStation_MediaTests)

# Input layer tests (mouse/keyboard/multi-touch translation)
add_executable(CarPlayStation_InputTests
    InputTests.cpp
)

target_include_directories(CarPlayStation_InputTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_InputTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_InputTests PRIVATE cxx_std_20)

add_test(NAME InputTests COMMAND CarPlayStation_InputTests)

# Protocol wire-replay tests (iAP2 byte-exact framing, RTSP replay, mic RTP)
add_executable(CarPlayStation_ProtocolReplayTests
    ProtocolReplayTests.cpp
)

target_include_directories(CarPlayStation_ProtocolReplayTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_ProtocolReplayTests PRIVATE
    CarPlayCore
)

target_compile_features(CarPlayStation_ProtocolReplayTests PRIVATE cxx_std_20)

add_test(NAME ProtocolReplayTests COMMAND CarPlayStation_ProtocolReplayTests)

# Service & application layer tests (event bus, core/application service lifecycle)
add_executable(CarPlayStation_ServiceTests
    ServiceTests.cpp
)

target_include_directories(CarPlayStation_ServiceTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_ServiceTests PRIVATE
    CarPlayApp
)

target_compile_features(CarPlayStation_ServiceTests PRIVATE cxx_std_20)

add_test(NAME ServiceTests COMMAND CarPlayStation_ServiceTests)

# UI layer tests (adaptation geometry, renderer geometry, input router, controller)
add_executable(CarPlayStation_UiTests
    UiTests.cpp
)

target_include_directories(CarPlayStation_UiTests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation_UiTests PRIVATE
    CarPlayUI
)

target_compile_features(CarPlayStation_UiTests PRIVATE cxx_std_20)

add_test(NAME UiTests COMMAND CarPlayStation_UiTests)

