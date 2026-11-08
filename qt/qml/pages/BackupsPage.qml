import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../dialogs"

ColumnLayout {
    id: page
    objectName: "backupsPage"
    required property var bridge
    required property var theme
    readonly property var s: bridge.state
    RowLayout {
        CompactComboBox {
            theme: page.theme
            visible: s.backupAvailableScope === "all"
            model: [bridge.trText(bridge.state.zh,"所有范围","All scopes"),bridge.trText(bridge.state.zh,"仅用户","User only"),bridge.trText(bridge.state.zh,"仅系统","System only")]
            currentIndex: s.backupScope === "user" ? 1 : s.backupScope === "system" ? 2 : 0
            enabled: !s.busy
            onActivated: bridge.setBackupScope(["all","user","system"][currentIndex])
        }
        CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "刷新", "Refresh"); enabled: !s.busy; onClicked: bridge.refreshBackups() }
        CommandButton { theme: page.theme; primary: true; text: bridge.trText(bridge.state.zh, "恢复到工作副本", "Restore into working copy"); enabled: !!s.backupId && !s.busy; onClicked: bridge.restoreBackup() }
        CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "导出", "Export"); enabled: !!s.backupId && !s.busy; onClicked: bridge.exportBackup() }
        CommandButton { theme: page.theme; danger: true; text: bridge.trText(bridge.state.zh, "删除备份", "Delete backup"); enabled: !!s.backupId && !s.busy; onClicked: bridge.deleteBackup() }
    }
    StatusBanner { theme: page.theme; text: bridge.trText(bridge.state.zh, "点击备份预览与当前工作副本的差异。恢复后仍需明确保存。", "Select a backup to compare it with your working copy. Restoring still requires an explicit Save.") }
    SplitView {
        Layout.fillWidth: true; Layout.fillHeight: true
        ListView {
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 240
            clip: true; model: bridge.backups
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var record
                width: ListView.view.width
                text: record.id + "\n" + record.scope + " · " + record.bytes + " B"
                highlighted: s.backupId === record.id
                onClicked: bridge.previewBackup(record.id)
            }
            Label { anchors.centerIn: parent; visible: parent.count === 0; text: bridge.trText(bridge.state.zh, "暂无备份", "No backups yet"); color: theme.muted }
        }
        SaveReview { bridge: page.bridge; theme: page.theme; SplitView.fillWidth: true }
    }
}
