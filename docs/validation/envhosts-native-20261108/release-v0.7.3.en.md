# envhosts v0.7.3 local tag and package validation

[简体中文](release-v0.7.3.md)

## Problem

On November 8, 2026, the user requested a new tag and `.wsplugin` containing
the verified native editor fixes. The earlier development package still
declared 0.7.2; this package declares 0.7.3.

## Cause

The version, fixed source, CHANGELOG, and archive manifest must agree. This
repository has no configured remote, so delivery consists of local commits,
an annotated tag, and the package.

## Changes

- `da06d5a` records the verified native dialog, split-item state, tests, and docs.
- `6ed276dc48bc45bc3ee1a0d4f74b9a61f4284463` bumps the version to 0.7.3 and
  prepares bilingual release notes and validation limits. The local annotated
  tag `v0.7.3` points to this commit.
- Package: `ra9fael.envhosts/dist/release-v0.7.3/ra9fael.envhosts-0.7.3.wsplugin`,
  with its `.sha256` and bilingual `release-notes.md` alongside it.

## Measured

Windows x64, MinGW Release, default native GDI editor. The plugin working tree
was clean when packaged.

| Check | Expected | Actual / evidence |
|---|---|---|
| Build and CTest | Current commit builds and tests pass | Build succeeded, CTest 3/3; `release-v0.7.3-ctest.txt` |
| Archive source and tag | Archive source matches tagged commit | All 79 source/document files match after Git line-ending normalization; `release-v0.7.3.json` |
| Archive and unpacked execution | Version 0.7.3, safe paths, runnable unpacked executable | 80 files, 5,940,418 bytes, 0 failures; protocol 37/37; `release-v0.7.3-package.txt` |
| Offline registry check | No errors | 0 errors; only the existing unreviewed-native warning, with no new permissions |
| Previously tested executable | Same binary as earlier GUI / L3 validation | Identical EXE SHA256; reuse this directory's `README.en.md` evidence: GUI 145, focused 19, dialogs 8 per locale/theme, and real host smoke |
| Tag and cleanup | Tag points to verified commit; temporary extraction removed | Annotated tag verified; this run's `package-check-*` directory cleaned by its temporary-directory context |

Package SHA256:
`a8f8c4b27b32ddd59aa9227472c64d21424e9922a7a01962c9236128b6aec20e`

Executable SHA256:
`1d301262a91081a7ed02fef81f44d0dc73cc939a7e1f8b3cdcb0310ad1bf2f5a`

The final archive, selected evidence, and `build/release-v0.7.3/` build/verification
logs are retained. The running user plugin, real environment, hosts, and plugin
data were not changed. Directories whose cleanup was blocked in the prior run
remain as previously documented; no deletion was retried.

## Not tested

- GUI / L3 were not repeated for this metadata-only version bump; binary hashes
  confirm that the earlier measured executable is packaged unchanged.
- MSVC / GitHub CI were not run. No tag push, GitHub Release, or online registry
  update was performed.
- Real elevated writes and physical multi-monitor DPI transitions remain outside
  the tested scope, as documented in this directory's `README.en.md`.

