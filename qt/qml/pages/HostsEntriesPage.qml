import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

SplitView {
    id: page
    objectName: "hostsPage"
    required property var bridge
    required property var theme
    readonly property var s: bridge.state
    orientation: Qt.Vertical
    function focusFilter() { entries.focusFilter() }
    EntryList {
        id: entries
        bridge: page.bridge; theme: page.theme
        SplitView.fillHeight: true
        SplitView.minimumHeight: 180
    }
    ColumnLayout {
        SplitView.preferredHeight: 140
        SplitView.minimumHeight: 130
        spacing: 6
        StatusBanner { theme: page.theme; visible: !!s.detail.section || !s.hostsWritable; text: !s.hostsWritable ? bridge.trText(bridge.state.zh, "只读：需要目标文件写入权限。", "Read-only: write access to the target file is required.") : bridge.trText(bridge.state.zh, "注释和未知行保留原文。可在“完整原文”中编辑。", "Comments and opaque lines retain their original text. Edit them in Full text.") }
        RowLayout {
            EditField { id: ip; bridge: page.bridge; theme: page.theme; rowId: s.currentId; fieldName: "ip"; label: "IP"; modelText: s.detail.ip || ""; readOnly: !s.detail.writable || s.busy; Layout.preferredWidth: 220 }
            EditField { bridge: page.bridge; theme: page.theme; rowId: s.currentId; fieldName: "names"; label: bridge.trText(bridge.state.zh, "主机名（空格分隔）", "Hostnames (separated by spaces)"); modelText: s.detail.names || ""; readOnly: !s.detail.writable || s.busy; Layout.fillWidth: true }
        }
        EditField { bridge: page.bridge; theme: page.theme; rowId: s.currentId; fieldName: "comment"; label: bridge.trText(bridge.state.zh, "行尾注释", "Inline comment"); modelText: s.detail.comment || ""; readOnly: !s.detail.writable || s.busy; Layout.fillWidth: true }
        Label { text: bridge.trText(bridge.state.zh, "Ctrl 点选 · Shift 连选 · Ctrl+A 全选当前筛选结果 · Space 批量启停", "Ctrl / Shift to select · Ctrl+A selects visible rows · Space toggles selected entries"); color: theme.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
    }
}
