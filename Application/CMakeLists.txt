add_library(CarPlayApp INTERFACE)

target_include_directories(CarPlayApp INTERFACE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayApp INTERFACE
    CarPlayCore
)

target_compile_features(CarPlayApp INTERFACE cxx_std_20)
