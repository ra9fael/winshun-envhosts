# ra9fael.envhosts — Env & hosts editor for WinShun

Browse, filter and edit **user environment variables** and **hosts file entries**
directly from the WinShun launcher. Every write is backed up first; admin-only
targets degrade honestly instead of pretending to succeed.

## Purpose

- `env` — list user (and optionally system) environment variables, filter by
  name or value, edit values inline, copy raw/expanded values, delete with
  confirmation, restore previous snapshots from backups.
- `hosts` — list hosts entries, filter, toggle comments on/off, copy lines,
  delete lines (double-Enter confirm), add entries, restore backups.

## Install

1. Download `ra9fael.envhosts-<version>.wsplugin` from the
   [latest release](https://github.com/ra9fael/winshun-envhosts/releases/latest).
   Optionally compare its SHA-256 with the `.sha256` file next to it
   (`Get-FileHash <file> -Algorithm SHA256`).
2. In Win顺 open **设置 → 插件 → 已安装 → 添加插件 → 压缩包（.zip / .wsplugin）**
   and pick the file.
3. A plugin installed from a local package is **not enabled automatically**.
   Turn it on in the list; enabling it is how you confirm its permissions
   (see [Permissions](#permissions-and-why)).

**Update:** install the newer `.wsplugin` the same way. Win顺 sees the same id,
asks before replacing the old version, and keeps the plugin's data folder.

**Uninstall:** remove it from **设置 → 插件 → 已安装**.

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
- Resident plugin (`mode: resident`), lazy start, exits after 600 s idle.

## Permissions and why

- `files:read` / `files:write` on `%SystemRoot%\System32\drivers\etc\hosts`
  (object form): the plugin reads hosts to list entries and writes it on
  toggle/add/delete/restore.
- **Registry access has no name in the WinShun permission system.** User
  environment variables live in `HKCU\Environment`; reading and writing them is
  done directly via `RegSetValueExW` (never `setx`, which silently truncates at
  1024 characters). This is declared honestly here rather than hidden.
- No `clipboard`, `processes`, or `elevated` permissions. `elevated` is planned
  for v0.2; in v0.1 admin-only targets are read-only with a clear error.

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
- `tests/integration.py` — drives the real exe over JSON-RPC on real Windows:
  protocol round-trips, edit/delete/restore on a scratch registry key, 20-query
  side-effect audit (hosts bytes + registry must be untouched), real System32
  hosts write-denial, `settingsChanged` propagation, system-scope delete
  refusal.

## License

MIT, see [LICENSE](LICENSE). Release history: [CHANGELOG.md](CHANGELOG.md).
