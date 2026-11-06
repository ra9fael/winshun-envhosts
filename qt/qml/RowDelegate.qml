// RowDelegate.qml - one table row: cells joined on one line, section rows span.
import QtQuick
import QtQuick.Layouts

Item {
    id: rowRoot

    required property int index
    required property var model

    property var bridge: null
    property int primaryWidth: 260
    property color inkColor: "#1f1f1f"
    property color dimColor: "#6b6b6b"
    property color accentColor: "#0c447c"
    property color hoverColor: "#eef3fa"

    readonly property int rowKind: model.kind
    readonly property var cells: model.cells
    readonly property bool current: ListView.isCurrentItem

    implicitHeight: rowKind === 1 ? 26 : 28

    Rectangle {
        anchors.fill: parent
        color: rowRoot.current ? rowRoot.hoverColor : "transparent"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10
        visible: rowRoot.rowKind !== 1

        Repeater {
            model: rowRoot.cells

            Text {
                text: modelData
                color: rowRoot.rowKind === 3 ? rowRoot.dimColor : rowRoot.inkColor
                font.pixelSize: 12
                font.strikeout: rowRoot.rowKind === 3
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                Layout.preferredWidth: index === 0 ? rowRoot.primaryWidth : -1
                Layout.fillWidth: index > 0
                Layout.preferredHeight: 28
            }
        }
    }

    Text {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        visible: rowRoot.rowKind === 1
        text: rowRoot.cells.length > 0 ? rowRoot.cells[0] : ""
        color: rowRoot.accentColor
        font.pixelSize: 12
        font.bold: true
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    MouseArea {
        anchors.fill: parent
        onClicked: {
            if (rowRoot.bridge)
                rowRoot.bridge.selectRow(rowRoot.index)
        }
    }
}
