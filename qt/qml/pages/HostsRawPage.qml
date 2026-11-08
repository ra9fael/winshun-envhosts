import QtQuick
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: page
    objectName: "hostsRawPage"
    required property var bridge
    required property var theme
    StatusBanner { theme: page.theme; text: bridge.trText(bridge.state.zh, "保留注释、未知行和未修改行的换行格式。切换页面或保存时暂存原文。", "Comments, opaque lines, and unchanged line endings are preserved. Text is staged when leaving this page or saving.") }
    EditField { bridge: page.bridge; theme: page.theme; fieldName: "raw"; modelText: bridge.state.raw; multiline: true; readOnly: !bridge.state.hostsWritable || bridge.state.busy; Layout.fillWidth: true; Layout.fillHeight: true }
}
