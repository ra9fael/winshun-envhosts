# 原生编辑器交互修正 / Native editor interaction fixes

[English](NATIVE_INTERACTION_FIXES.en.md)

## 约定 / Verdict

按用户本轮明确列出的五项问题修正现有原生编辑器。分隔项操作保存在各变量自己的
候选缓冲中；切换变量、筛选或标签只切换视图。只有“提交修改”或明确保存才把候选
应用到工作模型，注册表写入仍经过保存确认。

Implement the five requested fixes in the native editor. Keep item candidates
per variable. Navigation never commits them; Commit or an explicit Save applies
them to the working model, and registry writes still require save confirmation.

## 代码依据 / Grounded facts

- `applyLayout` clears the Up/Down rectangles; `hitFlat` has no matching targets.
- `buildChips` allocates fixed widths and only 16 px height for footer labels.
- Item `j`/`Ctrl+J` move by -1, `k`/`Ctrl+K` by +1; list dispatch omits dd/gg/G.
- Backup counts initialize only on opening Backups; scope exclusion is reversed.
- Item operations write joined values to the model immediately, while
  `refreshDetail` drops pending flags on navigation.

以上均为当前源码可直接确认的事实，修复前后将通过假数据真实窗口检查验证。

## 实施与交互 / Implementation and interaction

1. Keep visible Up / Down buttons with Alt+Up / Alt+Down labels in the footer
   Items group. Layout and hit testing use the same rectangles.
2. Measure footer labels with their actual font; wrap buttons onto another row
   when necessary and reserve the resulting footer height before placing panes.
3. Use j/Ctrl+J down, k/Ctrl+K up, Alt+J down, Alt+K up. Add list dd, gg and G;
   text fields retain literal typing. Reset partial commands on context changes.
4. Cache total backup count separately from the scoped page list; refresh at
   startup and after writes/deletions. Correct user/system scope predicates.
5. Cache per-variable candidate items, added/deleted flags, delimiter, caret,
   and undo/redo. Commit is one model undo step. Save explicitly collects pending
   variables; reload explicitly discards them. Drafts retain pending state.

中文：补回可点击的上下移按钮；底部按实际字体测宽并按需换行；统一 Vim 方向，
分隔列表补齐 dd/gg/G；总备份计数不受页面筛选影响；候选缓冲按变量保留，支持撤销，
提交后形成一次工作模型撤销记录，保存收集候选，重新载入明确丢弃，草稿保留候选。

## 失败矩阵与验证 / Failure matrix and validation

| 场景 / Scenario | 预期 / Expected |
|---|---|
| 默认/窄窗，中英 / Default/narrow, zh/en | Buttons and text fit, no child overlap |
| 列表/文字框 / List/text focus | Commands move correctly; literal text stays literal |
| 删除/新增/编辑/重排后切换 / Navigate after candidate operations | Candidate flags and text survive; model unchanged |
| 撤销重做/提交 / Undo, redo, commit | Candidate undo stays local; commit is one model step |
| 用户/系统/空备份筛选 / Backup scopes | Correct scope list; total tab count remains accurate |
| 保存/重载/草稿 / Save, reload, draft | Explicit actions only; no hidden writes |

L0: package/manifest consistency if producing a test archive. L1: existing
logic/model/layout CTest. L2: isolated registry/hosts protocol tests. L3: real
WinShun query and native editor opening in an isolated runtime profile. L4:
focused real-window regression plus existing GUI suite and screenshots. L5:
no new public release requested in this turn.

## 权限、交付与边界 / Permissions, delivery and limits

No new permissions or dependencies. Preserve running user applications and
real environment/hosts data. Update bilingual usage/changelog and validation
evidence under `WinShun/docs/validation/`. Physical multi-monitor DPI switching
and real elevated writes remain untested unless explicitly exercised.

不新增权限或依赖；保持真实用户数据和运行中的程序。更新双语说明及验证记录。
实机跨显示器 DPI 和真实提权写入未验证；实施结果在验证记录回填。

## 实施结果 / Delivery report — 2026-11-08

五项修复均已实现。候选草稿的识别和恢复共用校验逻辑，检查变量标识、来源值与
分隔符；仅有候选操作的草稿也会询问恢复。恢复后可一次撤销该变量候选并重做。
底部删除按钮为“删除 / 恢复 / 删除整行”三种文案预留宽度。

实测：CTest 3/3、协议 37/37、原有 GUI 145/145、交互专项 51/51、分隔项
状态 19/19、中英文恢复弹窗各 8/8。真实 WinShun 独立宿主查询并打开当前原生
编辑器成功，筛选词回读及运行 exe SHA256 一致，退出码 0。原有 GUI 的全套运行
在最后的候选草稿修正前完成，最后变更由交互专项和弹窗检查覆盖。

All five fixes are implemented. Candidate-only drafts now prompt for recovery;
validated recovery retains pending markers and supports undo/redo. The footer
reserves space for every delete/restore label. CTest 3/3, protocol 37/37,
original GUI 145/145, focused interaction 51/51, split state 19/19 and both
dialog locales 8/8 passed. The final draft changes were covered by the focused
and dialog suites. A real isolated WinShun host opened the matching native exe
with the expected filter, then exited with code 0.

验证记录 / Evidence: `WinShun/docs/validation/envhosts-interaction-20261108/`。
提供独立测试包，仍沿用 0.7.3 清单；不是新的发行版或新标签。
The separate test archive retains manifest 0.7.3; it is not a new release/tag.

## 追加方案：变量状态与底部操作区（已确认并实施）

用户在后续截图中指出 Path 的上方条目没有显示候选状态，并询问上下移是否应
合并到底部。当前绘制只读取 `m_env.rowState()`，没有合并 `m_partSessions`
中的 `partPending()`。这是源码确认的显示遗漏；不能通过提前写入模型来解决。

### 变量状态

- 按 `scope/name` 查询该变量的候选，与模型状态共同生成一份显示状态。
- 只有候选时显示琥珀色“待提交”；详情提示“有未提交的分隔项修改”。上方值列
  保持工作模型值，下方列表保留候选预览。
- 提交后、保存前显示“未保存”；新增/整行删除保留各自含义并提示尚未保存。
- 已有模型改动又有新候选时，同时显示“待提交 · 未保存”，不覆盖其中一种状态。
- 撤销至无候选时恢复对应模型状态；成功保存且无其他候选时清除修改标记。
- 状态列按中英文最长文案测宽，优先缩短值预览；错误/超限提示仍独立可见。
  选中行保留标签和左侧标记；切换变量、范围、筛选或恢复草稿后重新计算。
- 候选新增、编辑、删除和重排都触发上方行重绘，不能只检查删除计数。

### 底部操作区

将上下移移入现有底部按钮区域，标为“分隔项：上移 Alt+↑ / 下移 Alt+↓”，
与窗口级保存/关闭操作用间距分组。只在环境变量的分隔项模式显示；按钮始终作用
于当前变量的当前分隔项，点击后保持该项选中并恢复列表焦点。首项禁用上移，末项
禁用下移，空列表及只读变量禁用两者。键盘绑定不变。

去掉详情中独占的上下移行，将那一行高度还给列表；原帮助文字改成简短提示，
不保留空白行。“提交修改”仍靠近候选预览，和全局“保存”保持各自作用范围。
底部按实际文案测量并按组换行，布局先预留高度，避免再次挤压或遮挡。

### 验收与后续打包

自动检查新增/编辑/删除/重排 → 切换 → 撤销/重做 → 提交 → 保存的行状态；同时
覆盖“模型改动 + 候选”及两个范围中同名变量。检查按钮首尾禁用、点击对象、焦点、
真实键盘方向与中英 660/980 px 布局；候选阶段必须仍保持模型和注册表不变。
用户已确认方案。追加实现实测：交互专项 94/94、原有 GUI 145/145、CTest 3/3
通过；合成截图确认待提交/未保存组合状态与中英 660/980 px 底部分组完整显示。
按用户授权提交，使用 0.7.4 / v0.7.4 生成独立测试包；不公开发布或更新线上索引。
最终干净源码构建、包内程序与宿主验收结果见
`WinShun/docs/validation/envhosts-v0.7.4-20261108/`。

[English](NATIVE_INTERACTION_FIXES.en.md)
