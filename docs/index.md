# Project Docs

## Core Documents

- [README.md](C:/Users/ahmed/Documents/vlc_iclean/README.md)
  Full project overview, architecture, features, models, build flow, and testing notes.
- [docs/codex-workspace.md](C:/Users/ahmed/Documents/vlc_iclean/docs/codex-workspace.md)
  Codex-specific workspace scaffold, expected workflows, and repo-local skills.

## Code Entry Points

- [modules/video_filter/nsfw_filter.c](C:/Users/ahmed/Documents/vlc_iclean/modules/video_filter/nsfw_filter.c)
  Main VLC filter module.
- [src/nsfw_filter_core.cpp](C:/Users/ahmed/Documents/vlc_iclean/src/nsfw_filter_core.cpp)
  ONNX-backed detector core.
- [include/nsfw_filter.h](C:/Users/ahmed/Documents/vlc_iclean/include/nsfw_filter.h)
  Filter state and queue definitions.
- [include/nsfw_filter_core.h](C:/Users/ahmed/Documents/vlc_iclean/include/nsfw_filter_core.h)
  Public detector API.
- [CMakeLists.txt](C:/Users/ahmed/Documents/vlc_iclean/CMakeLists.txt)
  Build, download, staging, and install logic.

## Validation Paths

- [tests/nsfw_filter_core_test.cpp](C:/Users/ahmed/Documents/vlc_iclean/tests/nsfw_filter_core_test.cpp)
- [benchmarks/nsfw_filter_benchmark.cpp](C:/Users/ahmed/Documents/vlc_iclean/benchmarks/nsfw_filter_benchmark.cpp)
- [tools/nsfw_scan_ahead.py](C:/Users/ahmed/Documents/vlc_iclean/tools/nsfw_scan_ahead.py)
- [tools/nsfw_vlc_guard.py](C:/Users/ahmed/Documents/vlc_iclean/tools/nsfw_vlc_guard.py)
