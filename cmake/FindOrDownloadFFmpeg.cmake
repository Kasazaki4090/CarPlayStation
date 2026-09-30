# FindOrDownloadFFmpeg.cmake
#
# Automated FFmpeg sync for CarPlayStation (x64 / ARM64).
#
# Strategy:
#   1. Detect the host target architecture (x64 vs ARM64).
#   2. Auto-download the latest stable release-shared ZIP from Gyan.dev / BtbN.
#   3. Extract avcodec / avutil (and headers) into a local version cache.
#   4. Configure a POST_BUILD step to copy the runtime DLLs to the output dir.
#
# The result is a CMake imported target `FFmpeg::FFmpeg` (or an empty target if
# the DLL is not yet present) so the core build remains zero-dependency when the
# FFmpeg runtime is absent. FFmpegVideoDecoder uses dynamic LoadLibrary binding,
# so a missing DLL degrades gracefully to the Media Foundation / CPU path.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Architecture detection
# ---------------------------------------------------------------------------
set(CPS_FFMPEG_ARCH "x64")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(CPS_FFMPEG_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|X86_64|x86_64")
    set(CPS_FFMPEG_ARCH "x64")
endif()

# ---------------------------------------------------------------------------
# Version cache directory
# ---------------------------------------------------------------------------
set(CPS_FFMPEG_VERSION "7.1")
set(CPS_FFMPEG_CACHE_DIR "${CMAKE_SOURCE_DIR}/Lib/ffmpeg/${CPS_FFMPEG_ARCH}/ffmpeg-${CPS_FFMPEG_VERSION}")
set(CPS_FFMPEG_INCLUDE_DIR "${CPS_FFMPEG_CACHE_DIR}/include")
set(CPS_FFMPEG_LIB_DIR "${CPS_FFMPEG_CACHE_DIR}/lib")
set(CPS_FFMPEG_BIN_DIR "${CPS_FFMPEG_CACHE_DIR}/bin")

# ---------------------------------------------------------------------------
# Download + extract (only if the cache is missing)
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CPS_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
    message(STATUS "FFmpeg cache not found at ${CPS_FFMPEG_CACHE_DIR} - attempting download")

    # Gyan.dev release-shared layout (x64). BtbN mirrors the same layout.
    set(CPS_FFMPEG_URL "https://github.com/GyanD/codexffmpeg/releases/download/${CPS_FFMPEG_VERSION}/ffmpeg-${CPS_FFMPEG_VERSION}-${CPS_FFMPEG_ARCH}-shared.zip")
    set(CPS_FFMPEG_ZIP "${CMAKE_BINARY_DIR}/ffmpeg-${CPS_FFMPEG_VERSION}-${CPS_FFMPEG_ARCH}-shared.zip")

    file(DOWNLOAD
        "${CPS_FFMPEG_URL}"
        "${CPS_FFMPEG_ZIP}"
        STATUS _cps_ffmpeg_dl_status
        TIMEOUT 300)

    list(GET _cps_ffmpeg_dl_status 0 _cps_ffmpeg_dl_code)
    if(_cps_ffmpeg_dl_code EQUAL 0 AND EXISTS "${CPS_FFMPEG_ZIP}")
        file(MAKE_DIRECTORY "${CPS_FFMPEG_CACHE_DIR}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf "${CPS_FFMPEG_ZIP}"
            WORKING_DIRECTORY "${CPS_FFMPEG_CACHE_DIR}"
            RESULT_VARIABLE _cps_ffmpeg_extract_rc)
        if(_cps_ffmpeg_extract_rc EQUAL 0)
            # Gyan.dev zips nest a top-level folder; flatten if needed.
            file(GLOB _cps_ffmpeg_nested "${CPS_FFMPEG_CACHE_DIR}/*/include/libavcodec/avcodec.h")
            if(_cps_ffmpeg_nested)
                get_filename_component(_cps_ffmpeg_top "${_cps_ffmpeg_nested}" DIRECTORY)
                get_filename_component(_cps_ffmpeg_top "${_cps_ffmpeg_top}" DIRECTORY)
                if(NOT _cps_ffmpeg_top STREQUAL "${CPS_FFMPEG_CACHE_DIR}")
                    file(GLOB _cps_ffmpeg_top_items "${_cps_ffmpeg_top}/*")
                    foreach(_item ${_cps_ffmpeg_top_items})
                        file(COPY "${_item}" DESTINATION "${CPS_FFMPEG_CACHE_DIR}")
                    endforeach()
                    file(REMOVE_RECURSE "${_cps_ffmpeg_top}")
                endif()
            endif()
            message(STATUS "FFmpeg extracted to ${CPS_FFMPEG_CACHE_DIR}")
        else()
            message(WARNING "FFmpeg extraction failed (rc=${_cps_ffmpeg_extract_rc})")
        endif()
    else()
        message(WARNING "FFmpeg download failed (code=${_cps_ffmpeg_dl_code}). "
                        "Run scripts/Update-FFmpeg.ps1 manually. The build will "
                        "fall back to the Media Foundation / CPU decoder path.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Imported target (or empty fallback)
# ---------------------------------------------------------------------------
if(EXISTS "${CPS_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
    add_library(FFmpeg_avcodec UNKNOWN IMPORTED)
    add_library(FFmpeg_avutil UNKNOWN IMPORTED)
    set_target_properties(FFmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${CPS_FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CPS_FFMPEG_INCLUDE_DIR}")
    set_target_properties(FFmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${CPS_FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CPS_FFMPEG_INCLUDE_DIR}")

    # Aggregate interface target for convenience.
    add_library(FFmpeg_Face INTERFACE)
    target_link_libraries(FFmpeg_Face INTERFACE FFmpeg_avcodec FFmpeg_avutil)

    # POST_BUILD: copy runtime DLLs next to the executable.
    file(GLOB _cps_ffmpeg_dlls "${CPS_FFMPEG_BIN_DIR}/*.dll")
    foreach(_dll ${_cps_ffmpeg_dlls})
        get_filename_component(_dll_name "${_dll}" NAME)
        add_custom_command(TARGET CarPlayCore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:CarPlayCore>"
            COMMENT "Copying FFmpeg runtime DLL ${_dll_name}"
            VERBATIM)
    endforeach()

    set(CPS_FFMPEG_FOUND TRUE)
    message(STATUS "FFmpeg found at ${CPS_FFMPEG_CACHE_DIR}")
else()
    # Empty fallback target so the build graph stays intact.
    add_library(FFmpeg_Face INTERFACE)
    set(CPS_FFMPEG_FOUND FALSE)
    message(STATUS "FFmpeg not available - using Media Foundation / CPU fallback only")
endif()
