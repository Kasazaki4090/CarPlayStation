add_library(CarPlayApp STATIC
    ApplicationService.cpp
    MfiSignerFactory.cpp
)

target_include_directories(CarPlayApp PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayApp PUBLIC
    CarPlayCore
)

target_compile_features(CarPlayApp PUBLIC cxx_std_20)
