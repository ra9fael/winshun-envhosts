import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"
import "pages"
import "dialogs"

ApplicationWindow {
    id: root
    objectName: "editorWindow"
    required property var bridge
    readonly property var s: bridge.state
    DesignTokens { id: tokens; dark: s.dark }
    width: tokens.windowWidth
    height: tokens.windowHeight
    minimumWidth: tokens.minimumWidth
    minimumHeight: tokens.minimumHeight
    title: (s.dirty ? "● " : "") + s.title + " · EnvHosts"
    visible: false
    color: tokens.bg
    font.family: s.zh ? "Microsoft YaHei UI" : "Segoe UI"
    font.pixelSize: tokens.bodyFontSize
    palette.window: tokens.bg
    palette.windowText: tokens.ink
    palette.base: tokens.field
    palette.text: tokens.ink
    palette.button: tokens.panel
    palette.buttonText: tokens.ink
    palette.highlight: tokens.accent
    palette.highlightedText: tokens.onAccent
    palette.placeholderText: tokens.muted
    palette.mid: tokens.control
    palette.dark: tokens.control
    palette.light: tokens.panel
    palette.brightText: tokens.ink
    onClosing: close => { close.accepted = bridge.requestClose() }
    onActiveChanged: bridge.setActive(active)
    function composing() { return !!(activeFocusItem && activeFocusItem.inputMethodComposing) }
    Shortcut { sequence: "Ctrl+S"; enabled: !root.composing() && !s.busy && !s.dialog; onActivated: bridge.requestSave() }
    Shortcut { sequence: "Ctrl+F"; enabled: !root.composing() && !s.dialog; onActivated: { if (pageLoader.item && pageLoader.item.focusFilter) pageLoader.item.focusFilter() } }
    Shortcut { sequence: "F5"; enabled: !root.composing() && !s.busy && !s.dialog; onActivated: bridge.requestReload() }
    FocusScope {
        anchors.fill: parent
        focus: true
        Keys.onPressed: event => {
            if (root.composing()) return
            if ((event.key === Qt.Key_Z || event.key === Qt.Key_Y) && (event.modifiers & Qt.ControlModifier)) { bridge.history(event.key === Qt.Key_Y || !!(event.modifiers & Qt.ShiftModifier)); event.accepted = true }
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: tokens.pageMargin
            spacing: tokens.spacingUnit
            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    Label { text: s.title; font.pixelSize: tokens.headingFontSize; font.bold: true; color: tokens.ink }
                    Label { text: s.pending ? bridge.trText(bridge.state.zh, "输入尚未暂存", "Input pending") : s.dirty ? bridge.trText(bridge.state.zh, "工作副本有未保存修改", "Working copy has unsaved changes") : bridge.trText(bridge.state.zh, "与目标一致", "In sync with target"); color: s.dirty ? tokens.amber : tokens.muted }
                }
                Item { Layout.fillWidth: true }
                CommandButton { objectName: "undoButton"; theme: tokens; text: bridge.trText(bridge.state.zh, "撤销", "Undo"); enabled: s.canUndo && !s.busy && !s.pending; onClicked: bridge.history() }
                CommandButton { theme: tokens; text: bridge.trText(bridge.state.zh, "重做", "Redo"); enabled: s.canRedo && !s.busy && !s.pending; onClicked: bridge.history(true) }
                CommandButton { theme: tokens; text: bridge.trText(bridge.state.zh, "重新载入", "Reload"); enabled: !s.busy; onClicked: bridge.requestReload() }
                CommandButton { objectName: "saveButton"; theme: tokens; primary: true; text: s.writing ? bridge.trText(bridge.state.zh, "正在保存…", "Saving…") : bridge.trText(bridge.state.zh, "保存全部修改", "Save all changes"); enabled: s.ready && s.dirty && !s.busy; onClicked: bridge.requestSave() }
            }
            RowLayout {
                CommandButton { theme: tokens; primary: s.page === s.kind; text: s.kind === "env" ? bridge.trText(bridge.state.zh, "变量", "Variables") : bridge.trText(bridge.state.zh, "条目", "Entries"); enabled: !s.busy; onClicked: bridge.navigate(s.kind) }
                CommandButton { theme: tokens; visible: s.kind === "hosts"; primary: s.page === "raw"; text: bridge.trText(bridge.state.zh, "完整原文", "Full text"); enabled: !s.busy; onClicked: bridge.navigate("raw") }
                CommandButton { theme: tokens; primary: s.page === "backup"; text: bridge.trText(bridge.state.zh, "备份", "Backups"); enabled: !s.busy; onClicked: bridge.navigate("backup") }
                Item { Layout.fillWidth: true }
                BusyIndicator { running: s.busy; visible: running; implicitWidth: 24; implicitHeight: 24 }
                Label { text: bridge.trText(bridge.state.zh, "%1 项变更", "%1 changes").arg(s.changeCount); color: tokens.muted }
            }
            StatusBanner { theme: tokens; visible: !!s.error; error: true; text: s.error }
            StatusBanner { theme: tokens; visible: s.conflict; text: bridge.trText(bridge.state.zh, "目标已被外部修改；保存时需核对冲突。", "The target changed externally. Review conflicts before saving.") }
            StatusBanner { theme: tokens; visible: s.warnings.length > 0; text: s.warnings.join(" · ") }
            Loader {
                id: pageLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: s.page === "env" ? environment : s.page === "hosts" ? hosts : s.page === "raw" ? raw : backups
            }
            Label { objectName: "windowStatus"; text: s.notice || bridge.trText(bridge.state.zh, "修改自动暂存到工作副本 · 保存前可撤销", "Edits stage in the working copy · Undo before saving"); Layout.fillWidth: true; wrapMode: Text.Wrap; color: tokens.muted; font.pixelSize: tokens.secondaryFontSize }
        }
    }
    Component { id: environment; EnvironmentPage { bridge: root.bridge; theme: tokens } }
    Component { id: hosts; HostsEntriesPage { bridge: root.bridge; theme: tokens } }
    Component { id: raw; HostsRawPage { bridge: root.bridge; theme: tokens } }
    Component { id: backups; BackupsPage { bridge: root.bridge; theme: tokens } }
    Dialog {
        id: modal
        objectName: "stateDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(root.width - 64, 930)
        height: Math.min(root.height - 64, s.dialog === "review" || s.dialog === "conflict" ? 580 : 290)
        modal: true
        visible: !!s.dialog
        title: s.dialogTitle
        closePolicy: Popup.NoAutoClose
        contentItem: ColumnLayout {
            Keys.onEscapePressed: bridge.respond("cancel")
            Label { text: s.dialogText; wrapMode: Text.Wrap; Layout.fillWidth: true; color: tokens.ink }
            SaveReview { bridge: root.bridge; theme: tokens; visible: s.dialog === "review"; Layout.fillWidth: true; Layout.fillHeight: true }
            ConflictReview { bridge: root.bridge; theme: tokens; visible: s.dialog === "conflict"; Layout.fillWidth: true; Layout.fillHeight: true }
            Item { Layout.fillHeight: true; visible: s.dialog !== "review" && s.dialog !== "conflict" }
            RowLayout {
                Layout.fillWidth: true
                DraftRecovery { bridge: root.bridge; theme: tokens; visible: s.dialog === "draft" }
                CommandButton { theme: tokens; visible: s.dialog === "conflict"; text: bridge.trText(bridge.state.zh, "重新载入", "Reload"); onClicked: bridge.respond("reload") }
                CommandButton { theme: tokens; danger: true; visible: s.dialog === "conflict"; text: bridge.trText(bridge.state.zh, "覆盖并复核", "Overwrite and review"); onClicked: bridge.respond("overwrite") }
                CommandButton { theme: tokens; visible: s.dialog === "close"; primary: true; text: bridge.trText(bridge.state.zh, "保存", "Save"); onClicked: bridge.respond("save") }
                CommandButton { theme: tokens; visible: s.dialog === "close"; text: bridge.trText(bridge.state.zh, "保留草稿并关闭", "Keep draft and close"); onClicked: bridge.respond("draft") }
                CommandButton { theme: tokens; danger: true; visible: s.dialog === "close"; text: bridge.trText(bridge.state.zh, "丢弃修改", "Discard edits"); onClicked: bridge.respond("discard") }
                Item { Layout.fillWidth: true }
                CommandButton { objectName: "dialogCancel"; theme: tokens; text: s.dialog === "info" ? bridge.trText(bridge.state.zh,"关闭","Close") : bridge.trText(bridge.state.zh, "取消", "Cancel"); onClicked: bridge.respond("cancel") }
                CommandButton { objectName: "dialogConfirm"; theme: tokens; primary: true; visible: ["review", "normalize", "restore", "reload", "discardClose", "deleteBackup"].indexOf(s.dialog) >= 0; text: s.dialog === "review" ? bridge.trText(bridge.state.zh, "确认保存", "Confirm save") : bridge.trText(bridge.state.zh, "确认", "Confirm"); onClicked: bridge.respond("confirm") }
            }
        }
    }
}
