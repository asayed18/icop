foreach(_required_var IN ITEMS
        RELEASE_DIR
        RELEASE_VERSION_ROOT
        RELEASE_VERSION
        RELEASE_PLATFORM
        RELEASE_ARCH
        RELEASE_RUNTIME
        RELEASE_ARCHIVE)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required release variable: ${_required_var}")
    endif()
endforeach()

set(_payload_dir "${RELEASE_DIR}/plugins/video_filter")
if(NOT IS_DIRECTORY "${_payload_dir}")
    message(FATAL_ERROR "Release payload directory does not exist: ${_payload_dir}")
endif()

file(GLOB_RECURSE _payload_files
    LIST_DIRECTORIES false
    RELATIVE "${RELEASE_DIR}"
    "${_payload_dir}/*")
list(SORT _payload_files)

list(LENGTH _payload_files _file_count)
if(_file_count EQUAL 0)
    message(FATAL_ERROR "Release payload is empty: ${_payload_dir}")
endif()

set(_checksums "")
set(_json_files "")
set(_json_separator "")
foreach(_relative_path IN LISTS _payload_files)
    set(_absolute_path "${RELEASE_DIR}/${_relative_path}")
    file(SHA256 "${_absolute_path}" _sha256)
    file(SIZE "${_absolute_path}" _size)
    string(APPEND _checksums "${_sha256}  ${_relative_path}\n")
    string(APPEND _json_files
        "${_json_separator}    {\"path\": \"${_relative_path}\", \"size\": ${_size}, \"sha256\": \"${_sha256}\"}")
    set(_json_separator ",\n")
endforeach()

file(WRITE "${RELEASE_DIR}/SHA256SUMS" "${_checksums}")
file(WRITE "${RELEASE_DIR}/release.json"
    "{\n"
    "  \"name\": \"vlc-iclean\",\n"
    "  \"version\": \"${RELEASE_VERSION}\",\n"
    "  \"platform\": \"${RELEASE_PLATFORM}\",\n"
    "  \"architecture\": \"${RELEASE_ARCH}\",\n"
    "  \"runtime\": \"${RELEASE_RUNTIME}\",\n"
    "  \"vlcPluginDirectory\": \"plugins/video_filter\",\n"
    "  \"fileCount\": ${_file_count},\n"
    "  \"files\": [\n${_json_files}\n  ]\n"
    "}\n")

set(_release_index "{\n  \"name\": \"vlc-iclean\",\n  \"version\": \"${RELEASE_VERSION}\",\n  \"platforms\": {\n")
set(_platform_separator "")
foreach(_platform IN ITEMS windows linux mac)
    set(_platform_dir "${RELEASE_VERSION_ROOT}/${_platform}")
    file(MAKE_DIRECTORY "${_platform_dir}")
    if(EXISTS "${_platform_dir}/release.json")
        set(_platform_status "built")
        file(REMOVE "${_platform_dir}/NOT_BUILT.txt")
    else()
        set(_platform_status "not-built")
        file(WRITE "${_platform_dir}/NOT_BUILT.txt"
            "VLC iClean v${RELEASE_VERSION} for ${_platform} has not been built.\n"
            "Run the nsfw_package target on ${_platform} to create this release.\n")
    endif()
    string(APPEND _release_index
        "${_platform_separator}    \"${_platform}\": \"${_platform_status}\"")
    set(_platform_separator ",\n")
endforeach()
string(APPEND _release_index "\n  }\n}\n")
file(WRITE "${RELEASE_VERSION_ROOT}/release-index.json" "${_release_index}")

get_filename_component(_archive_name "${RELEASE_ARCHIVE}" NAME)
if(_archive_name MATCHES "\\.zip$")
    set(_archive_command
        "${CMAKE_COMMAND}" -E tar cf "${RELEASE_ARCHIVE}"
        --format=zip "${RELEASE_PLATFORM}")
else()
    set(_archive_command
        "${CMAKE_COMMAND}" -E tar czf "${RELEASE_ARCHIVE}"
        "${RELEASE_PLATFORM}")
endif()

file(GLOB _stale_platform_archives
    LIST_DIRECTORIES false
    "${RELEASE_VERSION_ROOT}/vlc-iclean-v${RELEASE_VERSION}-${RELEASE_PLATFORM}-*.zip"
    "${RELEASE_VERSION_ROOT}/vlc-iclean-v${RELEASE_VERSION}-${RELEASE_PLATFORM}-*.zip.sha256"
    "${RELEASE_VERSION_ROOT}/vlc-iclean-v${RELEASE_VERSION}-${RELEASE_PLATFORM}-*.tar.gz"
    "${RELEASE_VERSION_ROOT}/vlc-iclean-v${RELEASE_VERSION}-${RELEASE_PLATFORM}-*.tar.gz.sha256")
if(_stale_platform_archives)
    file(REMOVE ${_stale_platform_archives})
endif()
execute_process(
    COMMAND ${_archive_command}
    WORKING_DIRECTORY "${RELEASE_VERSION_ROOT}"
    RESULT_VARIABLE _archive_result)
if(NOT _archive_result EQUAL 0)
    message(FATAL_ERROR "Failed to create release archive: ${RELEASE_ARCHIVE}")
endif()

file(SHA256 "${RELEASE_ARCHIVE}" _archive_sha256)
file(WRITE "${RELEASE_ARCHIVE}.sha256"
    "${_archive_sha256}  ${_archive_name}\n")

message(STATUS
    "Release finalized: ${RELEASE_PLATFORM} ${RELEASE_VERSION} (${_file_count} payload files)")
