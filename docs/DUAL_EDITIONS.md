# GDI and QML editions / 双版本发布

Both editions use `ra9fael.envhosts`. They are alternative packages for one
installed plugin. Installing the other edition through WinShun replaces the
program directory, preserves its data/settings and stops the old process first.
Do not rename the ID or install a second copy under another ID.

两版是同一个插件的不同安装包。安装另一版时确认替换即可，支持往返切换；
较低版本可能显示降级确认。切换前请保存或关闭编辑窗口，使草稿完成落盘。
这里的互斥指同一 WinShun 用户插件目录，不是跨用户或跨 WinShun 实例的全局锁。

| Edition | Branch | Tag | Asset | Dependencies |
|---|---|---|---|---|
| GDI | `gdi` | `gdi/v0.7.5` | `ra9fael.envhosts-0.7.5.wsplugin` | Windows only |
| QML | `qml` | `qml/v0.8.0` | `ra9fael.envhosts-qml-0.8.0.wsplugin` | Host Qt 6.12 (WinShun ≥ 0.23.0) |

`plugin.json` is the sole version source. `edition.json` selects the backend;
the build and package tools reject mismatches. `--build-info` checks a binary's
ID, version and backend without opening a GUI or reading registry/hosts data.
Checksums are published with each immutable package. Tags point to the source
used to build that release. Older prototype history remains on
`qml-prototype-archive`. Common storage/protocol fixes must be ported and tested
on both maintenance branches; UI changes belong to their own branch.

## Data / 数据

Settings and backups remain shared. Drafts and UI state are backend-specific:

```text
<WS_DATA_DIR>/
  backups/env/          shared environment snapshots
  backups/hosts/        shared byte-exact hosts snapshots
  drafts/gdi/           native editor drafts
  drafts/qml/           QML editor drafts and recovery copies
  drafts/legacy/        original bytes of migrated legacy drafts
  qml-view.ini          QML column sizes
```

Old `drafts/env.json` and `drafts/hosts.json` are routed by their format, not by
which edition opens first. Originals are archived before removal from the old
path. Existing per-edition drafts are never overwritten. Unknown formats stay
in place. Clearing a migrated draft does not resurrect its archived original.
No draft is silently applied to the registry or hosts file.

旧草稿按格式识别并保留原文；切回对应版本时再恢复。两版的草稿不自动互转。
普通字符串变量和 hosts 备份可跨版本恢复。GDI 不会按旧的分号字符串格式
解释 QML 的无损 `REG_MULTI_SZ` 或原始未知类型备份，需切回 QML 恢复；
QML 同样拒绝无法证明保真的旧版多字符串备份。备份的用户/系统范围必须保留，
系统范围备份不能导致无关的用户变量被删除。

## Downloads and updates / 下载与更新

README offers separate release links. GDI is the default repository release;
QML releases are published with `--latest=false`. The current plugin-library
format has only one version/URL per ID, so its entry continues to represent GDI.
QML packages deliberately use `-qml-` in their asset filename to avoid being
picked up as a GDI library update by the current registry asset matcher.

QML 用户请从 QML 发布页更新。当前宿主不识别后端；手动接受插件库的 GDI
更新可能切回 GDI。没有通过虚构版本号或仅添加无效 manifest 字段规避这个限制。
以后如需插件库直接选择版本并沿用所选后端更新，需要同时扩展宿主和索引格式。

## Build and release

Use the matching branch. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
selects its edition automatically. QML additionally needs Qt 6.12.0 on
`CMAKE_PREFIX_PATH` (matching the host's shared Qt). Then build and run
`ctest --test-dir build --output-on-failure`.

GDI: `python tools/package.py --tag gdi/v0.7.5`.

QML (host Qt, no bundled runtime): `python tools/package.py --exe build/bin/envhosts.exe --tag qml/v0.8.1`.

Push a new edition version tag to build, test and publish automatically. Updated
or deleted tags do not build. For verification, dispatch `release.yml` with a
source `ref` and `publish=false`. Manual publication requires an existing tag
and `publish=true`. Both routes verify edition identity and branch ancestry;
existing release assets are never replaced. See [WORKFLOWS.md](../WORKFLOWS.md).

## Acceptance

- Install GDI, replace with QML, then replace with GDI: one installed ID and one
  command set throughout; Qt runtime files disappear with the QML directory.
- Same-version replacement and downgrade use WinShun's replacement confirmation.
- Preserve settings, both drafts and existing backups across each direction.
- Verify legacy migration, unknown formats, existing destination drafts and
  discarded drafts that must not reappear.
- Verify env/hosts backup compatibility, unsupported types and scope boundaries.
- Failed installation must retain/restore the old directory; cancellation must
  leave the installed edition and data untouched.
- Test the installed packages on the user's running WinShun after local checks.
  Isolated tests and package inspection are not evidence of live-host acceptance.

Automated evidence is recorded in `docs/EDITION_RELEASE_EVIDENCE.md`.
