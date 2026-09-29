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
