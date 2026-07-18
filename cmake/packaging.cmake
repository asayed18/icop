if(NOT NSFW_BUILD_VLC_MODULE)
    return()
endif()

if(WIN32)
    set(NSFW_RELEASE_PLATFORM "windows")
    set(NSFW_RELEASE_ARCHIVE_EXTENSION "zip")
elseif(APPLE)
    set(NSFW_RELEASE_PLATFORM "mac")
    set(NSFW_RELEASE_ARCHIVE_EXTENSION "tar.gz")
else()
    set(NSFW_RELEASE_PLATFORM "linux")
    set(NSFW_RELEASE_ARCHIVE_EXTENSION "tar.gz")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(NSFW_RELEASE_ARCH "universal2")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" NSFW_RELEASE_ARCH)
elseif(CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" NSFW_RELEASE_ARCH)
elseif(CMAKE_HOST_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" NSFW_RELEASE_ARCH)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NSFW_RELEASE_ARCH "x86_64")
else()
    set(NSFW_RELEASE_ARCH "x86")
endif()

if(NSFW_RELEASE_ARCH MATCHES "^(amd64|x86_64)$")
    set(NSFW_RELEASE_ARCH "x86_64")
elseif(NSFW_RELEASE_ARCH MATCHES "^(aarch64|arm64)$")
    set(NSFW_RELEASE_ARCH "arm64")
endif()

if(NSFW_INSTALL_CUDA_RUNTIME)
    set(NSFW_RELEASE_RUNTIME "cuda")
elseif(NSFW_GPU_RUNTIME)
    set(NSFW_RELEASE_RUNTIME "gpu")
elseif(WIN32)
    set(NSFW_RELEASE_RUNTIME "dml")
elseif(APPLE)
    set(NSFW_RELEASE_RUNTIME "coreml")
else()
    set(NSFW_RELEASE_RUNTIME "cpu")
endif()

set(NSFW_RELEASE_VERSION_ROOT
    "${ICOP_RELEASE_ROOT}/v${ICOP_RELEASE_VERSION}")
set(NSFW_RELEASE_PLATFORM_DIR
    "${NSFW_RELEASE_VERSION_ROOT}/${NSFW_RELEASE_PLATFORM}")
set(NSFW_RELEASE_PLUGIN_DIR
    "${NSFW_RELEASE_PLATFORM_DIR}/plugins/video_filter")
set(NSFW_RELEASE_ARCHIVE
    "${NSFW_RELEASE_VERSION_ROOT}/icop-v${ICOP_RELEASE_VERSION}-${NSFW_RELEASE_PLATFORM}-${NSFW_RELEASE_ARCH}.${NSFW_RELEASE_ARCHIVE_EXTENSION}")
set(_nsfw_package_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${NSFW_RELEASE_VERSION_ROOT}/windows"
        "${NSFW_RELEASE_VERSION_ROOT}/linux"
        "${NSFW_RELEASE_VERSION_ROOT}/mac"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "${NSFW_RELEASE_PLATFORM_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${NSFW_RELEASE_PLUGIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:icop_plugin>"
        "${NSFW_RELEASE_PLUGIN_DIR}/$<TARGET_FILE_NAME:icop_plugin>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:icop_core>"
        "${NSFW_RELEASE_PLUGIN_DIR}/$<TARGET_FILE_NAME:icop_core>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH}"
        "${NSFW_RELEASE_PLUGIN_DIR}/model.onnx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH}"
        "${NSFW_RELEASE_PLUGIN_DIR}/adamcodd.onnx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH}"
        "${NSFW_RELEASE_PLUGIN_DIR}/falconsai.onnx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}"
        "${NSFW_RELEASE_PLUGIN_DIR}/legacy.onnx"
)

if(WIN32)
    list(APPEND _nsfw_package_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:icop_cuda_host>"
            "${NSFW_RELEASE_PLUGIN_DIR}/$<TARGET_FILE_NAME:icop_cuda_host>")

    if(TARGET icop_dml_core)
        list(APPEND _nsfw_package_commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${NSFW_RELEASE_PLUGIN_DIR}/dml"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:icop_dml_core>"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/$<TARGET_FILE_NAME:icop_dml_core>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:icop_dml_host>"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/$<TARGET_FILE_NAME:icop_dml_host>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_DML_DLL_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/onnxruntime.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_DML_SHARED_DLL_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/onnxruntime_providers_shared.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_DIRECTML_DLL_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/DirectML.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_MODEL_MARQO_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/model.onnx"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_MODEL_ADAMCODD_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/adamcodd.onnx"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_MODEL_FALCONSAI_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/falconsai.onnx"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/dml/legacy.onnx"
        )
    endif()
endif()

if(NSFW_INSTALL_RUNTIME_DLL_PATH AND EXISTS "${NSFW_INSTALL_RUNTIME_DLL_PATH}")
    if(WIN32)
        list(APPEND _nsfw_package_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_INSTALL_RUNTIME_DLL_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/onnxruntime.dll")
    else()
        get_filename_component(_nsfw_runtime_name
            "${NSFW_INSTALL_RUNTIME_DLL_PATH}" NAME)
        list(APPEND _nsfw_package_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NSFW_INSTALL_RUNTIME_DLL_PATH}"
                "${NSFW_RELEASE_PLUGIN_DIR}/${_nsfw_runtime_name}")
    endif()
else()
    list(APPEND _nsfw_package_commands
        COMMAND "${CMAKE_COMMAND}" -E echo
            "WARNING: ONNX Runtime was not found, so this package will use detector fallback until the runtime is placed beside the plugin")
endif()

if(WIN32)
    foreach(_nsfw_provider_dll IN LISTS NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
        get_filename_component(_nsfw_provider_name
            "${_nsfw_provider_dll}" NAME)
        if(_nsfw_provider_name STREQUAL "onnxruntime_providers_shared.dll"
           OR (NSFW_INSTALL_CUDA_RUNTIME AND
               _nsfw_provider_name STREQUAL "onnxruntime_providers_cuda.dll"))
            list(APPEND _nsfw_package_commands
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_nsfw_provider_dll}"
                    "${NSFW_RELEASE_PLUGIN_DIR}/${_nsfw_provider_name}")
        endif()
    endforeach()

    if(NSFW_INSTALL_CUDA_RUNTIME)
        foreach(_nsfw_extra_runtime_dll IN LISTS
                NSFW_INSTALL_CUDA_DEPENDENCY_DLLS)
            if(EXISTS "${_nsfw_extra_runtime_dll}")
                get_filename_component(_nsfw_extra_runtime_name
                    "${_nsfw_extra_runtime_dll}" NAME)
                list(APPEND _nsfw_package_commands
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_nsfw_extra_runtime_dll}"
                        "${NSFW_RELEASE_PLUGIN_DIR}/${_nsfw_extra_runtime_name}")
            endif()
        endforeach()
    endif()

    foreach(_nsfw_compiler_runtime IN ITEMS
            "${NSFW_LIBSTDCXX_DLL}"
            "${NSFW_LIBGCC_DLL}"
            "${NSFW_LIBWINPTHREAD_DLL}")
        if(_nsfw_compiler_runtime AND EXISTS "${_nsfw_compiler_runtime}")
            get_filename_component(_nsfw_compiler_runtime_name
                "${_nsfw_compiler_runtime}" NAME)
            list(APPEND _nsfw_package_commands
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_nsfw_compiler_runtime}"
                    "${NSFW_RELEASE_PLUGIN_DIR}/${_nsfw_compiler_runtime_name}")
            if(TARGET icop_dml_core)
                list(APPEND _nsfw_package_commands
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_nsfw_compiler_runtime}"
                        "${NSFW_RELEASE_PLUGIN_DIR}/dml/${_nsfw_compiler_runtime_name}")
            endif()
        endif()
    endforeach()
else()
    foreach(_nsfw_provider_lib IN LISTS NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
        get_filename_component(_nsfw_provider_name
            "${_nsfw_provider_lib}" NAME)
        list(APPEND _nsfw_package_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_nsfw_provider_lib}"
                "${NSFW_RELEASE_PLUGIN_DIR}/${_nsfw_provider_name}")
    endforeach()
endif()

set(_nsfw_package_depends icop_plugin icop_core)
if(WIN32)
    list(APPEND _nsfw_package_depends icop_cuda_host)
    if(TARGET icop_dml_core)
        list(APPEND _nsfw_package_depends icop_dml_core icop_dml_host)
    endif()
endif()

list(APPEND _nsfw_package_commands
    COMMAND "${CMAKE_COMMAND}"
        "-DRELEASE_DIR=${NSFW_RELEASE_PLATFORM_DIR}"
        "-DRELEASE_VERSION_ROOT=${NSFW_RELEASE_VERSION_ROOT}"
        "-DRELEASE_VERSION=${ICOP_RELEASE_VERSION}"
        "-DRELEASE_PLATFORM=${NSFW_RELEASE_PLATFORM}"
        "-DRELEASE_ARCH=${NSFW_RELEASE_ARCH}"
        "-DRELEASE_RUNTIME=${NSFW_RELEASE_RUNTIME}"
        "-DRELEASE_ARCHIVE=${NSFW_RELEASE_ARCHIVE}"
        -P "${CMAKE_SOURCE_DIR}/cmake/finalize_release.cmake"
    COMMAND "${CMAKE_COMMAND}" -E echo
        "VLC release ready: ${NSFW_RELEASE_PLATFORM_DIR}")

add_custom_target(icop_package
    ${_nsfw_package_commands}
    DEPENDS ${_nsfw_package_depends}
    COMMENT "Creating icop v${ICOP_RELEASE_VERSION} ${NSFW_RELEASE_PLATFORM} release"
    VERBATIM
)
