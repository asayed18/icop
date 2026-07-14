---
name: build-plugin
description: Use when the task is to build the VLC plugin or detector core for this repository. Runs the standard repo build flow first, and optionally stage-installs if the task asks for runtime testing or packaging.
---

# Build Plugin

## Use This Skill When

- the user asks to build the plugin
- the user asks to rebuild after code changes
- the user asks for a quick compile sanity check

## Standard Command

```powershell
cmake --build build-ninja --target icop_plugin icop_core -j 8
```

## If Tests Or Benchmarks Are Relevant

```powershell
cmake --build build-ninja --target icop_test icop_benchmark -j 8
```

## If Runtime Packaging Is Relevant

```powershell
cmake --build build-ninja --target icop_package -j 8
```

This creates the current host package under
`releases/v<version>/<os>/plugins/video_filter`, plus release metadata,
checksums, and an archive. Use `cmake --install build-ninja` only when the task
specifically asks to install directly into a configured VLC tree.

## Reporting

Always state:

- whether the build succeeded
- which targets were built
- whether tests were also built or run
- whether `icop_package` or `cmake --install build-ninja` was run
- what the next logical runtime step is, if any
