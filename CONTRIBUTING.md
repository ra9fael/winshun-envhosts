# Contributing to ra9fael.envhosts

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting a bug

Open an issue with:

- the plugin version (Win顺 → 设置 → 插件 → 已安装) and your Win顺 version,
- Windows version and display setup (number of monitors, scaling),
- what you typed or did, what you expected, and what happened,
- the plugin's messages from Win顺's message log (消息记录), if any.

Remove anything private (paths with your user name, host names, IPs,
environment values) before posting.

## Development setup

- Windows 10/11 x64.
- CMake 3.21+ and a C++17 compiler: Visual Studio 2022 or later (MSVC, what
  CI and releases use) or MinGW-w64 GCC 13+.
- Python 3.10+ for `tools/package.py` and the integration tests (standard
  library only).
- Win顺 itself, to try the plugin.

```powershell
cmake -S . -B build -A x64                 # MSVC; for MinGW use -G "MinGW Makefiles"
cmake --build build --config Release       # GDI: envhosts.exe; QML: build/bin/envhosts.exe
ctest --test-dir build -C Release --output-on-failure
```

To run your build inside Win顺, link the repository into Win顺's plugin folder
instead of installing a copy (adding a folder through the settings page copies
it, so later builds would not reach Win顺):

```powershell
New-Item -ItemType Junction -Path "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.envhosts" -Target (Get-Location)
```

Then enable the plugin in **设置 → 插件 → 已安装**. Win顺 refuses to replace a
junction when you install a package of the same id. **Do not uninstall a linked
plugin from the settings page**: uninstall removes the folder recursively and
has no junction check, so it can delete the files in your clone. Remove the link
itself with `cmd /c rmdir "$env:LOCALAPPDATA\WinShun\plugins\ra9fael.envhosts"`
(this deletes only the junction). A running `envhosts.exe` is locked; rename it
(for example to `envhosts.exe.old`) before rebuilding, and Win顺 restarts the
plugin with the new exe. Remove an installed non-linked copy of the same id
first.

`winshun.h` is a byte-for-byte copy of the Win顺 SDK header
(`WinShun/src/plugins-sdk/winshun.h`). Do not edit it here; update it from
upstream as a separate commit.

## Tests

### QML design-to-code handoff

Before changing the Qt editor, read
[`docs/qml/QT_IMPLEMENTATION_STANDARD.md`](docs/qml/QT_IMPLEMENTATION_STANDARD.md).
It maps the approved prototype to QML components, C++ commands and acceptance
tests. Include the affected F / C / T IDs in each implementation slice and use
its evidence template. Shared visual values come from `design-tokens.json`;
regenerate the SVGs and prototype CSS rather than editing generated output.

Run `python tools/check_qml_contract.py` for the design handoff and the relevant
product tests for the implementation. The handoff check proves references and
design consistency only. When planned source files are implemented, explicitly
update their traceability states and the checker with product evidence; do not
remove the coverage checks or treat prototype tests as Qt acceptance. Prefer
the already-running WinShun instance for integration; use isolated storage for
all write tests. Product implementation and evidence are recorded in the QML documentation.

### Existing regression suites

- `ctest --test-dir build --output-on-failure` runs `envhosts_tests` (69 checks)
  and `editor_model_tests` (155 checks). Both are OS-free: hosts parsing with a
  byte-exact round trip on `tests/fixtures/hosts.sample`, the env model, the
  query grammar, validation, UTF-16 length limits, split/join (including the
  whitespace delimiter), row states, the three-snapshot diff, drafts and
  one-undo batch transactions.
- `python tests/integration.py` drives the built `envhosts.exe` over JSON-RPC
  (37 assertions). It edits a scratch registry key and a temporary hosts copy,
  never your real environment or hosts file, and checks that a write to the real
  System32 hosts is refused for a normal user. Run it before a PR that touches
  `main.cpp`, `native.h` or the locale resolution.
- `python tests/gui_preview.py --no-hold` opens both real windows, captures PNGs
  under `tests/fixtures/` and runs 103 Win32 assertions (layout, scrolling, the
  full-width item list, in-place editing, per-item dirty markers, multi-select,
  batch commands, the external-change watcher and read-only diff panel, the
  two-pane backup page, the space delimiter, the over-long `Path` sequence and
  live language switching). It uses isolated HKCU test subkeys and a temporary
  hosts copy and deletes both subkeys afterwards. Without `--no-hold` it keeps
  the windows open for manual inspection; **always** use `--no-hold` in CI.
- `python tests/verify_install.py` checks an unpacked, real-install `.wsplugin`
  (no junction). Use it after packaging, not on the working tree.

UI changes need both language variants. Every user-visible string in
`editor_win.h` is an inline `m_args.zh ? "中文" : "English"` pair resolved at
draw time; `i18n.h` only decides which language is active. The two read-only
Win32 hooks `WM_APP_KEY` (explicit modifier mask) and `WM_APP_GETSTATE` (packed
selection / deletion / disabled counts) exist solely so the GUI driver can
assert state; do not route them through JSON-RPC or use them for writes.

## Pull requests

1. Fork, create a topic branch from the relevant `gdi` or `qml` branch.
2. Keep a pull request to one topic. Follow the existing style: C++17; GDI has no extra runtime, QML bundles its Qt runtime. Code and comments stay in English.
   Documentation is bilingual: `README.md` is the Chinese default and
   `README.en.md` its English counterpart (see "Documentation languages").
   User-facing strings in plugin.json stay bilingual zh/en.
3. Add or update unit tests for logic changes. CI must pass.
4. Before opening the PR, run `env` and `hosts` in Win顺 and check one edit and one restore.
5. If behaviour, settings or permissions change, update **both**
   `README.md` and `README.en.md`, and add a line under `## [Unreleased]` in
   `CHANGELOG.md`.
6. Commit messages use the Angular convention
   ([Conventional Commits](https://www.conventionalcommits.org/)):
   `type(scope): imperative summary`, a blank line, then the body. Allowed
   types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`,
   `ci`, `chore`, `revert`. Lowercase summary, no trailing period.

Do not commit `envhosts.exe`, `build/` or `dist/`; `.gitignore` already excludes them.

### Permissions

Adding or widening a permission in `plugin.json` needs a reason in the PR and
in the README's permissions section. Win顺 asks users to confirm permissions
again when they change, so keep them minimal.

## Test data

Never commit a real hosts file, real environment values, user names, machine
names or private IP addresses. `tests/fixtures/hosts.sample` keeps the
structure of a real file (UTF-8 BOM, CRLF, a Docker section, a duplicate host)
with RFC 5737 documentation addresses. Extend it the same way.

## Documentation languages

Win顺 is Chinese-first but also ships an English UI (`winshun_en.ts`), so the
user-facing documentation follows the same split:

| File | Language | Role |
|---|---|---|
| `README.md` | 简体中文 | Default. GitHub renders it on the repository home page. |
| `README.en.md` | English | Full counterpart for English-UI users. |

Both files start with a language switch and end with a link back, so readers can
jump in either direction. **Keep both in sync**: any change to structure,
settings, permissions or install steps must be applied to both in the same PR.
Code identifiers, settings keys, command syntax, JSON and paths stay in English
in both files. The English README uses Win顺's real English UI labels
(`Settings → Plugins → Installed → Add plugin → Archive (.zip / .wsplugin)`),
while the Chinese one uses the Chinese labels.

`CONTRIBUTING.md`, `CHANGELOG.md`, `TESTPLAN.md` and `ROADMAP.md` stay in
English.

## Releasing (maintainers)

Maintain `gdi` and `qml` independently. Port common storage/protocol fixes to
both; keep interface changes on their own branch. Read [DUAL_EDITIONS.md](docs/DUAL_EDITIONS.md)
for build, package, migration and release acceptance requirements.

`plugin.json` is the only version source. Tags are `gdi/vX.Y.Z` or `qml/vX.Y.Z`.
The package tool checks the manifest, edition and executable before writing an
immutable package and SHA-256. QML bundles its dynamic Qt/VC dependencies;
GDI remains a standalone executable. Never replace published release assets.

Run CI, create/push the reviewed tag, and dispatch the release workflow once
for that tag. Local verified assets may instead be published with
`gh release create --verify-tag --notes-file ...`; never use both publishing
paths for the same tag. GDI is the default Latest release; QML uses `--latest=false`.
Operate GitHub sequentially, inspect each result, and avoid bulk/repeated requests.

The registry currently accepts only a single package URL for an ID. Keep its
entry on GDI; adding backend-aware installation/updates needs host and index
changes. Library submission is separate from publishing GitHub Releases.

## License

By contributing you agree that your contribution is licensed under the MIT
license of this repository.
