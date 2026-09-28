# Core — CarPlay protocol, session, transport and media interfaces (header-only contracts for now)
add_library(CarPlayCore INTERFACE)

target_include_directories(CarPlayCore INTERFACE ${CMAKE_SOURCE_DIR})

target_compile_features(CarPlayCore INTERFACE cxx_std_20)
