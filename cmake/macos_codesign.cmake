# Ensures every Mach-O in a macOS release payload carries a valid signature.
#
# Apple Silicon refuses to map arm64 code without one.  Linker-signed outputs
# and Microsoft's ONNX Runtime normally verify already and are left untouched;
# anything that does not verify is ad-hoc signed.  VLC.app is built with the
# com.apple.security.cs.disable-library-validation entitlement, so ad-hoc
# signed plugins are accepted.  Set CODESIGN_IDENTITY to a Developer ID to
# re-sign every file with a hardened-runtime signature instead.

if(NOT DEFINED PAYLOAD_DIR OR NOT IS_DIRECTORY "${PAYLOAD_DIR}")
    message(FATAL_ERROR "PAYLOAD_DIR must name the release plugin directory")
endif()
if(NOT DEFINED CODESIGN_IDENTITY OR CODESIGN_IDENTITY STREQUAL "")
    set(CODESIGN_IDENTITY "-")
endif()

find_program(_codesign codesign)
if(NOT _codesign)
    message(FATAL_ERROR "codesign is required to package icop for macOS")
endif()

file(GLOB_RECURSE _dylibs LIST_DIRECTORIES false "${PAYLOAD_DIR}/*.dylib")
if(NOT _dylibs)
    message(FATAL_ERROR "No .dylib files were found in ${PAYLOAD_DIR}")
endif()

foreach(_dylib IN LISTS _dylibs)
    if(CODESIGN_IDENTITY STREQUAL "-")
        execute_process(
            COMMAND "${_codesign}" --verify --strict "${_dylib}"
            RESULT_VARIABLE _verify_result
            OUTPUT_QUIET ERROR_QUIET)
        if(_verify_result EQUAL 0)
            continue()
        endif()
        set(_sign_args --force --sign - --timestamp=none)
    else()
        set(_sign_args --force --sign "${CODESIGN_IDENTITY}"
            --options runtime --timestamp)
    endif()

    execute_process(
        COMMAND "${_codesign}" ${_sign_args} "${_dylib}"
        RESULT_VARIABLE _sign_result)
    if(NOT _sign_result EQUAL 0)
        message(FATAL_ERROR "Failed to sign ${_dylib}")
    endif()
    message(STATUS "Signed ${_dylib} (${CODESIGN_IDENTITY})")
endforeach()
