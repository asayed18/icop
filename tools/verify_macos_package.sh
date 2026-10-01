#!/bin/sh
# Verifies a staged macOS icop release before it is published.
#
# Usage: verify_macos_package.sh RELEASE_DIR ARCH [MIN_MACOS]
#   RELEASE_DIR  releases/v<version>/mac (the folder holding release.json)
#   ARCH         arm64, x86_64 or universal2
#   MIN_MACOS    highest minimum macOS version any binary may require
#                (default 14.0)

set -eu

die() {
    printf 'verify_macos_package: %s\n' "$*" >&2
    exit 1
}

[ "$#" -ge 2 ] || die 'usage: verify_macos_package.sh RELEASE_DIR ARCH [MIN_MACOS]'
release_dir=$1
expected_arch=$2
max_minos=${3:-14.0}
payload="$release_dir/plugins/video_filter"

[ -d "$payload" ] || die "missing payload directory: $payload"
[ -f "$release_dir/release.json" ] || die 'missing release.json'
[ -f "$release_dir/install_icop_plugin.sh" ] || die 'missing bundled installer'
grep -Eq '"architecture"[[:space:]]*:[[:space:]]*"'"$expected_arch"'"' \
    "$release_dir/release.json" || die "release.json does not declare $expected_arch"
(cd "$release_dir" && shasum -a 256 -c SHA256SUMS >/dev/null) ||
    die 'SHA256SUMS verification failed'

# VLC for macOS only loads lib*_plugin.dylib.
[ -f "$payload/libicop_plugin.dylib" ] || die 'libicop_plugin.dylib is missing'
[ ! -e "$payload/libicop_plugin.so" ] || die 'libicop_plugin.so would be ignored by VLC'
[ -f "$payload/libicop_core.dylib" ] || die 'libicop_core.dylib is missing'
[ -f "$payload/model.onnx" ] || die 'model.onnx is missing'
set -- "$payload"/libonnxruntime*.dylib
[ -f "$1" ] || die 'the packaged ONNX Runtime dylib is missing'

version_le() {
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$1" ]
}

case "$expected_arch" in
    universal2) required_archs='x86_64 arm64' ;;
    *) required_archs=$expected_arch ;;
esac

for dylib in "$payload"/*.dylib; do
    name=${dylib##*/}
    archs=$(lipo -archs "$dylib")
    for arch in $required_archs; do
        case " $archs " in
            *" $arch "*) ;;
            *) die "$name is built for '$archs', not $arch" ;;
        esac
    done

    for arch in $required_archs; do
        minos=$(otool -arch "$arch" -l "$dylib" | awk '
            $1 == "cmd" && $2 == "LC_BUILD_VERSION" { build = 1; next }
            build && $1 == "minos" { print $2; exit }
            $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" { legacy = 1; next }
            legacy && $1 == "version" { print $2; exit }')
        [ -n "$minos" ] || die "$name ($arch) has no minimum macOS version"
        version_le "$minos" "$max_minos" ||
            die "$name ($arch) requires macOS $minos, newer than $max_minos"
    done

    codesign --verify --strict "$dylib" ||
        die "$name does not carry a valid code signature"

    # Only system libraries and siblings may be referenced; a Homebrew or
    # build-tree path would fail on every other Mac.
    otool -L "$dylib" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/Library/*|@rpath/*|@loader_path/*) ;;
            *) die "$name links a non-portable library: $dep" ;;
        esac
    done

    printf 'ok  %-34s %-14s\n' "$name" "$archs"
done

printf 'macOS %s package verified: %s\n' "$expected_arch" "$release_dir"
