# Contributing to VLC iClean

Thank you for improving VLC iClean. Contributions should preserve the central
safety invariant: a frame that requires analysis must not be shown before its
blocking decision is available.

## Before You Start

- Search existing issues and pull requests.
- Use an issue for substantial behavior, architecture, or packaging changes.
- Never submit explicit media, private videos, credentials, generated models,
  runtime binaries, build trees, or release archives.
- Use synthetic or redistributable fixtures and sanitize logs.

## Build and Test

Quick Linux or WSL verification without model downloads:

```sh
cmake -S . -B build-ci -G Ninja \
  -DNSFW_DOWNLOAD_MODELS=OFF \
  -DNSFW_EXPORT_FALCONSAI_BASE_ONNX=OFF \
  -DNSFW_BUILD_PLAYER_PROTOTYPE=OFF \
  -DNSFW_BUILD_BENCHMARKS=OFF
cmake --build build-ci --target nsfw_filter nsfw_filter_core nsfw_filter_core_test -j 2
ctest --test-dir build-ci --output-on-failure
```

The standard Windows development build is:

```powershell
cmake --build build-ninja --target nsfw_filter nsfw_filter_core nsfw_filter_core_test -j 8
ctest --test-dir build-ninja --output-on-failure
```

Real-model integration tests require the downloaded ONNX models. Packaging
requires a normal configuration with `NSFW_DOWNLOAD_MODELS=ON`.

## Pull Requests

- Keep changes focused and explain the user-visible behavior.
- Add or update tests for behavior changes.
- Update documentation when configuration, packaging, or runtime behavior changes.
- State which builds, tests, benchmarks, and runtime checks were run.
- Preserve existing file-level license notices.

By contributing, you agree that your contribution is licensed under the
repository license and any compatible file-level license already present in
the file you modify.
