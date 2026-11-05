# ra9fael.envhosts — Env & hosts editor for WinShun

> **English** · [简体中文（默认）](README.md)
>
> Win顺 ships an English UI, but the default documentation is Chinese.
> `README.md` is the Chinese original; this file is its English counterpart.
> The two are kept in sync and link to each other.

Browse, filter and edit **user environment variables** and **hosts file entries**
directly from the WinShun launcher. Every write is backed up first; admin-only
targets degrade honestly instead of pretending to succeed.

## Purpose

- `env` — list user (and optionally system) environment variables, filter by
  name or value, edit values inline, copy raw/expanded values, delete with
  confirmation, restore previous snapshots from backups.
- `hosts` — list hosts entries, filter, toggle comments on/off, copy lines,
  delete lines (double-Enter confirm), add entries, restore backups.
- **Interactive editor window** — the first result for `env` and `hosts` is always
  "进入交互式编辑界面" (Enter opens a standalone GDI window): full keyboard control,
  undo/redo, conflict detection, and auto-saved drafts under `WS_DATA_DIR/drafts/`.

## Install

1. Download `ra9fael.envhosts-<version>.wsplugin` from the
   [latest release](https://github.com/ra9fael/winshun-envhosts/releases/latest).
   Optionally compare its SHA-256 with the `.sha256` file next to it
   (`Get-FileHash <file> -Algorithm SHA256`).
2. In Win顺 open **Settings → Plugins → Installed → Add plugin →
   Archive (.zip / .wsplugin)** and pick the file. In the Chinese UI the same
   path reads **设置 → 插件 → 已安装 → 添加插件 → 压缩包（.zip / .wsplugin）**.
3. A plugin installed from a local package is **not enabled automatically**.
   Turn it on in the list; enabling it is how you confirm its permissions
   (see [Permissions](#permissions-and-why)).

**Update:** install the newer `.wsplugin` the same way. Win顺 sees the same id,
asks before replacing the old version, and keeps the plugin's data folder.

**Uninstall:** remove it from **Settings → Plugins → Installed**
(**设置 → 插件 → 已安装** in the Chinese UI).

This plugin is a native exe, so it is not listed in Win顺's in-app plugin
library (the community index accepts script plugins only). The releases of this
repository are the only distribution channel.

## Build from source

Requirements: Windows 10/11 x64, CMake 3.21+, and either Visual Studio 2022 or
later (MSVC) or MinGW-w64 GCC 13+. Python 3.10+ for packaging and the
integration tests.

```powershell
cmake -S . -B build -A x64                 # MSVC; for MinGW use -G "MinGW Makefiles"
cmake --build build --config Release       # writes envhosts.exe next to plugin.json
ctest --test-dir build -C Release --output-on-failure
python tools/package.py                    # dist/ra9fael.envhosts-<version>.wsplugin
```

To run a local build inside Win顺 without packaging, and for tests and the
release process, see [CONTRIBUTING.md](CONTRIBUTING.md).

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
- Ships as a statically linked `envhosts.exe`; no runtime libraries needed.
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
- No `processes` or `elevated` permission. Admin-only targets remain read-only
  with a clear error in v0.2; `elevated` is deferred.

## What is editable vs read-only

| Target | Read | Write |
|---|---|---|
| User env vars (`HKCU\Environment`) | yes | yes |
| System env vars (`HKLM\...\Session Manager\Environment`) | optional | **no** — delete/edit refuse with a clear message |
| hosts file | yes | only if the file is writable (probe first, never a blind write) |

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
- `drafts/editor.json` — unsaved edits in the interactive editor window, written
  automatically every 30 s and on shutdown/close. Reopening the editor offers to
  restore from the draft. Only the editor writes drafts; no registry/hosts writes
  happen here.
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
  path. Unsaved edits are flushed to `drafts/env.json` / `drafts/hosts.json`
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
  an unsaved new row is removed outright), `Enter`/`F2` enters the detail pane,
  `Esc` returns focus to the table, `Ctrl+Z`/`Ctrl+Y` undo/redo. Four buttons on
  the right of the detail pane (`+ Add`, `- Del`, `Undo`, `Redo`) call the same
  commands as the keyboard.
- **Two vertically split panes (env window only)**: a draggable splitter sits
  between the table and the detail editor; each pane keeps its minimum height
  (list 112px, detail 200px) and `WM_GETMINMAXINFO` refuses a window too small
  to hold both. The hosts window and the backup view keep the fixed detail strip.
- **Delimited-item editing**: the `Edit items` button in the detail pane (or
  `Ctrl+R`) splits a long value on a delimiter so you can work on one segment at
  a time — `Ins` inserts after the current item, `Del` removes it, `Alt+Up`/`Alt+Down`
  reorder, and the box beside the list edits it. Every change is rejoined into the
  variable's value immediately and stays a single undo step. Picking an item keeps
  the list where it is scrolled: updating an item in place never drags the viewport
  back to the first row. `Path` is pinned to `;`. `Ctrl+R` again returns to raw text,
  showing the rejoined string. Splitting is always explicit, so the raw value never
  becomes unreachable.
- **Delimiter**: the combo next to the mode button presets `semicolon / comma /
  colon / TAB / pipe` and also accepts any character or string you type (`=>`).
  Nothing re-splits until you press Enter or click `Apply`: the current items are
  first rejoined with the old delimiter and then split on the new one, so no value
  is lost. Separators inside `"quoted"` runs are not split points. The mode button,
  the combo and `Apply` share one flat self-drawn style that stays consistent in the
  light and dark themes.
- **Reload**: `F5` re-reads from disk and lands on the same row; unsaved edits are
  confirmed before they are dropped. When an external change arrives, a clean window
  reloads itself while a dirty one only shows a warning and keeps your edits.
- **Status bar**: two rows. Row 1 shows edit state, visible range, filter and
  writability; transient messages expire after 4 s. Row 2 is a strip of
  clickable shortcut chips, greyed out when unavailable.
- Inside the detail pane `Tab`/`Shift+Tab` move backwards and forwards between the
  fields, `Ctrl+Tab`/`Ctrl+Shift+Tab` switch tabs.
- `Ctrl+S` lists the individual changes (add / modify / delete) before writing.
  Only a change to the **user** scope (HKCU) after the window opened counts as a
  conflict, and then the choice is Abort / Retry / Ignore: abort writes nothing,
  retry re-reads the latest values, ignore overwrites with your edits. After a
  save the model is reloaded with the current tab settings, so system-variable rows
  survive it.

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `hostsPath` | text | `%SystemRoot%\System32\drivers\etc\hosts` | hosts file location |
| `backupCount` | number | 10 | backups kept per target |
| `maxResults` | number | 50 | max rows per query (capped at 200) |
| `showSystemVars` | bool | true | include read-only system vars in `env` |

## Tests

- `tests/test_envhosts_logic.cpp` — 69 OS-free unit checks (hosts byte-exact
  round-trip on a sanitised captured file, section comments, env model, grammar,
  validation, stable ids).
- `tests/test_editor_model.cpp` — 73 OS-free unit checks for the editor data model
  (dirty tracking, undo/redo cap at 200, validation, PATH split/join with quoted
  segments, custom-delimiter round-trip and switching, system-scope edits leaving
  the user-scope fingerprint untouched, multi-hostname hosts rows, byte-exact save
  preserving BOM/CRLF, fingerprint sensitivity, draft serialisation round-trip,
  backup loads as unsaved).
- `tests/integration.py` — drives the real exe over JSON-RPC on real Windows:
  protocol round-trips, edit/delete/restore on a scratch registry key, 20-query
  side-effect audit (hosts bytes + registry must be untouched), real System32
  hosts write-denial, `settingsChanged` propagation, system-scope delete
  refusal, **and the editor suite** (P1/P2 pinned first row, P3 openEditor wiring,
  P4 manifest fields, P5 draft location, P6 external-change path).
- `tests/gui_preview.py --no-hold` — 30 assertions against the real window on the
  desktop, driven over JSON-RPC with a fake 28-segment `Path` in an isolated
  registry key: split-pane layout, page scrolling, `Ins`/`Del`, list-mode splitting,
  selection keeping its scroll position, editing an item in place, a custom
  delimiter round-trip, the rejoined raw value, splitter drag, saving through the
  shortcut chip and reading the registry back, system rows surviving the save,
  the Abort branch of an external conflict, and the `F5` reload prompt. Both the
  user and the system scope point at isolated test keys, every sample value is
  fake and both keys are deleted afterwards.

## License

MIT, see [LICENSE](LICENSE). Release history: [CHANGELOG.md](CHANGELOG.md).
---

[← 简体中文（默认）](README.md) · English
