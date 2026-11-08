# ra9fael.envhosts — Env & hosts editor for WinShun

> **English** · [简体中文（默认）](README.md)
>
> Win顺 ships an English UI, but the default documentation is Chinese.
> `README.md` is the Chinese original; this file is its English counterpart.
> The two are kept in sync and link to each other.

Browse, filter and edit **user environment variables** and **hosts file entries**
directly from the WinShun launcher. Every write is backed up first; admin-only
targets degrade honestly instead of pretending to succeed.

This branch maintains the **GDI edition**. Features and shortcuts follow each edition’s documentation.

## Purpose

- `env` — list user (and optionally system) environment variables, filter by
  name or value, edit values inline, copy raw/expanded values, delete with
  confirmation, restore previous snapshots from backups.
- `hosts` — list hosts entries, filter, toggle comments on/off, copy lines,
  delete lines (double-Enter confirm), add entries, restore backups.
- **Interactive editor window** — the first result for `env` and `hosts` is always
  "进入交互式编辑界面" (Enter opens the installed edition’s editor): full keyboard control,
  undo/redo, conflict detection, and auto-saved drafts under `WS_DATA_DIR/drafts/`.

## Install

Both editions are maintained and published. Choose the interface you prefer:

| Edition | Download | Branch |
|---|---|---|
| Native GDI interface | [GDI 0.7.5](https://github.com/ra9fael/winshun-envhosts/releases/tag/gdi%2Fv0.7.5) | `gdi` |
| QML interface | [QML 0.8.0](https://github.com/ra9fael/winshun-envhosts/releases/tag/qml%2Fv0.8.0) | `qml` |

1. Download its `.wsplugin`; optionally verify the accompanying `.sha256`.
2. Open **Settings → Plugins → Installed → Add plugin → Archive (.zip / .wsplugin)**.
3. Confirm installation and permissions, then enable it as needed.

**Mutually exclusive installation:** both packages use `ra9fael.envhosts`.
Installing the other edition replaces the current one. No uninstall is needed.
Save or close editor windows first. Switching to a lower version may show a
downgrade confirmation. Settings/backups remain; drafts are separate per backend.

**Updates:** install updates from the release page for your chosen edition.
The current plugin library has one backend URL per ID. Accepting its GDI update
may switch QML back to GDI; use QML releases to stay on QML. Listing this newly
created repository in the library requires a separate submission; a GitHub
release alone does not add it. WinShun 0.22.0+ is required, with host confirmation
for native-plugin installs and updates.

See [edition switching, migration and acceptance](docs/DUAL_EDITIONS.md).

## Build from source

Windows 10/11 x64, CMake 3.21+, MSVC 2022+ and Python 3.10+.
Check out `gdi` or `qml`; `edition.json` selects the backend automatically.
QML also needs Qt 6.8.3 on `CMAKE_PREFIX_PATH`.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

GDI uses `python tools/package.py`. Deploy Qt before packaging QML.
See [edition instructions](docs/DUAL_EDITIONS.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Keywords and examples

| Input | Effect |
|---|---|
| `env` / `hosts` (Enter on the **top row**) | Open the interactive editor window (always the first result) |
| `env` | List all user env vars (plus system vars when enabled) |
| `env path` | Filter vars whose name or value contains `path` |
| `env:set NAME=value` | Inline edit row for `NAME`; Ctrl+Enter saves |
| `env:del` | List recent env backups for restore |
| `hosts` | List hosts entries |
| `hosts docker` | Filter entries matching `docker` |
| `hosts:add 127.0.0.1 my.dev` | Add-row with confirm |
| `hosts:del` | List recent hosts backups for restore |

Actions are keyboard-first and ordered by key: **Enter** (edit / toggle /
confirm), **Ctrl+Enter** (save / copy), **Shift+Enter** (copy expanded value /
open hosts file in the default editor).

## Runtime dependencies

- Windows 10/11, x64 (a 32-bit build is rejected at compile time: the Wow64
  file-system redirector would otherwise edit `SysWOW64\drivers\etc\hosts`).
- GDI is statically linked; QML bundles Qt 6.8.3 and the VC runtime.
- Resident plugin (`mode: resident`), lazy start, `idleExitSec: 0` — it stays
  resident until WinShun exits so an open editor window is never killed by idle.

## Permissions and why

- `files:read` / `files:write` on `%SystemRoot%\System32\drivers\etc\hosts`
  (object form): the plugin reads hosts to list entries and writes it on
  toggle/add/delete/restore.
- `ui` — required to open the plugin's own editor window.
- `clipboard:write` — the editor window copies values/lines to the clipboard
  (Ctrl+C). The launcher list still copies through the host, not this permission.
- **Registry access has no name in the WinShun permission system.** User
  environment variables live in `HKCU\Environment`; reading and writing them is
  done directly via `RegSetValueExW` (never `setx`, which silently truncates at
  1024 characters). This is declared honestly here rather than hidden.
- No `processes` permission. The `elevated` permission **is** requested (this
  package requires Win顺 ≥ 0.22.0): when Win顺 itself runs as administrator and you have clicked
  "Confirm" once on the envhosts card in Settings → Plugins, the plugin starts
  elevated and both system variables and hosts become writable. WinShun never
  shows UAC for a plugin; the confirmation must be repeated after the plugin
  files change; an elevated plugin ignores AI / command-line calls. The ⓘ at the
  right end of the editor's status bar always explains the current permissions
  and the two steps to get administrator rights.

## What is editable vs read-only

| Target | Read | Write |
|---|---|---|
| User env vars (`HKCU\Environment`) | yes | yes |
| System env vars (`HKLM\...\Session Manager\Environment`) | optional | writable when elevated; otherwise delete/edit refuse with a clear message |
| hosts file | yes | only if the file is writable (probe first, never a blind write); always writable when elevated |

Env writes preserve the existing value type (`REG_SZ` / `REG_EXPAND_SZ` /
`REG_MULTI_SZ`) and broadcast `WM_SETTINGCHANGE` so new processes pick up the
change. Already-running processes (including WinShun itself) do **not** see the
change until restarted — that is a Windows limitation, not a plugin bug.

## Data location

All data stays local under `WS_DATA_DIR`:

- `backups/env/<timestamp>.json` — user-scope env snapshots taken before every
  env write and delete (kept: `backupCount`, default 10).
- `backups/hosts/<timestamp>.hosts` — verbatim hosts bytes taken before every
  hosts write (kept: `backupCount`).
- `drafts/<gdi|qml>/env.json` / `drafts/<gdi|qml>/hosts.json` — unsaved edits in the interactive editor
  window, written automatically every 30 s and on shutdown/close. Reopening the
  editor asks Restore / Discard: Restore shows them as unsaved edits (Ctrl+S is
  still required), Discard deletes the draft file. Only the editor writes drafts;
  no registry/hosts writes happen here.
- Nothing is uploaded; no telemetry; secrets/settings are never logged.

## Failure handling

- hosts read-only (normal user on `System32\drivers\etc\hosts`): the row shows
  a 🔒 marker; actions return an explicit `E_DENIED` error row with a
  Shift+Enter escape hatch that opens the file in an external editor. No
  silent degradation, no fake success.
- hosts saved as UTF-16 by Notepad: refused with `E_HOSTS_UTF16` instead of
  corrupting the encoding.
- hosts locked by another process: `E_HOSTS_BUSY`.
- hosts writes are **in-place** (open + truncate + write) to preserve the file
  ACL. This is not atomic; a crash mid-write can leave a partial file. The
  pre-write backup in `WS_DATA_DIR/backups/hosts/` is the recovery path.
- Registry write failures surface as "删除失败/保存失败" notifications with the
  operation logged to stderr; the pre-write backup allows restore via
  `env:del` / `hosts:del`.
- `hostsPath` setting supports `%VAR%` expansion; a missing file yields a clear
  empty state.

### Interactive editor window

- Opening the window calls `CreateWindowExW`; if that fails (e.g. a headless/CI
  session, or a denied foreground switch), the action returns `notify` + `keep`
  and you stay in the launcher — no crash.
- When it does open, Windows may background the window: the launcher only promises
  a foreground switch on menu/picture/showSettings/hotkey, not on `action`, so
  bring it forward with Alt+Tab if it is behind.
- The editor writes nothing until you press **Ctrl+S**: it re-reads the target,
  compares a content fingerprint, and aborts with a conflict note if the underlying
  env/hosts changed since the window opened. The pre-write backup is the recovery
  path. Unsaved edits are flushed to `drafts/<gdi|qml>/env.json` / `drafts/<gdi|qml>/hosts.json`
  every 30 s and on close.
- The window runs on its own GUI thread; launcher writes to the same target
  `PostMessage` a reload (so an open window reflects external edits). They are
  serialised by `native::g_writeLock`.

### Inside the window

- **Scrolling**: mouse wheel, the vertical scrollbar, arrow keys, PgUp/PgDn and
  Home/End. The status bar reports `rows a-b / N` and each tab shows its count.
  The window is resizable and column widths scale with it.
- **Per-row commands**: `Ins` adds a row (and clears an incompatible filter so
  the new row is visible), `Del` marks a row deleted (press again to restore;
  an unsaved new row is removed outright; a marked row shows a red notice in the
  detail pane and stays read-only until restored), `Enter`/`F2` enters the detail pane,
  `Esc` returns focus to the table, `Ctrl+Z`/`Ctrl+Y` undo/redo. Four buttons on
  the right of the detail pane (`+ Add`, `- Del`, `Undo`, `Redo`) call the same
  commands as the keyboard.
- **Multi-select and batch commands (0.5.0)**: `Ctrl+Click` toggles a row,
  `Shift+Click` / `Shift+Up/Down` extends a contiguous range, `Ctrl+A` selects
  every row in the current view, `Ctrl+C` copies the selection to the clipboard
  (`NAME=value` for env, original lines for hosts), and `Esc` drops the
  selection before it closes the window. The selection applies `Del` (delete /
  restore) and, on hosts, `Space` / `Enter` (enable / disable) to every row at
  once. Read-only rows, hosts group headings, the Raw tab and the backup tab are
  skipped and the status bar reports how many were skipped. A whole batch is one
  undo step, so one `Ctrl+Z` reverts all of it. The status bar shows `N selected`.
- **UI language (0.5.0)**: a new `uiLanguage` setting (`auto` / `zh-CN` /
  `en-US`) follows WinShun or pins one language. A host `appearance`
  notification switches an **already open** window immediately: title, tabs,
  headers, shortcut chips, status bar, dialogs and native child-control labels
  all redraw without rebuilding the model or losing unsaved edits. A pinned
  language wins over the host locale; `auto` follows it again. WinShun only
  resolves `zh` / `en` and its manifest text is `{zh, en}`, so these are the two
  supported languages.
- **Over-long values and delete semantics (0.6.0)**: env values are measured in
  Windows' 32767 UTF-16 code units and the detail boxes no longer truncate
  silently at the default Win32 EDIT limit. Hard errors (empty / invalid name,
  control characters) still block saving; a value beyond the process-environment
  limit is now **saved with an explicit warning** (your call): a permanent
  `X / 32,767 chars` counter in the detail pane (amber above 90%, red past the
  cap), a red row, a persistent banner naming the variable and the excess, the
  save confirmation calling out the risk, and the success status keeping the
  warning. The protocol-level `env:set` still hard-rejects over-length values
  because that path has no UI to show a warning. In item mode `Del` removes
  **only the current split item**, never the whole variable. Use the `Del row`
  chip / button to delete a variable: it confirms by name, and the save dialog
  lists every variable that will be removed. A row marked for deletion
  immediately shows an empty, disabled item list, so a deleted item can no
  longer appear to still be there.
- **Variable status**: each row combines its opened snapshot with its own item
  candidates. Candidates show amber Pending; model changes show Unsaved, retaining
  New/Deleted labels where appropriate. Combined states remain visible together,
  including on selected rows. Items retain their added/edited/staged markers.
- **Live external-change watcher (0.6.0)**: a 2 s poll (10 s when unfocused,
  paused while a modal, an edit session or the diff panel is active)
  fingerprints the registry / hosts file with the same logic as the save-time
  conflict check. A clean window reloads silently; a dirty window shows an amber
  band (`Target changed externally: N difference(s) · view diff`) and nothing is
  merged automatically.
- **Read-only three-snapshot diff panel (0.6.0)**: a save that hits an external
  conflict no longer shows Abort / Retry / Ignore. It opens a read-only panel
  classifying every entry as mine / theirs / conflict / same with the
  base / mine / theirs values, offering exactly three whole-view actions:
  reload (discard my edits), force overwrite (save mine), defer.
- **Backup page (0.6.0)**: the Backup tab is now two panes — a timestamped list
  with counts on the left, a searchable per-variable preview with diff markers
  (`= same / ~ differs / + backup only / - current only`) on the right.
  Actions: refresh preview / restore (auto-backup + confirm, loads as unsaved
  changes) / delete (confirm) / export JSON.
- **Two vertically split panes (env window only)**: a draggable splitter sits
  between the table and the detail editor; each pane keeps its minimum height
  (list 112px, detail 200px) and `WM_GETMINMAXINFO` refuses a window too small
  to hold both. The hosts window and the backup view keep the fixed detail strip.
- **Delimited-item editing**: `Ctrl+R` switches between Raw and Items.
  Double-click or Enter edits inline; Esc restores that edit. Del, dd or the
  trailing × stages deletion; deleting again restores the item. Visible
  **Up Alt+↑ / Down Alt+↓** buttons in the footer's **Items** group reorder items,
  as do Alt+K / Alt+J. Boundary and read-only actions are disabled.
  j / Ctrl+J moves the caret down, k / Ctrl+K up, gg to the first item, G to the last.
  Adds, edits, reorders and deletions remain candidates for that variable.
  Switching variables, filters or tabs preserves them **without committing**.
  Ctrl+Z / Ctrl+Y undo and redo candidate operations. **Commit** applies this
  variable's candidates to the working model as one undo step; Ctrl+S collects
  all variables' candidates and asks for confirmation before writing to the
  registry. Closing the editor preserves candidates in its recovery draft.
  The variable row shows **Pending**, then **Unsaved** after Commit, or
  **Pending · Unsaved** when both apply. Undoing to the original value or saving
  successfully clears the corresponding indicator.
  Path defaults to semicolons; the toolbar shows item and character counts.
- **Delimiter**: a self-drawn dropdown button next to the mode button (e.g.
  `Semicolon (;) ▾`). Its menu offers `semicolon / comma / colon / Tab / pipe /
  space`, applied as soon as you pick one. `Custom…` swaps the button for a text
  box that takes any character or string (`=>`), applied with Enter and dropped
  with `Esc`. Re-splitting first rejoins the items with the old delimiter, so no
  value is lost. Separators inside `"quoted"` runs are not split points. Commit
  or undo pending deletions before changing delimiters. The
  **space** preset always asks first: on a PATH-like variable it warns that
  `C:\Program Files\...` would be cut in half (default No); elsewhere it
  explains that whitespace runs get normalised, and items containing spaces get
  an amber marker.
- **hosts entries tab**: the leading comment block (Microsoft's template text and
  its `rhino.acme.com` samples) folds into one "file header" row instead of
  showing up as disabled entries; the Raw tab shows it in full. A line that maps a
  host name an earlier enabled line already maps gets a red `!` next to its line
  number and the status bar counts them. Read-only no longer draws a red banner;
  the status bar says `read-only (needs admin)`.
- **hosts Raw tab**: the whole file in a monospace text box that fills the window.
  When hosts is writable you can edit it directly; the text is applied to the
  entries when you leave the tab, press `Ctrl+S` or close, and one `Ctrl+Z` on
  the entries tab undoes the whole raw edit. Inside the box `Ctrl+Z` / `Ctrl+Y`
  are text undo / redo. Line endings, BOM and the final newline are kept, so
  unchanged lines save byte-for-byte. Read-only hosts keep the box read-only.
- **Reload**: `F5` re-reads from disk and lands on the same row; unsaved edits are
  confirmed before they are dropped. When an external change arrives, a clean window
  reloads itself while a dirty one only shows a warning and keeps your edits.
- **Status bar**: two rows. Row 1 shows edit state, visible range, filter and
  writability; transient messages expire after 4 s. Row 2 is a strip of
  clickable shortcut chips, greyed out when unavailable.
- Inside the detail pane `Tab`/`Shift+Tab` move backwards and forwards between the
  fields, `Ctrl+Tab`/`Ctrl+Shift+Tab` switch tabs.
- `Ctrl+S` lists the individual changes (add / modify / delete) before writing;
  when there are deletions it also spells out `N variable(s) / hosts line(s)
  will be DELETED` with their names, and an over-length value is called out as
  "the registry accepts it, but new processes may not read the full value".
  Only a change to the **user** scope (HKCU) after the window opened counts as
  a conflict, and instead of a blind overwrite a read-only diff panel opens
  with three choices: reload (discard my edits), force overwrite (save mine),
  defer. After a save the model is reloaded with the current tab settings, so
  system-variable rows survive it.

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `hostsPath` | text | `%SystemRoot%\System32\drivers\etc\hosts` | hosts file location |
| `backupCount` | number | 10 | backups kept per target |
| `maxResults` | number | 50 | max rows per query (capped at 200) |
| `showSystemVars` | bool | true | include read-only system vars in `env` |
| `uiLanguage` | choice | `auto` | UI language: follow WinShun / pin Chinese / pin English; open windows switch at once |

Footer shortcut buttons use measured label widths and wrap in narrow windows.
The Backup tab displays the total count independently of the page's scope filter;
it loads on editor startup and refreshes after saving or deleting a backup.

## Tests

- `tests/gui_preview.py --interaction-check --no-hold`: visible reorder buttons,
  Vim bindings, Chinese/English narrow-window footer layout, backup totals/scopes,
  candidate navigation, explicit commit/save, and draft recovery.

- `tests/gui_preview.py --split-state-check --no-hold`: pending-delete
  undo/redo, row/filter/delimiter changes, adding items and mixed deletion;
  registry values must remain unchanged before Save.
- `tests/gui_preview.py --dialog-check --no-hold`: reproduces draft recovery
  and checks that the body and both buttons fit; add `--english --dark` for
  the English dark-theme variant.
- CTest `dialog_layout` checks consistent height before/after child creation
  and complete buttons at 100%, 150%, and 200% font sizes. This does not replace
  physical multi-monitor DPI transition testing.

- `tests/test_envhosts_logic.cpp` — 69 OS-free unit checks (hosts byte-exact
  round-trip on a sanitised captured file, section comments, env model, grammar,
  validation, stable ids, UTF-16 length and the 32767 limit boundary).
- `tests/test_editor_model.cpp` — 155 OS-free unit checks for the editor data model
  (dirty tracking, undo/redo cap at 200, validation, PATH split/join with quoted
  segments, custom-delimiter round-trip and switching, whitespace-delimiter
  normalisation, the four row states, the five diff3 verdicts, system-scope edits
  leaving the user-scope fingerprint untouched, multi-hostname hosts rows,
  byte-exact save preserving BOM/CRLF, fingerprint sensitivity, draft
  serialisation round-trip, backup loads as unsaved, one-undo batch delete /
  restore / enable transactions, and a shortened over-long `Path` producing an
  empty `toDelete`).
- `tests/integration.py` — drives the real exe over JSON-RPC on real Windows:
  protocol round-trips, edit/delete/restore on a scratch registry key, 20-query
  side-effect audit (hosts bytes + registry must be untouched), real System32
  hosts write-denial, `settingsChanged` propagation, system-scope delete
  refusal, **and the editor suite** (P1/P2 pinned first row, P3 openEditor wiring,
  P4 manifest fields, P5 draft location, P6 external-change path, T2.9 host
  locale / plugin override / live `appearance` switching). 37 assertions.
- `tests/gui_preview.py --no-hold` — 145 assertions against the real window on the
  desktop, driven over JSON-RPC with a fake 28-segment `Path` in an isolated
  registry key: split-pane layout, page scrolling, `Ins`/`Del`, the full-width
  item list, in-place editing by double-click, per-item dirty markers, a custom
  delimiter round-trip, the rejoined raw value, pane drag, saving through the
  shortcut chip and reading the registry back, system rows surviving the save,
  the read-only diff panel with its three whole-view actions on an external
  conflict, the `F5` reload prompt, the space preset (PATH confirmation, then a
  real two-address split), and the two-pane backup page. It also
  covers `Ctrl/Shift+Click`, `Ctrl+A` / `Ctrl+C` / `Esc`, a two-row batch delete
  with one-step undo, hosts batch enable/disable, the over-long `Path` sequence
  (error banner, remove one item, clear the item list on row deletion, variable
  still present after save), and live zh/en switching with a pinned override.
  Both the user and the system scope point at isolated test keys, every sample
  value is fake and both keys are deleted afterwards.

## License

MIT, see [LICENSE](LICENSE). Release history: [CHANGELOG.md](CHANGELOG.md).
---

[← 简体中文（默认）](README.md) · English
