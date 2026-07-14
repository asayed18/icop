---
name: code-change-verification
description: Use after changing code, build logic, docs tied to runtime behavior, or VLC packaging. Produces a repo-specific verification pass for build, tests, benchmarks, and smoke-test coverage.
---

# Code Change Verification

## Minimum Pass

Run the smallest relevant set:

```powershell
cmake --build build-ninja --target nsfw_filter nsfw_filter_core -j 8
```

## If Core Logic Changed

```powershell
cmake --build build-ninja --target nsfw_filter_core_test -j 8
ctest --test-dir build-ninja --output-on-failure
```

## If Model Or Performance Logic Changed

```powershell
cmake --build build-ninja --target nsfw_filter_benchmark -j 8
```

## If Packaging Or VLC Runtime Changed

```powershell
cmake --build build-ninja --target nsfw_package -j 8
Copy-Item .\releases\v0.1.0\windows\plugins\video_filter\* .\vlc-portable\plugins\video_filter\ -Force
.\vlc-portable\vlc.exe -vvv --file-logging --logfile=vlc-portable-test.log --video-filter=nsfw .\sample.mp4
```

## Review Checklist

- Did the build succeed?
- Were tests run when behavior changed?
- Was the relevant smoke test run when packaging/runtime changed?
- If docs describe changed behavior, were they updated?
- If something was not run, say so explicitly.
