// SshMain.qml - SSH connections & keys window.
//
// The bridge (qt_ssh.h) owns parsing, validation, writing and ssh-keygen; this
// file only lays the window out and keeps the two string tables below in sync
// with the host's language and theme.
//
// Code and comments are English.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "controls"


ApplicationWindow {
    id: root

    component Btn: CommandButton {
        theme: tokens
    }
    component Field: CompactTextField {
        theme: tokens
    }
    component SshHeaderCell: Item {
        property string title: ""
        property real columnWidth: 100
        property bool fills: false
        signal resized(real delta)
        implicitWidth: columnWidth
        Layout.preferredWidth: columnWidth
        Layout.fillWidth: fills
        Layout.fillHeight: true
        Text {
            anchors.fill: parent
            anchors.leftMargin: 7
            anchors.rightMargin: 10
            verticalAlignment: Text.AlignVCenter
            text: parent.title
            color: root.dimColor
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        MouseArea {
            anchors.right: parent.right
            width: 9
            height: parent.height
            cursorShape: Qt.SizeHorCursor
            property real originX: 0
            property real originWidth: 0
            onPressed: {
                originX = mapToItem(parent, mouse.x, mouse.y).x;
                originWidth = parent.columnWidth;
            }
            onPositionChanged: if (pressed)
                parent.resized(mapToItem(parent, mouse.x, mouse.y).x - originX)
        }
        Rectangle {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: parent.height - 10
            color: root.ruleColor
        }
    }


    property var bridge: null

    AppTheme {
        id: tokens
        appearance: bridge ? bridge.appearance : ({})
        fallbackDark: bridge ? bridge.dark : false
    }
    readonly property bool dark: tokens.dark
    readonly property color bgColor: tokens.bg
    readonly property color panelColor: tokens.panel
    readonly property color panelAlt: tokens.field
    readonly property color inkColor: tokens.ink
    readonly property color dimColor: tokens.muted
    readonly property color ruleColor: tokens.line
    readonly property color accentColor: tokens.accent
    readonly property color accentSoft: tokens.soft
    readonly property color hoverColor: tokens.soft
    readonly property color dangerColor: tokens.red
    readonly property color dangerSoft: tokens.redbg
    readonly property color warnColor: tokens.amber
    readonly property color warnSoft: tokens.amberbg
    readonly property color okColor: tokens.green
    readonly property color okSoft: tokens.greenbg

    readonly property bool narrow: width < 880
    readonly property int selectionCount: tab === "conn" ? selectedConns.length
                                                : tab === "keys" ? selectedKeys.length
                                                : tab === "agent" ? selectedAgent.length : selectedHosts.length
    readonly property bool detailHasSelection: selectionCount > 0
    readonly property int selectionColumnWidth: tokens.selectionWidth
    readonly property string monoFont: "Cascadia Mono"
    readonly property var str: bridge && bridge.zh ? zh : en

    readonly property bool dialogOpen: saveDlg.visible || conflictDlg.visible || deleteDlg.visible
                                       || keygenDlg.visible || infoDlg.visible || closeDlg.visible
                                       || keyDeleteDlg.visible || knownDeleteDlg.visible
                                       || agentPicker.visible || passphraseDlg.visible

    property string tab: "conn"
    property string connFilter: ""
    property string keyFilter: ""
    property string knownFilter: ""
    property string agentFilter: ""
    property string toastText: ""
    property bool toastBad: false
    property bool closePending: false
    property bool forceClose: false
    property bool connNoticeClosed: false
    property int menuConnRow: -1
    property var selectedConns: []
    property var selectedKeys: []
    property var selectedHosts: []
    property var selectedAgent: []
    property string agentAddName: ""
    property var connWidths: ({name: 190, user: 88, port: 48, key: 176})
    property var keyWidths: ({file: 190, type: 80, fingerprint: 180, state: 100})
    property real connMenuX: 0
    property real connMenuY: 0
    property int rangeAnchor: -1

    function resizeSshColumn(table, key, delta, minimum, maximum) {
        const source = table === "conn" ? connWidths : keyWidths;
        const next = {};
        for (const name in source)
            next[name] = source[name];
        if (delta === 0) {
            next[key] = minimum;
        } else {
            next[key] = Math.max(minimum, Math.min(maximum, source[key] + delta));
        }
        if (table === "conn")
            connWidths = next;
        else
            keyWidths = next;
    }

    width: tokens.windowWidth
    height: tokens.windowHeight
    minimumWidth: tokens.minimumWidth
    minimumHeight: tokens.minimumHeight
    visible: false
    title: bridge ? bridge.title : "SSH"
    color: bgColor
    objectName: "sshWindow"
    font.family: tokens.uiFontFamily.length > 0 ? tokens.uiFontFamily : Qt.application.font.family
    font.pixelSize: tokens.bodyFontSize
    palette.window: tokens.bg
    palette.windowText: tokens.ink
    palette.base: tokens.field
    palette.text: tokens.ink
    palette.button: tokens.panel
    palette.buttonText: tokens.ink
    palette.highlight: tokens.accentFill
    palette.highlightedText: tokens.onAccent
    palette.placeholderText: tokens.muted
    palette.mid: tokens.control
    palette.dark: tokens.control
    palette.light: tokens.panel
    palette.brightText: tokens.ink

    // ---- strings -------------------------------------------------------------

    readonly property var zh: ({
        tabConn: "连接", tabKeys: "密钥", tabAgent: "代理", tabKnown: "已知主机",
        filterConn: "筛选连接（名称 / 主机 / 用户）", filterKeys: "筛选密钥（文件 / 类型 / 注释）", filterKnown: "筛选已知主机（主机 / 算法 / 指纹）",
        add: "新增连接", gen: "生成密钥", save: "保存",
        undo: "撤销", redo: "重做", info: "说明",
        colName: "名称", colHost: "主机", colUser: "用户", colPort: "端口", colKey: "密钥",
        colFile: "文件", colType: "类型", colComment: "注释", colFp: "指纹", colState: "状态",
        noMatchConn: "没有匹配的连接", noMatchKeys: "没有匹配的密钥",
        noKeys: "目录中没有检测到密钥",
        emptyTitle: "尚未找到 SSH 配置文件",
        emptyBody: p => "插件会在 " + p + " 管理连接。现在可以新建一个只含注释的模板，之后按需添加连接。",
        create: "新建配置",
        lblHost: "Host（别名）", hHost: "例如 work",
        hHostName: "例如 git.example.com", hUser: "例如 git", hPort: "22",
        hIdentity: "~/.ssh/id_ed25519", hProxyJump: "例如 bastion", hintIdentity: "输入时列出已检测到的密钥文件",
        hintProxyJump: "输入时列出其他连接别名",
        lblExtra: "附加参数", extrasEmpty: "",
        errName: "别名必填，且不能含空格或 * ?", errHostName: "主机名必填", errPort: "端口需为 1–65535",
        errReadOnly: "该块只读，不能删除", errLocked: "该块只读，保存在表单中不可用",
        deletedNote: "已标记删除，保存后移除；再按一次删除键可恢复。",
        lockedNote: "以下块在本版本只读：保存时按原样保留，不在表单中展开。",
         segForm: "表单", segPreview: "预览", selectItem: name => "选择 " + name,
        previewLegend: "保存时将写入的内容；背景高亮对应列表中的选中连接，● 标记本次变更。",
        previewEmpty: "文件为空。",
        keyPath: "路径", keyType: "类型", keyFp: "指纹（SHA256）", keyUsed: "关联连接",
         keyPub: "公钥",
         keyNoPub: "缺少对应的公钥文件，无法复制公钥。",
        keyNoneUsed: "（未被任何连接使用）",
        selectHint: "选择一项查看详情",
        deleteSelected: "删除选中项",
         selectedCount: n => "已选 " + n + " 项",
         selectAll: "全选", clearSelection: "取消选择", readonly: "只读",
         connNotice: "Windows Terminal 使用当前 SSH config 和 alias；ssh:// 由系统关联的程序打开，是否复用 alias 设置取决于该程序。",
         connPreviewNote: "预览只读。外部工具动作请从连接行右侧 ⋯ 菜单进入。",
         editConn: "编辑连接", copySshCommand: "复制 SSH 命令", openTerminal: "用 Windows Terminal 打开",
         connectSshUrl: "使用 ssh:// 连接",
         toastSshCommand: "已复制 SSH 命令", toastTerminal: "已在 Windows Terminal 中打开",
         toastSshUrl: "已交给 ssh:// 关联程序",
         btnTest: "测试连接", btnConnect: "连接",
         testRunning: a => "正在测试 " + a + " …",
         testOk: (a, ms) => a + " 连接成功 · " + ms + " ms",
         testFail: (a, d) => a + " 连接失败 · " + d,
         testCancel: "已取消测试",
         noConnection: "未选择连接",
        keyDeleteTitle: "删除密钥？",
        keyDelete: "删除密钥",
        keyDeleteBody: files => "将永久删除以下文件，无法撤销：\n" + files.join("\n"),
        keyDeleteSuccess: files => "已删除：" + files.join("、"),
        knownHostWarning: "新指纹不会自动信任。删除旧记录前，请通过可信渠道核对服务器的新指纹。",
        knownHostEmpty: "没有可显示的已知主机记录。",
        knownHostConflict: "known_hosts 在读取后被外部修改，请重新选择记录。",
        toastKnownDeleted: b => "已移除旧主机记录 · 备份 " + b,
        keyStateBoth: "私钥 + 公钥", keyStatePub: "仅公钥", keyStatePriv: "仅私钥（缺公钥）",
        keyBits: n => n + " 位",
        copyPub: "复制公钥", copyRow: "复制整行",
        statusDirty: n => "● " + n + " 项未保存", statusClean: "已保存", statusSave: "Ctrl+S 保存",
        statusCount: n => "共 " + n + " 个连接",
        statusKeys: n => "共 " + n + " 个密钥",
        statusAgent: n => "代理中有 " + n + " 个密钥",
        agentEmpty: "当前代理没有已加载的密钥。",
        agentAdd: "加载密钥", agentRemove: "移除选中", agentClear: "清空代理", agentRefresh: "刷新",
        agentPick: "选择要加载的私钥", agentLoad: "加载", agentLoading: "正在加载…", agentBrowse: "浏览…", agentNoKeys: "没有可加载的私钥文件。", filterAgent: "筛选代理密钥（指纹 / 注释 / 类型）", agentFingerprint: "指纹", agentComment: "注释", agentSource: "来源",
        agentHint: "代理来源：" + (bridge ? bridge.agentSourceLabel : "auto") + "。只操作当前用户的代理；私钥正文和 passphrase 不会进入插件。",
        addToAgent: "添加到代理", agentLoaded: "密钥已添加到代理",
        bannerKeygen: "未检测到 ssh-keygen：生成密钥不可用。浏览、复制与编辑配置不受影响。",
        bannerLocked: "检测到 Include / Match / 通配符块：这些块只读，保存时原样保留。",
        bannerCreated: "已新建配置文件，保存后写入磁盘。",
        saveTitle: "变更列表",
        saveTarget: (p, b) => "将写入 " + p + "；写入前自动备份到 " + b + "。",
        saveNote: "未编辑的连接、注释与空行按原样保留。",
        changeAdd: "新增", changeMod: "修改", changeDel: "删除",
        btnSave: "保存", btnDiscard: "不保存", btnCancel: "取消", btnDelete: "删除",
        btnReload: "重新载入", btnForce: "仍然保存", btnCreate: "新建配置", btnGenerate: "生成",
        saveInvalid: "有字段不合法，保存已禁用",
        conflictTitle: "保存被中止",
        conflictWhy: "文件在窗口打开后已被外部修改。以下是磁盘内容与窗口内容的差异：",
        conflictNote: "「仍然保存」会用窗口内容覆盖外部修改；「重新载入」丢弃本次未保存的编辑。",
        delTitle: "删除连接？",
        delBody: a => "将从配置中移除「" + a + "」及其全部参数。写入前会自动备份。",
        delNote: "在保存前可按 Ctrl+Z 撤销。",
        keygenTitle: "生成密钥",
        keygenUnavailable: "未检测到 ssh-keygen。请先启用 Windows 的「OpenSSH 客户端」功能；浏览、复制与编辑配置不受影响。",
        kgName: "文件名", kgType: "类型", kgComment: "注释", kgPass: "口令", kgPass2: "确认口令",
        kgNameHint: d => "相对于 " + d + "；只填文件名，不要带路径。",
        kgCommentHint: "会写入公钥末尾，便于识别用途",
        kgPassHint: "留空表示无口令保护",
        kgErrName: "文件名必填，且不能包含 \\ / : * ? \" < > |",
        kgErrPass: "两次输入的口令不一致",
        kgErrPrefix: "请先修正：",
        kgErrExists: f => f + " 已存在。本插件不覆盖已有密钥文件，请换个文件名。",
        infoTitle: "关于 SSH 连接与密钥",
        infoLines: [
            "管理 SSH 客户端：~/.ssh/config 里的连接，以及 ~/.ssh 下的密钥对。",
            "不管理 Windows 的 SSH 服务；sshd 服务与 sshd_config 不在本界面范围内。",
            "私钥内容从不显示、从不复制、从不写入日志；界面只展示公钥与指纹。",
            "保存前自动备份到 Win顺 数据目录的 backups/ssh/。",
            "生成密钥调用本机 OpenSSH 的 ssh-keygen；未安装时仅该功能不可用。",
            "需要 %USERPROFILE%\\.ssh 的文件读写权限，不申请管理员权限。",
        ],
        infoClose: "关闭",
        toastSaved: b => b ? "已保存 · 备份 " + b : "已保存",
        toastCopyPub: "已复制公钥", toastCopyRow: "已复制整行",
        toastKeygen: f => "已生成密钥 " + f,
        toastCreated: "已新建配置", toastDeleted: "已标记删除，Ctrl+Z 可撤销", toastReloaded: "已重新载入",
        closeTitle: "关闭窗口？", closeBody: n => "有 " + n + " 项未保存的修改。保存后再关闭，或直接丢弃。",
        closeDiscard: "丢弃并关闭",
        closeAfterSave: "保存并关闭",
    })

    readonly property var en: ({
        tabConn: "Connections", tabKeys: "Keys", tabAgent: "Agent", tabKnown: "Known hosts",
        filterConn: "Filter connections (name / host / user)", filterKeys: "Filter keys (file / type / comment)", filterKnown: "Filter known hosts (host / type / fingerprint)",
        add: "New connection", gen: "Generate key", save: "Save",
        undo: "Undo", redo: "Redo", info: "About",
        colName: "Name", colHost: "Host", colUser: "User", colPort: "Port", colKey: "Key",
        colFile: "File", colType: "Type", colComment: "Comment", colFp: "Fingerprint", colState: "State",
        noMatchConn: "No matching connection", noMatchKeys: "No matching key",
        noKeys: "No keys found in this folder",
        emptyTitle: "No SSH config file yet",
        emptyBody: p => "The plugin manages connections in " + p + ". Create a commented template now and add connections as you go.",
        create: "Create config",
        lblHost: "Host (alias)", hHost: "e.g. work",
        hHostName: "e.g. git.example.com", hUser: "e.g. git", hPort: "22",
        hIdentity: "~/.ssh/id_ed25519", hProxyJump: "e.g. bastion", hintIdentity: "Lists the key files already detected",
        hintProxyJump: "Lists the other connection aliases",
        lblExtra: "Extra directives", extrasEmpty: "",
        errName: "Alias is required, no spaces or * ?", errHostName: "Host name is required",
        errPort: "Port must be 1–65535",
        errReadOnly: "This block is read-only and cannot be deleted", errLocked: "This block is read-only",
        deletedNote: "Marked for deletion: removed on save. Press Delete again to restore.",
        lockedNote: "This block is read-only in this version: it is written back verbatim and never expanded into the form.",
          segForm: "Form", segPreview: "Preview", selectItem: name => "Select " + name,
        previewLegend: "Exactly what will be written; highlighted blocks are selected in the list, and ● marks changed lines.",
        previewEmpty: "The file is empty.",
        keyPath: "Path", keyType: "Type", keyFp: "Fingerprint (SHA256)", keyUsed: "Used by",
         keyPub: "Public key",
         keyNoPub: "No matching public key file: there is nothing to copy.",
        keyNoneUsed: "(not used by any connection)",
        selectHint: "Select an item to see its details",
        deleteSelected: "Delete selected",
         selectedCount: n => n + " selected",
         selectAll: "Select all", clearSelection: "Clear selection", readonly: "Read-only",
         connNotice: "Windows Terminal uses the current SSH config and alias; ssh:// opens in whichever app is registered for it, and alias-specific settings apply only if that app reads the config.",
         connPreviewNote: "Read-only preview. Use the ⋯ menu at the end of a connection row for external tools.",
         editConn: "Edit connection", copySshCommand: "Copy SSH command", openTerminal: "Open in Windows Terminal",
         connectSshUrl: "Connect via ssh://",
         toastSshCommand: "SSH command copied", toastTerminal: "Opened in Windows Terminal",
         toastSshUrl: "Handed to the ssh:// handler",
         btnTest: "Test connection", btnConnect: "Connect",
         testRunning: a => "Testing " + a + " …",
         testOk: (a, ms) => a + " connected · " + ms + " ms",
         testFail: (a, d) => a + " failed · " + d,
         testCancel: "Test cancelled",
         noConnection: "No connection selected",
        keyDeleteTitle: "Delete keys?",
        keyDelete: "Delete keys",
        keyDeleteBody: files => "These files are deleted permanently and cannot be restored:\n" + files.join("\n"),
        keyDeleteSuccess: files => "Deleted: " + files.join(", "),
        knownHostWarning: "New fingerprints are never trusted automatically. Verify the server's new fingerprint through a trusted channel before removing an old record.",
        knownHostEmpty: "No known-host records to show.",
        knownHostConflict: "known_hosts changed on disk. Reload the list and select the records again.",
        toastKnownDeleted: b => "Old host records removed · backup " + b,
        keyStateBoth: "Private + public", keyStatePub: "Public only", keyStatePriv: "Private only (no public key)",
        keyBits: n => n + " bit",
        copyPub: "Copy public key", copyRow: "Copy line",
        statusDirty: n => "● " + n + " unsaved", statusClean: "Saved", statusSave: "Ctrl+S saves",
        statusCount: n => n + " connection(s)",
        statusKeys: n => n + " key(s)",
        statusAgent: n => n + " key(s) in agent",
        agentEmpty: "No keys are loaded in the current agent.",
        agentAdd: "Load key", agentRemove: "Remove selected", agentClear: "Clear agent", agentRefresh: "Refresh",
        agentPick: "Choose a private key", agentLoad: "Load", agentLoading: "Loading…", agentBrowse: "Browse…", agentNoKeys: "No private key files to load.", filterAgent: "Filter agent keys (fingerprint / comment / type)", agentFingerprint: "Fingerprint", agentComment: "Comment", agentSource: "Source",
        addToAgent: "Add to agent", agentLoaded: "Key added to agent",
        agentHint: "Agent source: " + (bridge ? bridge.agentSourceLabel : "auto") + ". Only the selected agent is affected; private-key text and passphrases never enter the plugin.",
        bannerKeygen: "ssh-keygen was not found: key generation is unavailable. Browsing, copying and editing the config are unaffected.",
        bannerLocked: "Include / Match / wildcard blocks found: they are read-only and written back verbatim.",
        bannerCreated: "Config created in memory; it is written on the next save.",
        saveTitle: "Change list",
        saveTarget: (p, b) => "Writes to " + p + "; a backup goes to " + b + " first.",
        saveNote: "Untouched connections, comments and blank lines are preserved byte for byte.",
        changeAdd: "Add", changeMod: "Modify", changeDel: "Delete",
        btnSave: "Save", btnDiscard: "Discard", btnCancel: "Cancel", btnDelete: "Delete",
        btnReload: "Reload", btnForce: "Save anyway", btnCreate: "Create config", btnGenerate: "Generate",
        saveInvalid: "A field is invalid, saving is disabled",
        conflictTitle: "Save aborted",
        conflictWhy: "The file changed on disk after this window opened. Difference between disk and window:",
        conflictNote: "\"Save anyway\" overwrites the external change; \"Reload\" discards the unsaved edits.",
        delTitle: "Delete connection?",
        delBody: a => "Removes \"" + a + "\" and all of its directives from the config. A backup is written first.",
        delNote: "Ctrl+Z undoes this until you save.",
        keygenTitle: "Generate key",
        keygenUnavailable: "ssh-keygen was not found. Enable the Windows \"OpenSSH Client\" feature first; browsing, copying and editing the config are unaffected.",
        kgName: "File name", kgType: "Type", kgComment: "Comment", kgPass: "Passphrase", kgPass2: "Confirm passphrase",
        kgNameHint: d => "Relative to " + d + "; a file name only, no path.",
        kgCommentHint: "Appended to the public key so you can tell keys apart",
        kgPassHint: "Empty means no passphrase",
        kgErrName: "File name is required and cannot contain \\ / : * ? \" < > |",
        kgErrPass: "The two passphrases do not match",
        kgErrPrefix: "Fix these first:",
        kgErrExists: f => f + " already exists. This plugin never overwrites a key file; pick another name.",
        infoTitle: "About SSH connections & keys",
        infoLines: [
            "Manages the SSH client: connections in ~/.ssh/config and the key pairs under ~/.ssh.",
            "Does not manage the Windows SSH service; sshd and sshd_config are out of scope.",
            "Private key content is never shown, copied or logged; only public keys and fingerprints appear here.",
            "Every save backs up first to backups/ssh/ in the WinShun data directory.",
            "Key generation calls the local OpenSSH ssh-keygen; without it only that action is unavailable.",
            "Needs read/write access to %USERPROFILE%\\.ssh; no administrator permission is requested.",
        ],
        infoClose: "Close",
        toastSaved: b => b ? "Saved · backup " + b : "Saved",
        toastCopyPub: "Public key copied", toastCopyRow: "Line copied",
        toastKeygen: f => "Generated key " + f,
        toastCreated: "Config created", toastDeleted: "Marked deleted, Ctrl+Z undoes it", toastReloaded: "Reloaded",
        closeTitle: "Close the window?", closeBody: n => n + " unsaved change(s). Save first, or discard them.",
        closeDiscard: "Discard and close",
        closeAfterSave: "Save and close",
    })

    // ---- helpers -------------------------------------------------------------

    function fieldError(code) {
        if (!code)
            return "";
        if (code === "E_ALIAS")
            return str.errName;
        if (code === "E_HOSTNAME")
            return str.errHostName;
        if (code === "E_PORT")
            return str.errPort;
        return code;
    }

    function stateText(s) {
        if (s === "both")
            return str.keyStateBoth;
        if (s === "public")
            return str.keyStatePub;
        return str.keyStatePriv;
    }

    function kindText(k) {
        if (k === "add")
            return str.changeAdd;
        if (k === "delete")
            return str.changeDel;
        return str.changeMod;
    }

    function showToast(text, bad) {
        toastText = text;
        toastBad = bad === true;
        toastTimer.restart();
    }

    function visibleRowCount() {
        if (tab === "conn") return connList.count;
        if (tab === "keys") return keyList.count;
        if (tab === "agent") return agentList.count;
        return knownList.count;
    }

    function selectedRows(kind) {
        return kind === "conn" ? selectedConns
             : kind === "keys" ? selectedKeys
             : kind === "agent" ? selectedAgent : selectedHosts;
    }

    function setSelectedRows(kind, rows) {
        if (kind === "conn") {
            selectedConns = rows;
            if (bridge) bridge.setConnSelection(rows);
            if (rows.length === 1)
                syncForm();
        } else if (kind === "keys") {
            selectedKeys = rows;
            if (bridge) bridge.setKeySelection(rows);
        } else if (kind === "agent") {
            selectedAgent = rows;
        } else {
            selectedHosts = rows;
        }
    }

    function toggleMultiSelection(kind, index) {
        const rows = selectedRows(kind).slice();
        const at = rows.indexOf(index);
        if (at >= 0)
            rows.splice(at, 1);
        else
            rows.push(index);
        setSelectedRows(kind, rows);
        if (kind === "conn" && bridge) bridge.selectConn(index);
        else if (kind === "keys" && bridge) bridge.selectKey(index);
        rangeAnchor = index;
    }

    function selectAllVisible() {
        const rows = [];
        for (let i = 0; i < visibleRowCount(); ++i)
            rows.push(i);
        setSelectedRows(tab, rows);
    }

    function clearCurrentSelection() {
        setSelectedRows(tab, []);
    }

    function syncForm() {
        if (!bridge)
            return;
        const d = bridge.connDetail;
        fAlias.text = d.alias || "";
        fHostname.text = d.hostname || "";
        fUser.text = d.user || "";
        fPort.text = d.port || "";
        fIdentity.text = d.identity || "";
        fProxyJump.text = d.proxyjump || "";
    }

    function syncFilterField() {
        const want = bridge ? bridge.filter : "";
        if (filterField.text !== want)
            filterField.text = want;
    }

    function switchTab(t) {
        setTab(t)
    }

    function setTab(t) {
        if (tab === t)
            return;
        if (tab === "conn")
            connFilter = filterField.text;
        else if (tab === "keys")
            keyFilter = filterField.text;
        else if (tab === "known")
            knownFilter = filterField.text;
        else
            agentFilter = filterField.text;
        tab = t;
        rangeAnchor = -1;
        if (bridge)
            bridge.filter = t === "conn" ? connFilter : t === "keys" ? keyFilter : "";
        if (bridge && t === "known")
            bridge.knownFilter = knownFilter;
        syncFilterField();
        if (t === "conn")
            connList.forceActiveFocus();
        else if (t === "keys")
            keyList.forceActiveFocus();
        else if (t === "agent")
            agentList.forceActiveFocus();
    }

    function openConnMenu(index, x, y) {
        menuConnRow = index;
        connMenuX = Math.max(8, Math.min(root.width - 220, x));
        connMenuY = Math.max(8, Math.min(root.height - 260, y));
        updateSelection("conn", index, Qt.NoModifier);
        connMenu.popup(root, Qt.point(connMenuX, connMenuY));
    }

    function editConnection() {
        if (menuConnRow < 0)
            return;
        if (bridge)
            bridge.selectConn(menuConnRow);
        syncForm();
        connMenu.close();
    }

    function externalConnectionAction(action) {
        if (!bridge || menuConnRow < 0)
            return;
        bridge.selectConn(menuConnRow);
        bridge.setConnSelection([menuConnRow]);
        const result = action === "copy" ? bridge.copySshCommand()
                     : action === "terminal" ? bridge.openWindowsTerminal()
                     : bridge.connectSshUrl();
        if (result.code === "") {
            if (action === "copy")
                showToast(str.toastSshCommand, false);
            else if (action === "terminal")
                showToast(str.toastTerminal, false);
            else
                showToast(str.toastSshUrl, false);
        } else if (result.code === "unsupported") {
            showToast(str.errReadOnly, true);
        } else {
            showToast(result.detail || result.code, true);
        }
        connMenu.close();
    }

    function openSelectedConnection() {
        if (!bridge || selectedConns.length !== 1)
            return;
        const result = bridge.openWindowsTerminal();
        if (result.code === "")
            showToast(str.toastTerminal, false);
        else if (result.code === "unsupported")
            showToast(str.errReadOnly, true);
        else
            showToast(result.detail || result.code, true);
    }

    function connectionTestText() {
        const t = bridge ? bridge.connectionTest : null;
        if (!t || !t.state || t.state === "idle")
            return "";
        if (t.state === "running")
            return str.testRunning(t.alias || "");
        if (t.state === "ok")
            return str.testOk(t.alias || "", t.ms || 0);
        return str.testFail(t.alias || "", t.detail || "");
    }

    function updateSelection(kind, index, modifiers) {
        let selected = kind === "conn" ? selectedConns.slice()
                      : kind === "keys" ? selectedKeys.slice()
                      : kind === "agent" ? selectedAgent.slice() : selectedHosts.slice();
        if (modifiers & Qt.ShiftModifier && rangeAnchor >= 0) {
            const low = Math.min(rangeAnchor, index);
            const high = Math.max(rangeAnchor, index);
            selected = [];
            for (let i = low; i <= high; ++i)
                selected.push(i);
        } else if (modifiers & Qt.ControlModifier) {
            const at = selected.indexOf(index);
            if (at >= 0)
                selected.splice(at, 1);
            else
                selected.push(index);
            rangeAnchor = index;
        } else {
            selected = [index];
            rangeAnchor = index;
        }
        if (kind === "conn") {
            setSelectedRows(kind, selected);
            bridge.selectConn(index);
        } else if (kind === "keys") {
            setSelectedRows(kind, selected);
            bridge.selectKey(index);
        } else if (kind === "agent") {
            setSelectedRows(kind, selected);
        } else {
            setSelectedRows(kind, selected);
        }
    }

    function agentRows() {
        if (!bridge)
            return [];
        const needle = agentFilter.trim().toLowerCase();
        const rows = bridge.agentKeys.map(function (row, index) {
            const source = row.sourcePath || row.source;
            const score = root.fuzzyScore(row.fingerprint + " " + row.comment + " " + row.type + " " + source, needle);
            return { row: row, index: index, score: score };
        });
        return rows.filter(function (item) { return item.score > -1000000; })
                    .sort(function (a, b) { return b.score - a.score || a.index - b.index; })
                    .map(function (item) { return item.row; });
    }

    function fuzzyScore(text, query) {
        const hay = String(text).toLowerCase();
        const needle = String(query).toLowerCase();
        if (!needle.length)
            return 0;
        let pos = 0;
        let previous = -2;
        let score = 0;
        for (let i = 0; i < needle.length; ++i) {
            const found = hay.indexOf(needle[i], pos);
            if (found < 0)
                return -1000000;
            const boundary = found === 0 || !/[a-z0-9_]/.test(hay[found - 1]);
            score += 10 + (boundary ? 12 : 0) + (found === previous + 1 ? 8 : 0) - (found - pos);
            previous = found;
            pos = found + 1;
        }
        return score - (hay.length - needle.length);
    }

    function addAgentKey() {
        if (!bridge || agentAddName === "")
            return;
        const result = bridge.addAgentKey(agentAddName);
        agentAddName = "";
        agentPicker.close();
        if (result.code !== "")
            showToast(result.detail || result.code, true);
    }

    function addSelectedKeyToAgent() {
        if (!bridge || selectedKeys.length !== 1)
            return;
        const key = bridge.keyDetail || ({ });
        if (!key.name || key.state === "public")
            return;
        const result = bridge.addAgentKey(key.name);
        if (result.code !== "")
            showToast(result.detail || result.code, true);
    }

    function pickExternalAgentKey() {
        if (!bridge)
            return;
        const result = bridge.browseAgentKey();
        if (result.code !== undefined && result.code !== "")
            showToast(result.detail || result.code, true);
    }

    function removeAgent() {
        const rows = agentRows();
        const fingerprints = [];
        selectedAgent.forEach(function (index) {
            if (rows[index])
                fingerprints.push(rows[index].fingerprint);
        });
        const result = bridge.removeAgentKeys(fingerprints);
        selectedAgent = [];
        if (result.code !== "")
            showToast(result.detail || result.code, true);
    }

    function refreshAgent() {
        const result = bridge.refreshAgent();
        if (result.code !== "")
            showToast(result.detail || result.code, true);
    }

    function deleteConnections() {
        if (!bridge || selectedConns.length === 0)
            return;
        if (selectedConns.length === 1) {
            askDelete();
            return;
        }
        deleteDlg.alias = str.selectedCount(selectedConns.length);
        deleteDlg.batch = true;
        deleteDlg.open();
    }

    function deleteKeys() {
        if (!bridge || selectedKeys.length === 0)
            return;
        const names = bridge.keyFiles(selectedKeys);
        if (names.length === 0)
            return;
        keyDeleteDlg.files = names;
        keyDeleteDlg.open();
    }

    function deleteKnownHosts() {
        if (!bridge || selectedHosts.length === 0)
            return;
        const result = bridge.deleteKnownHosts(selectedHosts);
        if (result.code === "") {
            selectedHosts = [];
            showToast(str.toastKnownDeleted(result.backup), false);
        } else {
            showToast(result.code === "conflict" ? str.knownHostConflict : (result.detail || result.code), true);
        }
    }

    function requestSave() {
        if (!bridge)
            return;
        if (bridge.anyInvalid) {
            showToast(str.saveInvalid, true);
            return;
        }
        if (!bridge.dirty) {
            showToast(str.statusClean, false);
            return;
        }
        saveDlg.open();
    }

    function doSave() {
        if (!bridge)
            return;
        const r = bridge.save();
        if (r.code === "") {
            saveDlg.close();
            showToast(str.toastSaved(r.id), false);
            if (closePending) {
                closePending = false;
                forceClose = true;
                root.close();
            }
        } else if (r.code === "conflict") {
            saveDlg.close();
            conflictDlg.open();
        } else if (r.code === "invalid") {
            showToast(str.saveInvalid, true);
        } else {
            showToast(r.detail || r.code, true);
        }
    }

    function doForceSave() {
        if (!bridge)
            return;
        const r = bridge.saveForce();
        if (r.code === "") {
            conflictDlg.close();
            showToast(str.toastSaved(r.id), false);
            if (closePending) {
                closePending = false;
                forceClose = true;
                root.close();
            }
        } else if (r.code === "conflict") {
            showToast(str.conflictTitle, true);
        } else {
            showToast(r.detail || r.code, true);
        }
    }

    function doReload() {
        if (!bridge)
            return;
        const r = bridge.reloadFromDisk();
        saveDlg.close();
        conflictDlg.close();
        if (r.code === "") {
            syncForm();
            showToast(str.toastReloaded, false);
            if (closePending) {
                closePending = false;
                forceClose = true;
                root.close();
            }
        } else {
            showToast(r.detail || r.code, true);
        }
    }

    function addConnection() {
        if (!bridge)
            return;
        const row = bridge.addConnection();
        if (row >= 0) {
            syncForm();
            fAlias.forceActiveFocus();
        }
    }

    function askDelete() {
        if (!bridge)
            return;
        const d = bridge.connDetail;
        if (!d || d.conn === undefined)
            return;
        if (d.locked) {
            showToast(str.errReadOnly, true);
            return;
        }
        if (d.deleted || d.isNew) {
            bridge.toggleDelete();
            return;
        }
        deleteDlg.batch = false;
        deleteDlg.alias = d.name || "";
        deleteDlg.open();
    }

    function openKeygen() {
        if (!bridge)
            return;
        if (!bridge.keygenAvailable) {
            showToast(str.keygenUnavailable, true);
            return;
        }
        kgName.text = "";
        kgComment.text = "";
        kgPass.text = "";
        kgPass2.text = "";
        kgType.currentIndex = 0;
        kgErrors.text = "";
        keygenDlg.open();
    }

    function generateKey() {
        if (!bridge)
            return;
        const name = kgName.text.trim();
        const errs = [];
        if (!name || /[\\/:*?"<>|\u0000-\u001f]/.test(name))
            errs.push(str.kgErrName);
        if (kgPass.text !== kgPass2.text)
            errs.push(str.kgErrPass);
        if (errs.length) {
            kgErrors.text = str.kgErrPrefix + "\n• " + errs.join("\n• ");
            return;
        }
        const r = bridge.generateKey(name, kgType.currentValue, kgComment.text.trim(), kgPass.text);
        if (r.code === "") {
            keygenDlg.close();
            showToast(str.toastKeygen(r.detail), false);
            return;
        }
        if (r.code === "E_KEYEXISTS")
            kgErrors.text = str.kgErrExists(r.detail);
        else if (r.code === "E_KEYNAME")
            kgErrors.text = str.kgErrName;
        else if (r.code === "E_NOTOOL")
            kgErrors.text = str.keygenUnavailable;
        else
            kgErrors.text = (r.detail && r.detail.length ? r.detail : str.kgErrPrefix);
    }

    function copySelected() {
        if (!bridge)
            return;
        if (tab === "keys") {
            if (!bridge.copyPublicKey())
                showToast(str.keyNoPub, true);
        } else {
            bridge.copyConnection();
        }
    }

    function moveConn(delta) {
        if (!bridge || connList.count === 0)
            return;
        const row = Math.max(0, Math.min(connList.count - 1, bridge.connSel + delta));
        bridge.selectConn(row);
        selectedConns = [row];
        bridge.setConnSelection(selectedConns);
    }

    function moveKey(delta) {
        if (!bridge || keyList.count === 0)
            return;
        const row = Math.max(0, Math.min(keyList.count - 1, bridge.keySel + delta));
        bridge.selectKey(row);
        selectedKeys = [row];
        bridge.setKeySelection(selectedKeys);
    }

    // ---- window chrome -------------------------------------------------------

    Shortcut {
        sequence: "Ctrl+S"
        enabled: !root.dialogOpen
        onActivated: root.requestSave()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !root.dialogOpen && root.tab === "conn"
        onActivated: if (root.bridge) root.bridge.undo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: !root.dialogOpen && root.tab === "conn"
        onActivated: if (root.bridge) root.bridge.redo()
    }
    Shortcut {
        sequence: "Ctrl+F"
        enabled: !root.dialogOpen
        onActivated: filterField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Ctrl+Tab"
        onActivated: root.setTab(root.tab === "conn" ? "keys" : "conn")
    }
    Shortcut {
        sequence: "Ctrl+Return"
        enabled: !root.dialogOpen
        onActivated: root.copySelected()
    }

    Timer {
        id: toastTimer
        interval: 2800
        onTriggered: root.toastText = ""
    }

    Menu {
        id: connMenu
        title: root.bridge && root.bridge.connDetail ? (root.bridge.connDetail.name || "") : ""
        MenuItem { text: root.str.editConn; onTriggered: root.editConnection() }
        MenuItem { text: root.str.copySshCommand; onTriggered: root.externalConnectionAction("copy") }
        MenuItem { text: root.str.openTerminal; onTriggered: root.externalConnectionAction("terminal") }
        MenuItem { text: root.str.connectSshUrl; onTriggered: root.externalConnectionAction("sshUrl") }
        MenuSeparator {}
        MenuItem { text: root.str.btnDelete; onTriggered: root.askDelete() }
    }

    Dialog {
        id: agentPicker
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(440, root.width - 40)
        closePolicy: Popup.CloseOnEscape
        onOpened: {
            root.agentAddName = "";
            privateKeyList.forceActiveFocus();
        }
        onAccepted: root.addAgentKey()
        onRejected: root.agentAddName = ""

        background: Rectangle {
            color: root.panelColor
            radius: tokens.cardRadius
            border.width: 1
            border.color: root.ruleColor
        }

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            radius: tokens.cardRadius
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.agentPick
                color: root.inkColor
                font.pixelSize: 15
                font.bold: true
                elide: Text.ElideRight
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: root.ruleColor
            }
        }

        contentItem: Item {
            implicitHeight: Math.max(tokens.rowHeight, Math.min(320, privateKeyList.contentHeight)) + 8

            ListView {
                id: privateKeyList
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: root.bridge ? root.bridge.privateKeyNames : []
                ScrollBar.vertical: ScrollBar {}
                Keys.onPressed: (event) => {
                    const names = root.bridge ? root.bridge.privateKeyNames : [];
                    if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                        if (names.length > 0) {
                            const step = event.key === Qt.Key_Down ? 1 : -1;
                            const at = names.indexOf(root.agentAddName);
                            const next = Math.max(0, Math.min(names.length - 1, at + step));
                            root.agentAddName = names[next];
                            privateKeyList.currentIndex = next;
                        }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (root.agentAddName !== "")
                            agentPicker.accept();
                        event.accepted = true;
                    }
                }
                delegate: Rectangle {
                    id: keyRow
                    required property string modelData
                    width: privateKeyList.width
                    height: tokens.rowHeight
                    color: root.agentAddName === modelData ? root.accentSoft
                         : privateKeyHover.hovered ? root.hoverColor : "transparent"

                    HoverHandler { id: privateKeyHover }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.ruleColor
                        opacity: 0.6
                    }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        text: keyRow.modelData
                        color: root.inkColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.agentAddName = keyRow.modelData
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: privateKeyList.count === 0
                text: root.str.agentNoKeys
                color: root.dimColor
                font.pixelSize: 12
            }
        }

        footer: Item {
            implicitHeight: 52
                                 RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Btn {
                    objectName: "agentBrowseButton"
                    text: root.str.agentBrowse
                    onClicked: {
                        agentPicker.close();
                        root.pickExternalAgentKey();
                    }
                }
                Item { Layout.fillWidth: true }
                Btn {
                    primary: true
                    text: root.str.agentLoad
                    enabled: root.agentAddName !== ""
                    onClicked: agentPicker.accept()
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: agentPicker.reject()
                }
            }
        }
    }

    Dialog {
        id: passphraseDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(460, root.width - 40)
        closePolicy: Popup.CloseOnEscape
        visible: root.bridge && root.bridge.passphrasePromptVisible
        onVisibleChanged: if (visible) passphraseField.forceActiveFocus()
        onRejected: if (root.bridge) root.bridge.cancelPassphrase()

        background: Rectangle {
            color: root.panelColor
            radius: tokens.cardRadius
            border.width: 1
            border.color: root.ruleColor
        }
        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.bridge && root.bridge.zh ? "SSH 口令" : "SSH passphrase"
                color: root.inkColor
                font.pixelSize: 15
                font.bold: true
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: root.ruleColor
            }
        }
        contentItem: Item {
            implicitHeight: 84
            id: passphraseContent
            ColumnLayout {
                id: passphraseColumn
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: {
                        if (!root.bridge)
                            return "";
                        if (!root.bridge.zh)
                            return root.bridge.passphrasePrompt;
                        const prompt = root.bridge.passphrasePrompt;
                        const marker = prompt.lastIndexOf(" for ");
                        return marker >= 0 ? "请输入私钥口令：" + prompt.slice(marker + 5) : "请输入私钥口令";
                    }
                    color: root.dimColor
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }
                Field {
                    id: passphraseField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: root.bridge && root.bridge.zh ? "口令" : "Passphrase"
                    onAccepted: if (root.bridge) root.bridge.respondPassphrase(text)
                }
            }
        }
        footer: Item {
            implicitHeight: 48
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    primary: true
                    text: root.bridge && root.bridge.zh ? "确定" : "Continue"
                    enabled: passphraseField.text.length > 0
                    onClicked: if (root.bridge) root.bridge.respondPassphrase(passphraseField.text)
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: if (root.bridge) root.bridge.cancelPassphrase()
                }
            }
        }
    }

    Connections {
        target: root.bridge
        function onDetailChanged() { root.syncForm() }
        function onFilterChanged() {
            if (root.bridge) {
                if (root.tab === "conn")
                    root.connFilter = root.bridge.filter;
                else
                    root.keyFilter = root.bridge.filter;
            }
            root.syncFilterField();
        }
        function onAgentChanged() { root.selectedAgent = [] }
        function onKnownHostsChanged() { root.selectedHosts = [] }
        function onToast(code, detail) {
            if (code === "saved")
                root.showToast(root.str.toastSaved(detail), false);
            else if (code === "copiedPub")
                root.showToast(root.str.toastCopyPub, false);
            else if (code === "copiedRow")
                root.showToast(root.str.toastCopyRow, false);
            else if (code === "deleted")
                root.showToast(root.str.toastDeleted, false);
            else if (code === "reloaded")
                root.showToast(root.str.toastReloaded, false);
            else if (code === "created")
                root.showToast(root.str.toastCreated, false);
            else if (code === "keygen")
                root.showToast(root.str.toastKeygen(detail), false);
            else if (code === "agentLoaded")
                root.showToast(root.str.agentLoaded, false);
            else if (code === "load" || code === "write")
                root.showToast(detail, true);
        }
    }

    Component.onCompleted: {
        if (bridge) {
            root.tab = bridge.startTab;
            root.connFilter = bridge.filter;
            root.keyFilter = bridge.filter;
            root.knownFilter = bridge.knownFilter;
            if (bridge.connDetail.conn !== undefined)
                root.selectedConns = [bridge.connSel];
            bridge.setConnSelection(root.selectedConns);
            if (bridge.keyDetail.name !== undefined && bridge.keyDetail.name !== "")
                root.selectedKeys = [bridge.keySel];
        }
        syncFilterField();
        syncForm();
    }


    ColumnLayout {
        anchors.fill: parent
        anchors.margins: tokens.pageMargin
        spacing: tokens.spacingUnit

        // header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 54
            color: root.panelColor
            radius: tokens.cardRadius

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    radius: 6
                    color: root.accentSoft
                    Text {
                        anchors.centerIn: parent
                        text: "SSH"
                        font.pixelSize: 12
                        font.bold: true
                        color: root.accentColor
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        text: root.bridge ? root.bridge.title : ""
                        font.pixelSize: 15
                        font.bold: true
                        color: root.inkColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        text: root.bridge ? root.bridge.configPath : ""
                        font.pixelSize: 11
                        font.family: root.monoFont
                        color: root.dimColor
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }

                Btn {
                    text: root.str.info
                    onClicked: infoDlg.open()
                }
            }
        }

        // tabs + toolbar
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            color: root.panelColor
            radius: tokens.cardRadius

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Btn {
                    text: root.str.tabConn
                    checkable: true
                    autoExclusive: true
                    checked: root.tab === "conn"
                    primary: checked
                    onClicked: root.setTab("conn")
                }
                 Btn {
                     text: root.str.tabKeys
                    checkable: true
                    autoExclusive: true
                    checked: root.tab === "keys"
                    primary: checked
                     onClicked: root.setTab("keys")
                 }
                 Btn {
                     text: root.str.tabAgent
                     checkable: true
                     autoExclusive: true
                     checked: root.tab === "agent"
                     primary: checked
                     onClicked: root.setTab("agent")
                 }
                Btn {
                    text: root.str.tabKnown
                    checkable: true
                    autoExclusive: true
                    checked: root.tab === "known"
                    primary: checked
                    onClicked: {
                        root.setTab("known");
                        if (root.bridge) {
                            root.bridge.knownFilter = root.knownFilter;
                            root.bridge.reloadKnownHosts();
                        }
                    }
                }

                Item { Layout.preferredWidth: 8 }

                Field {
                    id: filterField
                    Layout.fillWidth: true
                    Layout.maximumWidth: 320
                    Layout.minimumWidth: 120
                    visible: true
                    placeholderText: root.tab === "conn" ? root.str.filterConn
                                      : root.tab === "keys" ? root.str.filterKeys
                                      : root.tab === "agent" ? root.str.filterAgent : root.str.filterKnown
                        onTextEdited: {
                            if (!root.bridge)
                                return;
                        if (root.tab === "conn")
                            root.connFilter = text;
                        else if (root.tab === "keys")
                            root.keyFilter = text;
                        else if (root.tab === "known")
                            root.knownFilter = text;
                        else
                            root.agentFilter = text;
                        root.setSelectedRows(root.tab, []);
                        root.rangeAnchor = -1;
                        if (root.tab === "known")
                            root.bridge.knownFilter = text;
                        else
                            root.bridge.filter = text;
                    }
                }

                 Item { Layout.fillWidth: true }

                 Text {
                     text: root.selectionCount > 0 ? root.str.selectedCount(root.selectionCount) : root.str.selectHint
                     color: root.accentColor
                     font.pixelSize: 11
                  }
                  Btn {
                      text: root.str.selectAll
                      enabled: root.visibleRowCount() > 0 && root.selectionCount < root.visibleRowCount()
                      onClicked: root.selectAllVisible()
                  }
                  Btn {
                      text: root.str.clearSelection
                      enabled: root.selectionCount > 0
                      onClicked: root.clearCurrentSelection()
                  }
                 Btn {
                    visible: root.tab === "conn"
                    text: root.str.add
                     enabled: !!root.bridge
                    onClicked: root.addConnection()
                }
                 Btn {
                     visible: root.tab === "agent"
                     text: root.str.agentAdd
                     enabled: root.bridge && root.bridge.privateKeyNames.length > 0
                     onClicked: agentPicker.open()
                 }
                 Btn {
                     visible: root.tab === "agent" && root.selectedAgent.length > 0
                     danger: true
                     text: root.str.agentRemove
                     onClicked: root.removeAgent()
                 }
                 Btn {
                     visible: root.tab === "agent"
                     danger: true
                     text: root.str.agentClear
                     enabled: root.bridge && root.bridge.agentKeys.length > 0
                     onClicked: {
                         const result = root.bridge.clearAgent();
                         if (result.code !== "") root.showToast(result.detail || result.code, true);
                     }
                 }
                 Btn {
                     visible: root.tab === "agent"
                     text: root.str.agentRefresh
                     onClicked: root.refreshAgent()
                 }
                 Btn {
                     visible: root.tab === "keys"
                    text: root.str.gen
                    enabled: root.bridge && root.bridge.keygenAvailable
                    onClicked: root.openKeygen()
                }
                 Btn {
                     visible: root.tab === "conn"
                     text: root.str.undo
                    enabled: root.bridge && root.bridge.canUndo
                    onClicked: if (root.bridge) root.bridge.undo()
                }
                Btn {
                    visible: root.tab === "conn"
                    text: root.str.redo
                    enabled: root.bridge && root.bridge.canRedo
                    onClicked: if (root.bridge) root.bridge.redo()
                }
                Btn {
                    visible: root.tab === "conn" && root.selectedConns.length > 0
                    danger: true
                    text: root.str.deleteSelected
                    onClicked: root.deleteConnections()
                }
                Btn {
                    visible: root.tab === "keys" && root.selectedKeys.length > 0
                    danger: true
                    text: root.str.keyDelete
                    onClicked: root.deleteKeys()
                }
                Btn {
                    visible: root.tab === "known" && root.selectedHosts.length > 0
                    danger: true
                    text: root.str.deleteSelected
                    onClicked: knownDeleteDlg.open()
                }
                Btn {
                    primary: true
                    text: root.str.save
                    enabled: root.bridge && root.bridge.canSave
                    onClicked: root.requestSave()
                }
            }
        }

        StatusBanner {
            theme: tokens
            text: root.str.connNotice
            closable: true
            closeLabel: root.str.infoClose
            visible: root.tab === "conn" && !root.connNoticeClosed
            onClosed: root.connNoticeClosed = true
        }
        // banners
        ColumnLayout {
            Layout.fillWidth: true
            spacing: tokens.spacingUnit
             visible: root.bridge && (root.bridge.loadError !== ""
                                     || (root.tab === "keys" && !root.bridge.keygenAvailable)
                                     || root.bridge.hasLocked)

            StatusBanner {
                theme: tokens
                visible: root.bridge && root.bridge.loadError !== ""
                text: root.bridge ? root.bridge.loadError : ""
                error: true
            }
            StatusBanner {
                theme: tokens
                visible: root.tab === "keys" && root.bridge && !root.bridge.keygenAvailable
                text: root.str.bannerKeygen
            }
            StatusBanner {
                theme: tokens
                visible: root.tab === "conn" && root.bridge && root.bridge.hasLocked
                text: root.str.bannerLocked
            }
        }

        // table + detail
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            // list region
            Rectangle {
                SplitView.fillWidth: true
                SplitView.fillHeight: root.tab !== "conn" && root.tab !== "keys"
                SplitView.preferredHeight: root.tab === "conn" || root.tab === "keys" ? 300 : -1
                SplitView.minimumHeight: 180
                color: root.panelColor
                border.color: root.ruleColor
                radius: tokens.cardRadius
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // column headers
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: tokens.headerHeight
                        color: root.panelAlt
                        visible: !root.narrow && (root.tab === "conn" || root.tab === "keys")
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 8
                            spacing: 0
                            Rectangle {
                                Layout.preferredWidth: root.selectionColumnWidth
                                Layout.fillHeight: true
                                color: root.panelAlt
                                CompactCheckBox {
                                    anchors.fill: parent
                                    theme: tokens
                                    objectName: "sshSelectVisibleRows"
                                    checkState: root.selectionCount === 0 ? Qt.Unchecked
                                              : root.visibleRowCount() > 0 && root.selectionCount === root.visibleRowCount() ? Qt.Checked : Qt.PartiallyChecked
                                    Accessible.name: root.str.selectAll
                                    onClicked: root.visibleRowCount() > 0 && root.selectionCount === root.visibleRowCount()
                                               ? root.clearCurrentSelection() : root.selectAllVisible()
                                }
                            }
                            SshHeaderCell {
                                visible: root.tab === "conn"
                                title: root.str.colName
                                columnWidth: root.connWidths.name
                                onResized: root.resizeSshColumn("conn", "name", delta, 120, 360)
                            }
                            SshHeaderCell {
                                visible: root.tab === "conn"
                                title: root.str.colHost
                                fills: true
                                columnWidth: 180
                            }
                            SshHeaderCell {
                                visible: root.tab === "conn"
                                title: root.str.colUser
                                columnWidth: root.connWidths.user
                                onResized: root.resizeSshColumn("conn", "user", delta, 60, 220)
                            }
                            SshHeaderCell {
                                visible: root.tab === "conn"
                                title: root.str.colPort
                                columnWidth: root.connWidths.port
                                onResized: root.resizeSshColumn("conn", "port", delta, 40, 120)
                            }
                            SshHeaderCell {
                                visible: root.tab === "conn"
                                title: root.str.colKey
                                columnWidth: root.connWidths.key
                                onResized: root.resizeSshColumn("conn", "key", delta, 100, 360)
                            }
                            Item { visible: root.tab === "conn"; Layout.preferredWidth: 16; Layout.fillHeight: true }
                            Item { visible: root.tab === "conn"; Layout.preferredWidth: 30; Layout.fillHeight: true }
                            SshHeaderCell {
                                visible: root.tab === "keys"
                                title: root.str.colFile
                                columnWidth: root.keyWidths.file
                                onResized: root.resizeSshColumn("keys", "file", delta, 120, 360)
                            }
                            SshHeaderCell {
                                visible: root.tab === "keys"
                                title: root.str.colType
                                columnWidth: root.keyWidths.type
                                onResized: root.resizeSshColumn("keys", "type", delta, 60, 160)
                            }
                            SshHeaderCell {
                                visible: root.tab === "keys"
                                title: root.str.colComment
                                fills: true
                                columnWidth: 160
                            }
                            SshHeaderCell {
                                visible: root.tab === "keys"
                                title: root.str.colFp
                                columnWidth: root.keyWidths.fingerprint
                                onResized: root.resizeSshColumn("keys", "fingerprint", delta, 120, 360)
                            }
                            SshHeaderCell {
                                visible: root.tab === "keys"
                                title: root.str.colState
                                columnWidth: root.keyWidths.state
                                onResized: root.resizeSshColumn("keys", "state", delta, 80, 220)
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: root.ruleColor
                        visible: !root.narrow && (root.tab === "conn" || root.tab === "keys")
                    }

                            StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
        currentIndex: root.tab === "known" ? 0 : root.tab === "conn" ? 1 : root.tab === "keys" ? 2 : 3

                        Item {
                            visible: root.tab === "known"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                          spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: root.str.knownHostWarning
                                    color: root.warnColor
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                                ListView {
                                    id: knownList
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    model: root.bridge ? root.bridge.knownHosts : []
                                    ScrollBar.vertical: ScrollBar {}
                                    Keys.onPressed: (event) => {
                                        if (event.key === Qt.Key_Delete && root.selectedHosts.length > 0) {
                                            knownDeleteDlg.open();
                                            event.accepted = true;
                                        }
                                    }
                                    delegate: Rectangle {
                                        required property int index
                                        required property var modelData
                                        width: knownList.width
                                        height: 54
                                        color: root.selectedHosts.indexOf(index) >= 0 ? root.accentSoft : "transparent"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 4
                                            anchors.rightMargin: 8
                                            spacing: 4
                                            CompactCheckBox {
                                                theme: tokens
                                                Layout.preferredWidth: root.selectionColumnWidth
                                                Layout.fillHeight: true
                                                checked: root.selectedHosts.indexOf(index) >= 0
                                                Accessible.name: root.str.selectItem(modelData.hosts)
                                                onClicked: root.toggleMultiSelection("known", index)
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.hosts + (modelData.portDisplay ? ":" + modelData.portDisplay : "") + " · " + modelData.type
                                                    color: root.inkColor
                                                    font.pixelSize: 12
                                                    elide: Text.ElideRight
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.fingerprint
                                                    color: root.dimColor
                                                    font.pixelSize: 11
                                                    font.family: root.monoFont
                                                    elide: Text.ElideMiddle
                                                }
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            z: -1
                                            onClicked: root.updateSelection("known", index, mouse.modifiers)
                                        }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        visible: knownList.count === 0
                                        text: root.str.knownHostEmpty
                                        color: root.dimColor
                                        font.pixelSize: 12
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: root.selectedHosts.length > 0
                                    text: root.str.selectedCount(root.selectedHosts.length)
                                    color: root.accentColor
                                    font.pixelSize: 11
                                }
                            }
                        }

                        Item {
                            visible: root.tab === "conn"
                            ListView {
                                id: connList
                                anchors.fill: parent
                                clip: true
                                focus: root.tab === "conn" && !root.dialogOpen
                                model: root.bridge ? root.bridge.conns : null
                                currentIndex: root.bridge ? root.bridge.connSel : -1
                                ScrollBar.vertical: ScrollBar {}

                                Keys.onPressed: (event) => {
                                    if (event.key === Qt.Key_Delete && root.selectedConns.length > 0) {
                                        root.deleteConnections();
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Down) {
                                        root.moveConn(1);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Up) {
                                        root.moveConn(-1);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Home) {
                                        root.bridge.selectConn(0);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_End) {
                                        root.moveConn(connList.count);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Insert) {
                                        root.addConnection();
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Delete) {
                                        root.askDelete();
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                        if (root.bridge.connDetail.canEdit)
                                            fAlias.forceActiveFocus();
                                        event.accepted = true;
                                    }
                                }

                                delegate: Rectangle {
                                    required property int index
                                    required property string name
                                    required property string host
                                    required property string user
                                    required property string port
                                    required property string key
                                    required property string mark
                                    required property bool deleted

                                    width: connList.width
                                     height: root.narrow ? tokens.rowHeight * 2 : tokens.rowHeight
                                     color: root.selectedConns.indexOf(index) >= 0 ? root.accentSoft
                                           : connHover.hovered ? root.hoverColor : "transparent"

                                    HoverHandler { id: connHover }

                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 1
                                        color: root.ruleColor
                                        opacity: 0.6
                                    }

                                    // wide row
                                    RowLayout {
                                        anchors.fill: parent
                                         anchors.leftMargin: 4
                                        anchors.rightMargin: 10
                                          spacing: 0
                                         visible: !root.narrow

                                          CompactCheckBox {
                                              theme: tokens
                                              Layout.preferredWidth: root.selectionColumnWidth
                                              Layout.fillHeight: true
                                              checked: root.selectedConns.indexOf(index) >= 0
                                              Accessible.name: root.str.selectItem(name)
                                              onClicked: root.toggleMultiSelection("conn", index)
                                          }

                                        Text {
                                            text: name
                                            font.pixelSize: 12
                                            font.bold: true
                                            font.strikeout: deleted
                                            color: deleted ? root.dangerColor : root.inkColor
                                            Layout.preferredWidth: root.connWidths.name
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: host
                                            font.pixelSize: 12
                                            font.family: root.monoFont
                                            color: root.dimColor
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: user
                                            font.pixelSize: 12
                                            color: root.dimColor
                                            Layout.preferredWidth: root.connWidths.user
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: port
                                            font.pixelSize: 12
                                            color: root.dimColor
                                            Layout.preferredWidth: root.connWidths.port
                                        }
                                        Text {
                                            text: key
                                            font.pixelSize: 12
                                            font.family: root.monoFont
                                            color: root.dimColor
                                            Layout.preferredWidth: root.connWidths.key
                                            elide: Text.ElideMiddle
                                        }
                                         Text {
                                             text: mark
                                            font.pixelSize: 11
                                            color: root.warnColor
                                             Layout.preferredWidth: 16
                                         }
                                         ToolButton {
                                             Layout.preferredWidth: 30
                                             Layout.preferredHeight: 30
                                             text: "⋯"
                                             font.pixelSize: 17
                                             display: AbstractButton.TextOnly
                                              onClicked: {
                                                  const point = mapToItem(root, width / 2, height / 2);
                                                  root.openConnMenu(index, point.x, point.y);
                                              }
                                         }
                                    }

                                    // narrow row
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        anchors.topMargin: 4
                                        anchors.bottomMargin: 4
                                        spacing: 1
                                        visible: root.narrow

                                         RowLayout {
                                             Layout.fillWidth: true
                                              CompactCheckBox {
                                                  theme: tokens
                                                  Layout.preferredWidth: root.selectionColumnWidth
                                                  Layout.fillHeight: true
                                                  checked: root.selectedConns.indexOf(index) >= 0
                                                  Accessible.name: root.str.selectItem(name)
                                                  onClicked: root.toggleMultiSelection("conn", index)
                                              }
                                             Text {
                                                text: name
                                                font.pixelSize: 12
                                                font.bold: true
                                                font.strikeout: deleted
                                                color: deleted ? root.dangerColor : root.inkColor
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                             Text {
                                                 text: mark
                                                font.pixelSize: 11
                                                 color: root.warnColor
                                             }
                                             ToolButton {
                                                 Layout.preferredWidth: 30
                                                 Layout.preferredHeight: 30
                                                 text: "⋯"
                                                 font.pixelSize: 17
                                                 display: AbstractButton.TextOnly
                                                  onClicked: {
                                                      const point = mapToItem(root, width / 2, height / 2);
                                                      root.openConnMenu(index, point.x, point.y);
                                                  }
                                             }
                                        }
                                        Text {
                                            text: [host, user, port, key]
                                                  .filter(function (x) { return x; }).join(" · ")
                                            font.pixelSize: 11
                                            font.family: root.monoFont
                                            color: root.dimColor
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }

                                     MouseArea {
                                         anchors.fill: parent
                                         acceptedButtons: Qt.LeftButton | Qt.RightButton
                                         z: -1
                                         onClicked: {
                                             if (mouse.button === Qt.RightButton) {
                                                 const point = connList.mapToItem(root, mouse.x, mouse.y);
                                                 root.openConnMenu(index, point.x, point.y);
                                             } else {
                                                 root.updateSelection("conn", index, mouse.modifiers);
                                                 connList.forceActiveFocus();
                                             }
                                         }
                                     }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 40
                                    visible: connList.count === 0 && !(root.bridge && root.bridge.emptyState)
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    text: root.str.noMatchConn
                                    color: root.dimColor
                                    font.pixelSize: 12
                                }
                            }

                            // empty config state
                            ColumnLayout {
                                anchors.centerIn: parent
                                width: Math.min(420, parent.width - 40)
                                spacing: 8
                                visible: root.bridge && root.bridge.emptyState && root.tab === "conn"

                                Text {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignHCenter
                                    text: root.str.emptyTitle
                                    color: root.inkColor
                                    font.pixelSize: 15
                                    font.bold: true
                                }
                                Text {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    text: root.bridge ? root.str.emptyBody(root.bridge.configPath) : ""
                                    color: root.dimColor
                                    font.pixelSize: 12
                                }
                                Btn {
                                    Layout.alignment: Qt.AlignHCenter
                                    primary: true
                                    text: root.str.create
                                    enabled: !!root.bridge
                                    onClicked: if (root.bridge) root.bridge.createConfig()
                                }
                            }
                        }

                         Item {
                             visible: root.tab === "keys"
                            ListView {
                                id: keyList
                                anchors.fill: parent
                                clip: true
                                focus: root.tab === "keys" && !root.dialogOpen
                                model: root.bridge ? root.bridge.keys : null
                                currentIndex: root.bridge ? root.bridge.keySel : -1
                                ScrollBar.vertical: ScrollBar {}

                                Keys.onPressed: (event) => {
                                    if (event.key === Qt.Key_Delete && root.selectedKeys.length > 0) {
                                        root.deleteKeys();
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Down) {
                                        root.moveKey(1);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Up) {
                                        root.moveKey(-1);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_Home) {
                                        root.bridge.selectKey(0);
                                        event.accepted = true;
                                    } else if (event.key === Qt.Key_End) {
                                        root.moveKey(keyList.count);
                                        event.accepted = true;
                                    }
                                }

                                 delegate: Rectangle {
                                    required property int index
                                    required property string name
                                    required property string type
                                    required property string comment
                                    required property string fingerprint
                                    required property string keyState

                                    width: keyList.width
                                     height: root.narrow ? 46 : 28
                                     color: root.selectedKeys.indexOf(index) >= 0 ? root.accentSoft
                                            : keyHover.hovered ? root.hoverColor : "transparent"

                                    HoverHandler { id: keyHover }

                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 1
                                        color: root.ruleColor
                                        opacity: 0.6
                                    }

                                     RowLayout {
                                         anchors.fill: parent
                                         anchors.leftMargin: 4
                                         anchors.rightMargin: 8
                                         spacing: 8
                                         visible: !root.narrow

                                         CompactCheckBox {
                                             theme: tokens
                                             Layout.preferredWidth: root.selectionColumnWidth
                                             Layout.fillHeight: true
                                             checked: root.selectedKeys.indexOf(index) >= 0
                                              Accessible.name: root.str.selectItem(name)
                                             onClicked: root.toggleMultiSelection("keys", index)
                                         }

                                         Text {
                                            text: name
                                            font.pixelSize: 12
                                            font.bold: true
                                            color: root.inkColor
                                             Layout.preferredWidth: root.keyWidths.file
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: type
                                            font.pixelSize: 12
                                            color: root.dimColor
                                              Layout.preferredWidth: root.keyWidths.type
                                        }
                                        Text {
                                            text: comment
                                            font.pixelSize: 12
                                            color: root.dimColor
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: fingerprint
                                            font.pixelSize: 11
                                            font.family: root.monoFont
                                            color: root.dimColor
                                              Layout.preferredWidth: root.keyWidths.fingerprint
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            text: root.stateText(keyState)
                                            font.pixelSize: 11
                                            color: keyState === "private" ? root.warnColor : root.dimColor
                                              Layout.preferredWidth: root.keyWidths.state
                                            elide: Text.ElideRight
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        anchors.topMargin: 4
                                        anchors.bottomMargin: 4
                                        spacing: 1
                                        visible: root.narrow

                                         RowLayout {
                                             Layout.fillWidth: true
                                             CompactCheckBox {
                                                 theme: tokens
                                                 Layout.preferredWidth: root.selectionColumnWidth
                                                 Layout.fillHeight: true
                                                 checked: root.selectedKeys.indexOf(index) >= 0
                                                  Accessible.name: root.str.selectItem(name)
                                                 onClicked: root.toggleMultiSelection("keys", index)
                                             }
                                             Text {
                                                text: name
                                                font.pixelSize: 12
                                                font.bold: true
                                                color: root.inkColor
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: root.stateText(keyState)
                                                font.pixelSize: 11
                                                color: keyState === "private" ? root.warnColor : root.dimColor
                                            }
                                        }
                                        Text {
                                            text: [type, comment, fingerprint]
                                                  .filter(function (x) { return x; }).join(" · ")
                                            font.pixelSize: 11
                                            font.family: root.monoFont
                                            color: root.dimColor
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }

                                     MouseArea {
                                         anchors.fill: parent
                                         z: -1
                                         onClicked: {
                                            root.updateSelection("keys", index, mouse.modifiers);
                                            keyList.forceActiveFocus();
                                        }
                                    }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 40
                                    visible: keyList.count === 0
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    text: root.bridge && root.bridge.filter !== "" ? root.str.noMatchKeys : root.str.noKeys
                                    color: root.dimColor
                                    font.pixelSize: 12
                                }
                            }
                        }

                        Item {
                            visible: root.tab === "agent"
                            ListView {
                                id: agentList
                                anchors.fill: parent
                                clip: true
                                focus: root.tab === "agent" && !root.dialogOpen
                                model: root.agentRows()
                                ScrollBar.vertical: ScrollBar {}
                                Keys.onPressed: (event) => {
                                    if (event.key === Qt.Key_Delete && root.selectedAgent.length > 0) {
                                        root.removeAgent();
                                        event.accepted = true;
                                    }
                                }
                                delegate: Rectangle {
                                    required property int index
                                    required property var modelData
                                    width: agentList.width
                                    height: 52
                                    color: root.selectedAgent.indexOf(index) >= 0 ? root.accentSoft
                                           : agentHover.hovered ? root.hoverColor : "transparent"
                                     HoverHandler { id: agentHover }
                                     ToolTip.visible: agentHover.hovered && Boolean(modelData.sourcePath)
                                     ToolTip.text: modelData.sourcePath || ""
                                     ToolTip.delay: 500
                                     Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 1
                                        color: root.ruleColor
                                        opacity: 0.6
                                    }
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: root.selectionColumnWidth + 8
                                        anchors.rightMargin: 10
                                        anchors.topMargin: 5
                                        anchors.bottomMargin: 5
                                        spacing: 2
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.type + " · " + modelData.bits + " bit"
                                            color: root.inkColor
                                            font.pixelSize: 12
                                            font.bold: true
                                            elide: Text.ElideRight
                                         }
                                         Text {
                                             Layout.fillWidth: true
                                             text: modelData.fingerprint + (modelData.comment ? " · " + modelData.comment : "")
                                                    + ((modelData.sourcePath || modelData.source) ? " · " + (modelData.sourcePath || modelData.source) : "")
                                             color: root.dimColor
                                            font.family: root.monoFont
                                            font.pixelSize: 11
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                    CompactCheckBox {
                                        theme: tokens
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        width: root.selectionColumnWidth
                                        checked: root.selectedAgent.indexOf(index) >= 0
                                        Accessible.name: root.str.selectItem(modelData.fingerprint)
                                        onClicked: root.toggleMultiSelection("agent", index)
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        z: -1
                                        onClicked: {
                                            root.updateSelection("agent", index, mouse.modifiers);
                                            agentList.currentIndex = index;
                                            agentList.forceActiveFocus();
                                        }
                                    }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 32
                                    visible: agentList.count === 0
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    text: root.bridge && root.bridge.agentError !== "" ? root.bridge.agentError : root.str.agentEmpty
                                    color: root.dimColor
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: connectionDetailPanel
                objectName: "connectionDetailPanel"
                visible: root.tab === "keys" || root.tab === "conn"
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                SplitView.preferredHeight: 280
                SplitView.minimumHeight: 180
                color: root.panelAlt

                ColumnLayout {
                    id: detailPanelColumn
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        objectName: "detailPanelHeader"
                        Layout.fillWidth: true
                        implicitHeight: 36
                        color: root.panelColor
                        border.color: root.ruleColor
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: root.tab === "conn" && root.bridge
                                      ? (root.bridge.connDetail.name || root.str.selectHint)
                                      : root.str.tabKeys
                                color: root.accentColor
                                font.pixelSize: 13
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Item { Layout.fillWidth: true }
                            Btn {
                                objectName: "sshConnectButton"
                                visible: root.tab === "conn" && root.selectedConns.length === 1
                                primary: true
                                text: root.str.btnConnect
                                enabled: root.bridge && root.bridge.connDetail.conn !== undefined
                                         && !root.bridge.connDetail.locked
                                onClicked: root.openSelectedConnection()
                            }
                            Btn {
                                objectName: "sshTestButton"
                                visible: root.tab === "conn" && root.selectedConns.length === 1
                                text: root.str.btnTest
                                enabled: root.bridge && root.bridge.connDetail.conn !== undefined
                                         && !root.bridge.connDetail.locked && !root.bridge.testing
                                onClicked: if (root.bridge) root.bridge.startConnectionTest()
                            }
                            Btn {
                                visible: root.tab === "conn" && root.selectedConns.length === 1
                                 text: root.str.copyRow
                                 enabled: root.bridge && root.bridge.connDetail.conn !== undefined
                                 onClicked: if (root.bridge) root.bridge.copyConnection()
                             }
                        }
                    }

                    Rectangle {
                        objectName: "sshTestStrip"
                        Layout.fillWidth: true
                        implicitHeight: visible ? 26 : 0
                        visible: root.tab === "conn" && root.bridge
                                 && root.bridge.connectionTest.state !== "idle"
                        color: root.panelAlt
                        border.color: root.ruleColor
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            text: root.connectionTestText()
                            color: !root.bridge ? root.dimColor
                                 : root.bridge.connectionTest.state === "ok" ? root.okColor
                                 : root.bridge.connectionTest.state === "fail" ? root.dangerColor
                                 : root.accentColor
                            font.pixelSize: 11
                        }
                    }

                    StackLayout {
                        id: detailContent
                        objectName: "sshDetailContent"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: root.tab === "conn" ? 0 : 1

                    // ---- connection detail ----
                         Item {
                             visible: root.tab === "conn"
                         ColumnLayout {
                             anchors.fill: parent
                             spacing: 0
                              visible: true

                             Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 1
                                color: root.ruleColor
                            }

                             SplitView {
                                 id: detailHorizontalSplit
                                 objectName: "detailHorizontalSplit"
                                 Layout.fillWidth: true
                                 Layout.fillHeight: true
                                 orientation: Qt.Horizontal
                                 handle: Rectangle {
                                     implicitWidth: 7
                                     implicitHeight: 7
                                     color: SplitHandle.hovered || SplitHandle.pressed ? root.accentColor : root.ruleColor
                                 }

                                // form
                                 Item {
                                      SplitView.fillWidth: true
                                      SplitView.preferredWidth: 420
                                      SplitView.minimumWidth: 220
                                      visible: true
                                      Text {
                                          anchors.centerIn: parent
                                          width: parent.width - 32
                                              visible: root.selectedConns.length === 0
                                          horizontalAlignment: Text.AlignHCenter
                                          wrapMode: Text.WordWrap
                                              text: root.str.selectHint
                                          color: root.dimColor
                                          font.pixelSize: 12
                                      }
                                      Text {
                                         anchors.left: parent.left
                                         anchors.top: parent.top
                                         anchors.leftMargin: 14
                                         anchors.topMargin: 8
                                         text: root.str.segForm
                                         color: root.dimColor
                                         font.pixelSize: 11
                                     }
                                      Flickable {
                                          id: formFlick
                                          anchors.fill: parent
                                          anchors.topMargin: 28
                                              visible: root.selectedConns.length > 0
                                        contentWidth: width
                                        contentHeight: formColumn.implicitHeight
                                        clip: true
                                        ScrollBar.vertical: ScrollBar {}

                                        ColumnLayout {
                                            id: formColumn
                                            width: formFlick.width
                                            spacing: 6

                                            Item { Layout.preferredHeight: 8 }

                                            Text {
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                visible: root.bridge && root.bridge.connDetail.lockedNote !== ""
                                                text: root.bridge ? root.bridge.connDetail.lockedNote : ""
                                                color: root.warnColor
                                                font.pixelSize: 11
                                                wrapMode: Text.WordWrap
                                            }

                                            // editable fields
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                spacing: 4
                                                visible: root.bridge && !root.bridge.connDetail.locked

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: root.str.lblHost
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                     Field {
                                                        id: fAlias
                                                        Layout.fillWidth: true
                                                        placeholderText: root.str.hHost
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("alias", text)
                                                    }
                                                }
                                                Text {
                                                    Layout.leftMargin: 118
                                                    visible: root.bridge && (root.bridge.connDetail.fieldErrors.alias || "") !== ""
                                                    text: root.fieldError(root.bridge ? root.bridge.connDetail.fieldErrors.alias : "")
                                                    color: root.dangerColor
                                                    font.pixelSize: 11
                                                }

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: "HostName"
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                    Field {
                                                        id: fHostname
                                                        Layout.fillWidth: true
                                                        placeholderText: root.str.hHostName
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("hostname", text)
                                                    }
                                                }
                                                Text {
                                                    Layout.leftMargin: 118
                                                    visible: root.bridge && (root.bridge.connDetail.fieldErrors.hostname || "") !== ""
                                                    text: root.fieldError(root.bridge ? root.bridge.connDetail.fieldErrors.hostname : "")
                                                    color: root.dangerColor
                                                    font.pixelSize: 11
                                                }

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: "User"
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                    Field {
                                                        id: fUser
                                                        Layout.fillWidth: true
                                                        placeholderText: root.str.hUser
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("user", text)
                                                    }
                                                }

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: "Port"
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                    Field {
                                                        id: fPort
                                                        Layout.fillWidth: true
                                                        placeholderText: root.str.hPort
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("port", text)
                                                    }
                                                    Text {
                                                        visible: root.bridge && (root.bridge.connDetail.fieldErrors.port || "") !== ""
                                                        text: root.fieldError(root.bridge ? root.bridge.connDetail.fieldErrors.port : "")
                                                        color: root.dangerColor
                                                        font.pixelSize: 11
                                                    }
                                                }

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: "IdentityFile"
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                    Field {
                                                        id: fIdentity
                                                        Layout.fillWidth: true
                                                        font.family: root.monoFont
                                                        placeholderText: root.str.hIdentity
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("identity", text)
                                                    }
                                                }
                                                Text {
                                                    Layout.leftMargin: 118
                                                    text: root.str.hintIdentity
                                                    color: root.dimColor
                                                    font.pixelSize: 10
                                                }

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10
                                                    Text {
                                                        text: "ProxyJump"
                                                        color: root.dimColor
                                                        font.pixelSize: 11
                                                        Layout.preferredWidth: 108
                                                    }
                                                     Field {
                                                        id: fProxyJump
                                                        Layout.fillWidth: true
                                                        placeholderText: root.str.hProxyJump
                                                        enabled: root.bridge && root.bridge.connDetail.canEdit
                                                        onEditingFinished: if (root.bridge) root.bridge.setConnField("proxyjump", text)
                                                    }
                                                }
                                                Text {
                                                    Layout.leftMargin: 118
                                                    text: root.str.hintProxyJump
                                                    color: root.dimColor
                                                    font.pixelSize: 10
                                                }
                                            }

                                            // extra directives (read-only)
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                Layout.topMargin: 6
                                                spacing: 3
                                                visible: root.bridge && root.bridge.connDetail.extras.length > 0

                                                Text {
                                                    text: root.str.lblExtra
                                                    color: root.dimColor
                                                    font.pixelSize: 11
                                                }
                                                Repeater {
                                                    model: root.bridge ? root.bridge.connDetail.extras : []
                                                    Text {
                                                        required property var modelData
                                                        Layout.fillWidth: true
                                                        text: modelData.line
                                                        color: root.inkColor
                                                        font.pixelSize: 11
                                                        font.family: root.monoFont
                                                        wrapMode: Text.WrapAnywhere
                                                    }
                                                }
                                            }

                                            Item { Layout.preferredHeight: 14 }
                                        }
                                    }
                                }

                                // preview
                                Item {
                                     SplitView.fillWidth: true
                                     SplitView.preferredWidth: 420
                                     SplitView.minimumWidth: 200
                                     visible: true
                                     ColumnLayout {
                                         anchors.fill: parent
                                         spacing: 0
                                          Text {
                                              Layout.fillWidth: true
                                              Layout.fillHeight: true
                                              visible: root.selectedConns.length === 0
                                              horizontalAlignment: Text.AlignHCenter
                                              verticalAlignment: Text.AlignVCenter
                                              wrapMode: Text.WordWrap
                                              text: root.str.selectHint
                                             color: root.dimColor
                                             font.pixelSize: 12
                                         }

                                         Text {
                                             Layout.fillWidth: true
                                             Layout.leftMargin: 12
                                             Layout.rightMargin: 12
                                             Layout.topMargin: 8
                                              visible: root.selectedConns.length > 0
                                             text: root.str.segPreview
                                             color: root.dimColor
                                             font.pixelSize: 11
                                         }
                                         Text {
                                             Layout.fillWidth: true
                                             Layout.leftMargin: 12
                                             Layout.rightMargin: 12
                                             Layout.topMargin: 4
                                              visible: root.selectedConns.length > 0
                                             text: root.str.previewLegend
                                            color: root.dimColor
                                            font.pixelSize: 11
                                            wrapMode: Text.WordWrap
                                        }

                                         ListView {
                                             id: previewList
                                             Layout.fillWidth: true
                                             Layout.fillHeight: true
                                              visible: root.selectedConns.length > 0
                                            clip: true
                                            model: root.bridge ? root.bridge.previewLines : []
                                            ScrollBar.vertical: ScrollBar {}

                                            delegate: Rectangle {
                                                required property int index
                                                required property var modelData

                                                width: previewList.width
                                                height: 18
                                                 color: modelData.selected ? root.accentSoft
                                                        : modelData.changed ? root.warnSoft : "transparent"

                                                RowLayout {
                                                    anchors.fill: parent
                                                    spacing: 8
                                                    Text {
                                                        Layout.preferredWidth: 34
                                                        horizontalAlignment: Text.AlignRight
                                                        text: index + 1
                                                        color: root.dimColor
                                                        font.pixelSize: 10
                                                        font.family: root.monoFont
                                                    }
                                                    Text {
                                                        Layout.preferredWidth: 10
                                                          text: modelData.changed ? "●" : ""
                                                          color: root.warnColor
                                                        font.pixelSize: 9
                                                    }
                                                    TextEdit {
                                                        Layout.fillWidth: true
                                                        text: modelData.text.length ? modelData.text : " "
                                                        color: root.inkColor
                                                        font.pixelSize: 11
                                                        font.family: root.monoFont
                                                        readOnly: true
                                                        selectByMouse: true
                                                        persistentSelection: true
                                                        wrapMode: TextEdit.NoWrap
                                                         clip: true
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- key detail ----
                    Item {
                        visible: root.tab === "keys"
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: root.selectedKeys.length === 0
                            horizontalAlignment: Text.AlignHCenter
                            text: root.str.selectHint
                            color: root.dimColor
                            font.pixelSize: 12
                        }
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: root.selectedKeys.length > 1
                            horizontalAlignment: Text.AlignHCenter
                            text: root.str.selectedCount(root.selectedKeys.length)
                            color: root.inkColor
                            font.pixelSize: 14
                            font.bold: true
                        }
                        Flickable {
                            id: keyFlick
                            anchors.fill: parent
                            visible: root.selectedKeys.length === 1
                            contentWidth: width
                            contentHeight: keyColumn.implicitHeight
                            clip: true
                            ScrollBar.vertical: ScrollBar {}

                            ColumnLayout {
                                id: keyColumn
                                width: keyFlick.width
                                spacing: 4

                                Item { Layout.preferredHeight: 8 }

                                Text {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    visible: root.bridge && root.bridge.keyDetail.name !== undefined
                                    text: root.bridge ? (root.bridge.keyDetail.name || "") : ""
                                    font.pixelSize: 15
                                    font.bold: true
                                    color: root.accentColor
                                    elide: Text.ElideRight
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 6
                                    columns: 2
                                    columnSpacing: 10
                                    rowSpacing: 4
                                    visible: root.bridge && root.bridge.keyDetail.name !== undefined

                                    Text {
                                        text: root.str.keyPath
                                        color: root.dimColor
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                         text: root.bridge ? (root.bridge.keyDetail.path || "") : ""
                                        color: root.inkColor
                                        font.pixelSize: 11
                                        font.family: root.monoFont
                                        wrapMode: Text.WrapAnywhere
                                    }
                                    Text {
                                        text: root.str.keyType
                                        color: root.dimColor
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        text: {
                                            const k = root.bridge ? root.bridge.keyDetail : ({});
                                            return (k.type || "?") + " · " + root.str.keyBits(k.bits || 0);
                                        }
                                        color: root.inkColor
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        text: root.str.keyFp
                                        color: root.dimColor
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                         text: root.bridge ? (root.bridge.keyDetail.fingerprint || "") : ""
                                        color: root.inkColor
                                        font.pixelSize: 11
                                        font.family: root.monoFont
                                        wrapMode: Text.WrapAnywhere
                                    }
                                    Text {
                                        text: root.str.keyUsed
                                        color: root.dimColor
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: {
                                            const k = root.bridge ? root.bridge.keyDetail : ({});
                                            if (!k.usedBy || k.usedBy.length === 0)
                                                return root.str.keyNoneUsed;
                                            return k.usedBy.join(", ");
                                        }
                                        color: root.inkColor
                                        font.pixelSize: 11
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 8
                                    text: root.str.keyPub
                                    color: root.dimColor
                                    font.pixelSize: 11
                                }

                                TextArea {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.preferredHeight: 92
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextArea.WrapAnywhere
                                    text: root.bridge ? (root.bridge.keyDetail.pubText || root.str.keyNoPub) : ""
                                    color: root.bridge && root.bridge.keyDetail.hasPub ? root.inkColor : root.warnColor
                                    font.pixelSize: 11
                                    font.family: root.monoFont
                                    background: Rectangle {
                                        color: root.panelColor
                                        border.width: 1
                                        border.color: root.ruleColor
                                        radius: 4
                                    }
                                }

                                RowLayout {
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    spacing: 8
                                     Btn {
                                         text: root.str.copyPub
                                         enabled: root.bridge && root.bridge.keyDetail.hasPub
                                         onClicked: if (root.bridge) root.bridge.copyPublicKey()
                                     }
                                     Btn {
                                         objectName: "addAgentKeyButton"
                                         text: root.bridge && root.bridge.agentLoading ? root.str.agentLoading : root.str.addToAgent
                                         enabled: root.bridge && root.bridge.keyDetail.hasPrivate && !root.bridge.agentLoading
                                         onClicked: root.addSelectedKeyToAgent()
                                     }
                                     Item { Layout.fillWidth: true }
                                 }
                                 Item { Layout.preferredHeight: 14 }
                            }
                        }

                    }
                }

            }
        }
    }

        // status bar
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: tokens.rowHeight
            color: root.panelColor

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: root.ruleColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 16

                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.pixelSize: 11
                    color: root.dimColor
                    text: {
                        if (!root.bridge)
                            return "";
                        if (root.tab === "conn") {
                            const n = connList.count;
                            return (root.bridge.dirty ? root.str.statusDirty(root.bridge.dirtyCount) + " · " : "")
                                   + root.str.statusCount(n);
                        }
                        return root.str.statusKeys(keyList.count);
                    }
                }
                Text {
                    visible: root.tab === "conn"
                    font.pixelSize: 11
                    color: root.dimColor
                    text: root.str.statusSave
                }
            }
        }
    }

    // toast
    Rectangle {
        visible: root.toastText !== ""
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        anchors.bottom: parent ? parent.bottom : undefined
        anchors.bottomMargin: 40
        width: Math.min(parent ? parent.width - 40 : 400, toastLabel.implicitWidth + 28)
        height: toastLabel.implicitHeight + 16
        radius: 6
        color: root.toastBad ? root.dangerSoft : root.okSoft
        border.width: 1
        border.color: root.toastBad ? root.dangerColor : root.okColor

        Text {
            id: toastLabel
            anchors.centerIn: parent
            width: parent.width - 24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.toastText
            color: root.toastBad ? root.dangerColor : root.okColor
            font.pixelSize: 12
        }
    }

    // ---- save dialog ---------------------------------------------------------

    Dialog {
        id: saveDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(620, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.saveTitle
                font.pixelSize: 15
                font.bold: true
                color: root.inkColor
            }
        }

        contentItem: Item {
            implicitHeight: saveColumn.implicitHeight + 28
            ColumnLayout {
                id: saveColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: root.dimColor
                    text: root.bridge ? root.str.saveTarget(root.bridge.configPath, root.bridge.backupDir) : ""
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(230, saveChangeColumn.height)
                    contentHeight: saveChangeColumn.height
                    clip: true
                    ScrollBar.vertical: ScrollBar {}

                    Column {
                        id: saveChangeColumn
                        width: parent.width
                        spacing: 6
                        Repeater {
                            model: root.bridge ? root.bridge.changes : []
                            delegate: Column {
                                required property var modelData
                                width: saveChangeColumn.width
                                spacing: 2
                                RowLayout {
                                    width: parent.width
                                    spacing: 8
                                    Rectangle {
                                        implicitWidth: tag.implicitWidth + 12
                                        implicitHeight: 18
                                        radius: 3
                                        color: modelData.kind === "add" ? root.okSoft
                                               : modelData.kind === "delete" ? root.dangerSoft : root.warnSoft
                                        Text {
                                            id: tag
                                            anchors.centerIn: parent
                                            text: root.kindText(modelData.kind)
                                            font.pixelSize: 10
                                            color: modelData.kind === "add" ? root.okColor
                                                   : modelData.kind === "delete" ? root.dangerColor : root.warnColor
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.alias
                                        color: root.inkColor
                                        font.pixelSize: 12
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                }
                                Repeater {
                                    model: modelData.fields
                                    Text {
                                        required property var modelData
                                        width: saveChangeColumn.width - 12
                                        x: 12
                                        text: modelData.label + ": " + (modelData.from || "∅") + " → " + (modelData.to || "∅")
                                        color: root.dimColor
                                        font.pixelSize: 11
                                        font.family: root.monoFont
                                        wrapMode: Text.WrapAnywhere
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: root.dimColor
                    text: root.str.saveNote
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    primary: true
                    text: root.closePending ? root.str.closeAfterSave : root.str.btnSave
                    enabled: root.bridge && root.bridge.canSave
                    onClicked: root.doSave()
                }
                Btn {
                    text: root.str.btnDiscard
                    onClicked: root.doReload()
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: {
                        saveDlg.close();
                        root.closePending = false;
                    }
                }
            }
        }
    }

    // ---- conflict dialog ------------------------------------------------------

    Dialog {
        id: conflictDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(720, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.conflictTitle
                font.pixelSize: 15
                font.bold: true
                color: root.dangerColor
            }
        }

        contentItem: Item {
            implicitHeight: conflictColumn.implicitHeight + 28
            ColumnLayout {
                id: conflictColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: root.inkColor
                    text: root.str.conflictWhy
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(280, 18 * Math.max(1, root.bridge ? root.bridge.conflictDiff.length : 0))
                    clip: true
                    model: root.bridge ? root.bridge.conflictDiff : []
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 18
                        color: modelData.t === "+" ? root.okSoft : modelData.t === "-" ? root.dangerSoft : "transparent"
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: modelData.t + " " + (modelData.s.length ? modelData.s : " ")
                            color: root.inkColor
                            font.pixelSize: 11
                            font.family: root.monoFont
                            elide: Text.ElideRight
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: root.dimColor
                    text: root.str.conflictNote
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    text: root.str.btnReload
                    onClicked: root.doReload()
                }
                Btn {
                    danger: true
                    text: root.str.btnForce
                    onClicked: root.doForceSave()
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: {
                        if (root.bridge)
                            root.bridge.closeConflict();
                        conflictDlg.close();
                        root.closePending = false;
                    }
                }
            }
        }
    }

    // ---- delete dialog --------------------------------------------------------

    Dialog {
        id: deleteDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(480, root.width - 40)
        closePolicy: Popup.CloseOnEscape
         property string alias: ""
         property bool batch: false

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.delTitle
                font.pixelSize: 15
                font.bold: true
                color: root.inkColor
            }
        }

        contentItem: Item {
            implicitHeight: delColumn.implicitHeight + 28
            ColumnLayout {
                id: delColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: root.inkColor
                    text: root.str.delBody(deleteDlg.alias)
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: root.dimColor
                    text: root.str.delNote
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                 Btn {
                     danger: true
                     text: root.str.btnDelete
                     onClicked: {
                         if (root.bridge) {
                             if (deleteDlg.batch) {
                                 const count = root.bridge.deleteConnections(root.selectedConns);
                                 root.selectedConns = [];
                                 root.showToast(count ? root.str.toastDeleted : root.str.errReadOnly, count === 0);
                             } else {
                                 root.bridge.toggleDelete();
                             }
                         }

                         deleteDlg.close();
                     }
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: deleteDlg.close()
                }
            }
        }
    }

    Dialog {
        id: keyDeleteDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(500, root.width - 40)
        closePolicy: Popup.CloseOnEscape
        property var files: []
        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.keyDeleteTitle
                color: root.dangerColor
                font.pixelSize: 15
                font.bold: true
            }
        }
        contentItem: Item {
            implicitHeight: keyDeleteColumn.implicitHeight + 24
            ColumnLayout {
                id: keyDeleteColumn
                anchors.fill: parent
                anchors.margins: 14
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.str.keyDeleteBody(keyDeleteDlg.files)
                    color: root.inkColor
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }
        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                Item { Layout.fillWidth: true }
                Btn {
                    danger: true
                    text: root.str.keyDelete
                    onClicked: {
                        const result = root.bridge.deleteKeys(root.selectedKeys);
                        keyDeleteDlg.close();
                        if (result.code === "") {
                            root.selectedKeys = [];
                            root.showToast(root.str.keyDeleteSuccess(result.files), false);
                        } else {
                            root.showToast(result.detail || result.code, true);
                        }
                    }
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: keyDeleteDlg.close()
                }
            }
        }
    }

    Dialog {
        id: knownDeleteDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(520, root.width - 40)
        closePolicy: Popup.CloseOnEscape
        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.deleteSelected
                color: root.dangerColor
                font.pixelSize: 15
                font.bold: true
            }
        }
        contentItem: Item {
            implicitHeight: knownDeleteText.implicitHeight + 28
            Text {
                id: knownDeleteText
                anchors.fill: parent
                anchors.margins: 14
                text: root.str.knownHostWarning
                color: root.warnColor
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                Item { Layout.fillWidth: true }
                Btn {
                    danger: true
                    text: root.str.deleteSelected
                    onClicked: {
                        knownDeleteDlg.close();
                        root.deleteKnownHosts();
                    }
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: knownDeleteDlg.close()
                }
            }
        }
    }

    // ---- keygen dialog --------------------------------------------------------

    Dialog {
        id: keygenDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(520, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.keygenTitle
                font.pixelSize: 15
                font.bold: true
                color: root.inkColor
            }
        }

        contentItem: Item {
            implicitHeight: kgColumn.implicitHeight + 28
            ColumnLayout {
                id: kgColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 6

                Text {
                    Layout.fillWidth: true
                    visible: root.bridge && !root.bridge.keygenAvailable
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: root.dangerColor
                    text: root.str.keygenUnavailable
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: root.str.kgName
                        color: root.dimColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 116
                    }
                    Field {
                        id: kgName
                        Layout.fillWidth: true
                        implicitHeight: tokens.inputHeight
                        font.pixelSize: 12
                        font.family: root.monoFont
                        placeholderText: "id_ed25519"
                    }
                }
                Text {
                    Layout.leftMargin: 126
                    text: root.bridge ? root.str.kgNameHint(root.bridge.sshDir) : ""
                    color: root.dimColor
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: root.str.kgType
                        color: root.dimColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 116
                    }
                    ComboBox {
                        id: kgType
                        Layout.fillWidth: true
                        implicitHeight: tokens.inputHeight
                        font.pixelSize: 12
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: "Ed25519", value: "ed25519" },
                            { text: "RSA 2048", value: "rsa2048" },
                            { text: "RSA 4096", value: "rsa4096" },
                            { text: "ECDSA 256", value: "ecdsa256" },
                        ]
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: root.str.kgComment
                        color: root.dimColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 116
                    }
                    Field {
                        id: kgComment
                        Layout.fillWidth: true
                        implicitHeight: tokens.inputHeight
                        font.pixelSize: 12
                        placeholderText: "me@win11"
                    }
                }
                Text {
                    Layout.leftMargin: 126
                    text: root.str.kgCommentHint
                    color: root.dimColor
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: root.str.kgPass
                        color: root.dimColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 116
                    }
                    Field {
                        id: kgPass
                        Layout.fillWidth: true
                        implicitHeight: tokens.inputHeight
                        font.pixelSize: 12
                        echoMode: TextInput.Password
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: root.str.kgPass2
                        color: root.dimColor
                        font.pixelSize: 11
                        Layout.preferredWidth: 116
                    }
                    Field {
                        id: kgPass2
                        Layout.fillWidth: true
                        implicitHeight: tokens.inputHeight
                        font.pixelSize: 12
                        echoMode: TextInput.Password
                    }
                }
                Text {
                    Layout.leftMargin: 126
                    text: root.str.kgPassHint
                    color: root.dimColor
                    font.pixelSize: 10
                }

                Text {
                    id: kgErrors
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    wrapMode: Text.WordWrap
                    color: root.dangerColor
                    font.pixelSize: 11
                    text: ""
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    primary: true
                    text: root.str.btnGenerate
                    enabled: root.bridge && root.bridge.keygenAvailable
                    onClicked: root.generateKey()
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: keygenDlg.close()
                }
            }
        }

        onOpened: {
            kgName.text = "";
            kgComment.text = "";
            kgPass.text = "";
            kgPass2.text = "";
            kgType.currentIndex = 0;
            kgErrors.text = "";
            kgName.forceActiveFocus();
        }
    }

    // ---- close with unsaved edits ----------------------------------------------

    Dialog {
        id: closeDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(460, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        onClosed: {
            if (root.closePending) {
                root.closePending = false;
                root.forceClose = false;
            }
        }

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.closeTitle
                font.pixelSize: 15
                font.bold: true
                color: root.inkColor
            }
        }

        contentItem: Item {
            implicitHeight: closeColumn.implicitHeight + 28
            ColumnLayout {
                id: closeColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: root.inkColor
                    text: root.str.closeBody(root.bridge ? root.bridge.dirtyCount : 0)
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    primary: true
                    text: root.str.btnSave
                    enabled: root.bridge && root.bridge.canSave
                    onClicked: {
                        closeDlg.close();
                        root.requestSave();
                    }
                }
                Btn {
                    danger: true
                    text: root.str.closeDiscard
                    onClicked: {
                        closeDlg.close();
                        root.closePending = false;
                        root.forceClose = true;
                        root.close();
                    }
                }
                Btn {
                    text: root.str.btnCancel
                    onClicked: {
                        closeDlg.close();
                        root.closePending = false;
                    }
                }
            }
        }
    }

    // ---- info dialog ----------------------------------------------------------

    Dialog {
        id: infoDlg
        parent: root.contentItem
        anchors.centerIn: parent
        modal: true
        padding: 0
        width: Math.min(560, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        header: Rectangle {
            implicitHeight: 44
            color: root.panelColor
            Text {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: root.str.infoTitle
                font.pixelSize: 15
                font.bold: true
                color: root.inkColor
            }
        }

        contentItem: Item {
            implicitHeight: infoColumn.implicitHeight + 28
            ColumnLayout {
                id: infoColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 14
                spacing: 6
                Repeater {
                    model: root.str.infoLines
                    Text {
                        required property var modelData
                        Layout.fillWidth: true
                        text: "• " + modelData
                        color: root.inkColor
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Item { Layout.fillWidth: true }
                Btn {
                    text: root.str.infoClose
                    onClicked: infoDlg.close()
                }
            }
        }
    }

    onClosing: (close) => {
        if (!root.forceClose && root.bridge && root.bridge.dirty) {
            close.accepted = false;
            root.closePending = true;
            closeDlg.open();
        }
    }
}
