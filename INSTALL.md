# icop AI Content Filter — Installation Guide

Use this guide with an AI assistant to install the icop VLC plugin.

## Step 1 — Download

Get the package for your platform from the
[latest release](https://github.com/asayed18/icop/releases/latest).

| Platform | File |
|---|---|
| Windows x86_64 | `icop-v0.1.3-windows-x86_64.7z.001` and subsequent volumes |
| Linux x86_64 | `icop-v0.1.3-linux-x86_64.tar.gz` |
| Linux ARM64 | `icop-v0.1.3-linux-arm64.tar.gz` |
| macOS x86_64 | `icop-v0.1.3-macos-x86_64.tar.gz` |
| macOS ARM64 | `icop-v0.1.3-macos-arm64.tar.gz` |

For the Windows CUDA package, download every `.7z` volume into the same folder
and extract the `.7z.001` file with [7-Zip](https://www.7-zip.org/).

## Step 2 — Install into VLC

> [!NOTE]
> **For AI assistants:** Use `pkexec` for privileged commands on Linux (available
> on most distributions with PolicyKit). Use `sudo` on macOS. On Windows, use
> an elevated PowerShell (Run as Administrator). Do not ask the user to perform
> setup steps when they asked you to do the work directly.

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

**macOS**:
```bash
sudo /Applications/VLC.app/Contents/MacOS/vlc-cache-gen /Applications/VLC.app/Contents/MacOS/plugins
```

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

GPU-accelerated inference is selected automatically: CUDA on NVIDIA, MIGraphX on
AMD. The plugin falls back to CPU if no GPU runtime is found. For NVIDIA, ensure
the [CUDA 12 driver](https://www.nvidia.com/drivers) is installed. For AMD
GPUs, install [ROCm 6.2](https://rocm.docs.amd.com).
