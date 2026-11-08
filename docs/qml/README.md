# EnvHosts QML 设计评审包

日期：2026-11-08。最新补充：**insertion-1：新增项绿色，在最后一个选中项后插入，保留多选与撤销。**

状态：**repair-1：已实现紧凑列表修复，保留三类多选；完整产品验收尚未完成。** 先看 [实现与验证记录](IMPLEMENTATION.md)。

最新反馈的调查与已实施方案：[列表交互与紧凑布局修复方案](UI_REPAIR_PROPOSAL.md) · [HTML 修复演示](repair-prototype/index.html)。
该演示已获确认并落实到 QML、正式 tokens、功能和测试规格；实际验证与未覆盖变体见实现记录。

本分支从本地 `main@eb3514424bcc1a4bf2d1c10d0bf4f0efe9c9654c` 创建，分支名为
`envhosts-qml-design`。已有 `qml` 分支停在 `051e667`，没有作为新设计基线。
本地仓库未配置 remote，因此这里不声明与远端最新版本同步。

## 阅读顺序

1. [页面地图和保存流程 SVG](design/00-overview.svg)。
2. [全部页面评审画廊](design/index.html)：本地浏览器直接打开，无网络依赖。
3. [可点击交互原型](prototype/index.html)：PATH 添加目录、hosts 停用、备份恢复三条任务；仅演示内存。
4. [交互方法与控件状态矩阵](INTERACTION_SPEC.md)：任务流程、渐进披露、状态转换和焦点规则。
5. [功能 spec](FUNCTIONAL_SPEC.md)：功能、交互、状态、数据边界和实现切片。
6. [测试 spec](TEST_SPEC.md)：逐项输入、步骤、断言、隔离要求及发布门槛。
7. [原型到 Qt 实现规范](QT_IMPLEMENTATION_STANDARD.md)：页面 / 组件 / 命令映射、输入和撤销契约、线程和保存边界、验收门槛。
8. [机器可读追踪表](qt-traceability.json) 与 [设计 tokens](design-tokens.json)：明确哪些代码待新增、哪些需改造；支持自动检查。
9. [设计阶段验证历史](VALIDATION.md) 与 [实现验证记录](IMPLEMENTATION.md)：按阶段区分实际执行过的检查。

| 页面 | SVG | 主要功能要求 | 测试入口 |
|---|---|---|---|
| P00 | [页面地图 / 保存流程](design/00-overview.svg) | F01、F11、F19 | T01、T26–T32、T47 |
| P01 | [环境变量 / 原值编辑](design/01-environment.svg) | F02–F04、F07 | T03–T08、T15–T17 |
| P02 | [分隔项编辑](design/02-split-items.svg) | F05–F06 | T09–T14 |
| P03 | [hosts 条目](design/03-hosts-entries.svg) | F08、F10 | T18–T20、T24–T25 |
| P04 | [hosts 原文](design/04-hosts-raw.svg) | F09 | T21–T23 |
| P05 | [备份列表与差异预览](design/05-backups.svg) | F14 | T35–T38 |
| P06 | [保存确认 / 三快照冲突](design/06-save-conflict.svg) | F11–F12 | T26–T32 |
| P07 | [异常状态 / 草稿恢复](design/07-states-recovery.svg) | F13、F16 | T33–T34、T41–T42 |
| P08 | [暗色环境变量页](design/08-environment-dark.svg) | F17–F18 | T43–T46 |
| P09 | [编辑与保存状态转换](design/09-state-flow.svg) | F05、F11–F13、F15 | T09–T13、T26–T34、T39 |

SVG 是可编辑的矢量示意，并非运行中的 QML 截图。画板 1440 × 1040 包含页外评审注释，
产品窗口尺寸与布局约束以功能 spec 为准。全部路径、变量、主机名及时间均为合成数据。

## 基线与本轮决策

- `plugin.json` 声明版本 `0.7.2`；不能从旧迁移文档中的 `0.4.0` 推断当前状态。
- `qt/qt_editor.h/.cpp` 与 QML 页面已经从只读原型扩展为可编辑版本，接入 session、写入服务、备份与草稿。已执行的用例变体见实现记录。
- `editor_model.h`、`logic.h`、`native.h` 是复用和审计起点，不代表可以直接接上 QML 即通过验收。
- `QML_MIGRATION_PHASE0.md` 保留为历史材料；其中旧版键位、冲突动作和部署测量不能覆盖本规格。
- 原工作目录有未提交的原生编辑器与测试修改；未复制到本 worktree。本规格仍将分隔项错位、重复删除和切换上下文列为必测回归。
- proposal-2 删除“应用到变量”中间按钮：合法编辑自动暂存，切换变量、筛选和页签不弹提交确认；唯一系统写入入口是“保存到系统”。repair-1 使用用户 / 系统按钮，分别保存范围视图状态；主保存按钮为“保存全部修改”。
- 新 QML 版需完整覆盖当前编辑能力；云同步、自动提权、自动合并冲突、宿主内嵌 QML 页面不属于本规格。

## 生成与后续开发

在仓库根目录执行：

```powershell
python tools/gen_qml_design.py
python tools/gen_qml_design.py --check
python tools/check_qml_contract.py
```

生成器只依赖 Python 标准库，输出 10 张 SVG、画廊 HTML、原型 `tokens.css` 与 `DesignTokens.qml`，共 13 个资产。
颜色先改 `design-tokens.json`，画面结构先改生成器，再重新生成。新增的规格文档使用中文，代码与代码注释保持英文。
落地检查覆盖 F / P / C / T / R 引用、ADAPT / PLANNED / IMPLEMENTED 代码路径、测试来源与 tokens 一致性。IMPLEMENTED 只说明代码已建立，PARTIAL 只说明部分变体有证据；静态检查不会颁发产品 PASS。

原型的任务按钮模拟打开不同目标；最终插件仍按 F01 为 env / hosts 各复用一个窗口。
原型只支持三条任务需要的控件，以及撤销、只读、主题、保存取消、冲突和失败情境。
离线原型结果不能替代 QML 的编码保真、多选、权限、草稿与备份验证。

后续实现与联调优先使用**正在运行的 Win顺**，不得为方便测试另起 Win顺 实例。
最新只读检查发现现有宿主已安装 0.8.0-qml.2；新增项修复包 0.8.0-qml.3 仍需通过宿主更新与管理员副本确认流程。部署包和独立协议测试与宿主集成证据分开记录。

继续按功能 spec 的 M1–M5 完成剩余验收。**设计通过、离线原型检查通过、真实 QML 界面通过、宿主集成通过**是不同证据，不能相互替代。
