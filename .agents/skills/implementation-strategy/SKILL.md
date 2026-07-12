---
name: implementation-strategy
description: Use when changing VLC filter behavior, detector-core behavior, queueing, buffering, block windows, model/profile resolution, provider selection, or runtime packaging. Produces a short implementation plan grounded in this repository before editing.
---

# Implementation Strategy

## Use This Skill When

- changing `modules/video_filter/nsfw_filter.c`
- changing `src/nsfw_filter_core.cpp`
- changing model defaults or fallback logic
- changing buffering, block padding, or worker behavior
- changing install or staging behavior in `CMakeLists.txt`

## Checklist

1. Identify which layer is changing:
   - VLC filter
   - detector core
   - build/install/runtime packaging
2. Name the exact files to touch.
3. State the safety risk:
   - unsafe frame exposure
   - performance regression
   - packaging/runtime regression
   - config/UI drift
4. Decide the minimum verification needed:
   - build only
   - targeted tests
   - benchmark
   - portable VLC smoke test
   - installed VLC smoke test
5. If user-visible behavior changes, plan the doc update at the same time.

## Project Notes

- Prefer preserving the invariant that visible playback should not outrun the analysis window.
- Check whether the issue belongs in queue logic, pixel packing, model/provider resolution, or packaging before editing.
- Do not assume optional model files are installed.
