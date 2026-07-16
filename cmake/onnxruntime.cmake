# ONNX Runtime – set this to the installation root if not on a standard path.
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to ONNX Runtime installation root")
set(NSFW_EXTRA_RUNTIME_DLLS "" CACHE STRING
    "Semicolon-separated list of extra runtime DLL files to stage with the VLC plugin")
set(NSFW_ONNX_EXPORT_PYTHON "" CACHE FILEPATH
    "Python interpreter used to export optional ONNX models")
set(ICOP_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/releases" CACHE PATH
    "Root directory for versioned VLC plugin releases")
set(ICOP_RELEASE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Semantic version used in the VLC plugin release output")
if(NOT ICOP_RELEASE_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR
        "ICOP_RELEASE_VERSION must be a semantic version such as 0.1.0 or 0.2.0-rc.1")
endif()
set(NSFW_ONNXRUNTIME_VERSION "1.27.1")

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

set(NSFW_ONNXRUNTIME_HEADERS_DIR "${NSFW_ONNXRUNTIME_INCLUDE_DIR}")
if(ONNXRUNTIME_ROOT AND
   EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_c_api.h")
    set(NSFW_ONNXRUNTIME_HEADERS_DIR "${ONNXRUNTIME_ROOT}/include")
endif()

set(NSFW_ONNXRUNTIME_HEADER_BASE
    "https://raw.githubusercontent.com/microsoft/onnxruntime/main/include/onnxruntime/core/session")
set(NSFW_ONNXRUNTIME_HEADERS
    onnxruntime_c_api.h
    onnxruntime_cxx_api.h
    onnxruntime_cxx_inline.h
    onnxruntime_ep_c_api.h
    onnxruntime_error_code.h
    onnxruntime_float16.h)

if("${NSFW_ONNXRUNTIME_HEADERS_DIR}" STREQUAL
   "${NSFW_ONNXRUNTIME_INCLUDE_DIR}")
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
endif()

function(nsfw_download_model MODEL_PATH MODEL_URL MODEL_LABEL MODEL_SHA256)
    set(_nsfw_model_needs_download TRUE)
    string(TOLOWER "${MODEL_SHA256}" _nsfw_model_expected_hash)

    if(EXISTS "${MODEL_PATH}")
        file(SHA256 "${MODEL_PATH}" _nsfw_model_existing_hash)
        if(_nsfw_model_existing_hash STREQUAL "${_nsfw_model_expected_hash}")
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

set(NSFW_ONNXRUNTIME_DLL_PATH "")
set(NSFW_INSTALL_RUNTIME_DLL_PATH "")
set(NSFW_INSTALL_RUNTIME_PROVIDER_DLLS "")

if(NSFW_DOWNLOAD_MODELS)
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
else()
    message(STATUS
        "Built-in model downloads are disabled; ONNX integration tests and packaging require models supplied separately")
endif()

if(NSFW_GPU_RUNTIME AND NOT _nsfw_cuda_ver)
    if(NSFW_CUDA_VERSION)
        set(_nsfw_cuda_ver "${NSFW_CUDA_VERSION}")
    else()
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            set(_nsfw_cuda_ver "${CUDAToolkit_VERSION_MAJOR}")
        else()
            find_program(_nsfw_nvcc nvcc)
            if(_nsfw_nvcc)
                execute_process(COMMAND "${_nsfw_nvcc}" --version
                    OUTPUT_VARIABLE _nsfw_nvcc_output)
                if(_nsfw_nvcc_output MATCHES "release[ ]+([0-9]+)")
                    set(_nsfw_cuda_ver "${CMAKE_MATCH_1}")
                endif()
            endif()
        endif()
        if(NOT _nsfw_cuda_ver)
            foreach(_nsfw_candidate 12 13)
                find_library(_nsfw_cudart_test
                    NAMES cudart libcudart
                    PATHS /usr/local/cuda-${_nsfw_candidate}/lib64
                          /usr/lib/x86_64-linux-gnu
                          /usr/lib64
                    NO_DEFAULT_PATH)
                if(_nsfw_cudart_test)
                    set(_nsfw_cuda_ver "${_nsfw_candidate}")
                    unset(_nsfw_cudart_test CACHE)
                    break()
                endif()
                unset(_nsfw_cudart_test CACHE)
            endforeach()
        endif()
        if(NOT _nsfw_cuda_ver)
            message(FATAL_ERROR
                "NSFW_GPU_RUNTIME is ON but CUDA version could not be detected. "
                "Install CUDA or set NSFW_CUDA_VERSION to 12 or 13 manually.")
        endif()
    endif()
    message(STATUS "CUDA version for GPU ONNX Runtime: ${_nsfw_cuda_ver}")
endif()

if(WIN32)
    set(NSFW_INSTALL_RUNTIME_DIR "")
    if(ONNXRUNTIME_ROOT)
        unset(NSFW_ONNXRUNTIME_ROOT_DLL CACHE)
        unset(NSFW_ONNXRUNTIME_ROOT_DLL)
        find_file(NSFW_ONNXRUNTIME_ROOT_DLL
            NAMES onnxruntime.dll
            PATHS
                "${ONNXRUNTIME_ROOT}"
                "${ONNXRUNTIME_ROOT}/bin"
                "${ONNXRUNTIME_ROOT}/lib"
            NO_DEFAULT_PATH)
        if(NSFW_ONNXRUNTIME_ROOT_DLL)
            get_filename_component(NSFW_INSTALL_RUNTIME_DIR
                "${NSFW_ONNXRUNTIME_ROOT_DLL}" DIRECTORY)
            set(NSFW_ONNXRUNTIME_DLL_PATH
                "${NSFW_ONNXRUNTIME_ROOT_DLL}")
        endif()
    endif()
    if(NOT NSFW_INSTALL_RUNTIME_DIR AND NSFW_INSTALL_CUDA_RUNTIME)
        set(NSFW_PORTABLE_CUDA_RUNTIME_DIR
            "${CMAKE_SOURCE_DIR}/vlc-portable/plugins/video_filter")
        if(EXISTS "${NSFW_PORTABLE_CUDA_RUNTIME_DIR}/onnxruntime.dll" AND
           EXISTS "${NSFW_PORTABLE_CUDA_RUNTIME_DIR}/onnxruntime_providers_cuda.dll")
            set(NSFW_INSTALL_RUNTIME_DIR "${NSFW_PORTABLE_CUDA_RUNTIME_DIR}")
            message(STATUS
                "Using bundled portable CUDA runtime for VLC plugin staging: ${NSFW_INSTALL_RUNTIME_DIR}")
        elseif(NSFW_INSTALL_CUDA_RUNTIME)
            message(WARNING
                "NSFW_INSTALL_CUDA_RUNTIME is ON, but no CUDA runtime bundle was found beside the repo or in ONNXRUNTIME_ROOT; the install will stay CPU-only unless extra DLLs are supplied")
        endif()
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64)$")
        set(_nsfw_ort_suffix "win-arm64")
    elseif(NSFW_GPU_RUNTIME)
        set(_nsfw_ort_suffix
            "win-x64-gpu_cuda${_nsfw_cuda_ver}")
    else()
        set(_nsfw_ort_suffix "win-x64")
    endif()
    set(NSFW_ONNXRUNTIME_ZIP_PATH
        "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/onnxruntime-${_nsfw_ort_suffix}-${NSFW_ONNXRUNTIME_VERSION}.zip")
    if(NOT NSFW_ONNXRUNTIME_DLL_PATH AND NOT EXISTS "${NSFW_ONNXRUNTIME_ZIP_PATH}")
        file(DOWNLOAD
            "https://github.com/microsoft/onnxruntime/releases/download/v${NSFW_ONNXRUNTIME_VERSION}/onnxruntime-${_nsfw_ort_suffix}-${NSFW_ONNXRUNTIME_VERSION}.zip"
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
    if(NSFW_INSTALL_RUNTIME_DIR)
        file(GLOB NSFW_INSTALL_RUNTIME_PROVIDER_DLLS
            LIST_DIRECTORIES false
            "${NSFW_INSTALL_RUNTIME_DIR}/onnxruntime_providers*.dll")
        set(NSFW_INSTALL_RUNTIME_DLL_PATH "${NSFW_INSTALL_RUNTIME_DIR}/onnxruntime.dll")
    else()
        set(NSFW_INSTALL_RUNTIME_PROVIDER_DLLS "${NSFW_ONNXRUNTIME_PROVIDER_DLLS}")
        set(NSFW_INSTALL_RUNTIME_DLL_PATH "${NSFW_ONNXRUNTIME_DLL_PATH}")
    endif()

    if(NSFW_INSTALL_CUDA_RUNTIME AND NSFW_INSTALL_RUNTIME_DIR)
        file(GLOB NSFW_INSTALL_CUDA_DEPENDENCY_DLLS
            LIST_DIRECTORIES false
            "${NSFW_INSTALL_RUNTIME_DIR}/cublas*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/cudart*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/cudnn*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/cufft*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/curand*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/cusolver*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/cusparse*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/nvJitLink*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/nvrtc*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/nvToolsExt*.dll"
            "${NSFW_INSTALL_RUNTIME_DIR}/zlibwapi.dll")
    endif()
    list(APPEND NSFW_INSTALL_CUDA_DEPENDENCY_DLLS
        ${NSFW_EXTRA_RUNTIME_DLLS})
    list(REMOVE_DUPLICATES NSFW_INSTALL_CUDA_DEPENDENCY_DLLS)
else()
    unset(NSFW_ONNXRUNTIME_LIBRARY_PATH CACHE)
    unset(NSFW_ONNXRUNTIME_LIBRARY_PATH)
    if(ONNXRUNTIME_ROOT)
        find_library(NSFW_ONNXRUNTIME_LIBRARY_PATH
            NAMES onnxruntime libonnxruntime
            PATHS
                "${ONNXRUNTIME_ROOT}"
                "${ONNXRUNTIME_ROOT}/lib"
                "${ONNXRUNTIME_ROOT}/lib64"
            NO_DEFAULT_PATH)
    endif()
    if(NOT NSFW_ONNXRUNTIME_LIBRARY_PATH)
        find_library(NSFW_ONNXRUNTIME_LIBRARY_PATH
            NAMES onnxruntime libonnxruntime)
    endif()

    if(NOT NSFW_ONNXRUNTIME_LIBRARY_PATH)
        set(_nsfw_ort_archive_suffix "")
        set(_nsfw_ort_lib_name "libonnxruntime.so")
        set(_nsfw_ort_glob_providers "libonnxruntime_providers_*.so")

        if(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                set(_nsfw_ort_archive_suffix "osx-arm64")
            elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
                set(_nsfw_ort_archive_suffix "osx-x86_64")
            endif()
            set(_nsfw_ort_lib_name "libonnxruntime.dylib")
            set(_nsfw_ort_glob_providers "")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            if(NSFW_GPU_RUNTIME)
                set(_nsfw_ort_archive_suffix "linux-x64-gpu_cuda${_nsfw_cuda_ver}")
            else()
                set(_nsfw_ort_archive_suffix "linux-x64")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            set(_nsfw_ort_archive_suffix "linux-aarch64")
        endif()

        if(_nsfw_ort_archive_suffix)
            set(NSFW_ONNXRUNTIME_ORT_ARCHIVE
                "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}.tgz")
            set(NSFW_ONNXRUNTIME_ORT_ROOT
                "${NSFW_ONNXRUNTIME_RUNTIME_DIR}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}")

            if(NOT EXISTS "${NSFW_ONNXRUNTIME_ORT_ARCHIVE}")
                file(DOWNLOAD
                    "https://github.com/microsoft/onnxruntime/releases/download/v${NSFW_ONNXRUNTIME_VERSION}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}.tgz"
                    "${NSFW_ONNXRUNTIME_ORT_ARCHIVE}"
                    TLS_VERIFY ON
                    SHOW_PROGRESS
                    STATUS _ort_download_status
                    LOG _ort_download_log)
                list(GET _ort_download_status 0 _ort_download_code)
                if(NOT _ort_download_code EQUAL 0)
                    file(REMOVE "${NSFW_ONNXRUNTIME_ORT_ARCHIVE}")
                    message(WARNING
                        "Failed to download ONNX Runtime for ${_nsfw_ort_archive_suffix}; set ONNXRUNTIME_ROOT to create a complete package: ${_ort_download_log}")
                endif()
            endif()

            if(EXISTS "${NSFW_ONNXRUNTIME_ORT_ARCHIVE}" AND
               NOT EXISTS "${NSFW_ONNXRUNTIME_ORT_ROOT}/lib/${_nsfw_ort_lib_name}")
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -E tar xzf
                        "${NSFW_ONNXRUNTIME_ORT_ARCHIVE}"
                    WORKING_DIRECTORY "${NSFW_ONNXRUNTIME_RUNTIME_DIR}"
                    RESULT_VARIABLE _ort_extract_result
                    OUTPUT_QUIET
                    ERROR_QUIET)
                if(NOT _ort_extract_result EQUAL 0)
                    message(WARNING
                        "Failed to extract ONNX Runtime for ${_nsfw_ort_archive_suffix}")
                endif()
            endif()

            if(EXISTS "${NSFW_ONNXRUNTIME_ORT_ROOT}/lib")
                find_library(NSFW_ONNXRUNTIME_LIBRARY_PATH
                    NAMES onnxruntime libonnxruntime
                    PATHS "${NSFW_ONNXRUNTIME_ORT_ROOT}/lib"
                    NO_DEFAULT_PATH)
            endif()

            if(NSFW_GPU_RUNTIME AND _nsfw_ort_glob_providers AND
               EXISTS "${NSFW_ONNXRUNTIME_ORT_ROOT}/lib")
                file(GLOB NSFW_INSTALL_RUNTIME_PROVIDER_DLLS
                    LIST_DIRECTORIES false
                    "${NSFW_ONNXRUNTIME_ORT_ROOT}/lib/${_nsfw_ort_glob_providers}")
                message(STATUS
                    "GPU ONNX Runtime provider libraries: ${NSFW_INSTALL_RUNTIME_PROVIDER_DLLS}")
            endif()
        endif()
    endif()

    if(NSFW_ONNXRUNTIME_LIBRARY_PATH)
        set(NSFW_ONNXRUNTIME_DLL_PATH "${NSFW_ONNXRUNTIME_LIBRARY_PATH}")
        set(NSFW_INSTALL_RUNTIME_DLL_PATH "${NSFW_ONNXRUNTIME_LIBRARY_PATH}")
        get_filename_component(NSFW_ONNXRUNTIME_BIN_DIR
            "${NSFW_ONNXRUNTIME_LIBRARY_PATH}" DIRECTORY)
        message(STATUS
            "Using ONNX Runtime library: ${NSFW_ONNXRUNTIME_LIBRARY_PATH}")
        if(NOT WIN32 AND NOT APPLE AND NOT NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
            file(GLOB NSFW_INSTALL_RUNTIME_PROVIDER_DLLS
                LIST_DIRECTORIES false
                "${NSFW_ONNXRUNTIME_BIN_DIR}/libonnxruntime_providers_*.so")
            if(NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
                message(STATUS
                    "Provider libraries found alongside ORT: ${NSFW_INSTALL_RUNTIME_PROVIDER_DLLS}")
            endif()
        endif()
    else()
        message(WARNING
            "No ONNX Runtime shared library was found; Linux/macOS builds can still compile, but detector creation will fall back unless a runtime library is installed beside the plugin or provided via ONNXRUNTIME_ROOT")
    endif()
endif()

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
