#!/bin/sh
# Plays a clip through VLC.app with the icop filter and checks that VLC
# registered the plugin and that the detector reached ONNX Runtime.
#
# Usage: macos_vlc_smoke_test.sh VLC_APP VIDEO [PLUGIN_PATH] [LOG_FILE]
#   PLUGIN_PATH  optional folder exported as VLC_PLUGIN_PATH, e.g. an extracted
#                release's plugins/ folder, so the signed VLC.app bundle does
#                not have to be modified; omit it to test an installed plugin.

set -eu

die() {
    printf 'macos_vlc_smoke_test: %s\n' "$*" >&2
    exit 1
}

[ "$#" -ge 2 ] || die 'usage: macos_vlc_smoke_test.sh VLC_APP VIDEO [PLUGIN_PATH] [LOG_FILE]'
vlc_app=$1
video=$2
plugin_path=${3:-}
log_file=${4:-vlc-macos-smoke.log}
if [ -n "$plugin_path" ]; then
    [ -d "$plugin_path" ] || die "plugin path not found: $plugin_path"
    VLC_PLUGIN_PATH=$(CDPATH= cd -- "$plugin_path" && pwd)
    export VLC_PLUGIN_PATH
fi
vlc="$vlc_app/Contents/MacOS/VLC"
[ -x "$vlc" ] || die "VLC executable not found: $vlc"
[ -f "$video" ] || die "video not found: $video"

# perl ships with macOS; coreutils `timeout` does not.
with_timeout() {
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' "$@"
}

list_log="${log_file%.log}-modules.log"
with_timeout 120 "$vlc" -I dummy --list > "$list_log" 2>&1 || true
if ! grep -Eq '^[[:space:]]+icop[[:space:]]' "$list_log"; then
    grep -i icop "$list_log" >&2 || true
    die 'VLC did not register the icop module'
fi
printf 'VLC registered the icop module\n'

with_timeout 180 "$vlc" -I dummy --vout=dummy --aout=dummy \
    --no-video-title-show --play-and-exit --video-filter=icop \
    "$video" > "$log_file" 2>&1 || true
grep '^icop' "$log_file" || true

for failure in \
    'unable to load core DLL' \
    'unable to load the packaged ONNX Runtime' \
    'ONNX detector unavailable' \
    'detector initialization failed' \
    'does not support C API version'; do
    if grep -q "$failure" "$log_file"; then
        die "VLC reported: $failure (see $log_file)"
    fi
done
grep -Eq '^icop: using [a-z0-9_-]+ model profile' "$log_file" ||
    die "the icop filter never initialized its detector (see $log_file)"

printf 'icop filter initialized in %s\n' "$vlc_app"
