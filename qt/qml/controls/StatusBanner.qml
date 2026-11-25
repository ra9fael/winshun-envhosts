import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: banner
    Layout.fillWidth: true
    required property var theme
    property string text: ""
    property bool error: false
    // Only standing hints set this; an error banner has nothing to dismiss.
    property bool closable: false
    property string closeLabel: "Close"

    signal closed()

    implicitHeight: Math.max(message.implicitHeight, closeButton.visible ? closeButton.implicitHeight : 0) + 20
    color: error ? theme.redbg : theme.amberbg
    radius: theme.controlRadius

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        Label {
            id: message
            Layout.fillWidth: true
            color: banner.error ? theme.red : theme.amber
            text: banner.text
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Button {
            id: closeButton
            visible: banner.closable
            text: "×"
            implicitWidth: 20
            implicitHeight: 20
            padding: 0
            Accessible.name: banner.closeLabel
            contentItem: Text {
                text: closeButton.text
                color: closeButton.hovered ? banner.theme.amber : banner.theme.muted
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: banner.theme.controlRadius
                color: closeButton.hovered ? banner.theme.panel : "transparent"
            }
            onClicked: banner.closed()
        }
    }
}
