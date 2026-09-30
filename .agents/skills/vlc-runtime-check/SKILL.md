---
name: vlc-runtime-check
description: Use when diagnosing or validating staged, portable, or installed VLC plugin behavior. Focuses on side-by-side DLLs, model presence, cache generation, and the correct test commands for this repository.
---

# VLC Runtime Check

## Stage Runtime

Confirm the stage runtime contains the expected plugin files:

- `libicop_plugin.dll`
- `icop_core.dll`
- `onnxruntime.dll`
- `onnxruntime_providers_shared.dll`
- model `.onnx` files
- MinGW runtime DLLs

## Portable VLC Flow

```powershell
Copy-Item .\stage\plugins\video_filter\* .\vlc-portable\plugins\video_filter\ -Force
.\vlc-portable\vlc.exe -vvv --file-logging --logfile=vlc-portable-test.log --video-filter=icop .\sample.mp4
```

## Installed VLC Flow

```powershell
Copy-Item .\stage\plugins\video_filter\* "C:\Program Files\VideoLAN\VLC\plugins\video_filter\" -Force
"C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
& "C:\Program Files\VideoLAN\VLC\vlc.exe" -vvv --file-logging --logfile=vlc-installed-test.log --video-filter=icop .\sample.mp4
```

## macOS Flow

```sh
sh tools/verify_macos_package.sh releases/v<version>/mac arm64
sh tools/macos_vlc_smoke_test.sh /Applications/VLC.app sample.mp4 releases/v<version>/mac/plugins
sh releases/v<version>/mac/install_icop_plugin.sh --dry-run
```

- The plugin must be `libicop_plugin.dylib`; VLC for macOS ignores `.so`.
- The payload architecture must match VLC.app (`lipo -archs`), not the shell.
- Downloaded payloads carry `com.apple.quarantine`, which blocks `dlopen`.

## Failure Checklist

- Missing side-by-side DLLs
- Missing selected model file
- CUDA requested without CUDA provider/runtime DLLs
- stale installed VLC plugin cache
- wrong CLI filter name
- differences between stage, portable, and installed VLC trees

## Project Notes

- `falconsai-official` depends on `quantized_model.onnx`.
- Runtime packaging bugs can look like code bugs.
- If the issue only happens in installed VLC, check cache and copied artifacts before changing code.
