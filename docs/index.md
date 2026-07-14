# Project Docs

## Core Documents

- [README.md](../README.md)
  Full project overview, architecture, features, models, build flow, and testing notes.
- [docs/codex-workspace.md](codex-workspace.md)
  Codex-specific workspace scaffold, expected workflows, and repo-local skills.
- [Release process](releasing.md)
  Versioning, packaging, verification, and source-only publication policy.
- [Contributing](../CONTRIBUTING.md)
  Build commands, safety requirements, and pull-request expectations.
- [Security policy](../SECURITY.md)
  Supported versions and private vulnerability reporting.
- [Third-party notices](../THIRD_PARTY_NOTICES.md)
  Runtime, tool, and model sources and licensing notes.

## Code Entry Points

- [modules/video_filter/nsfw_filter.c](../modules/video_filter/nsfw_filter.c)
  Main VLC filter module.
- [src/nsfw_filter_core.cpp](../src/nsfw_filter_core.cpp)
  ONNX-backed detector core.
- [include/nsfw_filter.h](../include/nsfw_filter.h)
  Filter state and queue definitions.
- [include/nsfw_filter_core.h](../include/nsfw_filter_core.h)
  Public detector API.
- [CMakeLists.txt](../CMakeLists.txt)
  Build, download, staging, and install logic.

## Validation Paths

- [tests/nsfw_filter_core_test.cpp](../tests/nsfw_filter_core_test.cpp)
- [benchmarks/nsfw_filter_benchmark.cpp](../benchmarks/nsfw_filter_benchmark.cpp)
- [tools/nsfw_scan_ahead.py](../tools/nsfw_scan_ahead.py)
- [tools/nsfw_vlc_guard.py](../tools/nsfw_vlc_guard.py)
