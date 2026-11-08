# QML 实现记录 · implementation-1 / repair-1

2026-11-08 · branch `envhosts-qml-design` · base `eb35144` · Qt 6.12.0 MSVC x64 / Windows 11。
用户已明确授权产品实现，并特别要求多选。本记录与此前 [设计验证历史](VALIDATION.md) 分开。

## 已实现

- env、hosts 各自一个可复用 QML 窗口，变量 / hosts 条目 / 原文 / 备份页面，保存差异与外部冲突复核。
- 三类多选：Ctrl 点选、Shift 范围、Ctrl+A 当前可见项、复选框、键盘移动。过滤隐藏项会从选择中移除。
- 变量 / hosts 批量删除和恢复，hosts 批量启停，分隔项批量删除 / 恢复 / 移动；每次批量只入一次撤销。
- 每变量独立 PartsState：旧项保留删除标记，新项删除即移除，稳定 ID 随撤销恢复；合法文本自动暂存，无“应用到变量”。
- `REG_SZ` / `REG_EXPAND_SZ` 保留类型，`REG_MULTI_SZ` 按行显示有序元素、保留元素内分号；草稿与备份保存原始 UTF-16 字节。
- hosts 保留 BOM、未修改行的混合换行和末尾换行；未知行只在原文编辑，不参与批量启停。
- 输入缓冲、IME composing 阻止提交、字段硬错误、400 ms 原值暂存、文本撤销优先、模型统一历史。
- C++ 写入服务串行校验指纹 / 权限、验证备份、写入、读回、广播；区分部分失败、读回失败、已保存但广播失败。
- QML 构建的启动器写动作走相同服务；备份恢复转到窗口预览。完整确认前只改工作副本。
- v2 草稿保留未结束输入和分隔项元数据；旧草稿导入标记“原基线未知”。备份恢复前另存恢复草稿，避免自动保存覆盖恢复点。
- 中英文和深浅主题即时更新，同源 tokens；可独立部署 Qt 运行时的预览包。

## 实际验证与边界

执行命令：

```powershell
cmake --build build-qml --target envhosts qml_editor_tests envhosts_tests editor_model_tests
ctest --test-dir build-qml --output-on-failure
python tests/qml_protocol.py build-qml/bin/envhosts.exe
python tools/gen_qml_design.py --check
python tools/check_qml_contract.py
```

构建使用 Ninja、VS 2026 开发环境，Qt 路径 `D:/tools/Qt/6.12.0/msvc2022_64`。
CTest 运行时把 Qt `bin` 加入当前终端 PATH；插件独立包不依赖这个设置。

| 层与实际测试 | 证据范围 | 对应 T 变体 |
|---|---|---|
| `envhosts_logic`、`editor_model` | 既有纯逻辑回归 | 不能替代 QML 验收 |
| `tests/test_qml_editor.cpp` | 选择范围、一次批量撤销、删除标记、连续 / 非连续移动边界、缓冲错误与 composing | T04–T06、T09–T15、T39–T40 部分 |
| 同一 Qt 测试 / fake store | 备份先行 / 失败零写、指纹冲突、权限拒绝、请求范围预检、部分失败、读回失败、广播失败 | T24–T30、T31 部分 |
| 同一 Qt 测试 / native backend | 随机 HKCU 子键两范围、MULTI_SZ 中文与元素内分号、临时 hosts、v2 备份读取、写备份目录失败 | T07、T21、T27、T37 部分 |
| 同一 Qt 测试 / QML 键鼠事件 | 实际控件 Ctrl/Shift/全选/删除，分隔项多选，文本输入与撤销优先，非法名取消、新增行焦点、语言主题、原文切页、关闭时保存复核 | T05–T06、T13、T15、T22、T26、T39–T40、T43 部分 |
| 同一 Qt 测试 / 草稿与恢复 | v2 元数据和未结束输入、hosts 原文 round trip、旧草稿基线未知、损坏 ID 拒绝、恢复到工作副本可撤销且零目标写 | T33–T37 部分 |
| `tests/qml_protocol.py` | 实际 exe 内嵌资源；env/hosts 各连续打开三次只产生两窗；查询/打开零目标写；启动器改值保类型且备份；hosts 原始备份；shutdown 正常退出 | T01–T02、T07、T26、T48 部分 |

implementation-1 历史结果：CTest **3/3 PASS**；Qt 测试含 20 个功能测试（另有 init/cleanup，总计 22 PASS）。最新修复结果见下文。
协议检查 **6 组 PASS**。运行日志与真实 QML 截图放 `build-qml/evidence/`、CTest 日志放 `build-qml/Testing/Temporary/`。
截图只使用合成数据，未把机器真实环境变量写入证据。QML engine 警告也作为测试失败条件。

所有写入测试仅使用 `HKCU\Software\EnvHostsQmlTests\<UUID>\User|System` 和临时 hosts。
系统范围明确重定向到 HKCU；Python 协议测试通过独立 winreg / 文件读取判断结果。
**没有向真实 Environment、HKLM 系统环境或 System32 hosts 写入。**

## 打包和已有宿主

```powershell
./tools/package_qml.ps1 -QtRoot <Qt-msvc-x64> -VcRedistDir <VC-x64-CRT-directory> -OutputDir dist/qml-preview-<run>
python tests/qml_protocol.py dist/qml-preview-<run>/ra9fael.envhosts/envhosts.exe
```

脚本只创建新的 `dist/` 子目录，不覆盖旧包；包含 windeployqt 依赖、VC runtime、manifest、LICENSE、`.wsplugin` 和 SHA-256。
implementation-1 预览包使用 `0.8.0-qml.1` 与 512 MB 内存上限；源 `plugin.json` 的默认 GDI 配置不变。
implementation-1 独立包在只含 Windows 系统目录的 PATH 下通过全部 6 组协议测试；两窗口软件渲染单次工作集 240.36 MiB，
private bytes 147.90 MiB，因此预览包没有沿用原来的 64 MB。
该上限是预览配置，不代表 T47 性能验收通过。全主题 Qt 依赖当前完整携带，尚未精简体积。

implementation-1 历史包为 `dist/qml-preview-03/ra9fael.envhosts-qml.wsplugin`，旁边的 `.sha256` 文件用于核对完整性。

implementation-1 在 2026-11-08 的只读检查记录：现有 WinShun 主实例启动于 2026-11-07 20:06 +08，已安装的 envhosts
已由其他操作更新为 0.7.4。宿主日志显示 2026-11-08 18:57:59 从受保护副本启动原生插件。
一次部署预检查因标题匹配而停止，停止发生在任何文件改动之前；随后核对 HWND/PID，发现命中的是
`rundll32.exe` 的 Windows “Environment Variables”窗口，不是 envhosts 编辑器。

宿主源码 `Host::lookElevation / confirmElevation` 要求内容变更后的管理员插件重新确认受保护副本。
源码也支持“插件卡片 → 重新加载”和开发者目录监视，但后两者不能替代该确认流程。
本次没有启动另一 WinShun，也没有终止现有窗口、替换已安装插件、改动受保护副本或更改宿主配置。
当时 QML 包的宿主实际加载 / 重载仍 **NOT_RUN**；本轮新观察见下文。可在现有 WinShun 的“添加插件 → 压缩包”中导入上述包，
按宿主提示确认更新和管理员副本，再从 env / hosts 打开窗口进行宿主验收；更新前保留当前原生版安装包以便回退。

## 实现差异与待完成验收

- D002：职责接口落实为 `EditorBridge` 的 `state` map + 显式命令；列表共享增量 `EditorRows` 的 `record` role。具体函数名见追踪表，业务写操作仍在 C++。命令尚未全面改成规范中统一的结构化 Applied/NoOp/Deferred 返回值。
- D003：自动备份淘汰暂缓。当前保留恢复点并支持明确删除，避免按数量淘汰误删一次失败写入所需的备份。`backupCount` 不会自动清理 QML 备份；需要实现持久化 pin 后恢复此策略。
- D004：旧格式已经丢失 REG_MULTI_SZ 元素边界时明确拒绝并保留文件。旧草稿中未改动的 MULTI_SZ 可借当前同类型原始值迁移；无法无损判断的修改不猜测分号含义。
- 未实测真实 Windows 中文 IME、Narrator、混合显示器 DPI、多屏、10,000 行渲染与 200 历史步内存/响应、硬件渲染、clean-machine 安装卸载或宿主超时资源约束。
- 关闭等待草稿受宿主约 2 秒退出预算限制；当前协议回调最多等待 1.8 秒。磁盘卡顿 / 草稿失败的真实宿主强制退出情境仍需专门验收。
- 混合备份可选择所有 / 用户 / 系统范围；结构化错误目前主要显示错误码，完整可用性文案仍需打磨。
- 未跑完整 T01–T48 各变体，不宣称发布验收完成。机器追踪表只标 `PARTIAL` / `NOT_RUN`，静态检查不自动升级为 PASS。

原 GDI 实现和原工作目录未提交修改保持隔离，未创建提交、推送或发布。

## repair-1 · 用户确认 HTML 后的修复

已落实 [修复方案](UI_REPAIR_PROPOSAL.md) 的 Q1–Q6。保留用户确认的 [HTML 演示](repair-prototype/index.html)，同步更新正式 F / UI 测试规格、tokens、Qt 落地规范、追踪表和 P01 / P02 / P03 / P08 SVG。

- hosts 投影只包含有效映射，独立注释与未知行留在全文及原始字节中；停用映射仍可选。
- 共用 RowSurface 绘制删除红底 / 红色侧标 / 删除线和启用绿色、停用琥珀色标签；多选描边不覆盖删除背景。
- env 用用户 / 系统按钮切换，分别保留选择 / anchor / 当前行 / 筛选 / 排序 / 滚动位置；共用修改副本和撤销历史，保存复核包含隐藏范围。
- 列宽拖动、边界双击 / Enter 自适应、键盘微调、排序；用户设定宽度保存在插件 dataDir 的 `qml-view.ini`，不再被默认填充宽度覆盖。名称 / 值 / 类型 / 状态单元格双击分流，多选集合保留。
- 共享紧凑组件：行 28、按钮 26 起、单行输入 28 起、复选框标记 14、命中单元格 32×28 DIP。拆分项使用连续行与同色底板，新增全选表头。
- 点击单元格会清除原复选框子焦点；复选框 Space 仅选择，列表 Space 批量启停。200% 最小窗口下的详情区最小高度由内容决定，分隔符栏与底部提示不重叠。
- 万行重建改用 ID 索引；排序使用 layout 通知并映射 persistent indexes，避免逐行移动的二次复杂度。缩放、排序、列宽均不制造数据 undo。

新增真实 Qt 测试：

| 测试函数 | 实际覆盖 |
|---|---|
| `repairHostsProjectionAndScopes` | 注释保真、可见映射、范围选择隔离、批量撤销、非法输入 / composing 阻止切范围与排序、隐藏系统和只读 |
| `repairColumnStateAndLargeProjection` | 宽度最小值和持久化、排序稳定 ID / persistent index、无 modelReset、10,000 行投影 / 排序 / 调宽 / 自动适应 / 筛选 |
| `repairRealCellsColumnsAndStateColors` | 实际 QML 名称 / 值 / 类型 / 状态双击、类型 popup、列宽拖动与键盘、表头双击一次排序、排序后命中正确行、28 DIP 行与控件、多选、明暗删除背景、复选框与列表 Space |
| `repairRealScopeScrollAndSelection` | 每范围 100 条合成变量；按钮切换恢复各自当前行与 700 / 1120 DIP 滚动位置 |

既有 `realQmlMultiSelectionAndScreenshots` 同时覆盖主列表和拆分项多选、连续键盘、文字输入与 undo、最小 900×640 窗口下不重叠。
测试只在隔离 store / 临时文件 / 随机 HKCU 测试键执行；没有操作真实目标。

repair-1 验证：CTest **3/3 PASS**，Qt **24 个功能测试 + init/cleanup，共 26 PASS**。
指定真实 QML 测试在 `QT_SCALE_FACTOR=1 / 1.25 / 1.5 / 2` 下执行，中/英和明/暗截图在 `build-qml/evidence/repair-*.png`。
这是进程缩放检查，不能替代混合显示器、实际 IME 候选窗、大系统字体或 Narrator 验收。
200% 时首次截图发现底部重叠，已修复分区最小高度并增加几何断言。

10,000 个 env 合成条目的一次采样：投影 60 ms，反向排序 120 ms，200 次列宽调整 0 ms（低于毫秒计时分辨率），自适应测量 66 ms，筛选 46 ms。
原始日志 `build-qml/qml-editor-results.txt`；仅 bridge / model 时间，**不代表过滤到绘制 p95 或 T47 通过**。
HTML 离线检查仍为 14 组 / 78 断言；旧原型 13 组 / 89 断言；生成资产、设计契约与 diff 空白检查通过。

UI01–UI11、UI13 只对上述测试覆盖变体有证据，UI12 为指定进程缩放 / 主题 / 语言变体，UI14 为一次 bridge 采样。
真实 IME、辅助功能、混合屏 DPI、万行渲染与全部故障组合仍按完整测试 spec 验收，不批量标记 UI 或 T 为完整 PASS。

本轮宿主只读检查：WinShun 主 PID 46336 仍是 2026-11-07 20:06:40 启动的实例。
已安装 manifest 现在为 **0.8.0-qml.1**，日志显示 **2026-11-08 19:44:14** 已确认管理员副本，**19:44:25** 启动 envhosts PID 57800。
因此不能继续沿用先前“仍是原生 0.7.4”的状态。上述安装发生于本轮修复前，不是 repair-1 的宿主验收。
修复包需在同一宿主通过更新和受保护副本确认后加载；本轮未新开 WinShun、未替换其运行文件或中断已有编辑窗口。

repair-1 交付为 [qml-preview-05 包](../../dist/qml-preview-05/ra9fael.envhosts-qml.wsplugin)，版本 **0.8.0-qml.2**。
`qml-preview-04` 是补最小窗口重叠修复前的中间包，不用于本轮交付。新包包含 Qt / VC 运行时；ZIP 内容中的 exe 与本次构建逐字节一致，SHA-256 已核对。
仅有 Windows 系统目录的 PATH 下，最终包 **6/6 组协议检查 PASS**；本次两窗工作集 243.61 MiB / private bytes 151.40 MiB。
证据见 [机器验证报告](../../build-qml/evidence/repair-validation.json) 与 [独立包协议记录](../../build-qml/evidence/repair-package-protocol.json)。

真实 QML 合成数据截图：[env 浅色](../../build-qml/evidence/repair-environment-light.png)、[env 暗色](../../build-qml/evidence/repair-environment-dark.png)、[hosts 浅色](../../build-qml/evidence/repair-hosts-light.png)、[hosts 暗色](../../build-qml/evidence/repair-hosts-dark.png)、[200% 最小高度](../../build-qml/evidence/repair-environment-dark-2.png)。
本轮未通过原生宿主 UI 导入修复包；管理员受保护副本尚未确认，**repair-1 在现有宿主加载为 NOT_RUN**。继续在现有 WinShun 设置的“添加插件 → 压缩包”选择上述最终包，按宿主提示更新并确认受保护副本。


## insertion-1 · 新增项绿色与就地插入

已按用户最新要求完成：变量、hosts、拆分项的新增条目使用 green / greenbg、绿色侧标与 `+ 新增`；选择边框独立叠加。
单选插在该项后，非连续多选按当前可见次序插在最后一个选中项后；无选择才追加。新增后自动选中、滚动可见并获得输入焦点。
处于排序视图时，保留原来其他行的排列并退出列排序；再次点击表头才重新排序。清除会隐藏新增行的筛选，并保留用户 / 系统范围。

实现落在 EditorSession 和 bridge，RowSurface / PartsEditor 共用新增状态。HostsModel 同步修正序列化，将新行写在列表插入位置；原始注释、BOM、混合换行及草稿恢复不丢失。
HTML 演示增加拆分项“新增项”及绿色选中状态；功能 / 测试 / Qt 实现规范补充 UI15–UI16，SVG 分隔项页补充绿色新增状态。

真实 Qt 新增测试 `insertionOrderAndUndo`、`insertionRealButtonsAndGreenState`：三类列表、单选 / 多选 / 无选择、重复新增、排序 / 筛选、范围切换、撤销 / 重做、草稿、hosts 输出字节与隔离保存、真实按钮点击后立即输入、明暗绿色状态。实际截图见 [新增项浅色](../../build-qml/evidence/insertion-parts-light.png) 与 [新增项暗色](../../build-qml/evidence/insertion-parts-dark.png)。

结果：CTest **3/3 PASS**；Qt **26 个功能测试 + init/cleanup，共 28 PASS**，没有 QML warning；日志 [insertion-ctest-results.txt](../../build-qml/evidence/insertion-ctest-results.txt)。
HTML 离线检查 **16 组 / 89 断言 PASS**，设计契约与生成资产检查通过。完整发布的 IME、辅助功能、混合显示器和宿主验收边界仍与前文一致。

最新宿主只读检查：原 WinShun PID 46336 未重启，已安装 manifest 已更新到 **0.8.0-qml.2**。
本轮没有另开 WinShun 或替换运行中的受保护插件副本。新包 `0.8.0-qml.3` 需在该实例中更新并按宿主提示确认管理员副本。

新增项修复交付：[0.8.0-qml.3 安装包](../../dist/qml-preview-06/ra9fael.envhosts-qml.wsplugin)。SHA-256 与压缩包内 exe 已核对；在仅含 Windows 系统目录的 PATH 下通过 **6/6** 组独立协议检查。
[完整验证记录](../../build-qml/evidence/insertion-validation.json) · [协议结果](../../build-qml/evidence/insertion-package-protocol.json)。现有 WinShun 中的新包加载仍为 NOT_RUN。
