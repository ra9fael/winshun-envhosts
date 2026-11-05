# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`.

## [Unreleased]

### Added
- The env window is split into two vertically resizable panes (table / detail)
  with a draggable splitter; hosts and the backup view keep the fixed detail
  strip.
- Delimited-item editing of an env value: `Ctrl+R` or the mode button splits the
  value into items, then `Ins` / `Del` / `Alt+↑↓` / the item box edit one segment
  and rejoin it into the value. `Path` is pinned to `;`, other values cycle the
  delimiter through `; , : TAB |` with the delimiter button.

### Changed
- `README.md` is now the Chinese (default) documentation; `README.en.md` holds
  the full English text. Both link to each other. The English install steps now
  use Win顺's real English UI labels. No runtime change.

### Fixed
- Backup ids are now millisecond-precise. Two snapshots taken in the same second
  previously shared a file name, so the older one was silently overwritten and an
  undo point was lost.
- `tests/integration.py` decodes the plugin's UTF-8 JSON-RPC pipe explicitly
  (it used to inherit the ANSI codepage and crash on non-ASCII rows) and waits
  for the plugin process to exit, so a test run can no longer leave an instance
  holding `envhosts.exe` open during the next link step.

## [0.1.0] - 2026-11-05

First public release.

### Added
- `env`: list, filter, edit, copy (raw / expanded), delete with confirmation
  and restore user environment variables; system variables are shown read-only.
- `hosts`: list, filter, toggle comment, copy, delete, add and restore hosts
  entries. Writes need an elevated WinShun; otherwise the plugin reports the
  denial instead of pretending to succeed.
- A backup before every write, with a configurable `backupCount`.
- Byte-exact hosts round trip (UTF-8 BOM, CRLF, section comments preserved).

[Unreleased]: https://github.com/ra9fael/winshun-envhosts/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.1.0
