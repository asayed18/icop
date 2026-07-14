# Release Process

icop uses semantic versions and creates platform-specific release trees.

## Prepare a Version

```powershell
cmake -S . -B build-ninja "-DICOP_RELEASE_VERSION=0.2.0"
cmake --build build-ninja --target icop_package -j 8
```

Run the same `icop_package` target from Linux and macOS build hosts to populate
their platform folders. Never present an unbuilt platform marker as a release.

## Verify

- Run the complete test suite on every built platform.
- Verify each platform `SHA256SUMS` file.
- Verify the archive `.sha256` file.
- Inspect `release.json` and `release-index.json`.
- Confirm the archive contains only the plugin, detector core, runtime,
  selected models, metadata, and checksums.
- Review [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) before distributing
  any model or runtime binary.

## Public Repository Policy

The Git repository is source-only. Generated `releases/`, build trees, models,
runtimes, and portable VLC files remain ignored. Binary GitHub Releases require
a separate third-party redistribution review.
