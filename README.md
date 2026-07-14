<p align="center">
  <img src="assets/branding/icop-steel-scanner-animated.webp" width="256" alt="icop scanner icon">
</p>

<h1 align="center">icop</h1>

<p align="center">
  A privacy-first VLC video filter that detects and masks sensitive frames before they are shown.
</p>

<p align="center">
  <a href="https://github.com/asayed18/icop/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/asayed18/icop/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/asayed18/icop/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/asayed18/icop/actions/workflows/codeql.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: GPL-2.0-or-later" src="https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg"></a>
  <a href="https://github.com/sponsors/asayed18"><img alt="Sponsor on GitHub" src="https://img.shields.io/badge/sponsor-GitHub-EA4AAA.svg?logo=githubsponsors"></a>
</p>

> [!IMPORTANT]
> icop is a best-effort content filter. Detection models can produce
> false positives and false negatives, so test your configuration before
> relying on it for child-safety or accessibility needs.

## Overview

icop adds an `icop` video filter to VLC. It holds frames until they have
been classified, then displays the original frame or applies the selected black,
blur, or warning treatment. Processing stays on your device; video frames are
not uploaded to a service.

The public Git repository is source-only. Models, ONNX Runtime binaries, build
trees, portable VLC copies, and release archives are downloaded or generated
locally and are not committed.

## Platform Status

| Platform       | Status               | Current path                                   |
| -------------- | -------------------- | ---------------------------------------------- |
| Windows x86_64 | Tested               | CPU or D3D11 processing; CPU or CUDA inference |
| Linux x86_64   | Tested on WSL/Ubuntu | Portable synchronous CPU path                  |
| macOS          | Experimental         | Build path present, not yet validated          |

Windows and Linux are the current priorities. Contributions that improve native
Linux coverage or validate the macOS path are welcome.

## Highlights

- Buffers frames so playback does not intentionally outrun classification
- Runs inference locally with ONNX Runtime
- Supports multiple model profiles and automatic model-sized preprocessing
- Provides black, blur, and warning block styles
- Can mute audio while blocked output is shown
- Supports precomputed decision maps for scan-ahead playback
- Includes a D3D11 processing path for supported Windows hardware decoding
- Produces versioned, platform-specific packages with checksums and metadata
- Includes unit, integration, and benchmark targets

## How It Works

1. VLC decodes a video frame.
2. icop holds the frame before presentation.
3. The detector converts and resizes a sample for the selected model.
4. ONNX Runtime classifies the sample and applies the configured block window.
5. The plugin releases the original or masked frame to VLC.

The central safety invariant is simple: a frame that requires analysis should
not be shown before its decision is available.

## Quick Start

Requirements:

- CMake 3.16 or newer
- Ninja or another supported CMake generator
- A C compiler and a C++17 compiler
- VLC 3.0 development files for the target platform

Configure, build, and package:

```powershell
make build
make test
make release
```

On Windows, use `mingw32-make` instead of `make` when that is the installed GNU
Make command. Direct CMake commands remain available in
[CONTRIBUTING.md](CONTRIBUTING.md).

The package is written to `releases/v<version>/<os>/`. Copy the files under
`plugins/video_filter/` into VLC's matching plugin directory, regenerate VLC's
plugin cache when required, and enable the filter:

```text
--video-filter=icop
```

When upgrading from VLC iClean, remove `libnsfw_filter_plugin` and
`nsfw_filter_core` files from VLC's plugin directory before regenerating the
plugin cache. The new runtime files are named `libicop_plugin` and `icop_core`.

### Install on Windows

Build the release, detect the installed VLC, copy the matching x86/x64/ARM64
payload, and regenerate VLC's plugin cache:

```powershell
mingw32-make install_plugin
```

The installer validates release checksums, removes legacy VLC iClean files, and
requests administrator access only when VLC is installed in a protected
directory. Preview the operation or select a specific installation directly:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\install_icop_plugin.ps1 -WhatIf
powershell -ExecutionPolicy Bypass -File .\tools\install_icop_plugin.ps1 -VlcRoot "C:\Program Files\VideoLAN\VLC"
```

Close VLC before installation, or pass `INSTALL_ARGS=-StopVlc` to the Make
target. The automatic installer currently supports Windows; Linux packages can
be installed manually into the distribution's VLC plugin directory.

See [CONTRIBUTING.md](CONTRIBUTING.md) for lightweight Windows, Linux, and WSL
build and test commands. See [docs/releasing.md](docs/releasing.md) for package
versioning, checksums, and release verification.

## Model Profiles

| Profile     |   Input | Upstream project                                                                          | Upstream license             |
| ----------- | ------: | ----------------------------------------------------------------------------------------- | ---------------------------- |
| `marqo`     | 384x384 | [Marqo NSFW image detection](https://github.com/marqo-ai/nsfw-image-detection-384)        | Apache-2.0                   |
| `adamcodd`  | 384x384 | [AdamCodd/vit-base-nsfw-detector](https://huggingface.co/AdamCodd/vit-base-nsfw-detector) | Apache-2.0                   |
| `falconsai` | 224x224 | [Falconsai/nsfw_image_detection](https://huggingface.co/Falconsai/nsfw_image_detection)   | Verify before redistribution |
| `legacy`    | 299x299 | [iola1999/nsfw-detect-onnx](https://github.com/iola1999/nsfw-detect-onnx)                 | MIT                          |

Model and runtime files retain their upstream licenses. Review
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before redistributing a binary
package.

## Configuration

The VLC module settings cover the most common choices:

| Setting                    | Purpose                                                    |
| -------------------------- | ---------------------------------------------------------- |
| Model profile and path     | Select a bundled profile or custom ONNX model              |
| ONNX provider              | Choose CPU, CUDA, or automatic provider selection          |
| Processing backend         | Choose portable CPU or supported Windows D3D11 processing  |
| Detection threshold        | Control the model score that triggers blocking             |
| Analysis stride and buffer | Balance coverage, latency, and throughput                  |
| Block style and padding    | Choose the mask and extend detections around unsafe frames |
| Audio muting               | Mute playback while blocked frames are presented           |
| Decision map               | Reuse precomputed blocked time ranges                      |

### Recommended Settings

Use this best-practice profile as a sensitivity-focused starting point:

| VLC option                   | Recommended value |
| ---------------------------- | ----------------- |
| Model profile                | `marqo`           |
| Detection threshold          | `0.17`            |
| Mute audio on blocked frames | Enabled (`1`)     |
| Blocked frame style          | Black out         |
| Analysis stride              | `8`               |
| Block padding                | `20` frames       |
| Buffered frames              | `3`               |
| Worker threads               | `8`               |
| CUDA device id               | `0`               |

CUDA device `0` is used only when the CUDA execution provider is selected. The
runtime may reduce the effective worker count for providers that do not benefit
from parallel detector sessions. A `0.17` threshold prioritizes sensitivity and
can produce more false positives than the default threshold.

Defaults are designed for local use, but no single threshold or model is right
for every video. Validate the selected profile with representative, legally
shareable media.

## Build and Test

Build the plugin, detector core, tests, and benchmark:

```powershell
cmake --build build-ninja --target icop_plugin icop_core icop_test icop_benchmark -j 8
ctest --test-dir build-ninja --output-on-failure
```

CI also supports a lightweight source build with model downloads disabled. The
full integration suite requires locally downloaded ONNX models. Runtime testing
requires a VLC installation or portable VLC tree compatible with the package.

## Privacy and Limitations

- Classification runs locally and does not require uploading video frames.
- Optional debug dumps can write frame data to disk and should stay disabled for
  private media.
- Logs may contain local paths and should be reviewed before sharing.
- Detection quality depends on the model, threshold, source material, and
  processing configuration.
- Public issues and tests must use synthetic or redistributable media, never
  private or explicit personal content.

## Project Links

- [Contributing guide](CONTRIBUTING.md)
- [Release process](docs/releasing.md)
- [Changelog](CHANGELOG.md)
- [Security policy](SECURITY.md)
- [Support guide](SUPPORT.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## Support the Project

icop accepts sponsorship only through
[GitHub Sponsors](https://github.com/sponsors/asayed18). Sponsorship does not
affect issue priority, security handling, or project licensing.

## License

icop is licensed under
[GPL-2.0-or-later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html). See
[LICENSE](LICENSE) for the full license text.

VLC and VideoLAN are trademarks of VideoLAN. This independent project is not
affiliated with or endorsed by VideoLAN.
