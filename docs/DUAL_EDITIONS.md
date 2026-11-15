# Editions and lifecycle / 版本与生命周期

`ra9fael.envhosts` has one plugin ID and, since 0.9.0, one maintained edition:
**QML**. The native GDI edition ended at 0.7.5 (the `gdi` branch is archived).
Both are the same plugin identity: settings, backups, command keywords and the
data folder do not change when the edition changes. Do not rename the ID or
install a second copy under another ID.

从 0.9.0 起，`ra9fael.envhosts` 只维护 **QML 版**；原生 GDI 版停在 0.7.5
（`gdi` 分支归档）。两者仍是同一个插件：设置、备份、命令关键词和数据目录都
不变。不要改 ID，也不要另装一个第二份。

| Edition | Status | Branch | Tag | Asset |
|---|---|---|---|---|
| QML | maintained | `qml` | `qml/vX.Y.Z` | `ra9fael.envhosts-<version>.wsplugin` |
| GDI | retired | `gdi` | `gdi/v0.7.5` | `ra9fael.envhosts-0.7.5.wsplugin` |

## Upgrade and downgrade / 升级与降级

- Installing 0.9.0 over GDI replaces the program folder in place; settings,
  backups and the command set are kept. Close or save editor windows first.
- The QML package needs WinShun >= 0.23.0 (shared host Qt 6.12, `elevated`
  allowed). Older hosts are not offered the update because of `minWinShun`;
  they keep running GDI 0.7.5.
- Going back is a normal install of `gdi/v0.7.5`; WinShun asks for the
  downgrade/replacement confirmation. Keep both release pages bookmarked.

## Data / 数据

Settings and backups are shared; drafts are per backend:

```text
<WS_DATA_DIR>/
  backups/env/          shared environment snapshots
  backups/hosts/        shared byte-exact hosts snapshots
  drafts/gdi/           native editor drafts (GDI only)
  drafts/qml/           QML editor drafts and recovery copies
  drafts/legacy/        original bytes of migrated legacy drafts
  qml-view.ini          QML column sizes
```

Both editions keep lossless `REG_MULTI_SZ` and unknown backup formats intact;
a backup that cannot be proven faithful is rejected rather than rewritten.
GDI drafts are not read by the QML editor and vice versa; shared env/hosts
backups restore in both.

## Release and library / 发布与插件库

`plugin.json` is the only version source; tags are `qml/vX.Y.Z`. The package is
`ra9fael.envhosts-<version>.wsplugin`: the registry index accepts only
`<id>-<version>.wsplugin`, and this is the asset the library entry points at,
so a library update keeps the QML backend instead of switching back to GDI.
QML releases are the repository's Latest release. Published assets are
immutable; a bad release is followed by a new version, never overwritten.

Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` with Qt 6.12.0
on `CMAKE_PREFIX_PATH`, then `ctest --test-dir build --output-on-failure` and
`python tools/package.py --exe build/bin/envhosts.exe --tag qml/vX.Y.Z`.

Push a new `qml/vX.Y.Z` tag to build, test and publish automatically. Dispatch
`release.yml` with a source `ref` and `publish=false` to verify only. Manual
publication requires an existing tag and `publish=true`. Both routes check the
edition identity and branch ancestry; existing release assets are never
replaced. See [WORKFLOWS.md](../WORKFLOWS.md).

## Acceptance / 验收

- Upgrade GDI 0.7.5 -> 0.9.0 -> GDI 0.7.5: one installed ID, one command set,
  settings/backups preserved, drafts unchanged per backend.
- Same-version replacement and downgrade show WinShun's confirmation.
- A host below 0.23.0 is not offered the QML update and keeps working on GDI.
- Legacy draft migration, unknown formats and discarded drafts behave as before.
- Failed installation restores the previous folder; cancellation leaves the
  installed edition and data untouched.
- Test the installed package on the user's running WinShun; inspecting the
  archive alone is not acceptance.

Automated evidence is recorded in `docs/EDITION_RELEASE_EVIDENCE.md`.
