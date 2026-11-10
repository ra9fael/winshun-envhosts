import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control
    required property var theme
    property bool editingText: true
    implicitHeight: Math.max(theme.inputHeight, contentHeight + 8)
    implicitWidth: 180
    topPadding: 4; bottomPadding: 4; leftPadding: 6; rightPadding: 6
    selectByMouse: true
    background: Rectangle {
        color: control.theme.field; radius: control.theme.controlRadius
        border.color: control.activeFocus ? control.theme.accent : control.theme.control
        border.width: control.activeFocus ? control.theme.focusWidth : 1
    }
}
