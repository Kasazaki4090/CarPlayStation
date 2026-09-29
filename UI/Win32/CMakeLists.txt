add_executable(CarPlayStation
    main.cpp
)

target_include_directories(CarPlayStation PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CarPlayStation PRIVATE
    CarPlayApp
)

target_compile_features(CarPlayStation PRIVATE cxx_std_20)

if(WIN32)
    # Win32 subsystem configuration
    # set_target_properties(CarPlayStation PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
