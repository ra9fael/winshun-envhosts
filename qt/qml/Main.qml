// Main.qml - root of the Qt Quick editor prototype (read-only slice).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: root

    property var bridge: null

    readonly property bool dark: bridge ? bridge.dark : false
    readonly property color bgColor: dark ? "#1f1f1f" : "#f6f6f6"
    readonly property color panelColor: dark ? "#2a2a2a" : "#ffffff"
    readonly property color inkColor: dark ? "#eaeaea" : "#1f1f1f"
    readonly property color dimColor: dark ? "#a0a0a0" : "#6b6b6b"
    readonly property color ruleColor: dark ? "#3c3c3c" : "#dcdcdc"
    readonly property color accentColor: dark ? "#7fb6ef" : "#0c447c"
    readonly property color hoverColor: dark ? "#333333" : "#eef3fa"
    readonly property int primaryWidth: bridge ? bridge.primaryWidth : 260

    width: 940
    height: 620
    visible: false
    title: bridge ? bridge.title : ""
    color: bgColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 46
            color: root.panelColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12

                Text {
                    text: root.bridge ? root.bridge.title : ""
                    color: root.inkColor
                    font.pixelSize: 15
                    font.bold: true
                    Layout.fillWidth: true
                }

                TextField {
                    Layout.preferredWidth: 260
                    placeholderText: qsTr("筛选")
                    text: root.bridge ? root.bridge.filter : ""
                    onTextEdited: {
                        if (root.bridge)
                            root.bridge.filter = text
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.ruleColor
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 30
            color: root.panelColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                Repeater {
                    model: root.bridge ? root.bridge.columns : []

                    Text {
                        text: modelData
                        color: root.dimColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        Layout.preferredWidth: index === 0 ? root.primaryWidth : -1
                        Layout.fillWidth: index > 0
                        Layout.preferredHeight: 30
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.ruleColor
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.bridge ? root.bridge.rows : null
            ScrollBar.vertical: ScrollBar {}

            delegate: RowDelegate {
                width: list.width
                bridge: root.bridge
                primaryWidth: root.primaryWidth
                inkColor: root.inkColor
                dimColor: root.dimColor
                accentColor: root.accentColor
                hoverColor: root.hoverColor
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: list.count === 0
                color: root.bridge && root.bridge.errorText !== "" ? "#c0392b" : root.dimColor
                text: root.bridge && root.bridge.errorText !== "" ? root.bridge.errorText
                                                                 : qsTr("没有匹配的行")
            }
        }

        Connections {
            target: root.bridge

            function onCurrentRowChanged() {
                list.currentIndex = root.bridge ? root.bridge.currentIndex : -1
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.ruleColor
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 190
            color: root.panelColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Text {
                    text: root.bridge ? root.bridge.detailTitle : ""
                    color: root.accentColor
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Repeater {
                    model: root.bridge ? root.bridge.detailFields : []

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            text: modelData.label
                            color: root.dimColor
                            font.pixelSize: 12
                            Layout.preferredWidth: 72
                        }

                        Text {
                            text: modelData.value
                            color: root.inkColor
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.ruleColor
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 26
            color: root.panelColor

            Text {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: root.bridge ? root.bridge.statusLine : ""
                color: root.dimColor
                font.pixelSize: 12
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
