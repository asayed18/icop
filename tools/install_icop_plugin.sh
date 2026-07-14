#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
release_root="$script_dir/../releases"
vlc_root=""
version=""
dry_run=0
stop_vlc=0
skip_cache=0

usage() {
    cat <<'EOF'
Usage: install_icop_plugin.sh [options]

Options:
  --release-root PATH  Root containing versioned icop releases
  --vlc-root PATH      VLC installation prefix or VLC.app path
  --version VERSION    Install a specific release version
  --dry-run            Detect and verify without changing VLC
  --stop-vlc           Stop the selected platform's running VLC process
  --skip-cache         Do not regenerate VLC's plugin cache
  --help               Show this help
EOF
}

die() {
    printf 'icop installer: %s\n' "$*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --release-root)
            [ "$#" -ge 2 ] || die '--release-root requires a path'
            release_root=$2
            shift 2
            ;;
        --vlc-root)
            [ "$#" -ge 2 ] || die '--vlc-root requires a path'
            vlc_root=$2
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] || die '--version requires a value'
            version=${2#v}
            shift 2
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --stop-vlc)
            stop_vlc=1
            shift
            ;;
        --skip-cache)
            skip_cache=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

case "$(uname -s)" in
    Linux)
        platform=linux
        process_name=vlc
        ;;
    Darwin)
        platform=mac
        process_name=VLC
        ;;
    *)
        die 'this installer supports Linux and macOS; use install_icop_plugin.ps1 on Windows'
        ;;
esac

case "$(uname -m)" in
    x86_64|amd64)
        architecture=x86_64
        ;;
    arm64|aarch64)
        architecture=arm64
        ;;
    i386|i486|i586|i686)
        architecture=x86
        ;;
    *)
        die "unsupported host architecture: $(uname -m)"
        ;;
esac

find_plugin_directory() {
    if [ -n "$vlc_root" ]; then
        if [ "$platform" = mac ]; then
            candidates="
$vlc_root/Contents/MacOS/plugins/video_filter
$vlc_root/plugins/video_filter"
        else
            multiarch=""
            if command -v gcc >/dev/null 2>&1; then
                multiarch=$(gcc -print-multiarch 2>/dev/null || true)
            fi
            candidates="
$vlc_root/plugins/video_filter
$vlc_root/lib/vlc/plugins/video_filter
$vlc_root/lib64/vlc/plugins/video_filter"
            if [ -n "$multiarch" ]; then
                candidates="$candidates
$vlc_root/lib/$multiarch/vlc/plugins/video_filter"
            fi
        fi
    elif [ "$platform" = mac ]; then
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
}

plugin_directory=$(find_plugin_directory)
[ -n "$plugin_directory" ] || die 'no VLC video_filter plugin directory was found; pass --vlc-root'
plugin_root=$(dirname -- "$plugin_directory")

release_matches() {
    manifest=$1
    [ -f "$manifest" ] || return 1
    grep -Eq '"name"[[:space:]]*:[[:space:]]*"icop"' "$manifest" || return 1
    grep -Eq '"platform"[[:space:]]*:[[:space:]]*"'"$platform"'"' "$manifest" || return 1
    if [ "$platform" = mac ]; then
        grep -Eq '"architecture"[[:space:]]*:[[:space:]]*"('"$architecture"'|universal2)"' "$manifest"
    else
        grep -Eq '"architecture"[[:space:]]*:[[:space:]]*"'"$architecture"'"' "$manifest"
    fi
}

select_latest_release() {
    best_directory=""
    best_version=""
    version_sort=0
    if printf '1.0.0\n2.0.0\n' | sort -V >/dev/null 2>&1; then
        version_sort=1
    fi
    for candidate in "$release_root"/v*; do
        [ -d "$candidate" ] || continue
        candidate_version=${candidate##*/v}
        manifest="$candidate/$platform/release.json"
        release_matches "$manifest" || continue
        if [ -z "$best_directory" ]; then
            best_directory=$candidate
            best_version=$candidate_version
        elif [ "$version_sort" -eq 1 ]; then
            newest=$(printf '%s\n%s\n' "$best_version" "$candidate_version" | sort -V | tail -n 1)
            if [ "$newest" = "$candidate_version" ]; then
                best_directory=$candidate
                best_version=$candidate_version
            fi
        elif [ "$candidate_version" \> "$best_version" ]; then
            best_directory=$candidate
            best_version=$candidate_version
        fi
    done
    [ -n "$best_directory" ] || return 1
    printf '%s\n' "$best_directory"
}

if [ -n "$version" ]; then
    version_directory="$release_root/v$version"
    release_matches "$version_directory/$platform/release.json" ||
        die "no matching icop $platform $architecture release exists for version $version"
else
    version_directory=$(select_latest_release) ||
        die "no matching icop $platform $architecture release exists under $release_root"
    version=${version_directory##*/v}
fi

platform_release="$version_directory/$platform"
checksum_file="$platform_release/SHA256SUMS"
[ -f "$checksum_file" ] || die "release checksum file is missing: $checksum_file"

verify_release_checksums() {
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$platform_release" && sha256sum -c SHA256SUMS >/dev/null)
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$platform_release" && shasum -a 256 -c SHA256SUMS >/dev/null)
    else
        die 'sha256sum or shasum is required to verify the release'
    fi
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

verify_release_checksums || die 'release checksum verification failed'

payload_list=$(mktemp "${TMPDIR:-/tmp}/icop-payload.XXXXXX")
while IFS= read -r checksum_line; do
    relative_path=${checksum_line#*  }
    case "$relative_path" in
        plugins/video_filter/*)
            payload_relative=${relative_path#plugins/video_filter/}
            case "$payload_relative" in
                ''|/*|../*|*/../*|*/..)
                    rm -f "$payload_list"
                    die "unsafe release payload path: $relative_path"
                    ;;
            esac
            printf '%s\n' "$payload_relative" >> "$payload_list"
            ;;
        *)
            rm -f "$payload_list"
            die "unexpected release payload path: $relative_path"
            ;;
    esac
done < "$checksum_file"
[ -s "$payload_list" ] || {
    rm -f "$payload_list"
    die 'release payload is empty'
}

printf 'icop %s: %s %s -> %s\n' "$version" "$platform" "$architecture" "$plugin_directory"
if [ "$dry_run" -eq 1 ]; then
    printf 'Dry run: release checksums passed; no files were changed.\n'
    rm -f "$payload_list"
    exit 0
fi

if command -v pgrep >/dev/null 2>&1 && pgrep -x "$process_name" >/dev/null 2>&1; then
    if [ "$stop_vlc" -eq 1 ]; then
        if command -v pkill >/dev/null 2>&1; then
            pkill -x "$process_name" || true
        else
            rm -f "$payload_list"
            die 'pkill is required for --stop-vlc'
        fi
    else
        rm -f "$payload_list"
        die 'VLC is running; close it or pass --stop-vlc'
    fi
fi

needs_sudo=0
if [ ! -w "$plugin_directory" ]; then
    command -v sudo >/dev/null 2>&1 || {
        rm -f "$payload_list"
        die "plugin directory is not writable and sudo is unavailable: $plugin_directory"
    }
    needs_sudo=1
fi

run_admin() {
    if [ "$needs_sudo" -eq 1 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

backup_directory=$(mktemp -d "${TMPDIR:-/tmp}/icop-install-backup.XXXXXX")
existing_list="$backup_directory/existing"
created_list="$backup_directory/created"
: > "$existing_list"
: > "$created_list"
backup_count=0
success=0

backup_destination() {
    destination=$1
    if [ -f "$destination" ]; then
        backup_count=$((backup_count + 1))
        cp -p "$destination" "$backup_directory/original-$backup_count"
        printf '%s\n' "$destination" >> "$existing_list"
    else
        printf '%s\n' "$destination" >> "$created_list"
    fi
}

rollback_install() {
    while IFS= read -r destination; do
        [ -n "$destination" ] || continue
        run_admin rm -f "$destination" || true
    done < "$created_list"

    restore_count=0
    while IFS= read -r destination; do
        [ -n "$destination" ] || continue
        restore_count=$((restore_count + 1))
        run_admin mkdir -p "$(dirname -- "$destination")" || true
        run_admin cp -p "$backup_directory/original-$restore_count" "$destination" || true
    done < "$existing_list"
}

cleanup_install() {
    status=$?
    trap - 0 HUP INT TERM
    if [ "$success" -ne 1 ]; then
        rollback_install
    fi
    rm -rf "$backup_directory"
    rm -f "$payload_list"
    exit "$status"
}
trap cleanup_install 0 HUP INT TERM

while IFS= read -r payload_relative; do
    backup_destination "$plugin_directory/$payload_relative"
done < "$payload_list"
for legacy_name in \
    libnsfw_filter_plugin.so \
    libnsfw_filter_core.so \
    libnsfw_filter_plugin.dylib \
    libnsfw_filter_core.dylib; do
    backup_destination "$plugin_directory/$legacy_name"
done

while IFS= read -r payload_relative; do
    source_file="$platform_release/plugins/video_filter/$payload_relative"
    destination="$plugin_directory/$payload_relative"
    run_admin mkdir -p "$(dirname -- "$destination")"
    run_admin cp -p "$source_file" "$destination"
done < "$payload_list"

for legacy_name in \
    libnsfw_filter_plugin.so \
    libnsfw_filter_core.so \
    libnsfw_filter_plugin.dylib \
    libnsfw_filter_core.dylib; do
    run_admin rm -f "$plugin_directory/$legacy_name"
done

while IFS= read -r payload_relative; do
    source_file="$platform_release/plugins/video_filter/$payload_relative"
    destination="$plugin_directory/$payload_relative"
    [ "$(hash_file "$source_file")" = "$(hash_file "$destination")" ] ||
        die "installed checksum mismatch: $destination"
done < "$payload_list"

if [ "$skip_cache" -ne 1 ]; then
    cache_generator=""
    if command -v vlc-cache-gen >/dev/null 2>&1; then
        cache_generator=$(command -v vlc-cache-gen)
    elif [ -n "$vlc_root" ] && [ -x "$vlc_root/Contents/MacOS/vlc-cache-gen" ]; then
        cache_generator="$vlc_root/Contents/MacOS/vlc-cache-gen"
    elif [ -n "$vlc_root" ] && [ -x "$vlc_root/bin/vlc-cache-gen" ]; then
        cache_generator="$vlc_root/bin/vlc-cache-gen"
    elif [ -x "$(dirname -- "$plugin_root")/vlc-cache-gen" ]; then
        cache_generator="$(dirname -- "$plugin_root")/vlc-cache-gen"
    fi
    [ -n "$cache_generator" ] || die 'vlc-cache-gen was not found; rerun with --skip-cache only for testing'
    run_admin "$cache_generator" "$plugin_root"
fi

success=1
printf 'Installed icop %s into %s\n' "$version" "$plugin_directory"
