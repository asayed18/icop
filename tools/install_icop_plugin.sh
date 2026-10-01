#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
release_root=""
release_dir=""
vlc_root=""
requested_arch=""
version=""
dry_run=0
uninstall=0
stop_vlc=0
skip_cache=0

usage() {
    cat <<'EOF'
Usage: install_icop_plugin.sh [options]

Options:
  --release-root PATH  Root containing versioned icop releases
  --release-dir PATH   One extracted platform release (the folder holding
                       release.json); used automatically when this script
                       runs from inside a downloaded release archive
  --vlc-root PATH      VLC installation prefix or VLC.app path
  --arch ARCH          Override the payload architecture (x86_64 or arm64);
                       on macOS it defaults to the VLC.app architecture
  --version VERSION    Install a specific release version
  --dry-run            Detect and verify without changing VLC
  --uninstall          Remove this release's files from VLC instead of
                       installing them
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
        --release-dir)
            [ "$#" -ge 2 ] || die '--release-dir requires a path'
            release_dir=$2
            shift 2
            ;;
        --arch)
            [ "$#" -ge 2 ] || die '--arch requires a value'
            requested_arch=$2
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
        --uninstall)
            uninstall=1
            shift
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

normalize_arch() {
    case "$1" in
        x86_64|amd64) printf 'x86_64\n' ;;
        arm64|aarch64|arm64e) printf 'arm64\n' ;;
        i386|i486|i586|i686) printf 'x86\n' ;;
        *) return 1 ;;
    esac
}

# Every place VLC.app is commonly installed on macOS: the DMG from
# videolan.org and `brew install --cask vlc` both use /Applications (or
# ~/Applications with a per-user --appdir), MacPorts uses /Applications/MacPorts,
# and Spotlight finds renamed or relocated bundles by their bundle identifier.
mac_vlc_app_candidates() {
    printf '%s\n' /Applications/VLC.app "$HOME/Applications/VLC.app" \
        /Applications/MacPorts/VLC.app
    cask_appdir=$(printf '%s\n' "${HOMEBREW_CASK_OPTS:-}" |
        sed -n 's/.*--appdir[= ]\{1,\}\([^ ]*\).*/\1/p')
    if [ -n "$cask_appdir" ]; then
        case "$cask_appdir" in
            "~"/*) cask_appdir="$HOME/${cask_appdir#??}" ;;
        esac
        printf '%s\n' "$cask_appdir/VLC.app"
    fi
    if command -v mdfind >/dev/null 2>&1; then
        mdfind "kMDItemCFBundleIdentifier == 'org.videolan.vlc'" 2>/dev/null || true
    fi
}

mac_app_for_root() {
    case "$1" in
        *.app|*.app/) printf '%s\n' "${1%/}" ;;
        */Contents/MacOS|*/Contents/MacOS/) printf '%s\n' "${1%/Contents/MacOS*}" ;;
        *) return 1 ;;
    esac
}

find_plugin_directory() {
    if [ -n "$vlc_root" ]; then
        if [ "$platform" = mac ]; then
            candidates="
$vlc_root/Contents/MacOS/plugins/video_filter
$vlc_root/plugins/video_filter
$vlc_root/lib/vlc/plugins/video_filter"
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
        candidates=$(mac_vlc_app_candidates | while IFS= read -r app; do
            if [ -n "$app" ]; then
                printf '%s/Contents/MacOS/plugins/video_filter\n' "$app"
            fi
        done)
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

    # Stock VLC.app keeps every plugin directly in Contents/MacOS/plugins and
    # scans it recursively, so video_filter only exists after a previous
    # icop install; accept the plugins folder and let the install create it.
    printf '%s\n' "$candidates" | while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        if [ -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            break
        fi
        if [ "$platform" = mac ] &&
           [ -d "$(dirname -- "$candidate")" ] &&
           [ -f "$(dirname -- "$candidate")/../VLC" ]; then
            printf '%s\n' "$candidate"
            break
        fi
    done
}

plugin_directory=$(find_plugin_directory)
[ -n "$plugin_directory" ] || die 'no VLC video_filter plugin directory was found; pass --vlc-root'
plugin_root=$(dirname -- "$plugin_directory")

vlc_app=""
if [ "$platform" = mac ]; then
    case "$plugin_directory" in
        */Contents/MacOS/plugins/video_filter)
            vlc_app=${plugin_directory%/Contents/MacOS/plugins/video_filter}
            ;;
    esac
    [ -n "$vlc_app" ] || vlc_app=$(mac_app_for_root "$vlc_root" 2>/dev/null || true)
fi

# icop is built against the VLC 3.0 plugin ABI; VLC 4 nightlies cannot load it.
if [ -n "$vlc_app" ] && [ -f "$vlc_app/Contents/Info.plist" ]; then
    vlc_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$vlc_app/Contents/Info.plist" 2>/dev/null || true)
    case "$vlc_version" in
        3.*|'') ;;
        *) die "VLC $vlc_version at $vlc_app is not supported; icop requires VLC 3.0.x" ;;
    esac
fi

if [ -n "$requested_arch" ]; then
    architecture=$(normalize_arch "$requested_arch") ||
        die "unsupported --arch value: $requested_arch"
elif [ "$platform" = mac ]; then
    # The payload must match the process that loads it, not the shell: an
    # Intel-only VLC runs under Rosetta on Apple Silicon, and a Rosetta
    # terminal reports x86_64 even when VLC itself runs natively on arm64.
    native_arm64=0
    if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then
        native_arm64=1
    fi
    vlc_archs=""
    if [ -n "$vlc_app" ] && [ -f "$vlc_app/Contents/MacOS/VLC" ]; then
        vlc_archs=$(lipo -archs "$vlc_app/Contents/MacOS/VLC" 2>/dev/null || true)
    fi
    case " $vlc_archs " in
        *" arm64 "*)
            if [ "$native_arm64" -eq 1 ]; then
                architecture=arm64
            else
                architecture=x86_64
            fi
            ;;
        *" x86_64 "*)
            architecture=x86_64
            if [ "$native_arm64" -eq 1 ]; then
                printf 'icop installer: %s is an Intel-only VLC running under Rosetta; installing the x86_64 payload.\n' "$vlc_app" >&2
                printf 'icop installer: install the Apple Silicon or Universal VLC to run icop natively.\n' >&2
            fi
            ;;
        *)
            if [ "$native_arm64" -eq 1 ]; then
                architecture=arm64
            else
                architecture=$(normalize_arch "$(uname -m)") ||
                    die "unsupported host architecture: $(uname -m)"
            fi
            ;;
    esac
else
    architecture=$(normalize_arch "$(uname -m)") ||
        die "unsupported host architecture: $(uname -m)"
fi

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

# A downloaded release archive carries this script beside its release.json.
if [ -z "$release_dir" ] && [ -z "$release_root" ] &&
   [ -f "$script_dir/release.json" ]; then
    release_dir=$script_dir
fi
[ -n "$release_root" ] || release_root="$script_dir/../releases"

if [ -n "$release_dir" ]; then
    platform_release=$(CDPATH= cd -- "$release_dir" && pwd) ||
        die "release directory does not exist: $release_dir"
    release_matches "$platform_release/release.json" || {
        manifest_arch=$(sed -n 's/.*"architecture"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            "$platform_release/release.json" 2>/dev/null | head -n 1)
        die "$platform_release is not an icop $platform $architecture release${manifest_arch:+ (it is $manifest_arch); download the $architecture archive or pass --arch}"
    }
    release_version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$platform_release/release.json" | head -n 1)
    if [ -n "$version" ] && [ "$version" != "$release_version" ]; then
        die "$platform_release contains icop $release_version, not $version"
    fi
    version=$release_version
elif [ -n "$version" ]; then
    version_directory="$release_root/v$version"
    release_matches "$version_directory/$platform/release.json" ||
        die "no matching icop $platform $architecture release exists for version $version"
    platform_release="$version_directory/$platform"
else
    version_directory=$(select_latest_release) ||
        die "no matching icop $platform $architecture release exists under $release_root"
    version=${version_directory##*/v}
    platform_release="$version_directory/$platform"
fi

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
# video_filter may not exist yet (see find_plugin_directory); judge by the
# folder the install will create it in.
writable_directory=$plugin_directory
[ -d "$writable_directory" ] || writable_directory=$(dirname -- "$plugin_directory")
if [ ! -w "$writable_directory" ]; then
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

if [ "$uninstall" -eq 1 ]; then
    while IFS= read -r payload_relative; do
        run_admin rm -f "$plugin_directory/$payload_relative"
    done < "$payload_list"
    rm -f "$payload_list"
    printf 'Removed icop %s from %s\n' "$version" "$plugin_directory"
    exit 0
fi

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

legacy_names="
libnsfw_filter_plugin.so
libnsfw_filter_core.so
libnsfw_filter_plugin.dylib
libnsfw_filter_core.dylib
freepik-nsfw.onnx
nsfw-classifier-int8.onnx
dml/freepik-nsfw.onnx
dml/nsfw-classifier-int8.onnx"
if [ "$platform" = mac ]; then
    # Releases up to 0.1.6 shipped the macOS plugin with a .so suffix, which
    # VLC for macOS never loads.
    legacy_names="$legacy_names
libicop_plugin.so"
fi

while IFS= read -r payload_relative; do
    backup_destination "$plugin_directory/$payload_relative"
done < "$payload_list"
for legacy_name in $legacy_names; do
    backup_destination "$plugin_directory/$legacy_name"
done

while IFS= read -r payload_relative; do
    source_file="$platform_release/plugins/video_filter/$payload_relative"
    destination="$plugin_directory/$payload_relative"
    if ! run_admin mkdir -p "$(dirname -- "$destination")" ||
       ! run_admin cp -p "$source_file" "$destination"; then
        if [ "$platform" = mac ]; then
            printf 'icop installer: macOS blocked writing into %s.\n' "$vlc_app" >&2
            printf 'icop installer: allow your terminal in System Settings > Privacy & Security > App Management, then rerun.\n' >&2
        fi
        die "failed to copy $payload_relative into $plugin_directory"
    fi
done < "$payload_list"

for legacy_name in $legacy_names; do
    run_admin rm -f "$plugin_directory/$legacy_name"
done

while IFS= read -r payload_relative; do
    source_file="$platform_release/plugins/video_filter/$payload_relative"
    destination="$plugin_directory/$payload_relative"
    [ "$(hash_file "$source_file")" = "$(hash_file "$destination")" ] ||
        die "installed checksum mismatch: $destination"
done < "$payload_list"

if [ "$platform" = mac ]; then
    # Browsers and Archive Utility tag downloads with com.apple.quarantine and
    # `cp -p` carries it over; Gatekeeper then refuses to dlopen the dylibs
    # inside VLC.  The payload was checksum-verified above, so clear the flag.
    while IFS= read -r payload_relative; do
        destination="$plugin_directory/$payload_relative"
        if xattr "$destination" 2>/dev/null | grep -qx 'com.apple.quarantine'; then
            run_admin xattr -d com.apple.quarantine "$destination" ||
                die "could not clear the quarantine flag on $destination"
        fi
        case "$destination" in
            *.dylib)
                codesign --verify "$destination" >/dev/null 2>&1 ||
                    die "code signature is invalid: $destination"
                ;;
        esac
    done < "$payload_list"
fi

if [ "$skip_cache" -ne 1 ]; then
    cache_generator=""
    if [ -n "$vlc_app" ] && [ -x "$vlc_app/Contents/MacOS/vlc-cache-gen" ]; then
        cache_generator="$vlc_app/Contents/MacOS/vlc-cache-gen"
    elif command -v vlc-cache-gen >/dev/null 2>&1; then
        cache_generator=$(command -v vlc-cache-gen)
    elif [ -n "$vlc_root" ] && [ -x "$vlc_root/Contents/MacOS/vlc-cache-gen" ]; then
        cache_generator="$vlc_root/Contents/MacOS/vlc-cache-gen"
    elif [ -n "$vlc_root" ] && [ -x "$vlc_root/bin/vlc-cache-gen" ]; then
        cache_generator="$vlc_root/bin/vlc-cache-gen"
    elif [ -x "$(dirname -- "$plugin_root")/vlc-cache-gen" ]; then
        cache_generator="$(dirname -- "$plugin_root")/vlc-cache-gen"
    fi
    if [ -n "$cache_generator" ]; then
        run_admin "$cache_generator" "$plugin_root"
    elif [ "$platform" = mac ]; then
        # VLC.app does not always ship vlc-cache-gen.  VLC 3 still scans the
        # plugin folder and loads files missing from plugins.dat, so the
        # filter works without a cache refresh.
        printf 'icop installer: vlc-cache-gen is not bundled with this VLC.app; VLC will load icop without a cache entry.\n' >&2
    else
        die 'vlc-cache-gen was not found; rerun with --skip-cache only for testing'
    fi
fi

success=1
printf 'Installed icop %s into %s\n' "$version" "$plugin_directory"
