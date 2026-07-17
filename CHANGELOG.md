# Changelog

This file records the repository history by commit. Entries are ordered newest
first, and short hashes identify the commit that introduced each change.

## Unreleased

No unreleased changes are documented yet.

## 0.1.2 - 2026-07-17

### Windows packaged runtime repair

- Fixed side-by-side model and ONNX Runtime path resolution for the Windows
  plugin so detector initialization no longer falls back to an unrelated
  system `onnxruntime.dll`.
- Pinned downloaded ONNX Runtime headers to the packaged runtime version and
  refresh stale cached headers when that version changes.
- Added a regression test for module-sibling paths and a release smoke-test
  requirement that confirms VLC loads the packaged runtime.

## 0.1.0 - 2026-07-14

First public source-only release of the icop VLC content filter.

### Public discoverability

- Added a lightweight animated walkthrough preview to the README and improved
  its VLC, ONNX Runtime, local AI, privacy, and cross-platform descriptions.
- Expanded the public GitHub topics and repository description for relevant
  search and discovery terms.

### Rename to icop

- Renamed the project, VLC module shortcut, build targets, runtime binaries,
  release archives, repository metadata, and public documentation to `icop`.
- Kept the internal `nsfw_*` detector API and saved configuration keys stable
  to avoid unnecessary compatibility and classification risk.
- Added a Makefile for common build, test, release, and installation workflows.
- Added a checksum-verified Windows installer that detects VLC, selects the
  matching release architecture, removes legacy files, and regenerates the
  plugin cache.
- Added equivalent Linux/macOS installation with native VLC path detection,
  rollback, optional `sudo`, and host-OS dispatch from `make install_plugin`.

### Public repository preparation

- Added GPL-2.0-or-later repository licensing, third-party notices, trademark
  clarification, contribution guidance, security and support policies, a code
  of conduct, citation metadata, and GitHub Sponsors configuration.
- Added structured issue forms, a pull-request template, CODEOWNERS,
  Dependabot, Windows/Linux CI, and CodeQL analysis.
- Added a lightweight `NSFW_DOWNLOAD_MODELS=OFF` mode for compile-focused CI
  while keeping real-model testing and release packaging enabled by default.
- Reworked public documentation links, safety/privacy guidance, platform
  status, badges, release instructions, and source-only publication policy.

### D3D11 debug overlay

- Added a cached GPU-composited score and threshold overlay for D3D11 opaque
  pictures without forcing software conversion or full-frame CPU readback.
- Preserved the source picture if overlay composition fails and moved blocked
  frame dumps after overlay rendering so captures contain the final output.
- Extended the D3D11 runtime check to require live overlay rendering on P010
  input and documented GPU overlay behavior.

## 2026-07-13 - `2cf1710` - Add D3D11 GPU video processing backend

- Added an end-to-end D3D11 backend for opaque NV12 and P010 VLC pictures,
  including model-sized GPU conversion and readback.
- Added GPU black, strong separable blur, and bottom-right warning watermark
  effects with CPU fallback and fail-closed error handling.
- Added backend selection, resource pooling, profiling, runtime packaging, and
  a portable VLC integration harness covering effects, timing, queue limits,
  media-time decisions, and fallback behavior.

## 2026-07-13 - `93462bb` - Improve NSFW block rendering and debug overlay

- Expanded software pixel-format support and improved black, blur, and warning
  rendering across planar, semi-planar, packed RGB, and higher-bit-depth video.
- Added the score/threshold debug panel and strengthened blocked-frame dumping
  and standalone player rendering behavior.
- Updated build configuration and documentation for the new output controls.

## 2026-07-13 - `9162620` - Normalize NSFW model thresholds

- Added per-profile score normalization so model outputs share consistent
  sensitivity thresholds while preserving explicit user overrides.
- Added normalization and profile tests and updated defaults and documentation.

## 2026-07-13 - `1f418c6` - Fix CUDA auto selection and docs

- Corrected automatic CUDA provider selection and fallback behavior in the
  plugin and detector core.
- Updated runtime dependency packaging and provider documentation.

## 2026-07-13 - `b136af4` - Fix blocked-frame mute restore

- Corrected audio mute state tracking so playback is unmuted after the blocked
  output window ends.
- Improved output-mask timing and synchronization and documented mute behavior.

## 2026-07-13 - `5561cde` - Add Codex scaffold

- Added repository collaboration guidance, local build and verification skills,
  agent configuration, command rules, and workspace documentation.
- Added the main README covering architecture, configuration, build, install,
  testing, and runtime workflows.

## 2026-07-13 - `ee90d5f` - Fallback to installed model profiles

- Added model resolution that searches installed plugin locations when a
  selected runtime model file is missing.
- Improved diagnostics around selected and fallback model paths.

## 2026-07-12 - `614395b` - Add local VLC compatibility headers

- Added the VLC 3 compatibility declarations needed to build the plugin without
  relying on unavailable private development headers.
- Expanded local common, filter, picture, fourcc, audio, input, variable, and
  plugin API shims.

## 2026-07-12 - `342c394` - Add exported Falconsai base ONNX profile

- Added the exported Falconsai base model profile throughout the core, plugin,
  standalone player, benchmarks, and tests.
- Added a reproducible ONNX export utility and model packaging support.

## 2026-07-12 - `7a45df6` - Sanitize optional Falconsai download logging

- Prevented optional model download details from producing noisy or unsafe
  CMake log output while preserving useful failure diagnostics.

## 2026-07-12 - `72541b3` - Add official Falconsai ONNX profile

- Added the official quantized Falconsai profile to model selection, inference,
  benchmarks, tests, and packaging.
- Made its optional model download and integration tests conditional on the
  model artifact being available.

## 2026-07-12 - `aa1ebdc` - Add ONNX integration benchmarks and tests

- Established the VLC NSFW filter, reusable detector core, dynamic loader, and
  standalone FFmpeg prototype.
- Added ONNX preprocessing and inference, buffered VLC filtering, scan-ahead
  and guard tools, model fixtures, unit and integration tests, and benchmarks.
- Added the initial CMake build, compatibility headers, sample media, public
  APIs, and repository ignore rules.
