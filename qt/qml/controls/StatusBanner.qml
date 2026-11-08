import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Layouts

Rectangle {
    Layout.fillWidth: true
    required property var theme
    property string text: ""
    property bool error: false
    implicitHeight: message.implicitHeight + 20
    color: error ? theme.redbg : theme.amberbg
    radius: theme.controlRadius
    Label {
        id: message
        anchors.fill: parent
        anchors.margins: 10
        color: parent.error ? theme.red : theme.amber
        text: parent.text
        wrapMode: Text.Wrap
        Accessible.role: Accessible.AlertMessage
        Accessible.name: text
    }
}
