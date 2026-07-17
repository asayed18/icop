# Release Process

icop uses semantic versions and creates platform-specific release trees.

## Prepare a Version

```powershell
make release VERSION=0.2.0
```

The equivalent direct CMake commands use
`-DICOP_RELEASE_VERSION=0.2.0` followed by the `icop_package` target.

Run the same `icop_package` target from Linux and macOS build hosts to populate
their platform folders. Never present an unbuilt platform marker as a release.

## Verify

- Run the complete test suite on every built platform.
- Verify each platform `SHA256SUMS` file.
- Verify each archive `.sha256` file. The self-contained Windows CUDA archive
  is split into `.7z.001`, `.7z.002`, and so on; download every volume and
  extract `.7z.001` with 7-Zip.
- Inspect `release.json` and `release-index.json`.
- Confirm the archive contains only the plugin, detector core, runtime,
  selected models, metadata, and checksums.
- On Windows, run an installed or portable VLC smoke test and confirm the
  detector loads the side-by-side model and ONNX Runtime rather than a system
  DLL.
- Review [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) before distributing
  any model or runtime binary.

## Public Repository Policy

The Git repository is source-only. Generated `releases/`, build trees, models,
runtimes, and portable VLC files remain ignored. Binary GitHub Releases require
a separate third-party redistribution review.
