# 交互原型到 Qt 的实现规范

版本：contract-1 + repair-1 · 2026-11-08 · 设计基线：已确认紧凑列表方案。用户已授权开始实现，含变量、hosts 和分隔项多选。实际证据与未完成项见 [实现记录](IMPLEMENTATION.md)。

本规范回答三个问题：原型中的元素落在哪段代码；交互的数据和副作用如何保持一致；什么证据才能说明实现完成。
以下命令签名是职责契约。实际入口与文件状态由追踪表记录；组件存在不代表该组件的全部验收变体通过。

## 1. 基准、优先级与变更规则

| 对象 | 约束来源 | 用法 |
|---|---|---|
| 功能、数据保护、保存边界 | [功能 spec](FUNCTIONAL_SPEC.md) F01–F20 | 定义必须实现的行为；原型未演示的要求仍有效 |
| 状态、确认、焦点 | [交互 spec](INTERACTION_SPEC.md) 与本规范 K01–K10 | 定义动作前置、成功、拒绝、撤销和返回路径 |
| 外观与页面层级 | [SVG 画廊](design/index.html)、[tokens](design-tokens.json) | P01–P08 是产品画面；P00、P09 是解释图 |
| 任务演示 | [可点击原型](prototype/index.html) | J01–J03 的操作顺序与可见反馈；不移植演示存储算法 |
| 判据 | [测试 spec](TEST_SPEC.md) T01–T48 | 每个变体独立记录；R01–R13 仅证明离线原型行为 |
| 代码落点 | [机器可读追踪表](qt-traceability.json) | F → P / C / 组件 / T / R；区分 ADAPT 与 PLANNED |

用户后续明确决定优先。规格与原型不一致时先登记差异、修改相关规格及判据，再实现；不能把原型的简化行为当作规格豁免。
本规范可以细化未定的实现选择，但不得静默降低 F 或 T 的要求。不得只修改生成的 SVG / tokens.css。

每个实现切片需标注所覆盖 F、T、命令 C、组件 ID 和偏差记录 D；文件改名时同时更新追踪表。
设计通过、原型通过、产品测试通过、真实宿主通过分开记录，不能用文件存在或截图相似宣称功能完成。

## 2. 页面与代码映射

组件文件按下表职责拆分，implementation-1 已建立这些组件。
页面内共享控件放 `qt/qml/controls/`，页面放 `qt/qml/pages/`，状态组件放 `qt/qml/dialogs/`。
根 QML 模块仍为 `EnvHosts.Editor`；注册所有新组件和资源，禁止运行时依赖源码目录。

| 页面 / 原型位置 | Qt 组件 ID 与拟定文件 | 数据与命令 | 主要验收 |
|---|---|---|---|
| P00：入口；原型任务按钮外框 | HOST：`qt_editor.cpp` 的窗口控制器；SHELL：`Main.qml` | 每 kind 一个 session/window；C14 加载、C02 导航、C11 关闭 | T01、T02、T48 |
| P01：`renderEnv`、`renderDetail` | ENV：`pages/EnvironmentPage.qml`；ROWS：`RowDelegate.qml` | EnvRowsModel、当前 rowId；C03、C01、C02 | T03–T08、T15–T17 |
| P02：`partsHtml`、`partAction` | PARTS：`pages/PartsEditor.qml` | PartsModel、PartsState；C04、C01、C05 | T09–T14、T39–T40 |
| P03：`renderHosts` | HOSTS：`pages/HostsEntriesPage.qml` | HostsRowsModel；C03 批量启停 / 删除 | T18–T20、T24–T25 |
| P04：完整原文；原型只展示预览 | RAW：`pages/HostsRawPage.qml` | RawEditSession / 字节映射；C01 | T21–T23 |
| P05：`renderBackup` | BACKUPS：`pages/BackupsPage.qml` | BackupModel / 预览计划；C10、C13 | T35–T38 |
| P06：`requestSave`、`showConflict` | REVIEW：`dialogs/SaveReview.qml`、`ConflictReview.qml` | WriteService / ReviewPlan；C06、C07、C08 | T26–T32 |
| P07：banner、失败 / 草稿情境 | FEEDBACK：`controls/StatusBanner.qml`、`dialogs/DraftRecovery.qml` | 结构化错误 / 草稿 / 关闭决定；C09、C11 | T33–T34、T41–T42 |
| P08：theme | TOKENS：`DesignTokens.qml`；CONTROLS：`controls/CommandButton.qml`、`EditField.qml` | Theme / Locale；C12；不新建 session | T43–T46 |
| P09：状态转换解释图 | SESSION、WRITE：C++ 服务；上述 QML 状态投影 | C01–C12；无需单独制作“状态图页面” | T09–T13、T26–T34、T39 |

原型的评审标题、任务侧栏、模拟外变 / 失败 / 只读开关和假标题栏不进入产品。
同一个原型网页中的 env / hosts 切换，在产品中映射为各自独立的窗口，不共用全局 `page` 或 `selected`。
测试使用组件 `objectName` 和稳定 ID，例如 `envPage`、`saveButton`、`partEditor`；不用翻译文本定位控件。

## 3. 单一数据所有者与 Qt 边界

| 层 / 组件 ID | 必须负责 | 不得负责 |
|---|---|---|
| SESSION：`qt/editor_session.h/.cpp` | 基线、工作副本、稳定 ID、缓冲、命令历史、revision、校验 | 操作 QML 控件、直接写目标 |
| MODELS：`qt/editor_models.h/.cpp` | QAbstractListModel roles、可见行投影、增量通知 | 自建另一套可写数据、用显示下标充当身份 |
| BRIDGE：`qt_editor.h/.cpp` | 类型明确的属性 / 命令、可用性、结果信号、请求编排 | 向 QML 暴露任意路径写入、同步阻塞 GUI |
| WRITE：`qt/write_service.h/.cpp` | 后台读写、能力复检、锁、备份、读回、逐项结果 | 修改活的 GUI model、弹窗或决定用户同意 |
| STORAGE：`native.h` 的受测适配层 | 原始类型 / 字节、错误返回、目标身份 | 失败返回空内容并当成功、日志输出真实值 |
| QML 页面 / controls | 布局、焦点、选择展示、临时输入呈现、调用命令 | 解析注册表格式、拼接保存变更、判断确认 token 有效性 |

`EnvModel` / `HostsModel` 的纯逻辑可复用，但窗口只允许一套命令历史。
若由 EditorSession 拥有历史，就使用无独立 pushUndo 的底层变换，或扩展现有快照一次纳入所有元数据；禁止两套 undo 同时推进。

下列是数据契约示意，不是可直接复制编译的头文件：

```text
EditorSession {
  sessionId, targetIdentity, generation, revision;
  baseSnapshot, workingCopy;
  partsByRowId, activeTextBuffer, rawEditSession;
  undoStack, redoStack;
  currentRowId, selectionIds, anchorId;
  loadState, saveState, externalChange, capability, validation;
}
PartsState { rowId, sourceValueRevision, delimiter, preserveQuoted, parts[]; }
Part { partId, value, isNew, pendingDelete; }
TextEditBuffer {
  editId, rowId, partId?, field, before, text, bufferRevision,
  inputMethodComposing, validation, cursor, selection;
}
CommandSnapshot { valuesAndTypes, rowIds, partsMetadata, rawBytesAndLineMap; }
AsyncRequest { requestId, sessionId, generation, revision, targetIdentity; }
```

- rowId / partId 是 session 内稳定的不透明字符串；名称、索引、范围标签不作 ID。草稿恢复保留 ID 并检查重复。
- `revision` 单调增加；有效命令、undo、redo 都递增，undo 不把 revision 回退。QML 看到字符串 token，避免 JavaScript 数值精度限制。
- `dirty = effectiveWorkingCopy != baseSnapshot || hasPendingEdits`。已删除项参与元数据快照，但不再参与有效值合并；元数据历史与是否需要系统写入是两个问题。
- session 的 loading、saveState、外变、权限和输入状态分别建模；QML `State` 只投影视觉，不承担第二套事务状态机。
- 委托 roles 至少有 `rowId / partId`、`scope`、`rowState`、`writable`、`disabledReason`、`validation`、完整值。省略号只影响显示。
- env 类型值必须可表示有序字符串数组；REG_MULTI_SZ 不能先压成含分号字符串再声称保真。hosts 保留原始字节、行 ending 与当前缓冲的映射。

## 4. 命令契约

所有按钮、菜单和快捷键调用同一命令入口。每个入口在 C++ 重新校验目标、状态、能力和 ID，不依赖 QML `enabled`。
返回 `Applied / NoOp / Deferred / Invalid / Denied / Stale` 及结构化原因；异步返回 requestId，完成信号带请求上下文。
`Applied` 仅表示工作副本变化；存储成功使用 K06 的专用结果，不能共用一个模糊 bool。

| 命令 | 拟定入口及前置 | 结果 / 撤销 / 测试 |
|---|---|---|
| C01 | `finishTextEdit(editId, reason)`；检查缓冲 revision、IME、硬校验 | 合法输入一次暂存；IME 为 Deferred；非法保留原文及焦点；重复事件 NoOp。T08、T13、T22、T40 |
| C02 | `navigate(viewRequest)` / `selectRows(ids, anchor)`；先 C01 | 成功才切页 / 范围 / 选择；合法切换无确认；不改模型历史。T01、T04–T05、T11 |
| C03 | `editRows(command, ids, payload)`；rowId / scope / capability 有效 | 新增、字段、启停、整行删除恢复；批量一次 undo，跳过不可操作对象并报告。T06–T08、T18 |
| C04 | `editParts(rowId, command, partIds, payload)`；先 C01 | 增删恢复 / 稳定块移动 / 改分隔符；快照含元数据；无变化 NoOp。T09–T14 |
| C05 | `history(direction)`；活动输入按 K03 处理 | 值和元数据一同恢复；新命令清 redo，上限 200；Writing 禁止。T39–T40 |
| C06 | `requestSave()`；先 C01，无硬错且有可保存差异 | 构造 ReviewPlan；只读、无变化和错误不写目标；旧计划失效。T15、T24、T26 |
| C07 | `confirmSave(reviewToken)`；K06 全部匹配 | 单次消费并启动 WriteService；重复请求拒绝；失败保留意图。T25–T30、T32 |
| C08 | `observeExternalChange(snapshot)` / `resolveConflict(choice)` | Clean 可刷新；Dirty 只提示；覆盖生成新计划，不能直接写。T31–T32 |
| C09 | `requestReload()` / `confirmReload(decisionId)`；Dirty 要确认 | 取消保留输入 / undo；确认后成功读取才更换基线；读失败保留编辑。T11、T17、T41 |
| C10 | `requestRestoreBackup(id, scope)` / `confirmRestore(planId)` | 校验 ID / 内容 / scope；备份当前目标及工作副本恢复点成功后一次暂存；不写目标。T35–T37 |
| C11 | `resolveDraft(choice)` / `requestClose(reason, choice)` | 写中延迟；草稿持久化失败不误称已保留；关闭路径见 K07。T02、T33–T34 |
| C12 | `updateContext(settings, appearance)` | 主题语言不换 session；目标改变另走保存 / 草稿 / 丢弃决策；T45–T46 |
| C13 | `copySelection(mode)` / `exportBackup(id)` / `deleteBackup(id, decisionId)` | 剪贴板须显式操作；导出仅许可目录；删除准确 ID 并确认。T05、T37–T38 |
| C14 | `openOrFocus(OpenArgs)` / `loadTarget()` | 复用窗口；异步加载、错误保留、关闭重开无悬挂引用。T01–T03、T47–T48 |

### K01 — 编辑缓冲与稳定身份

输入事件先更新对应 editId 的缓冲和 dirty；不能在每个键击后重置整个列表。
`ensureParts(rowId)` 仅首次解析，重获焦点和切页返回已有状态；部分项删除后禁止再从合并字符串重建 tombstone。
原值实际改写、分隔符实际转换才重建 parts，并把 before / after 元数据放在同一命令。

### K02 — 完成输入与导航次序

Enter、Tab、blur、Ctrl+S 可能连续到达。用 editId + bufferRevision 去重，先 finish，再执行导航 / 保存；不能分别各 pushUndo。
Enter 在 IME 组合态先由输入法处理；完成组合后只处理一次待办请求。失焦并不保证 Qt 的 `editingFinished` 能代表所有非法 / IME 情况，C++ 必须保留独立缓冲。
硬错误时导航返回 Invalid，恢复到对应字段且保留光标；不得先销毁 delegate 再试图读取它的文本。
按 Esc 标记本次输入已取消，再还原文本，随后 blur 不得把取消内容提交回去。

### K03 — 三类文本的撤销边界

| 文本 | 暂存边界 | Ctrl+Z / Esc |
|---|---|---|
| env 原值 / 名称 / 类型输入 | 原值 400 ms 无新输入且非 IME，或离焦 / 保存；其余字段显式完成 | 未暂存部分先文本撤销；暂存后模型 undo；Esc 只取消最近未暂存片段 |
| 分隔项文本 | Enter / Tab / blur / 显式命令前 | 本次输入一次模型命令；不套用原值 400 ms 自动结束规则 |
| hosts 完整原文 | 离原文页 / 保存 / 关闭前一次完成 | 页内使用局部文本 undo；完成后才进入模型历史 |

400 ms 暂存后保持输入焦点 / 光标，后续输入形成新片段；Qt 控件旧的本地 undo 不能跨越已暂存边界反向覆盖模型历史。
实现必须证明本地编辑历史边界已更新，不能只连接全局 Ctrl+Z。原文换行与编码由字节适配层处理，不以 TextArea 的规范化文本替换所有原始字节。

### K04 — 分隔项可执行判据

用合成基线 `Path=A;B;C`，IDs 为 `pA,pB,pC`。后续 Qt 测试必须比较独立期望值：

| 动作 | 有效值 | 元数据 / 历史 | 目标 |
|---|---|---|---|
| 删除 pB | `A;C` | pB 保留且 pendingDelete=true；一条命令 | 仍 `A;B;C` |
| 切另一变量、原值、备份再回来 | `A;C` | ID / tombstone 不变；不增加命令 | 不变 |
| pC 上移一位 | `A;C` | 顺序 pA,pC,pB；pB 标记随 ID | 不变 |
| undo 移动，再 undo 删除 | `A;B;C` | pA,pB,pC；pB 恢复 | 不变 |
| 新增 pD 后删除 pD | `A;B;C` | pD 移除；undo 删除可恢复同一 pD | 不变 |
| 重放已完成的 editId / 越界移动 | 不变 | NoOp；revision / undo 均不增加 | 不变 |

多选移动按相邻选中块处理，保持块内顺序；遇边界的块不越界，不通过反复交换所选下标导致组内倒序。对应 T10 必须包含不连续选择。

### K05 — 模型通知、线程与过期结果

QAbstractItemModel 及其通知在所属 GUI 线程运行；后台服务只接收不可变请求和返回值对象。
值改变发送限定 roles 的 dataChanged，增删 / 移动使用 begin/end 配对；整模 reset 只用于初载、明确接受的重载 / 恢复，且先完成活动输入并按 ID 恢复焦点。
筛选可用代理模型，但下标先映射成 rowId 再发命令。不能将源下标再次用于已过滤的模型。

异步返回按 sessionId / generation / requestId / targetIdentity 检查；编辑相关结果还要匹配 revision。
切换目标、窗口销毁、较新请求已完成时丢弃旧只读结果；写入结果即使窗口已关闭，也必须由持有它的 session / shutdown 流程记录，不能无声丢弃实际副作用。
禁止在 GUI 线程 join 等待正在同步调用 GUI 的协议线程；关闭先停止受理新请求，再排空 / 完成事务，最后退出事件循环。

### K06 — 审阅到写入的契约

`ReviewPlan` 保存 `reviewId, sessionId, generation, targetIdentity, modelRevision, changeDigest, scopeFingerprints, capabilityRevision`。
确认返回不透明 token；内容由 C++ 保存并单次消费，QML 不能改 change set。任何值 / 元数据命令、undo / redo、目标或权限变化均使旧确认失效。

固定顺序：C01 → 校验 → 计算差异 → 读取实际范围 → 审阅 → 用户确认 → 获取共享写锁 → 再读 / 复检权限及指纹 → 可靠备份 → 写入 → 读回 → 广播 → 发布结果。
对 env 按确认的增改删操作，保留无关外部变量；hosts 整文件覆盖明确展示外部损失。
锁内不等待用户输入。写锁与现有启动器写入口共用；不可另外创建一把只保护 QML 的锁。

| 写入结果 | UI / 状态处理 |
|---|---|
| Succeeded | 全部写入且读回一致后更新基线 / Clean、清对应草稿 |
| StoredButBroadcastFailed | 数据已读回成功；Clean 加持续广播警告，不自动再次写入 |
| Rejected / Conflict | 零目标写入，保留工作副本；按原因重新校验 / 审阅 |
| PartialFailure | 逐项列成功 / 失败 / 未执行；后续项停止，保留原意图 / 恢复点 |
| ReadbackFailure | 不报告成功，保留意图 / 草稿，并显示目标状态尚未核实 |

所有结果带稳定错误码、范围、备份 ID、逐项结果与已知实际指纹；不记录变量真值。
无变更不备份；备份写 / close / 读回失败先于目标写入终止。失败重试必须生成新计划，不重用 token。
不宣称注册表多项操作或 hosts 原地写入为原子事务；本进程锁后的外部竞争窗口按 T32 如实记录。

### K07 — 草稿、备份与关闭

草稿仅写数据目录，保存 schema、targetIdentity、原基线、工作副本、parts 与已输入缓冲；不能把恢复后当前目标伪装成旧基线。
合法空备份、损坏、读取失败分开返回；新备份验证成功前不做 retention 清理。
恢复预览固定 backupId、备份内容散列、scope、targetIdentity 与工作副本 revision；确认时重新核对。
预览后选择了另一备份、工作副本改变或备份文件已变时，不使用旧恢复计划，重新展示实际差异。
窗口关闭先走 F13 决策；Writing 延迟关闭；正常“保留草稿”失败时留窗，shutdown 尽力保留最后成功草稿并准确报告失败。
窗口销毁与 hide 区分；窗口映射与 isEditorOpen 状态必须与实际生命周期一致，关闭再开测试不以“还能看到窗口”作为唯一判据。

### K08 — 视觉、组件与可访问性

[design-tokens.json](design-tokens.json) 是颜色和尺寸的唯一来源。SVG 生成器与 HTML 的 `tokens.css` 已消费颜色；未来 `DesignTokens.qml` 由同一源生成，作为只读语义属性，不复制散落的十六进制值。
生成器已经纳入 `DesignTokens.qml` 的 `--check`，同源生成共 13 个资产。

窗口默认 1120×760 DIP、最小 900×640；内容边距 12、基础间距 6、行高 28、按钮最低 26、输入及下拉框最低 28、焦点线宽 2。
复选标记 14 DIP，整个 32×28 选择单元格可点击。Compact 控件覆盖 Basic 默认背景、padding 和 indicator 隐式尺寸。
范围按钮、列宽、按列双击和行状态优先级遵循 [已批准修复规范](UI_REPAIR_PROPOSAL.md)。
用 Layout 的 implicit/minimum/preferred 尺寸、SplitView、滚动容器安排内容；不要把 SVG 1440×1040 的评审注释坐标抄进 QML。
原型外框 / 侧栏和 HTML 响应式断点不成为桌面窗口尺寸；紧凑布局以实际内容空间和最长双语标签计算。
颜色需同语义 token，几何位置在同尺寸基准下主区域边界允许 ±2 DIP；字体栅格化和原生标题栏不做逐像素相等。
长值不得裁切底层数据；32,768 / 100,000 UTF-16 单元输入必须验证控件最大长度配置。

共享控件同时覆盖 normal / hover / pressed / focused / disabled / invalid / readOnly；状态标记同时有符号和文字。
主动作使用 accent / onAccent，分段选择和开关的选中态使用 soft / accent；不能把所有选中控件都画成主提交按钮。
对比度目标：普通文字 4.5:1、交互边界和焦点 3:1；颜色数值检查只是必要条件，截图仍需检查叠加、透明度和背景。
Accessible 名称、作用、选中状态、错误 / 禁用原因与可见文案一致；屏幕阅读器验证单列人工证据。
中文 / 英文通过同一翻译层；语言 / 主题更新只发属性变化，不能重建正在输入的页面或 session。

### K09 — 按键与焦点

优先级：IME → 模态框 → 活动文本 → 当前列表 → 当前窗口。一个按键只由一个层消费。
窗口级 Shortcut 限当前窗口，并按焦点类型关闭冲突命令；不能仅凭 WindowShortcut 就假设它会让文本编辑优先。
Ctrl+A / Del / Ctrl+Z 在字段中先处理文本；分隔项列表上的 Del 才操作项；备份搜索的 Del 不删备份。
模态打开记录返回对象 ID 和字段；Esc 先取消当前模态；关闭后按 ID 恢复焦点，若对象已不存在则退到所属列表。
Tab 链覆盖主工具栏 → 页签 → 搜索 / 范围 → 列表 → 详情；列表选择与键盘焦点分别显示。新增行滚入视口并聚焦首个可编辑字段。

### K10 — 实现前必须处理的基线差距

| 现有位置 / 观察 | 落地要求 | 阻塞门槛 |
|---|---|---|
| `EditorRows::resetRows` 每次整模替换，选择使用 m_visible / m_sel | 改为稳定 ID 和增量通知；详情不混用源索引与过滤索引 | M1 / T04、T11 |
| `EditorBridge::writable` 的 env 返回 systemWritable | 分用户 / 系统实际能力，不能把系统只读误当整个 env 只读 | M2 / T24 |
| `EditorBridge::reload` 直接 load；外变调用 reload | dirty / pending 时只提示，不能覆盖编辑 | M2 / T31 |
| `native.h` 的 REG_MULTI_SZ 文本转换 | 无损原始类型适配或明确阻断编辑；补独立原生断言 | M1 / T07 |
| BackupStore 返回 ID、部分读失败折叠为空 | Result 类型、写 / close / 读回故障、合法空备份辨识 | M2 / T27、T37 |
| 控制器 close 与 destroyed、BlockingQueuedConnection | 证明 close/reopen 及 shutdown 无死锁，调整所有权和退出顺序 | M4 / T02 |
| 历史 QML 内存数据与 manifest 64 MB | 本实现实测后决定配置；不得拿旧测量当结果 | M4–M5 / T47–T48 |

这些是基线审计项。implementation-1 已处理稳定 ID、权限、冲突、无损类型、验证备份和异步窗口关闭；逐项证据与剩余验收见实现记录。

## 5. 从规范到验收的工作流程

1. 选择 M1–M5 中一个切片，列覆盖 F / P / C / T，并补 fixtures 的独立 expected；先完成本切片的数据与错误边界。
2. 对照本规范实现 C++ session / 服务，再接真实 QML 组件；不在页面 JavaScript 中复制 HTML 原型的 session / diff / save。
3. 注册 CTest / Qt Quick Test，驱动与产品相同组件和 bridge；fake store 只替换存储边界。Qt Quick Test 可通过 C++ setup 注入隔离 backend。
4. 按 T 的层逐项验证；故障顺序 trace、目标快照、UI 截图分开存档。Qt offscreen 可覆盖事件，不能替代 Windows IME / DPI / 屏幕阅读器。
5. 更新切片记录、追踪表和偏差，执行设计契约检查；只有要求的证据齐备才改变产品验收状态。

| 门槛 | 必须交付的证据 | 不能替代它的证据 |
|---|---|---|
| G0：设计契约 | 追踪检查、tokens 可重复生成、R 回归；本轮目标 | 文档写了“必须”不代表产品已实现 |
| M1：模型 | T03–T17 / T39 的 U 层；稳定 ID、双向元数据 undo、类型保真 | QML 按钮点得动 |
| M2：存储 | T24–T38 的 B 层与适用 I 层；所有故障断言 | 返回 ID / bool 或 UI 成功提示 |
| M3：真实界面 | T04–T23 / T39–T46 的 Q 层；J01–J03 完整操作；P01–P08 截图 | HTML / SVG 截图、GDI HWND hook |
| M4：宿主 | T01–T02、T47–T48 对应协议、生命周期、性能证据 | 单独运行 envhosts 成功 |
| M5：发布 | 全部 T 的全部变体无 FAIL / BLOCKED / NOT_RUN；P0/P1 均通过 | 少量 smoke 或历史测量 |

测试合并为 `tests/test_qml_editor.cpp`（session、故障注入、Qt Test 驱动真实 QML）与 `tests/qml_protocol.py`（独立进程、资源和原生读取）。不使用 GDI hook 代替 QML 交互证据。
三条任务验收使用与原型相同的合成数据和独立期望结果：J01 增加用户 Path 目录；J02 停用多 hostname 映射并撤销 / 重做；J03 用户范围恢复有新增、修改、删除且不改系统。
测试需要从 store 的独立读取证明结果，不能导入 prototype.js 的 `canonical()` 作为产品保存判据。

本轮可运行：

```powershell
python tools/gen_qml_design.py --check
python tools/check_qml_contract.py
node tools/check_qml_prototype.cjs
```

前两项只需 Python 标准库；Node / jsdom 复现依赖见测试 spec。检查脚本不会运行插件、构建产品或写系统目标。
追踪检查验证引用完整性、计划代码路径、F/T 一致、颜色与 assets 一致；**它不会验证 Qt 行为，也不会自动把任何 T 标为 PASS。**

## 6. 联调与评审记录

后续先记录正在运行的 Win顺 的 PID、路径、版本、启动时间、目标插件路径和安装形态，确认宿主已支持的加载 / 重载入口。
优先复用这个实例，不新开 Win顺。重载前保留可恢复包及配置，只重载目标插件，并保留现有窗口的未保存修改。
不得通过卸载 junction 插件来“清理”；不得覆盖正在使用的 exe。需要宿主退出 / 安装卸载的破坏性验证放一次性环境，不打断用户正在工作的实例。
测试实际存储仅可为断言过的临时目标 / fake backend；不能仅因存在 TEST_SUBKEY 环境变量就认定系统范围也已隔离。

每个实现切片用以下记录模板，证据放已忽略的 `build/qml-validation/<runId>/`；无真实数据 / secret：

```text
slice: Mx / title
sourceCommit + dirtyPatchHash + designRevision + tokenRevision:
requirements / pages / commands / component IDs:
changed files and why:
T id + variant + layer + PASS/FAIL/BLOCKED/NOT_RUN:
Qt / OS / host / architecture / DPI / locale / theme / window size:
fixture seed + SHA256 / target isolation assertion:
command + exit code + independent expected / actual:
evidence paths + before/after hashes + screenshots:
deviations: D-id / affected F,T / reason / approval or pending / resolution:
remaining work and integration rollback location:
```

本轮设计差异记录：D001 将 SVG 与 HTML 中略有差异的背景、字段、边界和文字色统一到 tokens；
主动作统一使用 accent / onAccent，包括暗色。此项为已实施的设计资产调整，不代表 Qt 控件已更改。
列表交互和尺寸以已确认的 repair-1 为准，其余保存 / 冲突规则沿用 proposal-2；新增交互或降低验收门槛需明确评审，不由实现自行取舍。

## 7. Qt 官方实现参考

2026-11-08 已查阅官方 Qt 6 文档。当前构建最低版本为 Qt 6.5；实际编译版本以实施记录为准，使用新 API 前核对最低版本，升级最低版本需单独记录。
以下参考解释 API 行为；本规范中的业务规则属于 EnvHosts 决策，不声称是 Qt 默认提供的功能。

- QAbstractItemModel：线程归属、dataChanged 与 begin/end 模型变更通知。
- TextInput：inputMethodComposing、editingFinished、文本撤销。
- Shortcut：WindowShortcut 的窗口范围；输入焦点优先仍需命令设计。
- Qt Quick Test：TestCase 及 QUICK_TEST_MAIN_WITH_SETUP 注入真实 C++ bridge。
- Threads and QObjects：事件循环、队列连接与同步等待风险。

```text
https://doc.qt.io/qt-6/qabstractitemmodel.html
https://doc.qt.io/qt-6/qml-qtquick-textinput.html
https://doc.qt.io/qt-6/qml-qtquick-shortcut.html
https://doc.qt.io/qt-6/qtquicktest-index.html
https://doc.qt.io/qt-6/threads-qobject.html
```

## 新增项落实约束（insertion-1）

插入位置由 C++ session 使用稳定 ID 与可见顺序计算，禁止 QML 直接改数组或只在界面调整次序。
Bridge 保存手动插入后的视图顺序并同步各范围状态；显式重新排序时清除手动排列。
HostsModel 序列化按工作行中的原始行位置插入新行，不把新行统一挪到末尾；原文格式与草稿行序需一起测试。
RowSurface 使用 `deleted > added > selected` 背景优先级，新增采用同源 green / greenbg，输入框在新增状态也保持绿色。
PartsEditor 等新 delegate 建立后再定位并聚焦输入；Qt 键鼠测试必须证明实际可输入，不能仅检查 currentId。
