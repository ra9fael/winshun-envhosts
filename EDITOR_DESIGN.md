# ra9fael.envhosts — interactive editor window (v0.2 design)

Status: **design, not implemented.** Nothing in this document is coded yet.
Scope: the pinned first row of the `env` and `hosts` result lists, and the
window it opens. The v0.1 inline (launcher) editing stays as it is.

Sources used for this design (read, not assumed):

- `WinShun/docs/plugins.md` — "有自己窗口的常驻插件" (own-window resident
  plugins), item fields, effects, `ui` / `sandbox` / `idleExitSec` semantics.
- `ra9fael.binarylab/editor.h` (sibling repository) + `ROADMAP.md` — the working GDI window pattern
  (dedicated GUI thread, `openEditorWindow` / `editorVisible` /
  `closeEditorAndWait`, spike measurements).
- `ra9fael.envhosts/{main.cpp,logic.h,native.h,plugin.json}` — current v0.1.

---

## 1. The pinned first row

### 1.1 Rule

Every response of the `env` and `hosts` commands starts with exactly one row
that opens the editor. "Every" includes list, filter, `set` / `add` preview,
`backup`, empty-result and error responses. The rest of the list is unchanged
v0.1 output.

| Field | `env` | `hosts` |
|---|---|---|
| `id` | `env:editor` (stable, never changes) | `hosts:editor` |
| `title` | 进入交互式编辑界面 | 进入交互式编辑界面 |
| `subtitle` | `用户 12 · 系统 34 · 过滤：path` | `启用 9 · 注释 4 · 只读` / `可写` |
| `icon` | same colour as the command | same colour as the command |
| `score` | 100 | 100 |
| `actions` (item-level, overrides the command's) | `[{id:"openEditor", key:"Enter"}]` | same |
| `data` | `{kind:"env", filter, select, draft}` | `{kind:"hosts", filter, select, draft}` |
| `valid` | true (even when the rest is an error) | true |

`data` carries the launcher context into the window:

| Query | `filter` | `select` | `draft` |
|---|---|---|---|
| `env` / `env path` | `""` / `"path"` | — | — |
| `env set FOO=bar` | `"FOO"` | `user/FOO` | `{name:"FOO", value:"bar"}` (shown as an unsaved change) |
| `hosts docker` | `"docker"` | — | — |
| `hosts add 127.0.0.1 a.test` | `"a.test"` | — | `{ip, host}` (unsaved new row) |
| `env backup` / `hosts backup` | `""` | — | opens on the 备份 tab |
| error rows (`E_HOSTS_UTF16`, read failure) | `""` | — | window opens read-only with the error banner |

The window never writes because of `data`. A `draft` is only an unsaved edit;
the user still has to save.

### 1.2 Why item-level actions

The command-level `actions` bind Enter to `edit` (env) / `toggle` (hosts).
Item-level `actions` override them for this row only, so Enter on row 1 opens
the window and Enter on any other row keeps v0.1 behaviour. Ctrl+Enter and
Shift+Enter are not bound on row 1.

---

## 2. Host integration

### 2.1 Manifest changes

| Field | v0.1 | v0.2 | Reason |
|---|---|---|---|
| `permissions` | `files:read`, `files:write` (hosts) | + `ui` | Docs: a resident plugin that opens its own window must declare `ui`, or manifest validation fails. |
| | | + `clipboard:write` | Ctrl+C inside the window copies directly. Host `copy` effects only exist in `action` replies, and the window acts outside those. Drop this if the window gets no copy commands. |
| `runtime.idleExitSec` | 600 | **0** | Idle counts protocol traffic. A user editing for 10 min sends no queries, and `shutdown` would close the window. binarylab uses 0 for the same reason. |
| `runtime.start` | `lazy` | `lazy` (unchanged) | No hotkey or push. The process starts on the first `env` / `hosts` query, which always comes before the editor row can be pressed. |
| `runtime.sandbox` | `normal` | `normal` (unchanged) | Medium integrity is needed for HKCU writes and hosts. It also avoids the open UIPI question binarylab has under `strict` (ROADMAP Q1). |
| `version` | 0.1.0 | 0.2.0 | Permission change. The host asks the user to confirm the new permissions again. |

`elevated` is **not** added by default. See decision D2 in section 9.

### 2.2 Opening the window

`action("openEditor")` handler:

1. Parse `data` into an `EditorSpec` (kind, filter, select, draft, locale,
   theme, settings snapshot). The GUI thread never reads `settings()`.
2. If the window for this kind is already open: post `WM_APP_FOCUS` with the
   new spec (apply filter/select, keep unsaved edits), return.
3. Otherwise `openEditorWindow(spec, error)`. The wait drops from binarylab's
   3 s to **1000 ms** because `timeoutMs` is 1500. Window creation took about
   10 ms in the binarylab spike.
4. Reply `[notify("已打开编辑窗口（Esc 关闭）"), hide]`, or on failure
   `[notify("无法打开编辑窗口：" + error), keep]`.

One window per kind. `env` and `hosts` may be open at the same time, each on
its own GUI thread (binarylab allows one window; this generalises it to a
small `kind → state` map).

### 2.3 Foreground (the main risk)

The docs say the host calls `AllowSetForegroundWindow` before `menu`,
`picture`, `showSettings` and `hotkey`. They say nothing about `action`. When
Enter is pressed the launcher owns the foreground, so a plain
`SetForegroundWindow` from the plugin may fail. The window then appears
behind other windows with only a flashing taskbar button.

Plan, in order, decided by measurement (test W1):

| Step | What | Keeps rules? |
|---|---|---|
| a | `ShowWindow(SW_SHOWNORMAL)` + `SetForegroundWindow`. Check `GetForegroundWindow() == hwnd` 100 ms after the host's `hide`. | yes |
| b | If (a) fails: return `keep` instead of `hide`, so the launcher stays foreground while the window is created, then the launcher is hidden by the user. | yes |
| c | If (b) is still poor: route opening through `showSettings` (the card's 设置 button) as a second entry point, which is documented to allow foreground. | yes |

Not used: `AttachThreadInput`, fake key presses (`keybd_event` Alt trick), or
`SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT)`. These work around the
foreground lock against Windows' intent.

---

## 3. Architecture

```
 main thread (protocol)            GUI thread "env"         GUI thread "hosts"
 serve(): query / action           message loop             message loop
   │  openEditor ───────────────►  EnvEditorModel           HostsEditorModel
   │  inline set/toggle/delete       │                        │
   │       │                         │ save                   │ save
   ▼       ▼                         ▼                        ▼
 ┌──────────────────── native.h (EnvStore / HostsStore / BackupStore) ───────┐
 │   one process-wide std::mutex g_writeLock around every read-modify-write  │
 └───────────────────────────────────────────────────────────────────────────┘
```

New files:

| File | Content | Depends on Win32? |
|---|---|---|
| `editor_model.h` | Working copy, dirty tracking, undo stack, validation, PATH split/join, conflict check, change list for the save dialog | no (unit-tested) |
| `editor_win.h` | Window, controls, keyboard map, painting, theme | yes |
| `main.cpp` | Pinned row, `openEditor` action, `WM_APP_EXTERNAL_CHANGE` after inline writes, close-and-join in the destructor | — |

Rules carried over from binarylab:

- The GUI thread writes nothing to stdout. Only the protocol thread talks
  JSON-RPC.
- Settings, locale and theme are snapshotted into the spec. `appearance`
  notifications are forwarded with `PostMessage(WM_APP_THEME)`.
- On `shutdown` and in the destructor: post `WM_CLOSE` to each window, wait up
  to 1500 ms, join. Unsaved edits go to drafts (section 6.4), never to the
  registry or hosts.

### 3.1 Control choice

| Option | Keyboard / screen reader | Theme fit | Effort |
|---|---|---|---|
| **A. Win32 common controls** (ListView report mode, Edit, ComboBox, Tab) | Free: Tab order, arrow keys, F2 rename, UIA from the OS | Light is native. Dark needs `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` for the title bar and `ListView_SetBkColor/TextColor`; headers stay partly light | medium |
| B. Custom GDI grid (binarylab style) | Must be written by hand | Full control | high |

**Choice: A.** This is a table editor, and the keyboard and accessibility
requirements come for free with ListView. Option B is worth it only for
binarylab's bit grid, which has no standard control. Dark-mode gaps are
listed as a known limitation.

---

## 4. env window

Title: `环境变量 — Win顺`. Default size 960×600 DIP, remembered in
`WS_DATA_DIR/editor.json` (position, size, column widths, last tab).

```
┌ 环境变量 — Win顺 ─────────────────────────────────────────────── □ ✕ ┐
│ [用户 (12)] [系统 (34) 🔒] [备份]          过滤: [path_______] (Ctrl+F)│
├──────────────────────────────────────────────────────────────────────┤
│ 名称            │ 类型           │ 值                               │●│
│ JAVA_HOME       │ REG_SZ         │ C:\jdk-21                        │ │
│▶Path            │ REG_EXPAND_SZ  │ %USERPROFILE%\bin;C:\tools;…     │●│
│ TEMP            │ REG_EXPAND_SZ  │ %USERPROFILE%\AppData\Local\Temp │ │
├──────────────────────────────────────────────────────────────────────┤
│ 名称 [Path____________]  类型 [REG_EXPAND_SZ ▾]                       │
│ ┌ 列表编辑（按 ; 拆分）──────────────────────┐ [新增 Ins] [删除 Del] │
│ │ %USERPROFILE%\bin      → C:\Users\r\bin  ✓ │ [上移 Alt+↑]          │
│ │ C:\tools               → C:\tools        ✓ │ [下移 Alt+↓]          │
│ │ D:\old\sdk             → D:\old\sdk      ✗ │ [去重] [删不存在]     │
│ └────────────────────────────────────────────┘ [原文 Ctrl+R]         │
│ 展开值: C:\Users\r\bin;C:\tools;D:\old\sdk          长度 812 / 32767 │
├──────────────────────────────────────────────────────────────────────┤
│ ● 2 项未保存 · 用户作用域可写 · 系统作用域只读（需管理员）   [保存 Ctrl+S]│
└──────────────────────────────────────────────────────────────────────┘
```

| Area | Behaviour |
|---|---|
| Tabs | 用户 (editable), 系统 (read-only unless D2 is taken; 🔒 badge and greyed editors), 备份 (list of `backups/env/*.json`, newest first). `showSystemVars=false` hides the 系统 tab. |
| Table | ListView report mode. Columns 名称 / 类型 / 值 (first line, truncated) / ● dirty marker. Sorted by name, case-insensitive. Filter matches name or value, the same rule as `envMatches`. |
| Detail pane | Edits the selected row. **List mode** is used when the value contains `;` or the name is in the list set (`Path`, `PATHEXT`, `PSModulePath`, `CLASSPATH`, `INCLUDE`, `LIB`). Otherwise a multi-line Edit. Ctrl+R switches between list and raw text. |
| List mode items | Each segment shows its expansion and an existence mark for directories: ✓ exists, ✗ missing, ? not checked (UNC or contains an unexpanded `%X%`). Checks run on the GUI thread with a 50 ms budget per refresh, local paths only, no network shares. |
| 类型 | REG_SZ / REG_EXPAND_SZ. Auto-switches to EXPAND when `%X%` is typed (same as `envTypeForValue`), but the user can override. REG_MULTI_SZ rows are shown read-only (v0.1 also does not edit them). |
| Validation | Live, from `isValidEnvName` / `isValidEnvValue`: red border and status text. Save is disabled while any row is invalid. Length counter turns amber above 1024 (a later `setx` by any other tool truncates there) and red above 32767 (the Windows limit for one variable). |
| New / delete | Ins adds a row with a focused empty name. Del marks the row deleted (strikethrough) until save; it does not disappear, so it can be undone. |

---

## 5. hosts window

Title: `hosts — Win顺`. Default 900×560 DIP.

```
┌ hosts — Win顺 ──────────────────────────────────────────────── □ ✕ ┐
│ C:\Windows\System32\drivers\etc\hosts · UTF-8 BOM · CRLF · 🔒 只读    │
│ ⚠ 当前没有写权限。以管理员身份运行 Win顺 后可保存。[打开文件位置]      │
│ [条目] [原文] [备份]                       过滤: [docker____] (Ctrl+F)│
├────────────────────────────────────────────────────────────────────┤
│ 启用 │ IP              │ 主机名                  │ 注释       │ 行 │●│
│ ── # Added by Docker Desktop ─────────────────────────────────── 22 │ │
│ [✓]  │ 192.168.1.5     │ host.docker.internal    │            │ 23 │ │
│ [ ]  │ 127.0.0.1       │ kubernetes.docker.inte… │ old        │ 25 │●│
│ [✓]  │ 10.0.0.300      │ a.test                  │            │ 31 │!│
├────────────────────────────────────────────────────────────────────┤
│ ! 第 31 行：IP 无效 (10.0.0.300)                                    │
│ ● 1 项未保存 · 重复主机名 0 · 冲突 0                     [保存 Ctrl+S]│
└────────────────────────────────────────────────────────────────────┘
```

| Area | Behaviour |
|---|---|
| Header | Path, encoding, line endings, writability. Writability comes from the same write probe as v0.1, checked when the window opens and again before each save. |
| Read-only banner | Shown when the probe fails. All edit controls are disabled. Navigation, filter, copy and 原文 view keep working. The banner says what is needed and offers **打开文件位置** (`SHOpenFolderAndSelectItems`), not a UAC prompt. See D2. |
| UTF-16 file | Same as v0.1: `E_HOSTS_UTF16` banner, the whole window is read-only, no conversion offer. |
| 条目 tab | One row per `HostsEntry`. Section dividers (comment-only lines) are shown as group headings, not as rows. Blank lines and free comments are not shown but are kept by line index, so saving reproduces them byte for byte. |
| Columns | 启用 (checkbox = not commented), IP, 主机名, 注释 (trailing `#…`), 行 (1-based line number), ● dirty / ! invalid. In-place edit with F2 or double-click (ListView label edit for 主机名, an overlay Edit for other cells). |
| Lines with several hostnames | v0.1 `parseHostLine` keeps only `tok[1]`. The editor needs all of them, so the model gets `std::vector<std::string> hostnames` and the cell shows `a.test b.test`. Editing re-joins with one space. Only edited lines are re-serialised; others stay raw. |
| Validation | `ipv4Valid`, an IPv6 check, `hostValid` per hostname. Warnings, not errors: the same hostname enabled on two lines (the first one wins, so the second does nothing), `0.0.0.0` vs `127.0.0.1` duplicates. |
| 原文 tab | Read-only monospace view of the file as it will be written (preview of the save). Becomes editable only in D3. |
| New row | Ins adds `127.0.0.1` + focused empty hostname at the end of the current group (or at the file end when no group is selected). |

---

## 6. Edit, save and conflict model

### 6.1 Working copy

`editor_model.h` holds three things per window:

| Name | Content |
|---|---|
| `base` | What was read from disk/registry when the window opened or last saved, plus a fingerprint: env = FNV-1a over sorted `scope/name/type/value`; hosts = file size + last-write time + FNV-1a of the bytes. |
| `work` | `base` + the user's edits. Dirty = `work != base` per row. |
| `undo` / `redo` | Stack of edit commands (set cell, add row, delete row, toggle, move segment), max 200. Ctrl+Z / Ctrl+Y. Cleared on save. |

Nothing is written while editing. All writes happen in **保存**.

### 6.2 Save sequence

1. Validate `work`. Any invalid row: stop, focus the first one.
2. Show the change list (modal, keyboard-first): `+ FOO = bar`,
   `~ Path: 3 段 → 4 段`, `- OLD_VAR`, `第 25 行 启用 → 注释`. Enter
   confirms, Esc returns to editing.
3. Take `g_writeLock` (shared with the inline v0.1 actions on the protocol
   thread).
4. Re-read the target and compare its fingerprint with `base`.
   - Same: continue.
   - Different: **conflict** (6.3). Release the lock and stop.
5. Back up with `BackupStore` (same rotation as v0.1, same `backupCount`).
   Backup failure stops the save.
6. Write:
   - env: only changed rows, one `RegSetValueExW` / `RegDeleteValueW` each,
     type kept unless the user changed it. Then one `WM_SETTINGCHANGE`
     broadcast, not one per variable.
   - hosts: serialise `work` (untouched lines raw) and write in place, as in
     v0.1 (keeps the ACL, not atomic).
7. Re-read, set `base = work = disk`, clear undo, release the lock.
8. Status bar: `已保存 3 项 · 备份 2026-11-05_2310.json`.

A partial env failure (for example, the 2nd of 3 writes fails) is reported
row by row. The rows that were written stay written, and the backup from
step 5 can restore all of them. This is stated in the status text, not
hidden.

### 6.3 External changes and conflicts

| Source | Detection | Behaviour |
|---|---|---|
| Inline v0.1 action in the launcher (same process) | The protocol thread posts `WM_APP_EXTERNAL_CHANGE` after its write. | If the window has no unsaved edits: reload silently. Otherwise: yellow bar `外部已修改，[重新载入] [查看差异]`. |
| Another program edits env | `RegNotifyChangeKeyValue` on `HKCU\Environment` (event, waited on by the GUI thread with `MsgWaitForMultipleObjects`) | Same as above. |
| Another program edits hosts | `FindFirstChangeNotificationW` on the `etc` folder, filtered by the fingerprint | Same as above. |
| Missed notification | The fingerprint check in save step 4 | Conflict dialog: 重新载入（丢弃我的修改） / 查看差异 / 取消. There is no "overwrite anyway" in v0.2. |

Re-basing (merge the user's edits onto the new disk state) is out of scope.
查看差异 shows `disk` and `work` side by side for the changed rows only.

### 6.4 Closing, shutdown, and drafts

| Event | Unsaved edits? | Behaviour |
|---|---|---|
| Esc / ✕ / Alt+F4 | no | Close. |
| | yes | Dialog: 保存 (S) / 不保存 (N) / 取消 (Esc). Default button 取消. |
| `shutdown` from host, plugin disabled, WinShun exit | yes | No dialog (the host waits only a short time). Write `work` as a draft to `WS_DATA_DIR/drafts/<kind>.json`, close, join. Never write to the registry or hosts. |
| Next open with a draft present | — | Banner `有上次未保存的修改（2026-11-05 23:10）[恢复] [丢弃]`. Restore applies the draft as unsaved edits on top of a fresh read. Rows changed on disk since then are flagged as conflicts. |
| Process killed (crash, job end) | yes | Lost. Drafts are also autosaved every 30 s while dirty, so at most 30 s of edits are lost. |

### 6.5 Backups tab

Same tab in both windows. Lists backups newest first with time, size, and
the number of rows that differ from the current state. Enter on a backup
loads it **into `work` as unsaved edits**, so the user sees exactly what
restore will change and must still press 保存. This replaces v0.1's
direct "restore" for the window path. The launcher path stays as it is.

---

## 7. Keyboard map (everything works without a mouse)

| Key | Where | Action |
|---|---|---|
| Tab / Shift+Tab | window | Cycle: tabs → filter → table → detail pane → buttons |
| Ctrl+Tab / Ctrl+Shift+Tab | window | Next / previous tab (用户 / 系统 / 备份, or 条目 / 原文 / 备份) |
| Ctrl+F | window | Focus the filter; Esc in the filter clears it, a second Esc goes back to the table |
| ↑ ↓ PgUp PgDn Home End | table | Move selection |
| Enter / F2 | table | Edit the selected row (env: focus value; hosts: focus hostname) |
| Space | hosts table | Toggle 启用 |
| Ins / Del | table, list mode | Add / mark deleted (Del again un-marks) |
| Alt+↑ / Alt+↓ | env list mode | Move a PATH segment |
| Ctrl+R | env detail | List mode ↔ raw text |
| Ctrl+C | table | Copy the value (env) or the full line (hosts) |
| Ctrl+Shift+C | env table | Copy the expanded value |
| Ctrl+Z / Ctrl+Y | window | Undo / redo |
| Ctrl+S | window | Save (opens the change list) |
| F5 | window | Reload from disk (asks first when dirty) |
| Esc | table | Close the window (asks first when dirty) |
| Alt+underlined letter | dialogs | Buttons (保存(&S) 不保存(&N)) |

Focus is always visible (system focus rectangle plus a highlighted row).
All controls have accessible names so Narrator reads 名称, 值, 启用.

---

## 8. Appearance, DPI, privacy

- Theme and locale from `initialize.appearance` and later `appearance`
  notifications (sent only to `ui` plugins). Light: system colours. Dark:
  immersive dark title bar + ListView/Edit colours; tab and header controls
  may stay light (known gap of option A).
- Per-monitor DPI v2 (`SetProcessDpiAwarenessContext`), `WM_DPICHANGED`
  re-layout. Font: `Segoe UI` / `Microsoft YaHei UI` for text, `Cascadia
  Mono` → `Consolas` for values and the 原文 tab.
- Data written by the editor: `WS_DATA_DIR/editor.json` (window layout),
  `drafts/*.json` (unsaved edits, may contain env values), plus the v0.1
  `backups/`. README gets a line that drafts can contain secrets such as
  tokens in env values and are deleted after save or discard.
- No network, no other processes, no hooks, no window enumeration. The `ui`
  permission is used only to create the plugin's own windows.

---

## 9. Decisions for you

| # | Question | Options | My recommendation |
|---|---|---|---|
| D1 | What does row 1 do when the window cannot open (old host, foreground fails)? | a) notify only; b) fall back to v0.1 inline list | a), plus the existing rows remain usable below it |
| D2 | System env and protected hosts | a) read-only + banner (no new permission); b) add `elevated`: writable only when WinShun itself runs as admin, never a UAC prompt | a) for v0.2. b) widens the trust needed for the whole plugin; it can come later as a separate step. |
| D3 | hosts 原文 tab editable? | a) read-only preview; b) full text editing with parse-back | a). b) makes the line-index model and conflict handling much harder. |
| D4 | Keep `clipboard:write`? | a) yes, Ctrl+C in window; b) no, copy only from launcher rows | a) — copying a value is the most common action |

---

## 10. Test plan

Same layering as `envhosts-plugin-design.md` (L0 manifest … L5 real host).
New cases only. All write tests use the existing overrides
`WS_ENVHOSTS_TEST_SUBKEY` (scratch `HKCU\Software\…` key) and
`WS_ENVHOSTS_TEST_HOSTS` (temp file). The real registry and the real hosts
are only read, except in the documented manual case M5.

### 10.1 L1 — model unit tests (`tests/test_editor_model.cpp`, no Win32)

| ID | Case | Expected |
|---|---|---|
| U1 | Pinned row present for: list, filter, `set` preview, `add` preview, `backup`, empty, `E_HOSTS_UTF16`, read failure | `items[0].id` is `env:editor` / `hosts:editor`, `valid` true, item-level Enter = `openEditor` |
| U2 | `data` for `env set FOO=bar` and `hosts add 127.0.0.1 a.test` | `filter` / `select` / `draft` exactly as in the table in 1.1 |
| U3 | PATH split/join round trip: `a;;b;`, trailing `;`, quoted `"C:\a;b"`, single segment | Join(split(x)) == x when unedited; edits only change the edited segment |
| U4 | Dirty tracking: edit then edit back | Row not dirty, change list empty |
| U5 | Undo/redo over 200 commands, cleared on save | Stack bound respected |
| U6 | hosts multi-hostname line `127.0.0.1 a b # c` | `hostnames == {a,b}`, comment `c`; untouched line serialises byte-identical |
| U7 | Save of the real fixture `hosts.real` with one toggle | Only that line differs (byte diff), BOM and CRLF kept |
| U8 | Fingerprint changes on: size, mtime, one byte; env: value, type, new var | Conflict detected in each case |
| U9 | Draft serialise/restore, including a value with `"`, `\`, CJK, 32767 chars | Identical after round trip |
| U10 | Backup loaded into `work` | Rows marked dirty, nothing written |

### 10.2 L3 — protocol (`tests/integration.py`, extended)

| ID | Case | Expected |
|---|---|---|
| P1 | `query env`, `query hosts`, `query hosts zzz-none` | Row 1 is the editor row in each response; v0.1 rows unchanged after it |
| P2 | `action openEditor` | Reply within 1000 ms: `notify` + `hide`; process alive; nothing extra on stdout (each stdout line parses as JSON-RPC) |
| P3 | `openEditor` twice | One window (count via `EnumThreadWindows` on the plugin's threads from the test harness), second call refocuses |
| P4 | Inline `env set` while the window is open and clean | Window reloads (checked through a test-only `WM_COPYDATA` "dump state" message in debug builds) |
| P5 | `shutdown` with unsaved edits | Process exits within 1500 ms, `drafts/env.json` exists, scratch registry unchanged |
| P6 | Manifest: `ui` present, `idleExitSec` 0, version 0.2.0 | Field check (same manual check as v0.1, no jsonschema) |

### 10.3 L4 — window automation (UI Automation, `tests/ui_editor.py`)

Driven with `pywinauto` (UIA backend) against the plugin started by the test
harness, then against the real host.

| ID | Case | Expected |
|---|---|---|
| W1 | **Foreground** after Enter on row 1 in the real launcher | `GetForegroundWindow()` is the editor 100 ms after the launcher hides. Decides step a / b / c in 2.3. |
| W2 | Keyboard only: open, Ctrl+F `path`, Enter, Ins segment, type, Ctrl+S, Enter | Scratch key value has the new segment, one backup file added, one `WM_SETTINGCHANGE` |
| W3 | hosts: Space on a row, Ctrl+S | Temp hosts differs only on that line |
| W4 | Invalid IP / env name | Save disabled; status shows the line; nothing written |
| W5 | External change while dirty (test edits the scratch key) | Yellow bar appears; save gives the conflict dialog; no write |
| W6 | Esc with edits | Dialog, default 取消; N closes without write |
| W7 | Read-only hosts (deny ACE on the temp file for the test user) | Banner, controls disabled, Ctrl+C still works |
| W8 | DPI 100 / 150 / 200 %, monitor move | No clipped controls (screenshot per step) |
| W9 | Dark / light switch while open | Colours update without reopening |

### 10.4 Four required situations

| Situation | env | hosts |
|---|---|---|
| Normal | W2 | W3 |
| Empty | Scratch key with no values: table empty, "没有变量，Ins 新增" | Empty temp file: same, save writes exactly the new line |
| Invalid | W4, U9 limits | W4, UTF-16 temp file → read-only window |
| Dependency unavailable | Scratch key missing / access denied → error banner, window still opens | Path setting points to a missing file → banner, no file created |

### 10.5 L5 — real WinShun, manual (human)

| ID | Steps | Check |
|---|---|---|
| M1 | Install 0.2.0, enable | Permission dialog lists `ui` (and `clipboard:write`) |
| M2 | Type `env`, `环境变量 path`, `hosts docker` | Row 1 is 进入交互式编辑界面 every time; Win顺 message log shows `query` / reply with `env:editor` first |
| M3 | Enter on row 1 | Window in front, focus in table (W1 by hand); message log shows `action openEditor` and the reply |
| M4 | Leave the window open 15 min, no queries | No `shutdown` in the log (idleExitSec 0) |
| M5 | Real user env: add `WS_TEST_TMP=1`, save; open a new terminal | `echo %WS_TEST_TMP%` prints 1; then delete it in the window and save |
| M6 | Disable the plugin while dirty | Draft saved; re-enable, reopen, 恢复 works |

### 10.6 Side-effect audit

After every automated run: export `HKCU\Environment` and hash the real hosts
before and after; both must be identical. The scratch key is deleted and the
temp files removed by the harness.

---

## 11. Not covered / unverified

- Whether WinShun allows the plugin to take the foreground from an `action`
  reply. Docs do not say; W1 measures it. Section 2.3 has the fallback.
- Whether the job object or UI restrictions block `RegNotifyChangeKeyValue`
  or `FindFirstChangeNotificationW` for a `normal` sandbox plugin. Not
  measured; save-time fingerprint check (6.2 step 4) is the backstop.
- The local `winshun.h` exposes `theme()` (light / dark) but no
  `appearanceChanged()` hook or `shuttingDown()`. Live theme switching (W9)
  needs either an SDK upgrade or handling the raw `appearance` notification;
  shutdown cleanup goes in the destructor, as in binarylab.
- `pywinauto` is not installed in the current Python; L4 needs it.
- The ListView dark-mode gaps in section 8 are expected, not measured.
