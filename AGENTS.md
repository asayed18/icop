# Codex Workspace Guide

This repository is set up for ChatGPT Codex collaboration.

## Start Here

Read these in order before making non-trivial changes:

1. [README.md](README.md)
2. [docs/index.md](docs/index.md)
3. [docs/codex-workspace.md](docs/codex-workspace.md)
4. [include/nsfw_filter.h](include/nsfw_filter.h)
5. [modules/video_filter/nsfw_filter.c](modules/video_filter/nsfw_filter.c)
6. [src/nsfw_filter_core.cpp](src/nsfw_filter_core.cpp)

## Project Priorities

- Unsafe frames must not be shown before the detector has a chance to block them.
- Prefer buffering and ahead-of-time classification over lower-latency but unsafe playback.
- Keep build, stage, and install workflows reproducible.
- If behavior changes, update docs and verification notes in the same change.

## Key Commands

```powershell
cmake --build build-ninja --target icop_plugin icop_core -j 8
cmake --build build-ninja --target icop_test icop_benchmark -j 8
ctest --test-dir build-ninja --output-on-failure
cmake --build build-ninja --target icop_package -j 8
```

Portable VLC copy/test:

```powershell
Copy-Item .\releases\v0.1.0\windows\plugins\video_filter\* .\vlc-portable\plugins\video_filter\ -Force
.\vlc-portable\vlc.exe -vvv --file-logging --logfile=vlc-portable-test.log --video-filter=icop .\sample.mp4
```

Installed VLC copy/test:

```powershell
Copy-Item .\releases\v0.1.0\windows\plugins\video_filter\* "C:\Program Files\VideoLAN\VLC\plugins\video_filter\" -Force
"C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
& "C:\Program Files\VideoLAN\VLC\vlc.exe" -vvv --file-logging --logfile=vlc-installed-test.log --video-filter=icop .\sample.mp4
```

## Repo-Local Skills

Use these workspace skills when relevant:

- `$build-plugin`
  Use for the standard plugin build flow in this repository.
- `$implementation-strategy`
  Use before changing filter behavior, queueing, buffering, block windows, model/profile selection, provider logic, or install layout.
- `$code-change-verification`
  Use after any code change that could affect runtime behavior, build output, tests, benchmarks, or packaging.
- `$vlc-runtime-check`
  Use before claiming VLC runtime or plugin-install issues are fixed.

## Change Rules

- Do not revert user changes you did not make.
- Do not assume `falconsai-official` is available unless `quantized_model.onnx` is present.
- Treat missing side-by-side runtime DLLs as first-class failure modes.
- Prefer small, verifiable edits to the VLC filter and detector core.
- If a change affects the user-visible workflow, update [README.md](README.md) or [docs/codex-workspace.md](docs/codex-workspace.md).
