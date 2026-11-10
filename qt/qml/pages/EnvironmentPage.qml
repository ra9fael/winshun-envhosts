import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

SplitView {
    id: page
    objectName: "envPage"
    required property var bridge
    required property var theme
    readonly property var s: bridge.state
    orientation: Qt.Vertical
    function focusFilter() { entries.focusFilter() }
    EntryList {
        id: entries
        bridge: page.bridge; theme: page.theme
        SplitView.preferredHeight: 280
        SplitView.minimumHeight: Math.max(200, implicitHeight)
    }
    ColumnLayout {
        SplitView.fillHeight: true
        SplitView.minimumHeight: Math.max(210, implicitHeight)
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            EditField { id: nameField; bridge: page.bridge; theme: page.theme; rowId: s.currentId; fieldName: "name"; label: bridge.trText(bridge.state.zh, "变量名", "Variable name"); modelText: s.detail.name || ""; readOnly: !s.detail.writable || s.busy; Layout.fillWidth: true }
            ColumnLayout {
                Label { text: bridge.trText(bridge.state.zh, "存储类型", "Storage type"); color: theme.muted }
                CompactComboBox {
                    id: typeField; theme: page.theme
                    objectName: "typeField"
                    Layout.preferredWidth: 150
                    model: ["REG_SZ", "REG_EXPAND_SZ", "REG_MULTI_SZ"]
                    currentIndex: Math.max(0, model.indexOf(s.detail.type))
                    enabled: !!s.detail.writable && !s.busy
                    onActivated: { let token = bridge.beginEdit(s.currentId, "type"); bridge.updateEdit(token, currentText); if (!bridge.finishEdit()) currentIndex = model.indexOf(s.detail.type) }
                }
            }
            ColumnLayout {
                Label { text: s.detail.scope === "system" ? bridge.trText(bridge.state.zh, "系统范围", "System scope") : bridge.trText(bridge.state.zh, "用户范围", "User scope"); color: theme.muted }
                RowLayout {
                    CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "拆分项", "Items"); primary: s.mode === "parts" && s.detail.type !== "REG_MULTI_SZ"; enabled: s.detail.type !== "REG_MULTI_SZ"; onClicked: bridge.setMode("parts") }
                    CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "原始值", "Raw value"); primary: s.mode === "raw" || s.detail.type === "REG_MULTI_SZ"; onClicked: bridge.setMode("raw") }
                }
            }
        }
        StatusBanner { theme: page.theme; visible: !!s.detail.formatError || (!!s.currentId && !s.detail.writable); text: s.detail.formatError || (s.detail.deleted ? bridge.trText(bridge.state.zh, "此变量已标记删除，可恢复。", "This variable is marked for deletion and can be restored.") : bridge.trText(bridge.state.zh, "此变量只读：可查看和复制。", "Read-only variable: viewing and copying are available.")) }
        Label { visible: !!s.currentId; text: String(s.detail.length || 0) + " / 32767 UTF-16" + (s.detail.length > 32767 ? bridge.trText(bridge.state.zh, " · 超出 %1", " · %1 over").arg(s.detail.length - 32767) : ""); color: s.detail.length > 32767 ? theme.red : s.detail.length >= 29491 ? theme.amber : theme.muted }
        PartsEditor { bridge: page.bridge; theme: page.theme; Layout.fillWidth: true; Layout.fillHeight: true; visible: s.mode === "parts" && s.detail.type !== "REG_MULTI_SZ" && !!s.currentId && !s.detail.formatError }
        EditField {
            id: valueField
            objectName: "rawValueEditor"
            bridge: page.bridge; theme: page.theme; rowId: s.currentId
            modelText: s.detail.value || ""; multiline: true; idleCommit: true
            readOnly: !s.detail.writable || s.busy
            visible: s.mode === "raw" || s.detail.type === "REG_MULTI_SZ" || !!s.detail.formatError
            label: s.detail.type === "REG_MULTI_SZ" ? bridge.trText(bridge.state.zh, "每行一个元素；分号保留在元素内", "One element per line; semicolons remain within an element") : bridge.trText(bridge.state.zh, "编辑后自动暂存，保存时写入系统", "Edits stage automatically; Save writes to the target")
            Layout.fillWidth: true; Layout.fillHeight: true
        }
    }
    Connections {
        target: bridge
        function onFocusField(row, field, part) {
            if (part.length || row !== s.currentId) return
            if (field === "type") Qt.callLater(() => { if (typeField.enabled) { typeField.forceActiveFocus(); typeField.popup.open() } })
            else if (field === "value") Qt.callLater(() => valueField.focusEditor(true))
        }
    }
}
