if(WIN32 AND (NSFW_BUILD_BENCHMARKS OR
              (NSFW_BUILD_TESTS AND NSFW_DOWNLOAD_MODELS)))
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

if(WIN32 AND (NSFW_BUILD_BENCHMARKS OR
              (NSFW_BUILD_TESTS AND NSFW_DOWNLOAD_MODELS))
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

    add_custom_target(icop_sample_fixtures
        DEPENDS
            "${NSFW_SAMPLE_RGB}"
            "${NSFW_SAMPLE_SKIN_RGB}"
    )
endif()
