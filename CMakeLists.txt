cmake_minimum_required(VERSION 3.16)
project(icop
    VERSION 0.1.1
    DESCRIPTION "Privacy-first sensitive-content filtering for VLC media player"
    HOMEPAGE_URL "https://github.com/asayed18/icop"
    LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/options.cmake)
include(cmake/onnxruntime.cmake)
include(cmake/vlc.cmake)
include(cmake/test_fixtures.cmake)

# ---------------------------------------------------------------------------
# Core library (shared helper, reusable, no VLC dependency)
# ---------------------------------------------------------------------------
add_library(icop_core SHARED
    src/nsfw_filter_core.cpp
    src/nsfw_model_registry.cpp
    src/nsfw_onnx_preload.cpp
    src/nsfw_onnx_providers.cpp
    src/nsfw_platform_utils.cpp
)

target_include_directories(icop_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(icop_core PRIVATE
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

target_include_directories(icop_core PRIVATE
    ${NSFW_ONNXRUNTIME_HEADERS_DIR}
)

if(WIN32)
    set_target_properties(icop_core PROPERTIES
        PREFIX ""
        OUTPUT_NAME "icop_core"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

if(MSVC)
    target_compile_options(icop_core PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(icop_core PRIVATE $<$<CONFIG:Release>:-O3>)
endif()

if(NOT WIN32 AND CMAKE_DL_LIBS)
    target_compile_definitions(icop_core PRIVATE _GNU_SOURCE)
    target_link_libraries(icop_core PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# VLC plugin module – built as a shared library
# ---------------------------------------------------------------------------
if(NSFW_BUILD_VLC_MODULE)
    set(NSFW_FILTER_SOURCES
        src/plugin/nsfw_filter.c
        src/plugin/nsfw_filter_core_loader.c
        src/plugin/nsfw_filter_config.c
        src/plugin/nsfw_filter_audio.c
        src/plugin/nsfw_filter_decision.c
        src/plugin/nsfw_filter_worker.c
        src/plugin/nsfw_backend.c
        src/plugin/nsfw_filter_d3d11.c
        src/plugin/nsfw_filter_d3d11_readback.c
        src/plugin/nsfw_filter_d3d11_effects.c
        src/plugin/nsfw_filter_vaapi.c
        src/plugin/nsfw_filter_vaapi_readback.c
        src/plugin/nsfw_filter_vaapi_effects.c
        src/plugin/platform_abstraction.c
        src/plugin/nsfw_window_icon.c
        src/plugin/frame_processor.c
        src/plugin/nsfw_debug_overlay.c
    )

    if(WIN32)
        enable_language(RC)
        set(NSFW_FILTER_ICON_SOURCE
            "${CMAKE_SOURCE_DIR}/assets/branding/icop-steel-scanner.ico")
        if(NOT EXISTS "${NSFW_FILTER_ICON_SOURCE}")
            message(FATAL_ERROR
                "The Windows VLC icon asset is missing: ${NSFW_FILTER_ICON_SOURCE}")
        endif()
        configure_file(
            "${CMAKE_SOURCE_DIR}/src/plugin/nsfw_filter_resources.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/nsfw_filter_resources.rc"
            @ONLY
        )
        set_source_files_properties(
            "${CMAKE_CURRENT_BINARY_DIR}/nsfw_filter_resources.rc"
            PROPERTIES OBJECT_DEPENDS "${NSFW_FILTER_ICON_SOURCE}"
        )
        list(APPEND NSFW_FILTER_SOURCES
            "${CMAKE_CURRENT_BINARY_DIR}/nsfw_filter_resources.rc")
    endif()

    add_library(icop_plugin MODULE ${NSFW_FILTER_SOURCES})

    target_include_directories(icop_plugin PRIVATE
        ${VLC_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/vlc-stubs
    )

    target_compile_definitions(icop_plugin PRIVATE
        __PLUGIN__
        MODULE_STRING="icop"
    )
    if(NOT WIN32 AND NOT APPLE)
        target_compile_definitions(icop_plugin PRIVATE NSFW_HAVE_VAAPI)
    endif()

    set_target_properties(icop_plugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME "libicop_plugin"
        C_VISIBILITY_PRESET hidden
    )

    if(MSVC)
        target_compile_options(icop_plugin PRIVATE $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(icop_plugin PRIVATE $<$<CONFIG:Release>:-O3>)
    endif()

    if(NOT WIN32 AND CMAKE_DL_LIBS)
        target_compile_definitions(icop_plugin PRIVATE _GNU_SOURCE)
        target_link_libraries(icop_plugin PRIVATE ${CMAKE_DL_LIBS})
    endif()

    if(MINGW OR WIN32)
        target_link_options(icop_plugin PRIVATE
            "-Wl,--allow-shlib-undefined"
            "-Wl,--unresolved-symbols=ignore-all"
        )
        target_link_libraries(icop_plugin PRIVATE
            d3d11
            d3dcompiler
            dxgi
            user32
        )
    endif()

    if(NOT WIN32)
        target_link_libraries(icop_plugin PRIVATE m)
    endif()
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(icop_plugin PRIVATE va va-drm)
    endif()
    if(APPLE)
        target_link_options(icop_plugin PRIVATE
            "-Wl,-undefined,dynamic_lookup"
        )
    endif()

    if(WIN32)
        set(NSFW_D3D11_RUNTIME_FFMPEG "ffmpeg.exe")
        if(EXISTS "${NSFW_FFMPEG_ROOT_DIR}/bin/ffmpeg.exe")
            set(NSFW_D3D11_RUNTIME_FFMPEG
                "${NSFW_FFMPEG_ROOT_DIR}/bin/ffmpeg.exe")
        endif()
        add_custom_target(icop_d3d11_runtime_check
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/nsfw_d3d11_runtime_check.ps1"
                -VlcRoot "${CMAKE_SOURCE_DIR}/vlc-portable"
                -VideoPath "${CMAKE_SOURCE_DIR}/sample.mp4"
                -PluginPath "$<TARGET_FILE:icop_plugin>"
                -CorePath "$<TARGET_FILE:icop_core>"
                -FfmpegPath "${NSFW_D3D11_RUNTIME_FFMPEG}"
            DEPENDS icop_plugin icop_core
            USES_TERMINAL
        )
    endif()

    if(NSFW_INSTALL_VLC_PLUGIN)
        if(NOT VLC_PLUGIN_INSTALL_DIR)
            message(FATAL_ERROR
                "NSFW_INSTALL_VLC_PLUGIN is ON, but VLC_PLUGIN_INSTALL_DIR is not set")
        endif()

        install(TARGETS icop_plugin icop_core
            RUNTIME DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            LIBRARY DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
        )
        install(CODE "file(REMOVE
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/libnsfw_filter_plugin.dll\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/nsfw_filter_core.dll\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/nsfw_filter_impl.dll\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/libnsfw_filter_plugin.so\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/libnsfw_filter_core.so\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/libnsfw_filter_plugin.dylib\"
            \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/libnsfw_filter_core.dylib\"
        )")
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
        install(CODE "file(REMOVE \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/falconsai_base.onnx\" \"\$ENV{DESTDIR}${VLC_PLUGIN_INSTALL_DIR}/quantized_model.onnx\")")
        install(FILES "${NSFW_ONNXRUNTIME_MODEL_LEGACY_PATH}"
            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
            RENAME "legacy.onnx"
        )
        if(NSFW_INSTALL_RUNTIME_DLL_PATH AND EXISTS "${NSFW_INSTALL_RUNTIME_DLL_PATH}")
            if(WIN32)
                install(FILES "${NSFW_INSTALL_RUNTIME_DLL_PATH}"
                    DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                    RENAME "onnxruntime.dll"
                )
                foreach(_ort_provider_dll IN LISTS NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
                    get_filename_component(_ort_provider_name "${_ort_provider_dll}" NAME)
                    if(_ort_provider_name STREQUAL "onnxruntime_providers_shared.dll")
                        install(FILES "${_ort_provider_dll}"
                            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                            RENAME "${_ort_provider_name}"
                        )
                    elseif(NSFW_INSTALL_CUDA_RUNTIME AND
                           _ort_provider_name STREQUAL "onnxruntime_providers_cuda.dll")
                        install(FILES "${_ort_provider_dll}"
                            DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                            RENAME "${_ort_provider_name}"
                        )
                    endif()
                endforeach()
                if(NSFW_INSTALL_CUDA_RUNTIME)
                    foreach(_nsfw_extra_runtime_dll IN LISTS
                            NSFW_INSTALL_CUDA_DEPENDENCY_DLLS)
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
            else()
                install(FILES "${NSFW_INSTALL_RUNTIME_DLL_PATH}"
                    DESTINATION "${VLC_PLUGIN_INSTALL_DIR}")
                foreach(_ort_provider_lib IN LISTS
                        NSFW_INSTALL_RUNTIME_PROVIDER_DLLS)
                    get_filename_component(_ort_provider_name
                        "${_ort_provider_lib}" NAME)
                    install(FILES "${_ort_provider_lib}"
                        DESTINATION "${VLC_PLUGIN_INSTALL_DIR}"
                        RENAME "${_ort_provider_name}")
                endforeach()
            endif()
        endif()
    endif()

    include(cmake/packaging.cmake)
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
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(icop_test
        tests/nsfw_filter_core_test.cpp
        tests/nsfw_filter_core_integration_test.cpp
    )

    target_link_libraries(icop_test PRIVATE
        icop_core
        GTest::gtest_main
    )

    if(TARGET icop_sample_fixtures)
        add_dependencies(icop_test icop_sample_fixtures)

        file(TO_CMAKE_PATH "${NSFW_SAMPLE_RGB}" NSFW_TEST_SAMPLE_RGB_CMAKE)
        file(TO_CMAKE_PATH "${NSFW_SAMPLE_SKIN_RGB}"
             NSFW_TEST_SAMPLE_SKIN_RGB_CMAKE)

        target_compile_definitions(icop_test PRIVATE
            NSFW_TEST_SAMPLE_RGB="${NSFW_TEST_SAMPLE_RGB_CMAKE}"
            NSFW_TEST_SAMPLE_SKIN_RGB="${NSFW_TEST_SAMPLE_SKIN_RGB_CMAKE}"
            NSFW_TEST_SAMPLE_FRAME_WIDTH=${NSFW_SAMPLE_FRAME_WIDTH}
            NSFW_TEST_SAMPLE_FRAME_HEIGHT=${NSFW_SAMPLE_FRAME_HEIGHT}
        )
    endif()

    include(GoogleTest)
    gtest_discover_tests(icop_test)
endif()

if(NSFW_BUILD_BENCHMARKS)
    add_executable(icop_benchmark
        benchmarks/nsfw_filter_benchmark.cpp
    )

    target_link_libraries(icop_benchmark PRIVATE
        icop_core
    )

    if(TARGET icop_sample_fixtures)
        add_dependencies(icop_benchmark icop_sample_fixtures)

        file(TO_CMAKE_PATH "${NSFW_SAMPLE_RGB}" NSFW_BENCHMARK_SAMPLE_RGB_CMAKE)
        file(TO_CMAKE_PATH "${NSFW_SAMPLE_SKIN_RGB}"
             NSFW_BENCHMARK_SAMPLE_SKIN_RGB_CMAKE)

        target_compile_definitions(icop_benchmark PRIVATE
            NSFW_BENCHMARK_SAMPLE_RGB="${NSFW_BENCHMARK_SAMPLE_RGB_CMAKE}"
            NSFW_BENCHMARK_SAMPLE_SKIN_RGB="${NSFW_BENCHMARK_SAMPLE_SKIN_RGB_CMAKE}"
            NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH=${NSFW_SAMPLE_FRAME_WIDTH}
            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT=${NSFW_SAMPLE_FRAME_HEIGHT}
        )
    endif()
endif()
