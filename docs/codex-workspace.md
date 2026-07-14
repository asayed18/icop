# Codex Workspace Scaffold

This repository contains a lightweight Codex workspace scaffold so future sessions can pick up project context quickly and work more consistently.

## Included Pieces

- `AGENTS.md`
  Durable repo instructions and project priorities.
- `.codex/config.toml`
  Project-scoped Codex document discovery settings.
- `.codex/rules/`
  Project rules for routine build and test command approval behavior.
- `.codex/agents/`
  Custom subagents for build/staging and runtime validation work.
- `.agents/skills/`
  Repo-local skills for repeated workflows.

## Expected Workflow

1. Read `AGENTS.md`.
2. Read `README.md` and `docs/index.md`.
3. Use `$build-plugin` for the standard build flow when the task is a build-only request.
4. If the task changes runtime behavior, use `$implementation-strategy`.
5. Make the code or documentation change.
6. Use `$code-change-verification`.
7. If VLC runtime behavior or installation is involved, use `$vlc-runtime-check`.
8. Optional personal prompt shortcuts:
   `/prompts:build`, `/prompts:portable-test`, `/prompts:installed-test`, `/prompts:scan-ahead`, `/prompts:guard-play`, `/prompts:bench`, `/prompts:runtime-triage`.

## Project-Specific Reminders

- The VLC plugin and ONNX detector core are separate layers.
- The live filter must not allow playback to outrun blocking decisions.
- Runtime packaging matters as much as code correctness.
- Installed VLC behavior can differ from staged or portable VLC because of plugin cache and side-by-side DLL issues.

## Common Commands

```powershell
cmake --build build-ninja --target icop_plugin icop_core -j 8
cmake --build build-ninja --target icop_package -j 8
cmake --install build-ninja
ctest --test-dir build-ninja --output-on-failure
```

`icop_package` creates a versioned release for the host OS under
`releases/v<version>/windows`, `releases/v<version>/linux`, or
`releases/v<version>/mac`, together with `release.json`, `SHA256SUMS`, and an
archive. `release-index.json` marks unbuilt platforms explicitly. Running the
target on one OS does not remove releases produced on the others.

## Skill Index

- `$build-plugin`
  Standard plugin build flow for this repository.
- `$implementation-strategy`
  Planning checklist for behavioral edits.
- `$code-change-verification`
  Verification checklist for builds, tests, and docs.
- `$vlc-runtime-check`
  Runtime checklist for stage, portable VLC, and installed VLC.

## Notes On Commands

Repo-shared custom workflows are best expressed as skills.

- Shared and recommended: `$build-plugin`
- Personal command shortcuts:
  `/prompts:build`, `/prompts:portable-test`, `/prompts:installed-test`, `/prompts:scan-ahead`, `/prompts:guard-play`, `/prompts:bench`, `/prompts:runtime-triage`

The personal prompt lives in the user-level Codex prompts directory rather than inside the repository.
