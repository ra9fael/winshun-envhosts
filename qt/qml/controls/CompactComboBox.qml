import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control
    required property var theme
    implicitHeight: Math.max(theme.inputHeight, contentItem.implicitHeight + 8)
    implicitWidth: Math.max(100, contentItem.implicitWidth + 32)
    padding: 4; leftPadding: 6; rightPadding: 24
    contentItem: Text {
        text: control.displayText; font: control.font
        color: control.enabled ? control.theme.ink : control.theme.muted
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    indicator: Text {
        x: control.width - width - 8; y: (control.height - height) / 2
        text: "⌄"; color: control.theme.muted; font.pixelSize: 14
    }
    background: Rectangle {
        color: control.theme.field; radius: control.theme.controlRadius
        border.color: control.activeFocus ? control.theme.accent : control.theme.control
        border.width: control.activeFocus ? control.theme.focusWidth : 1
    }
    delegate: ItemDelegate {
        required property var model
        required property int index
        width: control.popup.width
        height: Math.max(control.theme.rowHeight, implicitContentHeight + 8)
        padding: 4; leftPadding: 8; rightPadding: 8
        text: control.textRole ? model[control.textRole] : model.modelData
        highlighted: control.highlightedIndex === index
    }
}
