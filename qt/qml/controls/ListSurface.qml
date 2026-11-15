import QtQuick

Rectangle {
    required property var theme
    color: theme.panel
    border.color: theme.line
    radius: theme.cardRadius
    clip: true
}
