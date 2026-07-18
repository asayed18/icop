# ONNX Runtime – set this to the installation root if not on a standard path.
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to ONNX Runtime installation root")
set(NSFW_EXTRA_RUNTIME_DLLS "" CACHE STRING
    "Semicolon-separated list of extra runtime DLL files to stage with the VLC plugin")
set(NSFW_CUDA_RUNTIME_DIR "" CACHE PATH
    "Directory containing the NVIDIA CUDA runtime DLLs to stage with the VLC plugin")
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
set(NSFW_ONNXRUNTIME_VERSION "1.26.0")
# DirectML is shipped by ONNX Runtime as a separate Windows runtime, rather
# than as a provider DLL compatible with the CUDA package above.  Keep this
# version pinned with its headers so the isolated DirectML host has a matching
# API and ABI.
set(NSFW_ONNXRUNTIME_DML_VERSION "1.24.4")
set(NSFW_DIRECTML_VERSION "1.15.4")
set(NSFW_ONNXRUNTIME_DML_RUNTIME_DIR "")
set(NSFW_ONNXRUNTIME_DML_HEADERS_DIR "")
set(NSFW_ONNXRUNTIME_DML_DLL_PATH "")
set(NSFW_ONNXRUNTIME_DML_SHARED_DLL_PATH "")
set(NSFW_DIRECTML_DLL_PATH "")

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
    "https://raw.githubusercontent.com/microsoft/onnxruntime/v${NSFW_ONNXRUNTIME_VERSION}/include/onnxruntime/core/session")
set(NSFW_ONNXRUNTIME_HEADERS
    onnxruntime_c_api.h
    onnxruntime_cxx_api.h
    onnxruntime_cxx_inline.h
    onnxruntime_ep_c_api.h
    onnxruntime_float16.h)
set(NSFW_ONNXRUNTIME_HEADERS_VERSION_FILE
    "${NSFW_ONNXRUNTIME_INCLUDE_DIR}/.icop-onnxruntime-version")

if("${NSFW_ONNXRUNTIME_HEADERS_DIR}" STREQUAL
   "${NSFW_ONNXRUNTIME_INCLUDE_DIR}")
    set(_nsfw_cached_headers_version "")
    if(EXISTS "${NSFW_ONNXRUNTIME_HEADERS_VERSION_FILE}")
        file(READ "${NSFW_ONNXRUNTIME_HEADERS_VERSION_FILE}"
             _nsfw_cached_headers_version)
        string(STRIP "${_nsfw_cached_headers_version}"
               _nsfw_cached_headers_version)
    endif()
    if(NOT _nsfw_cached_headers_version STREQUAL
       "${NSFW_ONNXRUNTIME_VERSION}")
        foreach(_ort_header IN LISTS NSFW_ONNXRUNTIME_HEADERS)
            file(REMOVE "${NSFW_ONNXRUNTIME_INCLUDE_DIR}/${_ort_header}")
        endforeach()
    endif()
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
    file(WRITE "${NSFW_ONNXRUNTIME_HEADERS_VERSION_FILE}"
         "${NSFW_ONNXRUNTIME_VERSION}\n")
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
    if(NOT NSFW_INSTALL_RUNTIME_DIR AND NSFW_INSTALL_CUDA_RUNTIME AND
       NOT NSFW_GPU_RUNTIME)
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
        if(_nsfw_cuda_ver STREQUAL "13" AND
           NSFW_ONNXRUNTIME_VERSION VERSION_LESS "1.27.0")
            message(FATAL_ERROR
                "ONNX Runtime ${NSFW_ONNXRUNTIME_VERSION} does not provide a Windows CUDA 13 package; "
                "set NSFW_CUDA_VERSION=12 or upgrade the ONNX Runtime version")
        endif()
        set(_nsfw_ort_suffix
            "win-x64-gpu_cuda${_nsfw_cuda_ver}")
    else()
        set(_nsfw_ort_suffix "win-x64")
    endif()
    # Keep CPU and GPU archives in separate directories so switching build
    # modes cannot silently reuse an incompatible onnxruntime.dll.
    set(NSFW_ONNXRUNTIME_RUNTIME_DIR
        "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/runtime/${_nsfw_ort_suffix}")
    file(MAKE_DIRECTORY "${NSFW_ONNXRUNTIME_RUNTIME_DIR}")
    set(_nsfw_ort_archive_suffix "${_nsfw_ort_suffix}")
    # CUDA 12 was the default GPU build through 1.26, so those archives do
    # not carry a CUDA-major suffix. CUDA 13 archives retain it.
    if(NSFW_GPU_RUNTIME AND _nsfw_cuda_ver STREQUAL "12" AND
       NSFW_ONNXRUNTIME_VERSION VERSION_LESS "1.27.0")
        set(_nsfw_ort_archive_suffix "win-x64-gpu")
    endif()
    set(NSFW_ONNXRUNTIME_ZIP_PATH
        "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}.zip")
    set(_nsfw_ort_extract_dir
        "${NSFW_ONNXRUNTIME_RUNTIME_DIR}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}")
    set(_nsfw_ort_extracted_dll_path
        "${_nsfw_ort_extract_dir}/lib/onnxruntime.dll")
    if(NOT NSFW_ONNXRUNTIME_DLL_PATH AND NOT EXISTS "${NSFW_ONNXRUNTIME_ZIP_PATH}")
        file(DOWNLOAD
            "https://github.com/microsoft/onnxruntime/releases/download/v${NSFW_ONNXRUNTIME_VERSION}/onnxruntime-${_nsfw_ort_archive_suffix}-${NSFW_ONNXRUNTIME_VERSION}.zip"
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

    if(NOT NSFW_ONNXRUNTIME_DLL_PATH AND NOT EXISTS "${_nsfw_ort_extracted_dll_path}")
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
    endif()
    if(NOT NSFW_ONNXRUNTIME_DLL_PATH)
        if(NOT EXISTS "${_nsfw_ort_extracted_dll_path}")
            message(FATAL_ERROR
                "ONNX Runtime archive did not contain ${_nsfw_ort_extracted_dll_path}")
        endif()
        set(NSFW_ONNXRUNTIME_DLL_PATH "${_nsfw_ort_extracted_dll_path}")
    endif()
    get_filename_component(NSFW_ONNXRUNTIME_BIN_DIR "${NSFW_ONNXRUNTIME_DLL_PATH}" DIRECTORY)
    file(GLOB NSFW_ONNXRUNTIME_PROVIDER_DLLS
        LIST_DIRECTORIES false
        "${NSFW_ONNXRUNTIME_BIN_DIR}/onnxruntime_providers*.dll")
    if(NSFW_GPU_RUNTIME)
        # Keep the ORT core and provider DLLs from the same GPU archive.
        set(NSFW_INSTALL_RUNTIME_DIR "${NSFW_ONNXRUNTIME_BIN_DIR}")
    endif()
    if(NSFW_INSTALL_RUNTIME_DIR)
        file(GLOB NSFW_INSTALL_RUNTIME_PROVIDER_DLLS
            LIST_DIRECTORIES false
            "${NSFW_INSTALL_RUNTIME_DIR}/onnxruntime_providers*.dll")
        set(NSFW_INSTALL_RUNTIME_DLL_PATH "${NSFW_INSTALL_RUNTIME_DIR}/onnxruntime.dll")
    else()
        set(NSFW_INSTALL_RUNTIME_PROVIDER_DLLS "${NSFW_ONNXRUNTIME_PROVIDER_DLLS}")
        set(NSFW_INSTALL_RUNTIME_DLL_PATH "${NSFW_ONNXRUNTIME_DLL_PATH}")
    endif()

    # The official CUDA package does not contain DirectML.  Download the
    # official DirectML NuGet package into a separate side-by-side runtime for
    # the fallback helper, so CUDA and DirectML never load into vlc.exe.
    if(NSFW_GPU_RUNTIME)
        set(NSFW_ONNXRUNTIME_DML_ROOT
            "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/directml-${NSFW_ONNXRUNTIME_DML_VERSION}")
        set(NSFW_ONNXRUNTIME_DML_PACKAGE
            "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/Microsoft.ML.OnnxRuntime.DirectML-${NSFW_ONNXRUNTIME_DML_VERSION}.nupkg")
        set(NSFW_ONNXRUNTIME_DML_HEADERS_DIR
            "${NSFW_ONNXRUNTIME_DML_ROOT}/build/native/include")
        set(NSFW_ONNXRUNTIME_DML_RUNTIME_DIR
            "${NSFW_ONNXRUNTIME_DML_ROOT}/runtimes/win-x64/native")
        set(NSFW_ONNXRUNTIME_DML_DLL_PATH
            "${NSFW_ONNXRUNTIME_DML_RUNTIME_DIR}/onnxruntime.dll")
        set(NSFW_ONNXRUNTIME_DML_SHARED_DLL_PATH
            "${NSFW_ONNXRUNTIME_DML_RUNTIME_DIR}/onnxruntime_providers_shared.dll")
        file(MAKE_DIRECTORY "${NSFW_ONNXRUNTIME_DML_ROOT}")

        if(NOT EXISTS "${NSFW_ONNXRUNTIME_DML_PACKAGE}")
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${NSFW_ONNXRUNTIME_DML_VERSION}/microsoft.ml.onnxruntime.directml.${NSFW_ONNXRUNTIME_DML_VERSION}.nupkg"
                "${NSFW_ONNXRUNTIME_DML_PACKAGE}"
                TLS_VERIFY ON
                EXPECTED_HASH "SHA256=57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
                SHOW_PROGRESS
                STATUS _nsfw_dml_download_status
                LOG _nsfw_dml_download_log)
            list(GET _nsfw_dml_download_status 0 _nsfw_dml_download_code)
            if(NOT _nsfw_dml_download_code EQUAL 0)
                message(FATAL_ERROR
                    "Failed to download ONNX Runtime DirectML package: ${_nsfw_dml_download_log}")
            endif()
        endif()

        if(NOT EXISTS "${NSFW_ONNXRUNTIME_DML_DLL_PATH}")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E tar xvf
                    "${NSFW_ONNXRUNTIME_DML_PACKAGE}"
                WORKING_DIRECTORY "${NSFW_ONNXRUNTIME_DML_ROOT}"
                RESULT_VARIABLE _nsfw_dml_extract_result
                OUTPUT_QUIET
                ERROR_QUIET)
            if(NOT _nsfw_dml_extract_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to extract ONNX Runtime DirectML package")
            endif()
        endif()

        if(NOT EXISTS "${NSFW_ONNXRUNTIME_DML_DLL_PATH}" OR
           NOT EXISTS "${NSFW_ONNXRUNTIME_DML_SHARED_DLL_PATH}" OR
           NOT EXISTS "${NSFW_ONNXRUNTIME_DML_HEADERS_DIR}/onnxruntime_c_api.h")
            message(FATAL_ERROR
                "ONNX Runtime DirectML package is missing its Windows runtime or headers")
        endif()

        # The Windows system DirectML component can be older than the version
        # expected by ONNX Runtime.  Ship Microsoft's redistributable beside
        # the isolated DirectML host so current model kernels do not fall back
        # to the system copy.
        set(NSFW_DIRECTML_ROOT
            "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/directml-redist-${NSFW_DIRECTML_VERSION}")
        set(NSFW_DIRECTML_PACKAGE
            "${NSFW_ONNXRUNTIME_SCRATCH_DIR}/Microsoft.AI.DirectML-${NSFW_DIRECTML_VERSION}.nupkg")
        set(NSFW_DIRECTML_DLL_PATH
            "${NSFW_DIRECTML_ROOT}/bin/x64-win/DirectML.dll")
        file(MAKE_DIRECTORY "${NSFW_DIRECTML_ROOT}")

        if(NOT EXISTS "${NSFW_DIRECTML_PACKAGE}")
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/${NSFW_DIRECTML_VERSION}/microsoft.ai.directml.${NSFW_DIRECTML_VERSION}.nupkg"
                "${NSFW_DIRECTML_PACKAGE}"
                TLS_VERIFY ON
                EXPECTED_HASH "SHA256=4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9"
                SHOW_PROGRESS
                STATUS _nsfw_directml_download_status
                LOG _nsfw_directml_download_log)
            list(GET _nsfw_directml_download_status 0 _nsfw_directml_download_code)
            if(NOT _nsfw_directml_download_code EQUAL 0)
                message(FATAL_ERROR
                    "Failed to download DirectML redistributable: ${_nsfw_directml_download_log}")
            endif()
        endif()

        if(NOT EXISTS "${NSFW_DIRECTML_DLL_PATH}")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E tar xvf
                    "${NSFW_DIRECTML_PACKAGE}"
                WORKING_DIRECTORY "${NSFW_DIRECTML_ROOT}"
                RESULT_VARIABLE _nsfw_directml_extract_result
                OUTPUT_QUIET
                ERROR_QUIET)
            if(NOT _nsfw_directml_extract_result EQUAL 0)
                message(FATAL_ERROR "Failed to extract DirectML redistributable")
            endif()
        endif()

        if(NOT EXISTS "${NSFW_DIRECTML_DLL_PATH}")
            message(FATAL_ERROR
                "DirectML redistributable is missing bin/x64-win/DirectML.dll")
        endif()
    endif()

    if(NSFW_INSTALL_CUDA_RUNTIME)
        set(_nsfw_cuda_dependency_dirs "${NSFW_INSTALL_RUNTIME_DIR}")
        if(NSFW_GPU_RUNTIME)
            # NVIDIA dependency DLLs are not bundled by the official ONNX
            # Runtime archive.  They must come from an explicitly selected
            # CUDA runtime matching the requested provider, not from a
            # pre-existing portable tree that may target another CUDA major.
            if(NSFW_CUDA_RUNTIME_DIR)
                list(APPEND _nsfw_cuda_dependency_dirs
                    "${NSFW_CUDA_RUNTIME_DIR}")
            else()
                message(FATAL_ERROR
                    "NSFW_GPU_RUNTIME with NSFW_INSTALL_CUDA_RUNTIME requires "
                    "NSFW_CUDA_RUNTIME_DIR containing matching NVIDIA CUDA and cuDNN DLLs")
            endif()
        else()
            list(APPEND _nsfw_cuda_dependency_dirs
                "${CMAKE_SOURCE_DIR}/vlc-portable/plugins/video_filter")
        endif()
        foreach(_nsfw_cuda_dependency_dir IN LISTS _nsfw_cuda_dependency_dirs)
            if(NOT _nsfw_cuda_dependency_dir)
                continue()
            endif()
            file(GLOB_RECURSE _nsfw_cuda_dependency_dlls
                LIST_DIRECTORIES false
                "${_nsfw_cuda_dependency_dir}/cublas*.dll"
                "${_nsfw_cuda_dependency_dir}/cudart*.dll"
                "${_nsfw_cuda_dependency_dir}/cudnn*.dll"
                "${_nsfw_cuda_dependency_dir}/cufft*.dll"
                "${_nsfw_cuda_dependency_dir}/curand*.dll"
                "${_nsfw_cuda_dependency_dir}/cusolver*.dll"
                "${_nsfw_cuda_dependency_dir}/cusparse*.dll"
                "${_nsfw_cuda_dependency_dir}/nvJitLink*.dll"
                "${_nsfw_cuda_dependency_dir}/nvrtc*.dll"
                "${_nsfw_cuda_dependency_dir}/nvToolsExt*.dll"
                "${_nsfw_cuda_dependency_dir}/zlibwapi.dll")
            list(APPEND NSFW_INSTALL_CUDA_DEPENDENCY_DLLS
                ${_nsfw_cuda_dependency_dlls})
        endforeach()
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
if(NSFW_ONNXRUNTIME_DML_DLL_PATH)
    file(TO_CMAKE_PATH "${NSFW_ONNXRUNTIME_DML_DLL_PATH}" NSFW_ONNXRUNTIME_DML_DLL_PATH_CMAKE)
endif()

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
