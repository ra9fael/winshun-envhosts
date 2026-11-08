# EnvHosts QML 测试 spec

版本：repair-1 · 2026-11-08 · 对应 [功能 spec F01–F20](FUNCTIONAL_SPEC.md)。

**T01–T48 是完整验收计划；implementation-1 与 repair-1 已执行其中部分变体，尚未达到完整发布验收。**
实际 Qt、隔离存储与协议证据见 [实现记录](IMPLEMENTATION.md)；[VALIDATION.md](VALIDATION.md) 保留设计阶段历史。已有 GDI 测试通过不能替代 QML 验收。

## 1. 测试分层、隔离和判定

| 层 | 方法 | 独立判据 |
|---|---|---|
| U | 纯 C++ 模型 / session 单元测试 | 比较数据、命令历史、类型、ID、字节；不只断言按钮发出了信号 |
| B | C++ bridge / WriteService 故障注入 | fake store 记录 read / backup / write / broadcast 顺序，独立读取实际结果 |
| Q | Qt Quick Test / Qt Test + QML fixture | 键盘 / 鼠标驱动真实组件，验证可见内容、焦点、模型及副作用 |
| I | 隔离 Windows 子键 + 临时 hosts 的 exe 协议 / 文件集成 | 用第二进程或 winreg / 原始文件读取核对，不从 UI 文案推断成功 |
| H | 真实 Win顺 与真实安装包 | 消息日志、屏幕证据、安装权限、生命周期、工作集和帧时间 |

P0 = 数据、权限、保存恢复与协议核心；P1 = 主要交互、可访问性和性能。P0 和 P1 均属于正式版验收门槛。
每个 T 编号下面出现的多个变体都要单独记录结果，不能用一次成功代表整个组合。

隔离前置：

- 每次运行创建随机 runId：`HKCU\Software\EnvHostsQmlTests\<runId>\User`、临时 hosts、独立 WS_DATA_DIR。
- 系统范围通过可注入 StoreBackend 模拟 HKLM 或重定向到该 runId 下的独立键；启动前断言确实重定向，不能只相信变量名称。
- 测试进程不允许操作真实 HKCU Environment、HKLM 系统环境或 System32 hosts；假“管理员”能力也不能改变实际目标。
- 准备 `before` 原始快照与 SHA-256；所有取消、只读、失败前置场景的 `after` 必须相同。备份 / 草稿文件允许的写入单独计数。
- 故障注入包括读取拒绝、write probe 成功但实际 write 拒绝、备份写 / close / 读回失败、磁盘满、文件占用、逐项写失败、最终读回失败、广播失败和并发更新。
- fake clock 驱动 400 ms / 2 s / 10 s / 30 s / 4 s 定时行为；GUI 集成等待具体信号 / predicate，设置有限超时，不靠固定 sleep 判成功。
- 清理只删除本 runId 明确创建的目录和子键；记录失败留存路径。默认不运行现有 integration.py 中“真实系统 hosts 拒绝”分支。
- 发布前的真实管理员 / protected-copy 用例在一次性测试环境执行，目标仍重定向；不为了验证写能力改动用户实际环境。
- 本机联调优先复用正在运行的 Win顺，记录已有宿主的身份、版本及启动时间，通过已支持的加载 / 重载入口操作插件；不另起宿主实例。真实安装 / 卸载 / 退出用例不在用户正在工作的实例上直接执行。
- 用户已授权实现；复用现有 Win顺 的加载路径，不能以杀进程或丢弃现有编辑换取联调。

## 2. 数据集

| fixture | 内容 / 用途 |
|---|---|
| E-basic | 用户 Path=`A;B;C`、系统 Path=`SYS_A;SYS_B`、JAVA_HOME、空值、Unicode 名称、含 `%VAR%` 的原值、REG_SZ / REG_EXPAND_SZ / REG_MULTI_SZ |
| E-parts | `A;;B;`、`"C:\Program Files\SDK";D:\bin`、重复 `A;A`、多字符分隔符 `=>`、空格 / 制表符、未闭合引号、两变量不同长度列表 |
| E-length | UTF-16 长度 29,490 / 29,491 / 32,767 / 32,768，含 emoji 代理对；另有 100,000 单元压力值，不能静默截断 |
| H-bytes | 既有 `tests/fixtures/hosts.sample` + BOM 有 / 无、CRLF / LF / 混合、末尾换行有 / 无、空文件、Unicode 注释、头部示例、未知行 |
| H-entry | IPv4 / IPv6、单行多 hostname、开关状态、重复映射、空白、行尾注释、非法 IP / hostname、UTF-16 BOM、无效字节 |
| R-backup | 用户、系统、混合范围、旧格式缺 scope、合法空集合、合法空 hosts、损坏 JSON、未知字段、读取失败、`../` ID |
| D-draft | 现有格式样本、新 v2 envelope、基线缺失 / 不同、targetIdentity 不符、未知 schema、损坏 / 截断、分隔项元数据和未结束输入 / 原文 |
| Scale | 固定种子的 10,000 个变量、10,000 行 hosts；不得包含机器真实值，报告 seed 和文件散列 |

fixtures 必须有明确期望字节 / JSON，不直接把被测函数输出作为 golden。REG_MULTI_SZ 由独立的原生读取确认类型与元素。

## 3. 验收用例

### 生命周期、列表与变量

| ID | 需求 / 页面 / 层 / 优先级 | 输入和操作 | 必须断言 |
|---|---|---|---|
| T01 | F01 / P00 / B,I,H / P0 | initialize 后连续 query env / hosts；只对首条触发 openEditor；各打开 3 次，包含最小化状态 | query 目标 / 剪贴板 / 窗口零副作用；env、hosts 各一窗；已有 dirty、选择、undo 保留；stdout 每条均协议合法 |
| T02 | F01 / P00 / B,Q,I / P0 | QML 加载失败、关闭重开、打开模态后 shutdown、dirty 时宿主退出、后台任务在途关闭 | 失败返回可理解错误且 keep；close 后可重开；草稿先落盘；无 GUI / 协议线程死锁；退出后无孤立进程 |
| T03 | F02 / P01 / U,B,Q / P1 | E-basic、0 行、读失败；切 showSystemVars；加载 Scale 并滚动 | 类型 / 作用域准确，同名 Path 不混；Loading / Empty / Error 不同；编辑器不受 maxResults 截断；delegate 数随 viewport，不随总行数增长 |
| T04 | F02,F03 / P01,P03 / U,Q / P1 | 按名称、值、Unicode 搜索；结果为空再清除；滚动选中后筛选隐藏当前行 | 内容筛选正确；清除可恢复；隐藏选择移除；rowId 不被过滤索引替换；无隐形批量目标 |
| T05 | F03 / P01,P03 / U,Q / P1 | Ctrl+点击、Shift+点击 / 方向键、Ctrl+A；复制混合选择 | 集合、焦点、锚点分别正确；Ctrl+A 只含可见行；文本复制按可见顺序且格式正确；剪贴板仅显式命令更改 |
| T06 | F03 / P01,P03 / U,B,Q / P0 | 多选用户可写、系统只读、hosts 注释；批量删除 / 恢复或启停，撤销一次 | 只改变合法对象；准确报告跳过数量；一次 undo 完全回到 before；系统只读值零写入 |
| T07 | F04 / P01 / U,B,Q,I / P0 | 新增、重命名、改值、改类型；分别编辑三类注册表值，确认保存 | rename 显示旧名删除 + 新名新增；既有类型无隐式推断；独立读取原始值、类型、MULTI_SZ 顺序与终止正确 |
| T08 | F04 / P01 / U,Q / P1 | 快速输入、400 ms 空闲、切焦点；复制原值 / 展开值；编辑后 undo 回基线 | 编辑会话合并一次；原值保留 `%...%`；展开不反写；回基线 dirty=false；全部确认前目标 unchanged |

### 分隔项与校验

| ID | 需求 / 页面 / 层 / 优先级 | 输入和操作 | 必须断言 |
|---|---|---|---|
| T09 | F05 / P02 / U,B,Q / P0 | Path=A;B;C，标记 B；恢复；再次标记；切原值再打开保存确认并取消 | 无应用按钮；工作值即为 A;C，B 元数据仍可恢复；注册表仍 A;B;C；取消保存不丢编辑；一次模型 undo 回 A;B;C |
| T10 | F05 / P02 / U,Q / P0 | 新增 D，混选旧 B 与新 D 后 Del；undo / redo；连续和不连续多选分别移动到首尾 | B 保留待删除，D 即刻移除；标记随 partId 移动；组内顺序不反转；越界移动 no-op，不新增撤销步 |
| T11 | F05 / P02 / U,Q / P0 | 标记 B 后同一行重获焦点、切到另一变量、改筛选 / 范围 / 页签 / 原值；再回原行；含合法输入与硬错误输入变体 | 合法切换零确认弹窗；输入自动暂存一次；另一变量内容与标记零污染；原行 tombstone 保留；无按旧下标重复删除；硬错误保留缓冲并定位 |
| T12 | F05 / P02 / U,Q,I / P0 | 32,768 单元 Path 删除一项降到限制内；统一 undo / redo；改分隔符；再删整个变量并撤销 | 字数、项数、pendingDelete、isNew 全同步；删整变量无需额外确认，清空并禁用详情；撤销恢复正确值与元数据；保存前注册表仍 before |
| T13 | F05 / P02 / Q / P1 | 新增项进入编辑；Enter、Tab、失焦、Esc；连续投递 Enter + blur、Esc + blur；Ctrl+S 处理局部输入；切页回到原行 | 合法文本自动暂存一次；相同 editId / bufferRevision 不重复入历史；Esc 后 blur 不重新提交取消内容；Ctrl+S 不重复删除或新增 undo；无应用 / 重置按钮；保存取消保留暂存；局部错误阻止保存 |
| T14 | F06 / P02 / U,Q / P0 | E-parts 所有变体；在不同分隔符之间往返；自定义空字符串；空格模式取消 / 确认 | 未修改非空格 round trip 完全相同；引号内不拆，空段 / 重复保留；空分隔符拒绝；Path 空格警告默认取消；确认归一化可一次撤销 |
| T15 | F07 / P01,P07 / U,B,Q / P0 | 空名、等号、控制字符、同范围重名、跨范围同名、非法类型；按保存 | 无效字段可定位且硬阻止；跨范围同名允许；目标 writeCount=0；输入和光标保留，修正后可继续 |
| T16 | F07 / P01,P02 / U,B,Q,I / P0 | E-length 逐个粘贴、切模式、存草稿、恢复并保存 / 取消 | 独立 UTF-16 计数；29,491 起警告、32,768 红色；明确超出量；取消零写；确认后原值完整；32,767 边界不误拒；启动器超限仍拒绝 |
| T17 | F07 / P01,P07 / U,Q / P1 | 超长 / 有硬错误时切行、切页、改小值、撤销回超长、切语言 | 全局摘要不因离开行消失；缩短后警告清除；undo 后恢复；按钮可用性与实际验证一致；文案不覆盖计数 |

### hosts 与权限

| ID | 需求 / 页面 / 层 / 优先级 | 输入和操作 | 必须断言 |
|---|---|---|---|
| T18 | F08 / P03 / U,Q / P1 | H-entry 编辑 IP、多主机名、注释，增删、批量启停、撤销 | 正确映射到原行；合法 IPv4 / IPv6 通过，非法字段阻止条目保存；删除可恢复；批量一次 undo |
| T19 | F08 / P03 / U,Q / P0 | H-bytes 的微软风格头部示例、分组注释、未知行，筛选并保存另一映射 | 头部折叠，不能被误启用；注释 / 未知行及相对顺序保持；无关行字节 unchanged |
| T20 | F08 / P03 / U,Q / P1 | 启用行重复 hostname、同一 IP 重复、停用重复、IPv4 / IPv6 同名 | 后续重复启用映射标记和计数符合规则；停用不计；警告不自动删改；相关行号真实 |
| T21 | F09 / P04 / U,B,I / P0 | H-bytes 全组合：无编辑保存，改一行后保存，新增行后保存 | 无编辑字节全相同；仅目标行改变；BOM、混合换行、末尾换行保留；新增采用明确主换行；ACL 对比不变 |
| T22 | F09 / P03,P04 / U,Q / P0 | 原文编辑→条目→undo→redo；重复切页；原文文本 undo；关闭取消 | 原文变化只生成一次模型命令；局部文本 undo 不回滚别的模型命令；没有切页重复应用 / 丢字；关闭取消不额外写入 |
| T23 | F09 / P04,P07 / B,Q,I / P0 | UTF-16 BOM、无效编码、空文件、含非法原文行；只读原文输入 | UTF-16 明确拒绝且零写；无效编码不替换坏字节再保存；空文件可打开；非法行不伪装成映射；只读不接收修改 |
| T24 | F10 / P03,P07 / B,Q,I / P0 | 用户可写 / 系统只读 / hosts 只读；admin token=true 但目标拒绝；反向权限组合 | 控件按真实能力而非 token；只读可复制；命令层再次拒绝绕过 UI 的写请求；不弹 UAC，不降级到别的范围 |
| T25 | F10 / P06 / B,I / P0 | probe 成功后在确认前 / 写入前撤销权限；系统范围恢复时权限不足 | 明确 E_DENIED 与实际结果；不能跳过系统项后报全部成功；目标 unchanged 或按已发生部分结果如实呈现 |

### 保存、并发与恢复

| ID | 需求 / 页面 / 层 / 优先级 | 输入和操作 | 必须断言 |
|---|---|---|---|
| T26 | F11 / P06 / B,Q,I / P0 | no-op 保存；三项增改删；取消；确认；双击确认 / 连按 Ctrl+S；写入中关闭 | no-op 不备份；取消零目标写；确认清单等于独立期望 diff；只执行一次；写入前有备份；成功读回再 Clean；关闭等待结果 |
| T27 | F11 / P06,P07 / B,I / P0 | 备份创建、写入、close、读回分别失败；磁盘满；之后重试 | 任一失败目标 writeCount=0；上一备份保留；错误显示备份失败；不能仅因返回了 ID 判成功；成功重试仍重新校验 |
| T28 | F11 / P06,P07 / B,I / P0 | 三项注册表变更，第 2 项注入失败；第 1 项成功；读取每个键 | 结果为 PartialFailure：已完成 / 失败 / 未执行分明；第 3 项未写；基线不假装全成功；草稿和意图保留；恢复点可用 |
| T29 | F11 / P04,P07 / B,I / P0 | hosts 独占占用、截断后短写、flush / 读回失败、进程异常恢复 | 不把部分文件说成完整保存；保留逐字节写前备份；重开提示恢复；目标 ACL 不变；独立读取确认实况，不宣称原子回滚 |
| T30 | F11 / P06 / B,Q,I / P0 | 确认框打开后修改模型 revision；广播失败；fake store 返回成功但读回不同 | 旧 token 拒绝；广播失败显示“已保存，广播失败”；读回不一致为 ReadbackFailure、dirty / 草稿保留；不会清理未持久化意图 |
| T31 | F12 / P06,P07 / U,B,Q / P0 | base / mine / theirs 的五类差异；Clean / Dirty 外变；只改用户时系统外变、写系统时系统外变 | 分类覆盖增删改；Clean 重载，Dirty 只提示；scope 指纹独立；三个动作符合 spec；覆盖保留无关 env 项；hosts 整文件覆盖显示外部损失 |
| T32 | F12,F11 / P06 / B,I / P0 | 分别在首次读取、确认后、获取写锁前、最终复检前注入外变；启动器和编辑器同时保存 | 复检前变化均中止旧确认并重开差异；本进程写入串行；冲突仍备份先行；最终复检后的外部竞争窗口记录为残余边界，不虚报完全防并发 |
| T33 | F13 / P07 / U,B,Q,I / P0 | fake clock 29 / 30 秒、Dirty 关闭、shutdown、草稿写失败、未知 schema / 截断；再次打开 | 周期只写数据目录；最新分隔项 / 原文可恢复；写失败保留上一份；正常关闭不能误报草稿保留；损坏文件保留并报错 |
| T34 | F13 / P07 / U,Q,I / P0 | D-draft 新旧格式，恢复 / 丢弃 / 稍后，基线已变、路径已变、缺原基线 | 显式选择才应用；恢复仍 Dirty；基线变化走差异；目标不符不套用；旧基线未知有提示且再次确认；成功保存 / 明确丢弃才清理 |
| T35 | F14 / P05 / U,Q / P1 | R-backup 列表、范围筛选、搜索、选中预览；时区改变；毫秒相邻 ID | 排序与 scope 正确；ID 不受时区改变；四类差异与独立期望一致；数量匹配；不存在重复 / 错选 |
| T36 | F14 / P05 / B,Q,I / P0 | 用户、系统、混合、合法空备份恢复；先有未保存编辑；取消或确认恢复再 undo；预览后改变工作副本、备份内容或选择范围 | 只涉及选定范围；恢复前当前目标备份与工作副本恢复点可读；目标未写；空集合列出删除清单；恢复一次 undo；旧恢复计划失效并重新展示差异；再保存仍重新备份 |
| T37 | F14 / P05,P07 / B,Q,I / P0 | 损坏、无法读取、未知字段、缺 scope、合法空 hosts 备份；ID 路径穿越 | 错误不折叠为空恢复；旧缺 scope 明确按用户；合法空文件可恢复；穿越被拒绝且数据目录外无新文件 |
| T38 | F14 / P05 / B,Q,I / P1 | 导出 JSON / hosts，删除确认 / 取消，retention 达上限后新备份成功或失败 | 导出独立读取一致；取消不删除；只删指定 ID；新备份确认成功前不得 prune；本次恢复需要的备份保留 |

### 交互、状态与集成

| ID | 需求 / 页面 / 层 / 优先级 | 输入和操作 | 必须断言 |
|---|---|---|---|
| T39 | F15 / 全部 / U,Q / P0 | 模型 201 次命令、批量、活动文本 / 原文局部 undo；完成输入后与分隔项移动 / 删除命令交错 | 上限 200；文本与统一模型历史边界明确；值和逐项元数据同快照恢复；丢弃 redo 分支符合新命令规则；切页不新增命令 |
| T40 | F15 / 全部 / Q / P0 | 焦点在名称、值、搜索、原文、分隔项、列表、模态框分别按 Del / Enter / Esc / Ctrl+A / Ctrl+S；中文 IME | 字段按键不删整变量 / 备份；IME Enter 不提交；Esc 逐层只消费一次；鼠标和键盘结果一致；另一个窗口不响应快捷键 |
| T41 | F16 / P07 / B,Q / P1 | Loading、空数据、筛选空、读失败、缺文件、占用依次注入，重试恢复 | 每种状态文案和操作不同；错误持久、可重试；缓存不冒充新数据；恢复后输入 / dirty 合理保留 |
| T42 | F16 / P07 / B,I / P0 | 让含假 secret 值的变量在读写 / 备份 / 冲突中报错，捕获 stdout / stderr | stdout 仅协议；诊断有稳定错误码和操作目标，不能泄漏 secret / 完整 hosts；无“成功”假阳性 |
| T43 | F17 / P01–P08 / Q,H / P1 | 只用键盘完成 CRUD、PATH 移动、保存取消、备份恢复；屏幕阅读器浏览 | 焦点顺序、返回焦点、模态焦点约束正确；每控件有名称 / 状态 / 禁用原因；selected 与 focus 可辨 |
| T44 | F17 / P01–P08 / Q,H / P1 | Windows 100/125/150/200%，默认 / 最小窗口，跨屏；最长中文 / 英文标签与长值 | 无按钮覆盖、裁切、丢失保存入口；详情可滚动；计数换行；焦点环可见；文字及控件对比度达标 |
| T45 | F18 / P08 / Q,H / P1 | Clean / Dirty / 正在输入 / 模态时切明暗主题、auto / 固定中英文及宿主 appearance | 全部页签和弹窗及时刷新；固定语言优先；输入、IME、rowId、selection、undo、dirty 不丢；无旧主题残块 |
| T46 | F18 / P01,P07 / U,B,Q / P0 | 缺失 / 非法设置；dirty 时改 hostsPath；showSystemVars 开关；backupCount / maxResults 边界 | 默认回退明确；路径切换先决策；旧草稿不落新目标；隐藏系统不影响用户编辑；设置不触发目标保存 |
| T47 | F19 / P00 / B,Q,I,H / P1 | Scale 固定数据：20 次冷开 / 热开 / 筛选，后台保存时 query；记录硬件、Qt 和宿主版本 | filter p95≤100 ms，热开 p95≤200 ms，冷开≤1.5 s 目标；协议满足实际 timeout；无 GUI 长阻塞；工作集 / 包体实测，64 MB 限制按宿主实测判定 |
| T48 | F20,F01,F10 / 全部 / I,H / P0 | clean VM 上安装 QML 包，PATH 无 Qt；缺 DLL / QML 模块变体；旧数据升级；宿主权限确认、禁用和卸载 | 自带 runtime 正常加载；失败可诊断；ID / 数据沿用；用户明确启用后工作；授权失效如实拒绝；关闭无残留；Qt / 第三方许可材料随包核验 |

## 4. 需求追踪矩阵

| 功能 | 主要测试 | 功能 | 主要测试 |
|---|---|---|---|
| F01 | T01–T02、T48 | F11 | T26–T30、T32 |
| F02 | T03–T04 | F12 | T31–T32 |
| F03 | T04–T06 | F13 | T33–T34 |
| F04 | T07–T08 | F14 | T35–T38 |
| F05 | T09–T13 | F15 | T39–T40 |
| F06 | T14 | F16 | T41–T42 |
| F07 | T15–T17 | F17 | T43–T44 |
| F08 | T18–T20 | F18 | T45–T46 |
| F09 | T21–T23 | F19 | T47 |
| F10 | T24–T25、T48 | F20 | T48 |

## 5. 执行与证据格式

已有纯逻辑测试可在实现前建立基线；下面命令只构建测试目标，避免替换原生插件 exe：

```powershell
cmake -S . -B build
cmake --build build --config Release --target envhosts_tests editor_model_tests
ctest --test-dir build -C Release --output-on-failure
```

已有 QML 原型构建入口（Qt 路径按开发机实际配置，命令不是本轮执行结果）：

```powershell
cmake -S . -B build-qml -DENVHOSTS_QML_EDITOR=ON -DCMAKE_PREFIX_PATH="<Qt-x64-kit>"
cmake --build build-qml --config Release
```

实际测试合并在 `tests/test_qml_editor.cpp` 与 `tests/qml_protocol.py`，已注册 CTest，覆盖 session、写入服务、隔离存储与真实 QML 事件。
宿主安装 smoke 和完整人工变体仍待执行，实际命令与结果见实现记录。
现有 `tests/gui_preview.py` 通过 GDI HWND / WM_APP 消息驱动，不能直接当作 QML 测试。
现有 `tests/integration.py` 先完成目标隔离审计，再复用协议 fixture。

每个执行结果至少包含：

```text
testId + variant | requirementIds | sourceCommit | buildConfig | QtVersion
OS + hostVersion + DPI + locale + theme | fixtureSeed + SHA256
command | exitCode | expected | actual | before/after snapshot hashes
evidence paths (screenshot / protocol trace / fault-order trace) | PASS/FAIL/BLOCKED/NOT_RUN
```

截图用于验证可见布局，目标快照用于验证数据正确性；“按钮可点击”“进程没崩溃”均不足以证明保存正确。
快照只包含合成测试数据；故障 trace 不打印变量真值。Windows 10 / 11 x64、普通 / 模拟受限 / 实际授权宿主、
明暗和中英文至少覆盖一轮；DPI 与窗口尺寸按 T44 完整交叉。

## 6. 进入和退出条件

- 开始写入集成测试前，隔离断言通过；无法确认实际目标时该用例标 BLOCKED，不降级写真实目标。
- 每个里程碑保存基线和本次结果；任何 data-loss、权限绕过、假成功、字节破坏都阻塞下一阶段。
- 发布：所有 P0 / P1 通过；F01–F20 有证据；T47 报告预算结果，超限需先修复或单独规格变更；T48 必须真实安装验证。
- 未执行的人工 / 宿主检查显式列 NOT_RUN，不把本轮 SVG 评审记录算成通过。

## 7. 本轮离线原型检查（与产品验收分开）

`tools/check_qml_prototype.cjs` 使用 jsdom 驱动本地 HTML 事件、比较合成工作副本和目标，
不运行浏览器、不连接网络、不操作真实插件。R 编号不等同于 T 编号通过。

| 原型检查 | 内容 |
|---|---|
| R01 | 初始状态、禁用保存、无“应用到变量”、网络连接禁用策略 |
| R02 | PATH 自动暂存、保存取消、显式保存与读回演示结果 |
| R03 | 待删除项跨变量 / 页签保持，恢复后回到 Clean |
| R04 | stable item 移动、新增项删除、统一 undo / redo |
| R05 | hosts 关闭、撤销、重做、保存 |
| R06 | 备份预览、恢复可撤销、保存仅影响用户范围 |
| R07 | 外部变化不覆盖编辑，冲突覆盖需要新的保存审阅 |
| R08 | 保存失败保持工作副本，重试重新审阅 |
| R09 | 只读、筛选、无结果和主题切换保留编辑 |
| R10 | Esc 取消活动输入，Enter 暂存，整变量删除 / 恢复 |
| R11 | 重新载入取消 / 确认 |
| R12 | Clean 外变自动读取、Ctrl+S 打开审阅 |
| R13 | 未结束输入也显示 Dirty，Esc 后回到 Clean |

复现命令（Node.js，依赖只安装到已忽略的 build 目录）：

```powershell
npm --prefix build/qml-prototype-check install --ignore-scripts --no-audit --no-fund jsdom@26.1.0
node tools/check_qml_prototype.cjs
```

结果写到 `build/qml-prototype-check/results.json`。使用受控计时器和 dialog 测试替身，
只证明事件 / 状态行为；不覆盖真实浏览器模态焦点、布局、IME、DPI、可访问性和 QML 产品功能。

## 8. 原型到 Qt 的契约检查

[Qt 实现规范](QT_IMPLEMENTATION_STANDARD.md) 定义 G0 设计门槛与 M1–M5 实施证据，
[机器可读追踪表](qt-traceability.json) 映射 F → P / C / 组件 / T / R。执行：

```powershell
python tools/check_qml_contract.py
```

仅使用 Python 标准库；检查完整引用、现有 / 计划文件的准确状态、T 关联与本文件一致、双主题颜色对比、
本地文档资源和生成资产。它只读取仓库，不执行插件或写目标，不把任何产品 T 自动改为 PASS。
颜色检查不包含实际渲染、字体、透明叠加或焦点引擎；布局、IME 与可访问性仍按 Q / H 层验证。
未来新增产品源文件时应同步更新追踪表及其状态检查，不能通过把 PLANNED 写成已完成而跳过产品测试证据。

## 紧凑列表修复验收（repair-1）

追加 [UI01–UI14 定向测试](UI_REPAIR_PROPOSAL.md)，对应 F02 / F03 / F04 / F05 / F08 / F17 / F18。实际执行证据见实现记录，未执行变体不得升级为 PASS。

新增项回归追加 [UI15–UI16](UI_REPAIR_PROPOSAL.md)：颜色及选中叠加、插入位置、输入焦点、排序 / 筛选后的插入、hosts 真实序列化与撤销 / 草稿。Qt 实际用例：`insertionOrderAndUndo`、`insertionRealButtonsAndGreenState`。
