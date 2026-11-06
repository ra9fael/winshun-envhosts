# Changelog

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`.

## [Unreleased]

### Added
- Custom delimiters. The env delimiter control is now a combo with five localised
  presets (`分号 / 逗号 / 冒号 / 制表符 / 竖线`) that also accepts any free text such
  as `=>`, and it takes effect only on `Enter` or the `应用` button: the items are
  rejoined with the old delimiter first and re-split afterwards, so `;` → `=>` → `;`
  returns the identical value. The choice is remembered per `<scope>/<name>`.
- `F5` reloads the window from disk. With unsaved edits it confirms first, and it
  lands back on the row you were on.
- `Tab` / `Shift+Tab` move forwards and backwards between the detail fields and hand
  focus to the table at either end; `Ctrl+Tab` / `Ctrl+Shift+Tab` switch tabs from
  inside a field.
- Separators inside `"quoted"` runs are no longer split points, so a PATH-style
  value with a quoted `;` survives a split/join round trip.
- The launcher's `env` and `hosts` entries use real glyphs instead of a colour swatch.
- The env window is split into two vertically resizable panes (table / detail)
  with a draggable splitter; hosts and the backup view keep the fixed detail
  strip.
- Delimited-item editing of an env value: `Ctrl+R` or the mode button splits the
  value into items, then `Ins` / `Del` / `Alt+↑↓` / the item box edit one segment
  and rejoin it into the value. `Path` is pinned to `;`. (The cycling delimiter
  button that came with this is superseded by the combo above.)

### Changed
- The delimiter is a self-drawn dropdown button with a preset menu instead of a
  native combo; picking a preset applies it at once, and `自定义…` opens a text box
  in its place that is applied with Enter / `应用`.
- hosts read-only is reported only in the status bar (`只读（需管理员）`); the red
  banner, which also covered the top of the table and the backup list, is gone.
- The mode toggle, the delimiter combo and `应用` share one flat self-drawn style on
  one row and follow the dark theme; the env detail pane's minimum height is 200px.
- An external user-scope change at save time now offers 中止 / 重试 / 忽略 (abort
  writes nothing, retry reloads, ignore force-overwrites) instead of cancelling.
- `tests/gui_preview.py` is an assertion suite (30 checks over the live window)
  rather than a screenshot run. It now redirects the system scope too, through a
  new test-only `WS_ENVHOSTS_TEST_SYSTEM_SUBKEY` override (the path may name its
  own root, e.g. `HKCU\…`), so the captured 系统 tab shows fake rows instead of
  this machine's real HKLM variables.
- `README.md` is now the Chinese (default) documentation; `README.en.md` holds
  the full English text. Both link to each other. The English install steps now
  use Win顺's real English UI labels. No runtime change.

### Fixed
- A leftover draft is no longer applied silently when the window opens. A draft
  that had marked the user `Path` deleted made the row look locked: greyed fields,
  no delimiter, no item buttons. The window now asks 恢复 / 丢弃, and a row marked
  deleted shows a red notice in the detail pane.
- hosts parsing: the leading comment block is folded into one header row, so the
  Microsoft template's two sample lines are no longer counted as disabled entries;
  section headings no longer report a bogus `enable line N` change; a later
  enabled line for an already-mapped host name is flagged `!`.
- The hosts 原文 tab was always empty; it now shows the file as it would be saved.
- The column header band of both tables was reserved but never drawn.
- The hosts 启用 checkbox used the bold bitmap System font on a grey block.
- Flat buttons drew `Rectangle()` with the default white brush over their fill,
  so the active 分隔项 cell showed white text on white.
- The env mode/delimiter row overlapped the 名称/类型 fields by 2px.
- Selecting or editing a delimited item no longer scrolls the item list back to its
  first row; the in-place update keeps both the selection and the viewport.
- Saving no longer fails with a false `外部已修改，已取消保存`. The conflict check
  compared the system+user snapshot although only HKCU is writable; it now compares
  the user-scope fingerprint, and the modal dialog is shown with the write lock
  released so it cannot block the protocol thread.
- A save no longer erases the read-only system-variable rows from the table: the
  model is rebuilt with the current `showSystemVars` setting.
- Child controls (item list, edits, combo) are painted with the theme's colours
  instead of staying white in dark mode.
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
