import QtQuick

Rectangle {
    id: surface
    required property var theme
    property bool selected: false
    property bool deleted: false
    property bool added: false
    property bool inactive: false
    property bool hovered: false
    property bool focused: false
    color: deleted ? theme.redbg : added ? theme.greenbg : selected ? theme.soft : inactive || hovered ? theme.field : theme.panel
    border.color: selected || focused ? theme.accent : "transparent"
    border.width: focused ? theme.focusWidth : 1
    Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 3; visible: surface.deleted || surface.added; color: surface.deleted ? surface.theme.red : surface.theme.green }
    Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: surface.theme.line; visible: !surface.selected && !surface.focused }
}
