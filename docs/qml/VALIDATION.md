# 设计阶段验证历史

以下记录对应此前已完成的设计阶段，保持历史事实。2026-11-08 用户随后授权产品实现，新增证据见 [实现记录](IMPLEMENTATION.md)。

日期：2026-11-08 · proposal-2 + contract-1。对象：设计、specs、离线交互原型与 Qt 落地规范；不是 QML 产品验收报告。

## 工作区与范围

- 来源：`main@eb3514424bcc1a4bf2d1c10d0bf4f0efe9c9654c`；分支 `envhosts-qml-design`。
- worktree：`C:\Users\renfei\.codex\worktrees\envhosts-qml-design\ra9fael.envhosts`。
- 新增 `docs/qml/`、`tools/gen_qml_design.py`、`tools/check_qml_prototype.cjs`、`tools/check_qml_contract.py`，在 `CONTRIBUTING.md` 加入规范入口；产品源码、manifest 和运行时未改。
- 预览 PNG、jsdom 依赖、离线检查结果均放在已忽略的 `build/` 下。
- 原工作区未提交的原生编辑器修改未复制；原 `main` / `qml` 引用未改。
- 只读进程检查确认机器上已有 Win顺 在运行。本轮没有启动新 Win顺、停止进程或更换插件。

## proposal-2 已执行检查（历史记录）

| 检查 | 方法 | 结果 |
|---|---|---|
| JavaScript 语法 | `node --check docs/qml/prototype/prototype.js` | PASS |
| 离线原型行为 | `node tools/check_qml_prototype.cjs`，jsdom 26.1.0 | PASS：13 组、89 条断言 |
| 三条核心任务 | 离线事件驱动 + 独立期望值 | PATH 添加、hosts 停用、备份恢复均通过 |
| 保存取消 / 冲突 / 失败 | 检查目标未写、工作副本保留、新审阅和重试结果 | PASS，仅合成内存目标 |
| 元数据与视图切换 | 待删除项、稳定身份、移动、undo / redo、跨页选择 | PASS |
| 生成资产 | Python 标准库运行生成器 | 10 SVG + 1 画廊 HTML |
| 可重复生成 | `tools/gen_qml_design.py --check` | PASS：11 个生成资产一致 |
| SVG 渲染与结构 | ElementTree + sharp / librsvg，新增 P09 状态图 | PASS：10 张均解析和渲染成功；查看 P02 / P09，未见溢出或遮挡 |
| 文档追踪 | F01–F20、T01–T48、R01–R13、链接和静态资源 | PASS：20 功能、48 产品用例、13 原型用例、25 文档链接、34 HTML 引用 |
| CSS 语法 | rrweb-cssom 本地解析样式文件 | PASS：140 条规则；不等同于布局测试 |

离线检查执行了真实原型 JavaScript 及 DOM 事件；jsdom 不提供浏览器布局。
测试使用受控计时器和 dialog 替身，不把它们计为真实浏览器焦点 / 模态 / 性能证据。
最初测试观察接口的作用域问题已修正；最终无未通过的 R 用例。

原型无服务器、无持久化存储，无注册表、hosts 或 Win顺 API。CSP 禁止 connect-src；
生成器只使用 Python 标准库。jsdom 固定版本只用于忽略目录中的离线检查，不进入原型发布资源。

## contract-1 补充检查

用户再次确认本轮补齐落地规范，产品实现另开下一轮。新增 [Qt 实现规范](QT_IMPLEMENTATION_STANDARD.md)、
机器可读追踪表、共享 tokens 和只读检查脚本；同步评审索引、功能 / 测试 / 交互 specs。
SVG 与 HTML 的颜色由同一个 JSON 驱动，生成原型 tokens.css；新增文件名和命令均明确为计划，未冒充现有 API。

| 检查 | 实际结果 |
|---|---|
| `python tools/check_qml_contract.py` | PASS：20 F、10 P、14 C、17 组件、48 T、13 R 的完整追踪；77 个本地链接 / 引用和生成资产一致 |
| 状态边界 | 全部产品要求仍 NOT_RUN；5 个组件需改造、12 个组件待新增；没有产品完成声明 |
| tokens | PASS：双主题 34 组文字 / 控件对比；未发现原型覆写共享颜色 |
| 负例检查（内存副本） | PASS：拒绝缺功能、缺 T 映射、未知命令、伪造产品 PASS、计划路径其实已存在、低对比色和重复 JSON 键，7 项 |
| 生成一致性 | PASS：10 SVG + 1 画廊 HTML + 1 tokens.css，共 12 个资产 |
| JS 语法与原型回归 | PASS：13 组、89 条断言；没有产品写入测试 |
| CSS 语法 | tokens.css 2 条、prototype.css 140 条规则均解析成功；不证明布局 |
| SVG | 10 张解析 / 渲染成功；检查浅色 P01 与暗色 P08，主动作文字清晰，未见溢出遮挡 |

负例检查通过 importlib 加载检查脚本后修改 deepcopy 的 JSON 对象，未改实际规格文件。
tokens 对比只计算不透明色对；浏览器实点、实际 Qt 渲染、字体和透明背景不由这项检查证明。
Python 使用本机可用运行时，工具脚本本身不依赖固定安装路径。

## 受限与未执行

- 浏览器自动化尝试打开本地原型 `file://` URL 时被浏览器安全策略拒绝：只允许 http/https，禁止绕过。
  因而未通过替代浏览器、CDP 或启动服务器绕过；浏览器实点、交互原型截图与布局验收为 **BLOCKED**。
- SVG 使用本地矢量渲染检查；SVG 图片不冒充浏览器原型截图。
- 未执行真人可用性测试，未测首次任务耗时或用户理解率。
- 未实现 / 构建新的 QML 功能；未运行 CTest、Windows 写入集成、Qt GUI 或实际插件安装。
- 产品 T01–T48 全部仍是 **NOT_RUN**，不能用 R01–R13 代替。
- 性能数值是规格目标；历史迁移文档测量不是本轮结果。

## 后续联调约束

用户要求优先通过**正在运行的 Win顺 加载插件**。后续实现阶段先确认当前实例和受支持的插件加载 / 重载路径，
不为方便自动化另起 Win顺 实例。本轮仍只做设计，因此未触碰宿主配置和已安装插件。
