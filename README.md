# ra9fael.envhosts — 环境变量与 hosts 编辑器（Win顺 插件）

> **简体中文（默认）** · [English](README.en.md)
>
> Win顺 默认使用中文界面，也自带英文模式。因此中文说明放在 `README.md`，
> 英文说明放在 `README.en.md`，两份内容保持一致，可互相跳转。

直接在 Win顺 启动器里浏览、筛选、编辑**用户环境变量**和 **hosts 文件条目**。
每次写入前都会先备份；只有管理员才能改的目标会如实报错，而不是假装成功。

## 设计目的

- `env` —— 列出用户环境变量（可选含系统变量），按名称或值筛选，行内编辑，
  复制原始 / 展开后的值，二次确认删除，从备份恢复历史快照。
- `hosts` —— 列出 hosts 条目，筛选，切换注释，复制行，删除行（双击 Enter 确认），
  新增条目，恢复备份。
- **交互式编辑窗口** —— `env` 和 `hosts` 的第一条结果永远是
  "进入交互式编辑界面"（Enter 打开独立的 GDI 窗口）：全键盘操作、撤销 / 重做、
  冲突检测、草稿自动保存到 `WS_DATA_DIR/drafts/`。

## 安装

1. 从 [最新 Release](https://github.com/ra9fael/winshun-envhosts/releases/latest)
   下载 `ra9fael.envhosts-<version>.wsplugin`。可选：用旁边的 `.sha256` 文件核对
   SHA-256（`Get-FileHash <file> -Algorithm SHA256`）。
2. 在 Win顺 里打开 **设置 → 插件 → 已安装 → 添加插件 → 压缩包（.zip / .wsplugin）**，
   选中该文件。
3. 从本地安装包安装的插件**不会自动启用**。请在列表里打开开关；启用这一步就是你
   在确认它申请的权限（见 [权限说明](#权限说明)）。

**更新：** 用同样方式安装新版的 `.wsplugin`。Win顺 认出是同一个 id，会先询问是否
替换旧版本，并保留插件的数据目录。

**卸载：** 在 **设置 → 插件 → 已安装** 里移除。

本插件是原生 exe，不会出现在 Win顺 应用内的插件库里（社区索引只收脚本插件）。
本仓库的 Release 是唯一的分发渠道。

## 从源码构建

环境要求：Windows 10/11 x64、CMake 3.21+，以及 Visual Studio 2022 及以上（MSVC）
或 MinGW-w64 GCC 13+。打包和集成测试需要 Python 3.10+。

```powershell
cmake -S . -B build -A x64                 # MSVC；MinGW 用 -G "MinGW Makefiles"
cmake --build build --config Release       # 在 plugin.json 旁边生成 envhosts.exe
ctest --test-dir build -C Release --output-on-failure
python tools/package.py                    # dist/ra9fael.envhosts-<version>.wsplugin
```

本地构建如何直接在 Win顺 里运行，以及测试和发版流程，见
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 关键词与示例

| 输入 | 效果 |
|---|---|
| `env` / `hosts`（在**首行**按 Enter） | 打开交互式编辑窗口（永远是第一条结果） |
| `env` | 列出全部用户环境变量（启用后含系统变量） |
| `env path` | 筛选名称或值包含 `path` 的变量 |
| `env:set NAME=value` | `NAME` 的行内编辑行；Ctrl+Enter 保存 |
| `env:del` | 列出最近的环境变量备份以供恢复 |
| `hosts` | 列出 hosts 条目 |
| `hosts docker` | 筛选匹配 `docker` 的条目 |
| `hosts:add 127.0.0.1 my.dev` | 带确认的新增行 |
| `hosts:del` | 列出最近的 hosts 备份以供恢复 |

动作以键盘优先、按 key 排序：**Enter**（编辑 / 切换 / 确认）、
**Ctrl+Enter**（保存 / 复制）、**Shift+Enter**（复制展开值 / 用默认编辑器打开 hosts 文件）。

## 运行时依赖

- Windows 10/11、x64（编译期拒绝 32 位：否则 Wow64 文件系统重定向会让它改到
  `SysWOW64\drivers\etc\hosts`）。
- 以静态链接的 `envhosts.exe` 发布；不需要运行库。
- 常驻插件（`mode: resident`）、懒启动、`idleExitSec: 0` —— 一直驻留到 Win顺 退出，
  保证打开的编辑窗口不会被空闲回收杀掉。

## 权限说明

- `files:read` / `files:write`，作用于
  `%SystemRoot%\System32\drivers\etc\hosts`（对象形式）：读取 hosts 以列出条目，
  在切换 / 新增 / 删除 / 恢复时写入。
- `ui` —— 打开插件自己的编辑窗口所必需。
- `clipboard:write` —— 编辑窗口把值 / 行复制到剪贴板（Ctrl+C）。启动器列表里
  仍然是通过宿主复制，不走这个权限。
- **注册表访问在 Win顺 权限系统里没有对应名称。** 用户环境变量位于
  `HKCU\Environment`；读写直接调用 `RegSetValueExW`（绝不用 `setx`，它会在
  1024 字符处静默截断）。这里如实声明，而不是藏起来。
- 不申请 `processes`、`elevated`。仅管理员可改的目标在 v0.2 中保持只读并给出
  明确错误；`elevated` 留待以后。

## 可编辑范围

| 目标 | 读 | 写 |
|---|---|---|
| 用户环境变量（`HKCU\Environment`） | 是 | 是 |
| 系统环境变量（`HKLM\...\Session Manager\Environment`） | 可选 | **否** —— 删除 / 编辑都会给出明确拒绝信息 |
| hosts 文件 | 是 | 仅当文件可写（先探测，绝不盲写） |

环境变量写入会保留原有的值类型（`REG_SZ` / `REG_EXPAND_SZ` / `REG_MULTI_SZ`），
并广播 `WM_SETTINGCHANGE`，让新进程能读到改动。已经在运行的进程（包括 Win顺 自己）
**不会**立即看到变化，必须重启——这是 Windows 的限制，不是插件 bug。

## 数据位置

所有数据都留在本地 `WS_DATA_DIR` 下：

- `backups/env/<timestamp>.json` —— 每次环境变量写入 / 删除前的用户范围快照
  （保留数量由 `backupCount` 控制，默认 10）。
- `backups/hosts/<timestamp>.hosts` —— 每次 hosts 写入前的逐字节原文（保留数量同样
  由 `backupCount` 控制）。
- `drafts/env.json` / `drafts/hosts.json` —— 交互式编辑窗口里未保存的编辑，
  每 30 s 及关闭 / 退出时自动写入。重新打开编辑器时会自动载入草稿（作为未保存的
  修改，不会写回目标）。只有编辑器写草稿，这里不发生任何注册表 / hosts 写入。
- 不上传任何内容；无遥测；密钥和设置从不写日志。

## 失败处理

- hosts 只读（普通用户面对 `System32\drivers\etc\hosts`）：该行显示 🔒 标记，
  动作返回明确的 `E_DENIED` 错误行，并提供 Shift+Enter 的逃生口——用外部编辑器
  打开文件。不静默降级，也不假装成功。
- hosts 被记事本存成 UTF-16：返回 `E_HOSTS_UTF16`，而不是破坏编码。
- hosts 被其他进程占用：`E_HOSTS_BUSY`。
- hosts 写入采用**原地写入**（打开 + 截断 + 写入），以保留文件 ACL。这不是原子操作；
  写入中途崩溃可能留下不完整文件，恢复路径是
  `WS_DATA_DIR/backups/hosts/` 里的写前备份。
- 注册表写入失败会弹出"删除失败 / 保存失败"通知，操作记录到 stderr；
  写前备份可通过 `env:del` / `hosts:del` 恢复。
- `hostsPath` 设置支持 `%VAR%` 展开；文件不存在时给出明确的空状态。

### 交互式编辑窗口

- 窗口已在真实桌面上打开并逐项核对过渲染（分栏表格、表头、详情编辑框、
  状态栏、过滤与 Tab 切换均正常）。`CreateWindowExW` 失败时（例如无桌面会话或
  前台切换被拒），动作返回 `notify` + `keep`，你仍留在启动器里，不会崩溃。
- 窗口打开后，Windows 可能把它放到后台：启动器只承诺在
  menu / picture / showSettings / hotkey 时切到前台，不保证 `action` 也切；
  被挡住时用 Alt+Tab 找回来。
- 在按下 **Ctrl+S** 之前，编辑器不写任何东西：它会重新读取目标、比较内容指纹，
  若窗口打开期间底层 env / hosts 被改过，则中止并给出冲突提示。写前备份是恢复路径。
  未保存的编辑每 30 s 及关闭时刷入 `drafts/env.json` / `drafts/hosts.json`。
- 窗口运行在自己的 GUI 线程上；启动器对同一目标的写入会 `PostMessage` 通知重载
  （让打开的窗口反映外部改动）。两者由 `native::g_writeLock` 串行化。

#### 窗口内操作

- **列表滚动**：鼠标滚轮、右侧滚动条、`↑ ↓`、`PgUp / PgDn`、`Home / End`；
  状态栏显示「第 a–b / 共 N 行」，Tab 上带行数。窗口可拖动调整大小，列宽按比例自适应。
- **单项增删改**：`Ins` 新增一行（自动滚动到新行并清空冲突的过滤词）、
  `Del` 标记删除（再按一次恢复，未提交的新行则直接移除）、`Enter / F2` 进入详情编辑、
  `Esc` 从输入框回到表格、`Ctrl+Z / Ctrl+Y` 撤销 / 重做。详情面板右侧另有
  「+ 新增 / - 删除 / 撤销 / 重做」四个按钮，与键盘共用同一套命令。
- **上下分栏（仅 env 窗口）**：表格与详情面板之间有可拖动的分隔条，两栏高度按
  最小值（列表 112px / 详情 150px）互相挤占；窗口最小尺寸由 `WM_GETMINMAXINFO`
  保证两栏都放得下。hosts 窗口与备份视图保持原来的固定高度详情条。
- **分隔项编辑**：详情面板点「分隔项编辑」或按 `Ctrl+R`，把长值按分隔符拆成列表
  逐项增删改——`Ins` 在当前项后插入、`Del` 删除、`Alt+↑ / ↓` 上下移动、右侧输入框
  改当前项，每次改动都即时拼回该变量的值（仍算一次撤销）。`Path` 固定按 `;` 拆分，
  其他值点「分隔符」按钮在 `; , : TAB |` 之间切换；再按 `Ctrl+R` 回到原文编辑，
  值框显示重新拼接后的字符串。拆分只在用户显式进入，不会自动拆值，原文始终可达。
- **状态栏**：两行。第一行是修改状态、可见行范围、过滤词与读写权限，临时提示
  4 秒后自动消失；第二行是可点击的快捷键条（不可用的操作会灰显）。
- `Ctrl+S` 会先列出逐条变更（新增 / 修改 / 删除），确认后才写盘。

## 设置

| 键 | 类型 | 默认值 | 含义 |
|---|---|---|---|
| `hostsPath` | text | `%SystemRoot%\System32\drivers\etc\hosts` | hosts 文件位置 |
| `backupCount` | number | 10 | 每个目标保留的备份数 |
| `maxResults` | number | 50 | 每次查询的最大行数（上限 200） |
| `showSystemVars` | bool | true | 在 `env` 中包含只读的系统变量 |

## 测试

- `tests/test_envhosts_logic.cpp` —— 69 项不依赖操作系统的单元检查
  （脱敏的真实 hosts 样本逐字节往返、段落注释、env 模型、语法、校验、稳定 id）。
- `tests/test_editor_model.cpp` —— 41 项编辑器数据模型单元检查
  （脏标记、撤销 / 重做上限 200、校验、带引号段的 PATH 拆分 / 合并、
  多主机名 hosts 行、保留 BOM/CRLF 的逐字节保存、指纹敏感性、
  草稿序列化往返、备份加载为未保存状态）。
- `tests/integration.py` —— 在真实 Windows 上通过 JSON-RPC 驱动真实 exe：
  协议往返、临时注册表键上的编辑 / 删除 / 恢复、20 次查询的副作用审计
  （hosts 字节和注册表必须完全未变）、真实 System32 hosts 写入被拒、
  `settingsChanged` 传递、系统范围删除被拒，**以及编辑器套件**
  （P1/P2 固定首行、P3 openEditor 接线、P4 manifest 字段、P5 草稿位置、
  P6 外部改动路径）。

## 许可证

MIT，见 [LICENSE](LICENSE)。发布历史见 [CHANGELOG.md](CHANGELOG.md)。

---

简体中文（默认） · [English →](README.en.md)
