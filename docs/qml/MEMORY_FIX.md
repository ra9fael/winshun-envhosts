# QML memory and launcher handoff repair

Package: `0.8.0-qml.7`, `dist/qml-preview-10/ra9fael.envhosts-qml.wsplugin`.

## Findings

The previous engine lifetime fix alone did not return the process to its startup
footprint. An isolated run of `0.8.0-qml.6` reproduced approximately 26 MiB at
startup, 271 MiB with env open, 290 MiB with both windows open, and 245 MiB after
both closed. Its first env action took 1828 ms; WinShun discards action responses
after the manifest's 1500 ms deadline, including the `hide` effect. The subsequent
hosts action took 34 ms. Both paths already requested `hide`.

Changing only the rendering backend to software left 233 MiB after closing.
Adding `windows:fontengine=gdi` to that same executable reduced the closed
working set to 65 MiB and private commit from 125 MiB to 22 MiB. This A/B result
identifies the font backend as a major contributor on this machine; it is not
an allocation-by-allocation heap profile or a claim that all retained memory
is a leak.

## Changes

- Keep lazy QML engine creation and deferred engine destruction after all window
  objects are gone.
- Default to the Windows GDI font backend, selected before QGuiApplication is
  created, and Qt Quick software/native text rendering for the 2D editor.
  An explicit `QT_QPA_PLATFORM` remains available for diagnostics.
- Release scene resources on accepted close. Keeping another editor window
  open preserves its engine and editing state.
- Queue QML creation without blocking the protocol thread. Both the editor
  entry and backup entry return their hide effect promptly. The native GDI
  build retains its original synchronous path.
- Record numeric open timing and memory measurements to stderr; asynchronous
  creation errors are also logged there.

No working-set trimming is used. The resident protocol process remains alive;
post-close memory is not expected to equal a freshly started process.

## Verification

The final packaged executable passed three isolated open/close cycles using
scratch registry keys and a temporary hosts file. All scratch targets remained
unchanged. Closing env preserved hosts, reopening worked, and protocol shutdown
completed normally. Existing WinShun and installed plugin processes were not
controlled or replaced.

| State | qml.6 first cycle | qml.7 first cycle |
|---|---:|---:|
| Startup working set | 26.09 MiB | 26.12 MiB |
| Env open | 271.20 MiB | 65.24 MiB |
| Env + hosts open | 289.95 MiB | 73.38 MiB |
| All closed | 244.68 MiB | 61.14 MiB |
| Closed private commit | 172.21 MiB | 20.86 MiB |
| First env action response | 1828.29 ms | 0.34 ms |

Final closed working sets were 61.14, 61.03 and 61.62 MiB across three cycles;
all six action replies were under 1 ms. These are measurements on this machine
with small fixture data. Actual WinShun search-window handoff, real data,
scrolling and fractional DPI rendering remain for user verification.

Reproduction: `tools/check_qml_lifecycle.py` accepts an executable, optional
backend/font overrides, cycle count and JSON output path. Evidence:

- `build-qml/evidence/lifecycle-qml6-hardware.json`
- `build-qml/evidence/lifecycle-qml6-software.json`
- `build-qml/evidence/lifecycle-qml6-software-gdi.json`
- `build-qml/evidence/lifecycle-qml7-final.json`

The build succeeded and the staged executable SHA-256 matches the build.
