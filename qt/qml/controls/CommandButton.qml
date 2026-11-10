import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    required property var theme
    property bool primary: false
    property bool danger: false
    property string reason: ""
    implicitHeight: Math.max(theme.controlMinHeight, contentItem.implicitHeight + 8)
    implicitWidth: contentItem.implicitWidth + 16
    padding: 4
    leftPadding: 8
    rightPadding: 8
    focusPolicy: Qt.StrongFocus
    Accessible.name: text
    Accessible.description: reason
    ToolTip.visible: hovered && reason.length > 0
    ToolTip.text: reason
    contentItem: Text {
        text: control.text
        color: !control.enabled ? theme.muted : control.primary ? theme.onAccent : control.danger ? theme.red : theme.ink
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: theme.controlRadius
        color: !control.enabled ? theme.field : control.primary ? (control.hovered ? theme.accentHover : theme.accent) : control.down || control.hovered ? theme.soft : theme.panel
        border.color: control.activeFocus ? theme.accent : control.danger ? theme.red : theme.control
        border.width: control.activeFocus ? theme.focusWidth : control.primary ? 0 : 1
    }
}
