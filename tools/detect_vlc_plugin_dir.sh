#!/bin/sh
set -eu

case "$(uname -s)" in
    Linux) platform=linux ;;
    Darwin) platform=mac ;;
    *) exit 1 ;;
esac

if [ "$platform" = mac ]; then
    candidates="
/Applications/VLC.app/Contents/MacOS/plugins/video_filter
$HOME/Applications/VLC.app/Contents/MacOS/plugins/video_filter"
else
    multiarch=""
    if command -v gcc >/dev/null 2>&1; then
        multiarch=$(gcc -print-multiarch 2>/dev/null || true)
    elif command -v dpkg-architecture >/dev/null 2>&1; then
        multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
    fi
    candidates="
/usr/lib/vlc/plugins/video_filter
/usr/lib64/vlc/plugins/video_filter
/usr/local/lib/vlc/plugins/video_filter
/opt/homebrew/lib/vlc/plugins/video_filter"
    if [ -n "$multiarch" ]; then
        candidates="$candidates
/usr/lib/$multiarch/vlc/plugins/video_filter
/usr/local/lib/$multiarch/vlc/plugins/video_filter"
    fi
fi

printf '%s\n' "$candidates" | while IFS= read -r candidate; do
    [ -n "$candidate" ] || continue
    if [ -d "$candidate" ]; then
        printf '%s\n' "$candidate"
        break
    fi
done
