# icop AI Content Filter — Installation Guide

Use this guide with an AI assistant to install the icop VLC plugin.

## Step 1 — Download

Get the package for your platform from the
[latest release](https://github.com/asayed18/icop/releases/latest).

| Platform | File |
|---|---|
| Windows x86_64 | `icop-v0.1.4-windows-x86_64.7z.001` and subsequent volumes |
| Linux x86_64 | `icop-v0.1.4-linux-x86_64.tar.gz` |
| Linux ARM64 | `icop-v0.1.4-linux-arm64.tar.gz` |
| macOS Intel (x86_64) | `icop-v0.1.4-mac-x86_64.tar.gz` |
| macOS Apple Silicon (M1/M2/M3/M4, ARM64) | `icop-v0.1.4-mac-arm64.tar.gz` |

On macOS, pick the archive that matches **VLC**, not the Mac: an Apple Silicon
Mac running the Intel-only VLC build needs the x86_64 archive. The bundled
installer (Step 2) detects this for you. Releases up to v0.1.6 shipped a macOS
plugin that VLC could not load; use a newer release.

For the Windows CUDA package, download every `.7z` volume into the same folder
and extract the `.7z.001` file with [7-Zip](https://www.7-zip.org/).

## Step 2 — Install into VLC

> [!NOTE]
> **For AI assistants:** Use `pkexec` for privileged commands on Linux (available
> on most distributions with PolicyKit). Use `sudo` on macOS. On Windows, use
> an elevated PowerShell (Run as Administrator). Do not ask the user to perform
> setup steps when they asked you to do the work directly.

On macOS, follow [Step 2 (macOS)](#step-2-macos--install-into-vlcapp) below.

Extract the archive and copy the files from `plugins/video_filter/` into VLC's
plugin directory:

| Platform | VLC plugin directory |
|---|---|
| Windows | `C:\Program Files\VideoLAN\VLC\plugins\video_filter\` |
| Linux | `/usr/lib/vlc/plugins/video_filter/` |
| macOS | `/Applications/VLC.app/Contents/MacOS/plugins/video_filter/` |

After copying, regenerate VLC's plugin cache:

**Windows** (run PowerShell as Administrator):
```powershell
& "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
```

**Linux**:
```bash
sudo /usr/lib/vlc/vlc-cache-gen /usr/lib/vlc/plugins
```

## Step 2 (macOS) — Install into VLC.app

Requirements: macOS 14 (Sonoma) or newer and VLC **3.0.x** (VLC 4 nightlies
use a different plugin ABI). All of these VLC installs are supported:

| How VLC was installed | Location the installer finds |
|---|---|
| DMG from [videolan.org](https://www.videolan.org/vlc/download-macosx.html) | `/Applications/VLC.app` |
| `brew install --cask vlc` | `/Applications/VLC.app`, or the `--appdir` in `HOMEBREW_CASK_OPTS` |
| Per-user install | `~/Applications/VLC.app` |
| MacPorts | `/Applications/MacPorts/VLC.app` |
| Anywhere else | Found through Spotlight, or pass `--vlc-root /path/to/VLC.app` |

**Homebrew (recommended):** quit VLC, then run:

```bash
brew tap asayed18/icop
brew install --cask icop
```

Use `brew reinstall --cask icop` after a VLC update and
`brew uninstall --cask icop` to remove the plugin. The cask runs the same
installer described below.

**Direct download:** quit VLC, then extract the archive and run the installer
that ships inside it:

```bash
tar xzf icop-v<version>-mac-arm64.tar.gz
sh mac/install_icop_plugin.sh --dry-run   # shows which VLC.app and payload it picked
sh mac/install_icop_plugin.sh
```

The installer:

- verifies `SHA256SUMS`, then copies the payload into
  `VLC.app/Contents/MacOS/plugins/video_filter/`, asking for `sudo` only if the
  bundle is not writable, and rolls back if any step fails;
- checks the VLC.app architecture with `lipo`, so an Intel-only VLC running
  under Rosetta on Apple Silicon gets the x86_64 payload (use `--arch` to
  override);
- clears the `com.apple.quarantine` flag that browsers add to downloads.
  Without this, Gatekeeper refuses to load the dylibs inside VLC;
- removes the unusable `libicop_plugin.so` left by releases up to v0.1.6;
- regenerates VLC's plugin cache when `vlc-cache-gen` is bundled. When it is
  not, VLC still loads the new plugin at launch.

If the copy fails with *Operation not permitted*, macOS App Management is
protecting VLC.app. Allow your terminal app under
*System Settings → Privacy & Security → App Management* and rerun.

VLC updates (Sparkle auto-update or `brew upgrade --cask vlc`) replace
VLC.app and remove the plugin, so rerun the installer after each VLC update.

Manual install, if you prefer not to run the script:

```bash
PLUGINS=/Applications/VLC.app/Contents/MacOS/plugins
cp mac/plugins/video_filter/* "$PLUGINS/video_filter/"
xattr -d com.apple.quarantine "$PLUGINS"/video_filter/libicop_* "$PLUGINS"/video_filter/libonnxruntime*.dylib "$PLUGINS"/video_filter/*.onnx 2>/dev/null
rm -f "$PLUGINS/video_filter/libicop_plugin.so"
/Applications/VLC.app/Contents/MacOS/VLC --reset-plugins-cache vlc://quit
```

Start VLC from Terminal once to confirm the filter loads:

```bash
/Applications/VLC.app/Contents/MacOS/VLC --video-filter=icop /path/to/video.mp4 2>&1 | grep '^icop'
```

Expect a line such as `icop: using marqo model profile (...)`. If it reports
`unable to load core DLL`, the plugin files are not all in the same folder.

## Step 3 — Enable the filter

Launch VLC with the icop filter active on the command line:

```bash
vlc --video-filter=icop
```

To enable permanently, open VLC preferences
(*Tools → Preferences → Show Settings = All → Video → Filters*),
check **icop**, and save. Restart VLC.

## Step 4 — Apply best-practice settings

Set these recommended values (VLC preferences → *Show Settings = All* →
*Video → Filters → icop*):

| Setting | Value |
|---|---|
| Model profile | `marqo` |
| Detection threshold | `0.17` |
| Mute audio on blocked frames | Enabled |
| Blocked frame style | Black out |
| Analysis stride | `8` |
| Block padding | `20` |
| Buffered frames | `3` |
| Worker threads | `8` |
| CUDA device id | `0` |

These values prioritise sensitivity. The `marqo` model at threshold `0.17`
catches more unsafe frames at the cost of occasional false positives. Adjust
threshold upward if false alarms are too frequent. The `8`-frame stride with
`20`-frame padding extends each detection by roughly 0.7 seconds at 30 FPS.

## Step 5 — Verify

Open any video with the command above. If the filter is working, you should see
the configured block style (or original frames) depending on the content. The
VLC status bar may show `icop` in the active filter list under
*Tools → Messages (Ctrl+M)*.

On Windows, automatic model evaluation always tries CUDA first, then DirectML,
then CPU. CUDA uses NVIDIA's runtime when it is available; DirectML uses the
Windows graphics stack and can run on supported NVIDIA, AMD, and Intel GPUs.
The plugin reaches CPU only if neither GPU provider can create a usable model
session. For CUDA, install a current [NVIDIA driver](https://www.nvidia.com/drivers).
