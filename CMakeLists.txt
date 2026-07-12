cmake_minimum_required(VERSION 3.16)
project(vlc_nsfw_filter VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NSFW_BUILD_TESTS       "Build unit tests"       ON)
option(NSFW_BUILD_BENCHMARKS  "Build benchmark executable" ON)
option(NSFW_BUILD_VLC_MODULE  "Build VLC filter module" ON)
option(NSFW_INSTALL_VLC_PLUGIN "Install VLC module into a VLC tree" OFF)
option(NSFW_INSTALL_CUDA_RUNTIME
    "Install CUDA/TensorRT runtime DLLs with the VLC plugin" OFF)
option(NSFW_BUILD_PLAYER_PROTOTYPE "Build experimental ffplay-like prototype" ON)
option(NSFW_EXPORT_FALCONSAI_BASE_ONNX
    "Attempt to export Falconsai/nsfw_image_detection to ONNX when missing" ON)

# ONNX Runtime – set this to the installation root if not on a standard path.
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to ONNX Runtime installation root")
set(NSFW_EXTRA_RUNTIME_DLLS "" CACHE STRING
    "Semicolon-separated list of extra runtime DLL files to stage with the VLC plugin")
set(NSFW_ONNX_EXPORT_PYTHON "" CACHE FILEPATH
    "Python interpreter used to export optional ONNX models")

set(NSFW_ONNXRUNTIME_SCRATCH_DIR "${CMAKE_BINARY_DIR}/_scratch_build/onnxruntime")
set(NSFW_ONNXRUNTIME_INCLUDE_DIR "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/include")
set(NSFW_ONNXRUNTIME_RUNTIME_DIR "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/runtime")
set(NSFW_ONNXRUNTIME_MODEL_MARQO_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/model.onnx")
set(NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/adamcodd.onnx")
set(NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/falconsai.onnx")
set(NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/falconsai_base.onnx")
set(NSFW_ONNXRUNTIME_FALCONSAI_BASE_SOURCE_DIR "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/falconsai_base_model")
set(NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/quantized_model.onnx")
set(NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/legacy.onnx")
file(MAKE_DIRECTORY "${NSFW_ONNXRUNTIME_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${NSFW_ONNXRUNTIME_RUNTIME_DIR}")

set(NSFW_ONNXRUNTIME_HEADER_BASE
    "https://raw.githubusercontent.com/microsoft/onnxruntime/main/include/onnxruntime/core/session")
set(NSFW_ONNXRUNTIME_HEADERS
    onnxruntime_c_api.h
    onnxruntime_cxx_api.h
    onnxruntime_cxx_inline.h
    onnxruntime_ep_c_api.h
    onnxruntime_error_code.h
    onnxruntime_float16.h)

foreach(_ort_header IN LISTS NSFW_ONNXRUNTIME_HEADERS)
    set(_ort_header_path "${NSFW_ONNXRUNTIME_INCLUDE_DIR}/${_ort_header}")
    if(NOT EXISTS "${_ort_header_path}")
        file(DOWNLOAD
            "${NSFW_ONNXRUNTIME_HEADER_BASE}/${_ort_header}"
            "${_ort_header_path}"
            TLS_VERIFY ON
            STATUS _ort_header_status
            LOG _ort_header_log)
        list(GET _ort_header_status 0 _ort_header_code)
        if(NOT _ort_header_code EQUAL 0)
            message(FATAL_ERROR
                "Failed to download ONNX Runtime header ${_ort_header}: ${_ort_header_log}")
        endif()
    endif()
endforeach()

function(nsfw_download_model MODEL_PATH MODEL_URL MODEL_LABEL MODEL_SHA256)
    set(_nsfw_model_needs_download TRUE)

    if(EXISTS "${MODEL_PATH}")
        file(SHA256 "${MODEL_PATH}" _nsfw_model_existing_hash)
        if(_nsfw_model_existing_hash STREQUAL "${MODEL_SHA256}")
            set(_nsfw_model_needs_download FALSE)
        else()
            file(REMOVE "${MODEL_PATH}")
        endif()
    endif()

    if(_nsfw_model_needs_download)
        file(DOWNLOAD
            "${MODEL_URL}"
            "${MODEL_PATH}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            EXPECTED_HASH "SHA256=${MODEL_SHA256}"
            STATUS _nsfw_model_status
            LOG _nsfw_model_log)
        list(GET _nsfw_model_status 0 _nsfw_model_code)
        if(NOT _nsfw_model_code EQUAL 0)
            message(FATAL_ERROR
                "Failed to download ${MODEL_LABEL}: ${_nsfw_model_log}")
        endif()
    endif()
endfunction()

function(nsfw_download_optional_model MODEL_PATH MODEL_URL MODEL_LABEL)
    if(EXISTS "${MODEL_PATH}")
        file(SIZE "${MODEL_PATH}" _nsfw_model_existing_size)
        if(_nsfw_model_existing_size GREATER 0)
            return()
        endif()
        file(REMOVE "${MODEL_PATH}")
    endif()

    set(_nsfw_model_http_headers)
    if(DEFINED ENV{HF_TOKEN} AND NOT "$ENV{HF_TOKEN}" STREQUAL "")
        list(APPEND _nsfw_model_http_headers "Authorization: Bearer $ENV{HF_TOKEN}")
    elseif(DEFINED ENV{HUGGINGFACE_HUB_TOKEN} AND NOT "$ENV{HUGGINGFACE_HUB_TOKEN}" STREQUAL "")
        list(APPEND _nsfw_model_http_headers "Authorization: Bearer $ENV{HUGGINGFACE_HUB_TOKEN}")
    endif()

    if(_nsfw_model_http_headers)
        file(DOWNLOAD
            "${MODEL_URL}"
            "${MODEL_PATH}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            HTTPHEADER ${_nsfw_model_http_headers}
            STATUS _nsfw_model_status)
        list(GET _nsfw_model_status 0 _nsfw_model_code)
        if(NOT _nsfw_model_code EQUAL 0)
            file(REMOVE "${MODEL_PATH}")
            list(LENGTH _nsfw_model_status _nsfw_model_status_len)
            set(_nsfw_model_message "download failed")
            if(_nsfw_model_status_len GREATER 1)
                list(GET _nsfw_model_status 1 _nsfw_model_message)
            endif()
            message(WARNING
                "Failed to download optional ${MODEL_LABEL} (code ${_nsfw_model_code}): ${_nsfw_model_message}")
        endif()
    else()
        message(STATUS
            "Optional ${MODEL_LABEL} not downloaded; set HF_TOKEN or place the file manually to enable it")
    endif()
endfunction()

function(nsfw_export_falconsai_base_onnx MODEL_PATH SOURCE_DIR)
    set(_nsfw_export_script "${CMAKE_SOURCE_DIR}/tools/export_falconsai_base_onnx.py")
    set(_nsfw_export_python "${NSFW_ONNX_EXPORT_PYTHON}")
    set(_nsfw_export_stdout "")
    set(_nsfw_export_stderr "")
    set(_nsfw_export_rc 0)

    if(EXISTS "${MODEL_PATH}")
        file(SIZE "${MODEL_PATH}" _nsfw_model_existing_size)
        if(_nsfw_model_existing_size GREATER 0)
            return()
        endif()
        file(REMOVE "${MODEL_PATH}")
    endif()

    if(NOT NSFW_EXPORT_FALCONSAI_BASE_ONNX)
        message(STATUS
            "Skipping Falconsai base ONNX export because NSFW_EXPORT_FALCONSAI_BASE_ONNX is OFF")
        return()
    endif()

    if(NOT _nsfw_export_python OR _nsfw_export_python STREQUAL "")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            set(_nsfw_export_python "${Python3_EXECUTABLE}")
        endif()
    endif()

    if(NOT _nsfw_export_python OR _nsfw_export_python STREQUAL "")
        message(STATUS
            "Skipping Falconsai base ONNX export because no Python interpreter was found")
        return()
    endif()

    if(NOT EXISTS "${_nsfw_export_script}")
        message(WARNING
            "Cannot export Falconsai base ONNX because the exporter script is missing: ${_nsfw_export_script}")
        return()
    endif()

    execute_process(
        COMMAND
            "${_nsfw_export_python}"
            "${_nsfw_export_script}"
            --repo "Falconsai/nsfw_image_detection"
            --snapshot-dir "${SOURCE_DIR}"
            --output "${MODEL_PATH}"
        RESULT_VARIABLE _nsfw_export_rc
        OUTPUT_VARIABLE _nsfw_export_stdout
        ERROR_VARIABLE _nsfw_export_stderr
    )

    if(NOT _nsfw_export_rc EQUAL 0)
        file(REMOVE "${MODEL_PATH}")
        message(WARNING
            "Failed to export Falconsai base ONNX (code ${_nsfw_export_rc}): ${_nsfw_export_stderr}")
    elseif(EXISTS "${MODEL_PATH}")
        message(STATUS
            "Exported Falconsai base ONNX model to ${MODEL_PATH}")
    endif()
endfunction()

set(NSFW_ONNXRUNTIME_HEADERS_DIR "${NSFW_ONNXRUNTIME_INCLUDE_DIR}")
set(NSFW_ONNXRUNTIME_DLL_PATH "")

if(ONNXRUNTIME_ROOT)
    if(EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_c_api.h")
        set(NSFW_ONNXRUNTIME_HEADERS_DIR "${ONNXRUNTIME_ROOT}/include")
    endif()

    file(GLOB_RECURSE NSFW_ONNXRUNTIME_ROOT_DLLS
        LIST_DIRECTORIES false
        "${ONNXRUNTIME_ROOT}/onnxruntime.dll"
        "${ONNXRUNTIME_ROOT}/**/onnxruntime.dll")
    if(NSFW_ONNXRUNTIME_ROOT_DLLS)
        list(GET NSFW_ONNXRUNTIME_ROOT_DLLS 0 NSFW_ONNXRUNTIME_DLL_PATH)
    else()
        message(FATAL_ERROR
            "ONNXRUNTIME_ROOT is set, but no onnxruntime.dll was found under it")
    endif()
endif()

nsfw_download_model(
    "${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH}"
    "https://huggingface.co/Marqo/nsfw-image-detection-384/resolve/refs%2Fpr%2F5/onnx/model.onnx"
    "Marqo/nsfw-image-detection-384 ONNX model"
    "7B81155313D894BBA3A3B9BACE059A6DA2A0C509D10CEA43FCB0B3C5A4EDD26D")
nsfw_download_model(
    "${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH}"
    "https://huggingface.co/AdamCodd/vit-base-nsfw-detector/resolve/main/onnx/model_quantized.onnx"
    "AdamCodd/vit-base-nsfw-detector ONNX model"
    "432763A6899EBC418C55F784B98F90565E5FC694C778D2FFCB0294B12F6A7404")
nsfw_download_model(
    "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH}"
    "https://huggingface.co/onnx-community/nsfw_image_detection-ONNX/resolve/main/onnx/model_quantized.onnx"
    "Falconsai/nsfw_image_detection ONNX model"
    "D9AFB1E057104E6CC8616D174F0F6A8B8B0389C839EEA7D5EFA4BDF1A77EFD27")
nsfw_download_optional_model(
    "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH}"
    "https://huggingface.co/Falconsai/nsfw_image_detection_26/resolve/main/quantized_onnx/quantized_model.onnx"
    "Falconsai/nsfw_image_detection_26 ONNX model")
nsfw_export_falconsai_base_onnx(
    "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH}"
    "${NSFW_ONNXRUNTIME_FALCONSAI_BASE_SOURCE_DIR}")
nsfw_download_model(
    "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}"
    "https://github.com/iola1999/nsfw-detect-onnx/releases/download/v1.0.0/model.onnx"
    "legacy NSFW ONNX model"
    "7205637AEF1A3956932670F28CE802538D8980741E747862F8DCB4B5A127C268")

set(NSFW_ONNXRUNTIME_ZIP_PATH "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/onnxruntime-win-x64-1.27.1.zip")
if(NOT NSFW_ONNXRUNTIME_DLL_PATH AND NOT EXISTS "${NSFW_ONNXRUNTIME_ZIP_PATH}")
    file(DOWNLOAD
        "https://github.com/microsoft/onnxruntime/releases/download/v1.27.1/onnxruntime-win-x64-1.27.1.zip"
        "${NSFW_ONNXRUNTIME_ZIP_PATH}"
        TLS_VERIFY ON
        SHOW_PROGRESS
        STATUS _ort_zip_status
        LOG _ort_zip_log)
    list(GET _ort_zip_status 0 _ort_zip_code)
    if(NOT _ort_zip_code EQUAL 0)
        message(FATAL_ERROR
            "Failed to download ONNX Runtime binary package: ${_ort_zip_log}")
    endif()
endif()

if(NOT NSFW_ONNXRUNTIME_DLL_PATH)
    file(GLOB_RECURSE NSFW_ONNXRUNTIME_DLL_PATH
        LIST_DIRECTORIES false
        "${NSFW_ONNXRUNTIME_RUNTIME_DIR}/**/onnxruntime.dll")
endif()
if(NOT NSFW_ONNXRUNTIME_DLL_PATH)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${NSFW_ONNXRUNTIME_ZIP_PATH}"
        WORKING_DIRECTORY "${NSFW_ONNXRUNTIME_RUNTIME_DIR}"
        RESULT_VARIABLE _ort_unzip_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _ort_unzip_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to extract ONNX Runtime binary package")
    endif()
    file(GLOB_RECURSE NSFW_ONNXRUNTIME_DLL_PATH
        LIST_DIRECTORIES false
        "${NSFW_ONNXRUNTIME_RUNTIME_DIR}/**/onnxruntime.dll")
endif()

list(GET NSFW_ONNXRUNTIME_DLL_PATH 0 NSFW_ONNXRUNTIME_DLL_PATH)
get_filename_component(NSFW_ONNXRUNTIME_BIN_DIR "${NSFW_ONNXRUNTIME_DLL_PATH}" DIRECTORY)
file(GLOB NSFW_ONNXRUNTIME_PROVIDER_DLLS
    LIST_DIRECTORIES false
    "${NSFW_ONNXRUNTIME_BIN_DIR}/onnxruntime_providers*.dll")
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH}" NSFW_ONNXRUNTIME_MODEL_MARQO_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH}" NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH}" NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH}" NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH}" NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}" NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH_CMAKE)
file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_DLL_PATH}" NSFW_ONNXRUNTIME_DLL_PATH_CMAKE)

if(MINGW)
    get_filename_component(NSFW_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(NSFW_MINGW_ROOT_DIR "${NSFW_MINGW_BIN_DIR}" DIRECTORY)
    find_file(NSFW_LIBSTDCXX_DLL
        NAMES libstdc++-6.dll
        PATHS "${NSFW_MINGW_BIN_DIR}"
        NO_DEFAULT_PATH)
    find_file(NSFW_LIBGCC_DLL
        NAMES libgcc_s_seh-1.dll
        PATHS "${NSFW_MINGW_BIN_DIR}"
        NO_DEFAULT_PATH)
    find_file(NSFW_LIBWINPTHREAD_DLL
        NAMES libwinpthread-1.dll
        PATHS "${NSFW_MINGW_BIN_DIR}"
        NO_DEFAULT_PATH)
    find_library(NSFW_WINPTHREAD_STATIC
        NAMES libwinpthread.a winpthread
        PATHS "${NSFW_MINGW_ROOT_DIR}/lib"
        NO_DEFAULT_PATH)
endif()

# VLC source/include root. If omitted, we fall back to the unpacked official
# source tree placed in _scratch_build for this workspace.
set(VLC_INCLUDE_DIR "" CACHE PATH "Path to VLC include directory")
if(NOT VLC_INCLUDE_DIR)
    if(EXISTS "${CMAKE_SOURCE_DIR}/_scratch_build/vlc-3.0.20/include")
        set(VLC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/_scratch_build/vlc-3.0.20/include")
    endif()
endif()

# VLC installation root or explicit plugin destination.
set(VLC_ROOT "" CACHE PATH "Path to a VLC installation root")
set(VLC_PLUGIN_INSTALL_DIR "" CACHE PATH "Directory for the VLC plugin output")

if(NSFW_INSTALL_VLC_PLUGIN AND NOT VLC_PLUGIN_INSTALL_DIR)
    if(VLC_ROOT AND EXISTS "${VLC_ROOT}/plugins")
        set(VLC_PLUGIN_INSTALL_DIR "${VLC_ROOT}/plugins/video_filter")
    else()
        set(VLC_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/vlc/plugins/video_filter")
    endif()
endif()

if(NSFW_BUILD_PLAYER_PROTOTYPE OR NSFW_BUILD_TESTS OR NSFW_BUILD_BENCHMARKS)
    set(NSFW_FFMPEG_SCRATCH_DIR "${CMAKE_BINARY_DIR}/_scratch_build/ffmpeg")
    set(NSFW_FFMPEG_ZIP_PATH "${NSFW_FFMPEG_SCRATCH_DIR}/ffmpeg-win64-shared.zip")
    set(NSFW_FFMPEG_ROOT_DIR
        "${NSFW_FFMPEG_SCRATCH_DIR}/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1")
    file(MAKE_DIRECTORY "${NSFW_FFMPEG_SCRATCH_DIR}")

    if(NOT EXISTS "${NSFW_FFMPEG_ZIP_PATH}")
        file(DOWNLOAD
            "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip"
            "${NSFW_FFMPEG_ZIP_PATH}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            STATUS _ffmpeg_zip_status
            LOG _ffmpeg_zip_log)
        list(GET _ffmpeg_zip_status 0 _ffmpeg_zip_code)
        if(NOT _ffmpeg_zip_code EQUAL 0)
            message(FATAL_ERROR
                "Failed to download FFmpeg shared package: ${_ffmpeg_zip_log}")
        endif()
    endif()

    if(NOT EXISTS "${NSFW_FFMPEG_ROOT_DIR}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xvf "${NSFW_FFMPEG_ZIP_PATH}"
            WORKING_DIRECTORY "${NSFW_FFMPEG_SCRATCH_DIR}"
            RESULT_VARIABLE _ffmpeg_unzip_result
            OUTPUT_QUIET
            ERROR_QUIET)
        if(NOT _ffmpeg_unzip_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to extract FFmpeg shared package")
        endif()
    endif()
    set(NSFW_FFMPEG_INCLUDE_DIR "${NSFW_FFMPEG_ROOT_DIR}/include")
    set(NSFW_FFMPEG_LIB_DIR "${NSFW_FFMPEG_ROOT_DIR}/lib")
    file(GLOB NSFW_FFMPEG_RUNTIME_DLLS
        LIST_DIRECTORIES false
        "${NSFW_FFMPEG_ROOT_DIR}/bin/*.dll")
endif()

set(NSFW_SAMPLE_FRAME_WIDTH 320)
set(NSFW_SAMPLE_FRAME_HEIGHT 240)
set(NSFW_SAMPLE_FRAME_DIR "${CMAKE_BINARY_DIR}/test_fixtures")
set(NSFW_SAMPLE_RGB "")
set(NSFW_SAMPLE_SKIN_RGB "")

if((NSFW_BUILD_TESTS OR NSFW_BUILD_BENCHMARKS)
   AND EXISTS "${NSFW_FFMPEG_ROOT_DIR}/bin/ffmpeg.exe"
   AND EXISTS "${CMAKE_SOURCE_DIR}/sample.mp4"
   AND EXISTS "${CMAKE_SOURCE_DIR}/sample_skin.mp4")
    file(MAKE_DIRECTORY "${NSFW_SAMPLE_FRAME_DIR}")

    set(NSFW_SAMPLE_RGB "${NSFW_SAMPLE_FRAME_DIR}/sample_0005.rgb")
    set(NSFW_SAMPLE_SKIN_RGB "${NSFW_SAMPLE_FRAME_DIR}/sample_skin_0005.rgb")

    add_custom_command(
        OUTPUT "${NSFW_SAMPLE_RGB}"
        COMMAND "${NSFW_FFMPEG_ROOT_DIR}/bin/ffmpeg.exe"
            -loglevel error
            -y
            -ss 0.5
            -i "${CMAKE_SOURCE_DIR}/sample.mp4"
            -vf
            "scale=${NSFW_SAMPLE_FRAME_WIDTH}:${NSFW_SAMPLE_FRAME_HEIGHT}:flags=bicubic"
            -frames:v 1
            -pix_fmt rgb24
            -f rawvideo
            "${NSFW_SAMPLE_RGB}"
        DEPENDS "${CMAKE_SOURCE_DIR}/sample.mp4"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${NSFW_SAMPLE_SKIN_RGB}"
        COMMAND "${NSFW_FFMPEG_ROOT_DIR}/bin/ffmpeg.exe"
            -loglevel error
            -y
            -ss 0.5
            -i "${CMAKE_SOURCE_DIR}/sample_skin.mp4"
            -vf
            "scale=${NSFW_SAMPLE_FRAME_WIDTH}:${NSFW_SAMPLE_FRAME_HEIGHT}:flags=bicubic"
            -frames:v 1
            -pix_fmt rgb24
            -f rawvideo
            "${NSFW_SAMPLE_SKIN_RGB}"
        DEPENDS "${CMAKE_SOURCE_DIR}/sample_skin.mp4"
        VERBATIM
    )

    add_custom_target(nsfw_filter_sample_fixtures
        DEPENDS
            "${NSFW_SAMPLE_RGB}"
            "${NSFW_SAMPLE_SKIN_RGB}"
    )
endif()

# ---------------------------------------------------------------------------
# Core library (shared helper, reusable, no VLC dependency)
# ---------------------------------------------------------------------------
add_library(nsfw_filter_core SHARED
    src/nsfw_filter_core.cpp
)

target_include_directories(nsfw_filter_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(nsfw_filter_core PRIVATE
    NSFW_HAS_ONNXRUNTIME=1
    NSFW_DEFAULT_MODEL_PATH="${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH_CMAKE}"
    NSFW_MODEL_PATH_MARQO="${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH_CMAKE}"
    NSFW_MODEL_PATH_ADAMCODD="${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH_CMAKE}"
    NSFW_MODEL_PATH_FALCONSAI="${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH_CMAKE}"
    NSFW_MODEL_PATH_FALCONSAI_BASE="${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH_CMAKE}"
    NSFW_MODEL_PATH_FALCONSAI_OFFICIAL="${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH_CMAKE}"
    NSFW_MODEL_PATH_LEGACY="${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH_CMAKE}"
    NSFW_ONNXRUNTIME_DLL_PATH="${NSFW_ONNXRUNTIME_DLL_PATH_CMAKE}"
)

target_include_directories(nsfw_filter_core PRIVATE
    ${NSFW_ONNXRUNTIME_HEADERS_DIR}
)

if(WIN32)
    set_target_properties(nsfw_filter_core PROPERTIES
        PREFIX ""
        OUTPUT_NAME "nsfw_filter_core"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

if(NSFW_BUILD_PLAYER_PROTOTYPE)
    add_executable(nsfw_ffplay_proto
        src/nsfw_ffplay.cpp
    )

    target_include_directories(nsfw_ffplay_proto PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${NSFW_FFMPEG_INCLUDE_DIR}
    )

    target_link_libraries(nsfw_ffplay_proto PRIVATE
        nsfw_filter_core
        "${NSFW_FFMPEG_LIB_DIR}/libavformat.dll.a"
        "${NSFW_FFMPEG_LIB_DIR}/libavcodec.dll.a"
        "${NSFW_FFMPEG_LIB_DIR}/libavutil.dll.a"
        "${NSFW_FFMPEG_LIB_DIR}/libswscale.dll.a"
        gdi32
        user32
    )

    if(WIN32)
        foreach(_ffmpeg_runtime_dll IN LISTS NSFW_FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET nsfw_ffplay_proto POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_ffmpeg_runtime_dll}"
                    "$<TARGET_FILE_DIR:nsfw_ffplay_proto>"
            )
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# VLC plugin module – built as a shared library
# ---------------------------------------------------------------------------
if(NSFW_BUILD_VLC_MODULE)
    add_library(nsfw_filter MODULE
        modules/video_filter/nsfw_filter.c
    )

    target_include_directories(nsfw_filter PRIVATE
        ${VLC_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    )

    target_compile_definitions(nsfw_filter PRIVATE
        __PLUGIN__
        MODULE_STRING="nsfw_filter"
    )

    # VLC modules do not link against libvlccore at build time;
    # they resolve symbols from the VLC host at load time.
    # Suppress undefined symbol errors for the plugin linker.
    set_target_properties(nsfw_filter PROPERTIES
        PREFIX ""
        OUTPUT_NAME "libnsfw_filter_plugin"
        C_VISIBILITY_PRESET hidden
    )

    if(MINGW OR WIN32)
        target_link_options(nsfw_filter PRIVATE
            "-Wl,--allow-shlib-undefined"
            "-Wl,--unresolved-symbols=ignore-all"
        )
    endif()

    # Link math library on non-Windows (Windows includes it in the C runtime)
    if(NOT WIN32)
        target_link_libraries(nsfw_filter PRIVATE m)
    endif()

    if(NSFW_INSTALL_VLC_PLUGIN)
        if(NOT VLC_PLUGIN_INSTALL_DIR)
            message(FATAL_ERROR
                "NSFW_INSTALL_VLC_PLUGIN is ON, but VLC_PLUGIN_INSTALL_DIR is not set")
        endif()

        install(TARGETS nsfw_filter nsfw_filter_core
            RUNTIME DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            LIBRARY DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
        )
        if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/nsfw_filter_impl.dll")
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nsfw_filter_impl.dll"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}")
        endif()
        install(FILES "${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "model.onnx"
        )
        install(FILES "${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "adamcodd.onnx"
        )
        install(FILES "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "falconsai.onnx"
        )
        if(EXISTS "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH}")
            install(FILES "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_BASE_PATH}"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                RENAME "falconsai_base.onnx"
            )
        endif()
        if(EXISTS "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH}")
            install(FILES "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_OFFICIAL_PATH}"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                RENAME "quantized_model.onnx"
            )
        endif()
        install(FILES "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "legacy.onnx"
        )
        install(FILES "${NSFW_ONNXRUNTIME_DLL_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "onnxruntime.dll"
        )
        foreach(_ort_provider_dll IN LISTS NSFW_ONNXRUNTIME_PROVIDER_DLLS)
            get_filename_component(_ort_provider_name "${_ort_provider_dll}" NAME)
            if(_ort_provider_name STREQUAL "onnxruntime_providers_shared.dll")
                install(FILES "${_ort_provider_dll}"
                    DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                    RENAME "${_ort_provider_name}"
                )
            elseif(NSFW_INSTALL_CUDA_RUNTIME)
                install(FILES "${_ort_provider_dll}"
                    DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                    RENAME "${_ort_provider_name}"
                )
            endif()
        endforeach()
        if(NSFW_INSTALL_CUDA_RUNTIME)
            foreach(_nsfw_extra_runtime_dll IN LISTS NSFW_EXTRA_RUNTIME_DLLS)
                if(EXISTS "${_nsfw_extra_runtime_dll}")
                    get_filename_component(_nsfw_extra_runtime_name
                        "${_nsfw_extra_runtime_dll}" NAME)
                    install(FILES "${_nsfw_extra_runtime_dll}"
                        DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                        RENAME "${_nsfw_extra_runtime_name}"
                    )
                else()
                    message(WARNING
                        "NSFW_EXTRA_RUNTIME_DLLS entry not found: ${_nsfw_extra_runtime_dll}")
                endif()
            endforeach()
        endif()
        if(NSFW_LIBSTDCXX_DLL)
            install(FILES "${NSFW_LIBSTDCXX_DLL}"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}")
        endif()
        if(NSFW_LIBGCC_DLL)
            install(FILES "${NSFW_LIBGCC_DLL}"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}")
        endif()
        if(NSFW_LIBWINPTHREAD_DLL)
            install(FILES "${NSFW_LIBWINPTHREAD_DLL}"
                DESTINATION "${VLC_PLUGIN_INSTALL_DIR}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(NSFW_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # Prevent GoogleTest from overriding compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(nsfw_filter_core_test
        tests/nsfw_filter_core_test.cpp
    )

    target_link_libraries(nsfw_filter_core_test PRIVATE
        nsfw_filter_core
        GTest::gtest_main
    )

    if(TARGET nsfw_filter_sample_fixtures)
        add_dependencies(nsfw_filter_core_test nsfw_filter_sample_fixtures)

        file(TO_CMAKE_PATH "${NSFW_SAMPLE_RGB}" NSFW_TEST_SAMPLE_RGB_CMAKE)
        file(TO_CMAKE_PATH "${NSFW_SAMPLE_SKIN_RGB}"
             NSFW_TEST_SAMPLE_SKIN_RGB_CMAKE)

        target_compile_definitions(nsfw_filter_core_test PRIVATE
            NSFW_TEST_SAMPLE_RGB="${NSFW_TEST_SAMPLE_RGB_CMAKE}"
            NSFW_TEST_SAMPLE_SKIN_RGB="${NSFW_TEST_SAMPLE_SKIN_RGB_CMAKE}"
            NSFW_TEST_SAMPLE_FRAME_WIDTH=${NSFW_SAMPLE_FRAME_WIDTH}
            NSFW_TEST_SAMPLE_FRAME_HEIGHT=${NSFW_SAMPLE_FRAME_HEIGHT}
        )
    endif()

    include(GoogleTest)
    gtest_discover_tests(nsfw_filter_core_test)
endif()

if(NSFW_BUILD_BENCHMARKS)
    add_executable(nsfw_filter_benchmark
        benchmarks/nsfw_filter_benchmark.cpp
    )

    target_link_libraries(nsfw_filter_benchmark PRIVATE
        nsfw_filter_core
    )

    if(TARGET nsfw_filter_sample_fixtures)
        add_dependencies(nsfw_filter_benchmark nsfw_filter_sample_fixtures)

        file(TO_CMAKE_PATH "${NSFW_SAMPLE_RGB}" NSFW_BENCHMARK_SAMPLE_RGB_CMAKE)
        file(TO_CMAKE_PATH "${NSFW_SAMPLE_SKIN_RGB}"
             NSFW_BENCHMARK_SAMPLE_SKIN_RGB_CMAKE)

        target_compile_definitions(nsfw_filter_benchmark PRIVATE
            NSFW_BENCHMARK_SAMPLE_RGB="${NSFW_BENCHMARK_SAMPLE_RGB_CMAKE}"
            NSFW_BENCHMARK_SAMPLE_SKIN_RGB="${NSFW_BENCHMARK_SAMPLE_SKIN_RGB_CMAKE}"
            NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH=${NSFW_SAMPLE_FRAME_WIDTH}
            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT=${NSFW_SAMPLE_FRAME_HEIGHT}
        )
    endif()
endif()
