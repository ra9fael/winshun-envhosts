# Changelog

[English](CHANGELOG.en.md)

All notable changes to this plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The version in `plugin.json` is the
single source of truth; release tags are `v<version>`.

## [Unreleased]

## [0.7.5] - 2026-11-09

### Fixed

- 修复同时关闭环境变量和 hosts 编辑器时的线程等待死锁，并加入双窗口退出回归。
- 修复可执行文件图标，补齐打包中的 ICO 和资源脚本。 为窗口显式设置大小图标，随 DPI 更新并释放图标句柄。

### Notes

- 本次仅生成本地标签与安装包。实际验证范围见 docs/icon-validation.md；未进行 GitHub 发布。更新后需重新启动插件进程。


## [0.7.4] - 2026-11-08

本地标签测试版，未公开发布。Local tagged test build; not publicly published.

### Fixed / 修复

- 分隔项“上移 / 下移”按钮统一放入底部“分隔项”操作组，显示 Alt+↑/↓，首尾和
  只读时禁用相应操作；点击保留选中项及列表焦点。底部按钮按实际文字测量、按组
  换行，移除原有独占按钮行。Group Up/Down actions in the footer with Alt+↑/↓
  labels, correct boundary/read-only states and preserved list focus. Measure
  labels, wrap groups and reclaim the old movement toolbar row.
- 分隔列表和变量表统一 j/Ctrl+J 向下、k/Ctrl+K 向上；分隔列表支持 dd、gg、G，
  Alt+J/K 按下/上重排。Use consistent Vim directions and support dd/gg/G in the
  item list; Alt+J/K reorder down/up without interfering with text entry.
- 分隔项候选按变量保留，切换变量、筛选或标签不再提交；撤销/重做和草稿恢复保留
  待提交状态。只有提交或明确保存才应用到工作模型。Keep item candidates per
  variable across navigation, undo/redo and draft recovery; apply them only on
  Commit or explicit Save, with registry writes still behind confirmation.
- 上方变量行显示“待提交”“未保存”及组合状态，包含新增、编辑、删除、重排；
  状态按变量和范围隔离，随撤销/重做、提交、保存及草稿恢复同步。状态列按中英
  文案测宽。Show Pending, Unsaved and combined status on the variable row;
  track candidate operations independently by variable/scope across undo/redo,
  Commit, Save and draft recovery. Measure the status column for both locales.
- 备份标签显示独立总数，在打开编辑器和保存/删除备份后刷新；修正用户/系统范围
  筛选。Initialize and refresh the total backup count independently of scope
  filtering, and correct the user/system backup predicates.

## [0.7.3] - 2026-11-08

### Fixed / 修复

- **恢复草稿弹窗按钮被截断。** 无明细区的弹窗现在也按实际正文高度计算窗口与
  按钮位置；深色按钮圆角补齐背景，正文遵循主题色，英文按钮宽度按文字测量。
  Restore-draft and other confirmations now measure the body before creating
  controls and use that height for the complete window, including dialogs
  without a detail block. Dark button corners/body backgrounds follow the
  theme and button widths accommodate their labels.
- **分隔项删除后的撤销及切换状态。** 撤销/重做、重开当前变量、切换变量、筛选和
  分隔符不再重复套用旧删除下标；混选新增项和已有项时保留已有项的待删除标记。
  Split-list rebuilds no longer apply old deletion indices to a new value;
  undo/redo, refocus, row/filter/delimiter changes preserve the correct items.
  Mixed batches immediately remove new items while existing items stay staged.
- **新增和多选操作。** 新增项直接进入行内编辑；移动时同步新增状态，多选及单行
  × 操作不会把旧编辑框的内容写入其它项；行内编辑正确转发窗口快捷键。
  Adding an item opens its inline editor. Reordering preserves new-item
  metadata, modifier clicks and row actions keep the correct edit value, and
  inline editing forwards window shortcuts and Alt reorder keys.
- GUI/RPC regression runs use unique scratch registry keys; GUI cleanup also
  runs after exceptions. Added focused split-state, restore-dialog, and
  100/150/200% font-size layout regressions.
  GUI/协议测试使用每次运行独立的临时注册表键，GUI 异常退出也清理进程和数据；
  新增分隔项状态、恢复弹窗及 100/150/200% 字体尺寸的布局回归。

### Known limitations / 已知验证边界

- 150%/200% coverage measures font layout; physical multi-monitor DPI
  transitions and dialogs taller than the screen have not been verified.
  150%/200% 仅覆盖字体布局测量，未验证实机跨显示器 DPI 切换和高于屏幕的超长弹窗。
- Elevated writes to the real system environment and hosts file were not
  exercised; write/restore checks use isolated test data.
  未实测提权后写入真实系统环境变量和 hosts 文件，写入/恢复使用隔离测试数据。

## [0.7.2] - 2026-11-08

### Fixed

- **Split-item counter occlusion.** The parent-painted
  `共 N 项 · 合计 X / 32,767 字符` counter now fills its strip with an opaque
  panel background before drawing, so a child-control repaint (delimiter combo
  or commit-button hover) can no longer cut through the glyphs. The complete
  line is drawn without ellipsis; only the short character-count fallback is
  ellipsized when a narrow window cannot fit it.

## [0.7.1] - 2026-11-08

### Added

- **Split-item list multi-select.** `Ctrl+Click` and `Shift+Click` select
  multiple items in the split-item list; Del stages ALL selected items in one
  go and 提交修改 / Ctrl+S applies the batch (one undo transaction).
- **Vim-style navigation** on the main table and the split-item list, with no
  conflicts with the `Ctrl`-modifier keymap. Table (no modifiers): `j`/`k` move
  the caret, `gg` first row, `G` last row, a digit run is a count prefix
  (`3j`, `5G`), `x` or `dd` marks the row deleted, `u` undoes, `/` focuses the
  filter, `l`/Enter opens the row, `h` returns focus to the table. Item list:
  bare `j`/`k` move the caret (`Alt+J/K` still reorder, `Ctrl+J/K` remain the
  explicit caret bindings). The caret row is exposed to the GUI driver in
  `WM_APP_GETSTATE` bits 56-63 for regression assertions.

### Fixed

- **Readable always-on notice bands.** Hard-error, over-length and near-cap
  bands now carry their own foreground colour (`kBandErrFg`, `kBandOverFg`,
  `kBandWarnFg`) instead of reusing the amber warning text on dark saturated
  backgrounds. The three band actions and relative timestamp use the same
  readable foreground.
- **Diff footer clipping.** The read-only external-change panel now reserves a
  dedicated footer strip below its three actions; the explanatory line is no
  longer reduced to a 2px rect and cut by the panel border.
- **Split-item counter clipping.** The `共 N 项 · 合计 X / 32,767 字符` counter
  has a guaranteed toolbar strip after `+ 新增项`, is left-aligned like the
  design draft, and falls back to the character-count portion with ellipsis
  when a narrow window cannot fit the complete line.
- The backup-page scope switch (全部 / 用户 / 系统) ignored clicks on the
  upper half of each segment (hit test used the wrong origin), making the
  segmented control look dead.

## [0.7.0] - 2026-11-08

### Added

- **Single-source theming.** All 82 colour roles and shared metrics now live in
  `editor_theme.h` (one X-macro list holding light + dark values). The painter
  reads `th().kRole` at runtime; `tools/gen_design_svg.py` parses the SAME file
  to emit the SVG design drafts in `docs/design/` (`--check` fails when a draft
  is stale). Draft and product physically cannot drift; the 113 inline
  `RGB(...)` literals in the painter are gone.
- **Backup page rework** (user-reported clicks landing on the wrong rows).
  Painting and hit-testing now share one geometry source per region
  (`backupListHit` / `backupPreviewHit`), so every click lands where the pixel
  it sits on is drawn. Entries are two-line (id + time · count) with 用户 /
  系统 scope chips, the preview table gained a 范围 column with per-row chips,
  a segmented 备份范围：全部 / 用户 / 系统 switch in the tab strip filters both
  the backup list and the preview, and the redundant 返回编辑 pill is gone
  (the top tabs already switch).
- **Two-stage split-item delete** (user request). The `×` on an item only
  STAGES it: the row turns red with a strikethrough and `− 待删除`, the joined
  value excludes it, and clicking `×` again restores it. A green 提交修改
  button in the item toolbar (with a staged-count badge) applies staged
  deletions; Ctrl+S applies them too. Added items still vanish at once (they
  never existed in the value); inline editing of staged rows is blocked and
  reordering waits until staged rows are committed.
- **Themed modal dialogs** (`editor_dialog.h`, user request). All ten
  `MessageBoxW` confirmations (draft restore, reload, space delimiter,
  permissions, backup restore/delete, save confirmation with the change list
  in a monospace detail block, whole-variable delete, close-with-unsaved
  ×2) now render the design draft: icon bubble, title + dim subtitle, wrapped
  body, optional monospace detail block and right-aligned plain / primary /
  danger buttons. Enter presses the default button, Esc the cancel, Tab
  cycles. The window uses a process-local `#32770` class so UI automation and
  the GUI regression driver keep working unchanged.

### Fixed

- Backup page click-through: the left list drew from `top+24`, the preview
  from `top+70`, but the hit tests assumed `top+4` / `top+24` - three
  disagreeing origins made every click land on the wrong row or nothing at
  all. Also fixes the preview search box overlapping the column header.
- The tab strip on the backup page swallowed clicks (rows in the work area
  returned before tab handling), which made it impossible to leave the page
  by clicking the top tabs.

## [0.6.0] - 2026-11-08

### Added

- **Full-width split-item list.** The PATH item list now owns the whole detail
  pane; the old right-hand action pane (four wide buttons, a separate edit box
  and the draggable vertical splitter) is gone. Editing is in place: double-click
  (or Enter) floats an edit box over the row, `Esc` restores the captured text,
  and the selected row shows trailing `↑ ↓ ×` buttons (`Alt+↑/↓` still reorder).
  A live counter reports `共 N 项 · 合计 X / 32,767 字符`.
- **Unsaved-change marking.** Every row is compared against the opened snapshot:
  new rows get a green bar with `+`, modified rows an amber bar with `●`, deleted
  rows keep the red bar with `−`. The same marking applies inside the split-item
  list (per-item dot) so an edit made through either path is visible at a glance.
- **Live external-change watcher.** A 2 s timer (10 s when unfocused, paused
  while a modal, an edit session or the diff panel is active) fingerprints the
  registry / hosts file with the same logic as the save-time conflict check.
  A clean window reloads silently; a dirty window shows an amber band
  (`目标已被外部修改：N 项差异 · 查看差异`) and nothing is merged automatically.
- **Read-only three-snapshot diff panel.** Opens from the band or automatically
  when a save hits a conflict (the old Abort/Retry/Ignore dialog is gone).
  Entries are classified as 我改 / 外部改 / 冲突 / 一致 with `原 / 我 / 外`
  values, and the panel offers exactly three whole-view actions:
  重新载入（丢弃我的改动）, 强制覆盖（保存我的）, 暂不处理.
- **Over-length values save with an explicit warning** (user decision). Hard
  errors (empty/invalid name, control characters) still block the save; a value
  above the 32,767 UTF-16-unit process-environment limit now only warns: the
  length counter turns red, the row turns red, a persistent band names the
  variable and the excess, the save confirmation lists the risk, and the
  success status keeps the warning. The protocol-level `env:set` still
  hard-rejects over-length values because it has no UI to show a warning.
- **Two-pane backup page.** The 备份 tab now lists backups with counts on the
  left and a searchable preview on the right with diff markers
  (`= 相同 ~ 值不同 + 仅备份有 - 当前多出`). Actions: 刷新预览 / 恢复
  (auto-backup + confirm, loads as unsaved changes) / 删除 (confirm) /
  导出 JSON. `BackupStore::readEnv` / `remove` / `exportTo` back the page.
- **Space delimiter preset** (`空格 (Space)`), command ids `200..205` with
  自定义… at 206. Applying it always asks first: on a PATH-like variable the
  dialog warns that `C:\Program Files\...` would be cut in half (default No);
  elsewhere it explains that whitespace runs are normalised. Items containing
  spaces get an amber marker in the list.

### Fixed

- `LB_GETITEMRECT` was compared against `LB_OKAY`, but modern list controls
  return 1 for a visible item, so the in-place editor never opened and the
  per-item dirty markers never painted. Only `LB_ERR` is a failure.
- The hosts save-confirmation wording now says 行 instead of 变量.
- The diff panel no longer has the detail EDIT controls cut into it: opening
  the panel hides the detail children and closing restores them.
- A modal MessageBox can swallow a synthetic click sent while it is still being
  created; the GUI test now retries until the dialog actually closes.

## [0.5.0] - 2026-11-07

### Added

- **Multi-select in both editor windows.** Click, `Ctrl+Click` and `Shift+Click`
  select rows, `Shift+Up/Down` extends a range, `Ctrl+A` selects every row in
  the current view and `Esc` drops the selection before it closes the window.
  The selection is stored as model-row indices, so filtering, adding and deleting
  rows cannot make a stale view index point at the wrong variable.
- **Minimal batch commands.** `Del` marks / restores all selected rows,
  `Space` / `Enter` on a hosts selection enables or disables them, `Ctrl+C`
  copies the selected rows as `NAME=value` (env) or hosts lines to the Windows
  clipboard. Read-only rows, hosts group headings, the hosts Raw tab and the
  backup tab are skipped and the status bar reports how many were skipped.
  Each batch is one undo step, so a single `Ctrl+Z` reverts all of it.
- **Solid zh-CN / en-US UI language support.** A new `uiLanguage` setting
  (`auto` / `zh-CN` / `en-US`) follows Win顺 or pins one language. The host's
  `appearance` notification switches an already open window immediately:
  title, tabs, headers, chips, status bar, dialogs and native child-control
  labels all redraw in the new language without rebuilding the model. The
  setting wins over the host locale; `auto` follows it again. Win顺 only ships
  `{zh, en}` manifest text and resolves its own locale to `zh` / `en`, so those
  are the two languages the plugin supports.
- **Visible error banner.** Invalid names, values over the 32767 UTF-16-unit
  limit and invalid hosts rows now produce a red, readable message in the
  column-header band instead of a save that silently does nothing.

### Fixed

- **PATH over-length data loss (reported bug).** Deleting one split item from an
  overlong `Path` used to leave the item visible in the item list, mark the
  whole variable deleted, and remove `Path` from the registry on save. Causes
  fixed together:
  - `m_valueEdit`, `m_partEdit` and `m_rawEdit` now call `EM_SETLIMITTEXT, 0, 0`;
    the default single-line EDIT limit silently truncated long values on
    `SetWindowTextW`.
  - `envh::isValidEnvValue()` measures UTF-16 code units (`envh::utf16Units()`),
    not UTF-8 bytes, so CJK and long ASCII values are judged by the real
    Windows limit.
  - A deleted env row now shows an **empty, disabled** split-item list instead
    of rebuilding it from the stale value; the detail text says the whole row
    is marked for deletion.
  - `Ins` / `Del` in a text field are no longer forwarded to the table: only
    the split-item LISTBOX uses them as item commands. `Alt+Up/Down` still
    reorders items.
  - `deletePart()` re-checks `m_partSel` after committing a pending item edit,
    so a stale index cannot erase out of bounds.
  - Deleting an existing env variable now asks for confirmation by name (and by
    split-item count in item mode); the delete chip is labelled "Del 删除整行" /
    "Del row" in item mode. The save dialog lists every variable that will be
    deleted before anything is written.
  - The error banner is painted last, over the column-header band. Painting it
    before the table left only a thin red line with unreadable text.
- `appearance` and `settingsChanged` are no longer no-ops: both push a
  `WM_APP_APPEARANCE` message to open editor windows.

### Changed

- `plugin.json` version is `0.5.0`; `minWinShun` stays `0.22.0`.
- Tests: `editor_model` 124/124, `logic` 69/69, `integration.py` 37/37,
  `gui_preview.py` 90 assertions. The GUI driver now covers multi-select,
  batch delete/undo, hosts batch enable/disable, clipboard copy, the PATH
  over-length sequence, and live language switching with a pinned override.
- `editor_win.h` exposes two read-only test hooks (`WM_APP_KEY` for synthetic
  keys with explicit modifiers, `WM_APP_GETSTATE` for a packed state snapshot).
  Neither writes any data and neither is reachable through the JSON-RPC surface.

### Not changed

- The Qt Quick prototype (`qt/`, `ENVHOSTS_QML_EDITOR=ON`) is unchanged. The
  shipped build remains the GDI editor (`ENVHOSTS_QML_EDITOR=OFF`).
- No new permissions: the batch copy uses the existing `clipboard:write`.

## [0.4.0] - 2026-11-07

### Added
- Qt Quick editor prototype (opt-in, `-DENVHOSTS_QML_EDITOR=ON`, qml branch).
  A read-only env/hosts window built with Qt Quick replaces the GDI window in
  that build while the protocol, models and storage stay unchanged. The
  prototype builds into `build-qml/bin` so it never overwrites the shipped
  `envhosts.exe`; editing, drafts, backups and the hosts 原文 tab are not
  migrated yet. `QML_MIGRATION_PHASE0.md` records the contract that migration
  must preserve.
- QML deployment: the plugin ships its own Qt 6 runtime.
  `tools/deploy_qml.ps1` assembles a pruned ~45 MB runtime (Basic style only,
  Qt LGPLv3 text included) and `tools/package.py --runtime-dir` bundles it into
  the `.wsplugin`; the packaged exe comes from the runtime folder, so a QML
  package never mixes the GDI binary with the Qt runtime. Switching the default
  to QML requires `runtime.memoryMB: 512` (measured 253 MB with one window,
  268 MB with both).
- The plugin declares the `elevated` permission (available from Win顺 0.19.0;
  the manifest floor is 0.22.0 because the plugin is distributed as an
  unreviewed-native package). When Win顺 runs as administrator and the user confirmed
  the plugin card once, the plugin starts elevated: the 系统 tab edits system
  variables (and the launcher's system-var delete works), save/backup/conflict
  detection cover both scopes, and hosts writes succeed. There is still no UAC
  prompt from the plugin — write access follows a real registry probe, never the
  token alone. The 权限 button at the right end of the status bar (amber when
  restricted, blue when writable) opens a permission
  explanation (what is writable now and the two steps to run elevated).
- The hosts 原文 tab is editable when hosts is writable. The text is parsed back
  into the entries when you leave the tab, press Ctrl+S or close; one undo step
  reverts the whole raw edit. BOM, line endings and the final newline are kept,
  and untouched lines save byte-for-byte. The tab fills the window, uses Consolas,
  shows only the text box's scrollbar, and its count is the file's line count.
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
- In env list mode, the item list and its action pane are separated by a
  draggable vertical splitter (minimum 180px / 200px). Drag the grip, or use
  `Ctrl+Shift+Left/Right` from the table to move the split by 5%. The ratio is
  preserved across window resizes; the cursor changes to a horizontal resize
  arrow over the grip.

### Changed
- `winshun.h` is now the host's 0.22.0 SDK (`WinShun/src/plugins-sdk/winshun.h`,
  the same file already used by bosskey / volume / findmouse). It adds `details`
  and `appearance` dispatch, the `winshun/refresh` notification, `toast`, query
  `filters`, action `files` triggers, five new item fields, the `reveal` effect,
  lifecycle hooks, UTF-8-safe JSON quoting and mutex-protected sends. Every
  existing override and helper still compiles unchanged. `minWinShun` is
  `0.22.0`: WinShun-plugins requires that floor for an unreviewed-native
  package, so installation shows the native-plugin risk confirmation. The
  0.22.0 `runtime.qt: "host"` contract cannot be used here because it is
  mutually exclusive with `elevated`; the QML prototype ships its own Qt.
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
- `tests/gui_preview.py` no longer shells out to `ffmpeg` (which is not on this
  machine's PATH). It encodes the captured 32-bit BGRA DIB as PNG with the
  standard library (`zlib` + `struct`), forcing the undefined GDI alpha byte to
  opaque. All preview fixtures were regenerated with the new encoder.
  The suite now also drags the new vertical splitter and asserts that the item
  list grows while the action pane stays clear of it.

### Fixed
- `LoadCursorW` calls no longer depend on the `UNICODE` build macro. The `IDC_*`
  resource ids are cast explicitly, so a plain `g++` build compiles exactly like
  the CMake build (which defines `UNICODE`).
- Moving or picking a 分隔项 now updates the "当前项 N/M" counter and the
  Up/Down enable states immediately (they were painted in the window buffer and
  only refreshed on the next unrelated repaint).
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

[Unreleased]: https://github.com/ra9fael/winshun-envhosts/compare/v0.7.4...HEAD
[0.7.4]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.7.4
[0.7.3]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.7.3
[0.7.2]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.7.2
[0.7.1]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.7.1
[0.7.0]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.7.0
[0.4.0]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.4.0
[0.1.0]: https://github.com/ra9fael/winshun-envhosts/releases/tag/v0.1.0
