import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: control
    required property var theme
    implicitWidth: theme.selectionWidth
    implicitHeight: Math.max(theme.rowHeight, contentItem.implicitHeight + 8)
    padding: 0
    spacing: 4
    indicator: Rectangle {
        width: control.theme.checkSize; height: width
        x: (control.width - width) / 2; y: (control.height - height) / 2
        radius: 2
        color: control.checked ? control.theme.accent : control.theme.panel
        border.color: control.activeFocus ? control.theme.accent : control.theme.control
        border.width: control.activeFocus ? 2 : 1
        Text {
            anchors.centerIn: parent
            text: control.checkState === Qt.PartiallyChecked ? "−" : control.checked ? "✓" : ""
            font.pixelSize: 12
            color: control.checked ? control.theme.onAccent : control.theme.ink
        }
    }
    contentItem: Item { implicitWidth: 0; implicitHeight: 0 }
}
