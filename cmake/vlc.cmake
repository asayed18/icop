# VLC source/include root. If omitted, we fall back to the unpacked official
# source tree placed in _scratch_build for this workspace.
set(VLC_INCLUDE_DIR "" CACHE PATH "Path to VLC include directory")
if(NOT VLC_INCLUDE_DIR)
    set(NSFW_VLC_SOURCE_VERSION "3.0.21")
    set(NSFW_VLC_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/_scratch_build/vlc-${NSFW_VLC_SOURCE_VERSION}")
    set(NSFW_VLC_SOURCE_ARCHIVE
        "${CMAKE_SOURCE_DIR}/_scratch_build/vlc-${NSFW_VLC_SOURCE_VERSION}.tar.xz")
    if(NOT EXISTS "${NSFW_VLC_SOURCE_DIR}/include")
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/_scratch_build")
        if(NOT EXISTS "${NSFW_VLC_SOURCE_ARCHIVE}")
            file(DOWNLOAD
                "https://download.videolan.org/pub/videolan/vlc/${NSFW_VLC_SOURCE_VERSION}/vlc-${NSFW_VLC_SOURCE_VERSION}.tar.xz"
                "${NSFW_VLC_SOURCE_ARCHIVE}"
                EXPECTED_HASH "SHA256=24dbbe1d7dfaeea0994d5def0bbde200177347136dbfe573f5b6a4cee25afbb0"
                SHOW_PROGRESS
            )
        endif()
        file(ARCHIVE_EXTRACT INPUT "${NSFW_VLC_SOURCE_ARCHIVE}"
             DESTINATION "${CMAKE_SOURCE_DIR}/_scratch_build")
    endif()
    if(EXISTS "${NSFW_VLC_SOURCE_DIR}/include")
        set(VLC_INCLUDE_DIR "${NSFW_VLC_SOURCE_DIR}/include")
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
