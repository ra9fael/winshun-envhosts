import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ListView {
    id: review
    required property var bridge
    required property var theme
    property bool conflict: false
    objectName: "reviewDiff"
    clip: true
    model: bridge.state.diff
    spacing: 8
    ScrollBar.vertical: ScrollBar {}
    delegate: Rectangle {
        required property var modelData
        width: review.width - 12
        height: content.implicitHeight + 24
        color: theme.field
        radius: theme.controlRadius
        border.color: theme.line
        ColumnLayout {
            id: content
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top; anchors.margins: 12
            Label { text: modelData.name + " · " + modelData.operation; color: theme.ink; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                Repeater {
                    model: review.conflict ? ["before", "after", "mine"] : ["before", "after"]
                    ColumnLayout {
                        required property string modelData
                        readonly property var change: content.parent.modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Label { text: modelData === "before" ? bridge.trText(bridge.state.zh, "基线", "Baseline") : modelData === "mine" ? bridge.trText(bridge.state.zh, "我的修改", "My edits") : review.conflict ? bridge.trText(bridge.state.zh, "外部内容", "External") : bridge.trText(bridge.state.zh, "保存后", "After save"); color: theme.muted }
                        TextArea { text: change[modelData] || ""; readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere; color: theme.ink; font.family: "Consolas"; background: null; Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
