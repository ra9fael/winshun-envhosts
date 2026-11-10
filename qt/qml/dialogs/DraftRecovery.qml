import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

RowLayout {
    id: recovery
    required property var bridge
    required property var theme
    CommandButton { theme: recovery.theme; primary: true; text: bridge.trText(bridge.state.zh, "恢复草稿", "Restore draft"); onClicked: bridge.respond("restore") }
    CommandButton { theme: recovery.theme; danger: true; text: bridge.trText(bridge.state.zh, "丢弃草稿", "Discard draft"); onClicked: bridge.respond("discard") }
}
