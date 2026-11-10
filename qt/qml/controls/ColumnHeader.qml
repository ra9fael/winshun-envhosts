import QtQuick
import QtQuick.Controls.Basic

Row {
    id: header
    required property var bridge
    required property var theme
    required property var columns
    required property var widths
    height: theme.headerHeight
    Rectangle {
        width: header.theme.selectionWidth; height: header.height; color: header.theme.field
        CompactCheckBox {
            theme: header.theme; anchors.fill: parent
            objectName: "selectVisibleRows"
            checkState: bridge.state.selected === 0 ? Qt.Unchecked : bridge.state.selected === bridge.state.count ? Qt.Checked : Qt.PartiallyChecked
            Accessible.name: bridge.trText(bridge.state.zh,"全选当前可见行","Select visible rows")
            onClicked: { if (bridge.state.selected === bridge.state.count) bridge.clearSelection(); else bridge.selectAll() }
        }
    }
    Repeater {
        model: header.columns.length
        Rectangle {
            id: cell
            required property int index
            readonly property var column: header.columns[index]
            width: header.widths[index]; height: header.height
            color: header.theme.field
            Label {
                anchors.fill: parent; anchors.leftMargin: 7; anchors.rightMargin: 10
                verticalAlignment: Text.AlignVCenter
                text: cell.column.title + (bridge.state.sortKey === cell.column.key ? (bridge.state.sortDescending ? " ↓" : " ↑") : "")
                elide: Text.ElideRight; color: header.theme.muted
            }
            MouseArea {
                objectName: "columnCaption_" + cell.column.key
                anchors.fill: parent; anchors.rightMargin: 9
                onClicked: captionClick.restart()
                onDoubleClicked: { captionClick.stop(); bridge.sortColumn(cell.column.key) }
            }
            Timer { id: captionClick; interval: Qt.styleHints.mouseDoubleClickInterval; onTriggered: bridge.sortColumn(cell.column.key) }
            Item {
                id: grip
                objectName: "columnGrip_" + cell.column.key
                width: 9; height: parent.height; anchors.right: parent.right
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: cell.column.title + bridge.trText(bridge.state.zh,"列宽；左右键调整，Enter 适应内容"," column width; arrows resize, Enter fits contents")
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) { bridge.resizeColumn(cell.column.key, cell.width + (event.key === Qt.Key_Left ? -1 : 1) * (event.modifiers & Qt.ShiftModifier ? 20 : 4)); bridge.saveColumns(); event.accepted = true }
                    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { bridge.autoSizeColumn(cell.column.key, header.fontForColumn(cell.column.key)); event.accepted = true }
                }
                Rectangle { anchors.centerIn: parent; width: grip.activeFocus ? 2 : 1; height: parent.height - 10; color: grip.activeFocus ? header.theme.accent : header.theme.control }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.SizeHorCursor
                    property real originX: 0
                    property real originWidth: 0
                    onPressed: mouse => { if (!bridge.finishEdit()) { mouse.accepted = false; return } originX = mapToItem(header,mouse.x,mouse.y).x; originWidth = cell.width; grip.forceActiveFocus() }
                    onPositionChanged: mouse => { if (pressed) bridge.resizeColumn(cell.column.key, originWidth + mapToItem(header,mouse.x,mouse.y).x - originX) }
                    onReleased: bridge.saveColumns()
                    onDoubleClicked: bridge.autoSizeColumn(cell.column.key, header.fontForColumn(cell.column.key))
                }
            }
        }
    }
    function fontForColumn(key) { return key === "value" || key === "ip" || key === "names" ? mono.font : ui.font }
    Text { id: mono; visible: false; font.family: "Consolas"; font.pixelSize: header.theme.bodyFontSize }
    Text { id: ui; visible: false; font.family: bridge.state.zh ? "Microsoft YaHei UI" : "Segoe UI"; font.pixelSize: header.theme.bodyFontSize }
}
