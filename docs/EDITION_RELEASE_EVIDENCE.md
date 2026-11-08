# Dual-edition release evidence — 2026-11-09

Scope: GDI 0.7.5 from the 0.7.4 maintenance line; QML 0.8.0 from the approved
QML worktree. Both manifests use `ra9fael.envhosts`. GDI remains static-CRT;
QML was built and deployed with Qt 6.8.3/MSVC x64 and bundled VC runtime.

## Completed locally

- Both release executables built successfully. Package creation checks their
  `--build-info` against the manifest ID/version and edition backend.
- GDI CTest: four suites passed (logic, editor model, dialog layout and edition
  storage). The new storage checks cover legacy migration, original archives,
  existing destinations, draft isolation, no resurrection after discard,
  unknown formats, shared string backups and unsupported/mismatched backups.
- QML CTest: logic, editor model and edition storage passed; all 33 Qt Test
  cases passed after updating the recovery-path expectation to the new backend
  directory. Includes the save-selection regression and shared storage tests.
- The deployed QML executable passed the isolated protocol test: repeated
  editor opens reused windows, launcher writes were backed up and read back,
  registry types/unrelated scope were preserved, and shutdown completed.
- `tests/edition_packages.py` inspected the actual release archives, verified
  checksums and limits, and unpacked GDI -> QML -> GDI into one temporary plugin
  directory. Each executable reported the expected edition and answered queries
  against scratch registry/hosts targets, without Qt on PATH. Data sentinels,
  both draft paths and backup/settings bytes were unchanged. The final GDI
  directory contained no Qt DLLs.
- QML lifecycle probe: two open/close cycles passed, with no target writes.
  Working set was 24.16 MiB at startup, 73.50/74.03 MiB with both editors open,
  and 60.98/62.03 MiB after closing both. Action replies were 0.25–0.77 ms.
  These are fixture measurements, not a promise for every user's data/machine.

## Package checksums

| Package | SHA-256 |
|---|---|
| `ra9fael.envhosts-0.7.5.wsplugin` | `4bfa05c2f0ae0602cc8627e977fb70350d7644943d0730acccb207f29f450560` |
| `ra9fael.envhosts-qml-0.8.0.wsplugin` | `546f0ab3312ab084dd8dc38aa2b47b8530ba95010d2a70f000c09e484810f233` |

## Remaining manual acceptance

The user's running WinShun and installed plugin were not replaced or restarted.
The temporary directory test is not the host installer. Confirm same-version
replacement/downgrade, cancellation, locked-directory/install failure rollback
and preserved settings through the real installation UI. Extended IME, DPI and
accessibility acceptance remains as documented in the QML implementation notes.

The release pipeline is ready for both branches. GitHub publication, hosted CI
and plugin-library listing are distinct outcomes; the library is not modified
by creating a GitHub Release. The current index has no backend-aware updating.
