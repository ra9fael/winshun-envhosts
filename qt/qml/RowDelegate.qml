import QtQuick
import QtQuick.Controls
import "controls"

RowSurface {
    id: delegate
    required property var record
    required property int index
    required property var bridge
    required property var columns
    required property var widths
    signal cellFocused(string field)
    objectName: "row_" + record.id
    height: Math.max(theme.rowHeight, metrics.height + 10)
    selected: record.selected
    deleted: record.deleted
    added: record.state === "added"
    inactive: bridge.state.kind === "hosts" && record.enabled === false
    focused: record.focused && ListView.view.activeFocus
    Accessible.role: Accessible.ListItem
    Accessible.name: record.c1 + ", " + record.c2 + ", " + record.c3
    Accessible.selected: record.selected
    Accessible.focusable: true
    Accessible.onPressAction: bridge.selectCell(record.id)
    Text { id: metrics; visible: false; text: "Ag中"; font.pixelSize: theme.bodyFontSize }
    Row {
        CompactCheckBox {
            objectName: "rowSelection_" + record.id
            theme: delegate.theme; width: delegate.theme.selectionWidth; height: delegate.height
            checked: record.selected
            Accessible.name: bridge.trText(bridge.state.zh,"选择 ","Select ") + record.c1
            onClicked: bridge.select(record.id,Qt.ControlModifier)
        }
        Repeater {
            model: delegate.columns.length
            Item {
                id: cell
                required property int index
                readonly property string field: delegate.columns[index].key
                readonly property bool stateCell: field === "state"
                readonly property bool enabledCell: field === "enabled"
                readonly property bool badge: enabledCell || (stateCell && record.state !== "clean")
                readonly property color tone: record.deleted ? delegate.theme.red : enabledCell ? (record.enabled ? delegate.theme.green : delegate.theme.amber) : record.state === "added" ? delegate.theme.green : delegate.theme.amber
                objectName: "cell_" + record.id + "_" + field
                width: delegate.widths[index]; height: delegate.height
                Rectangle {
                    x: 6; y: (parent.height - height) / 2
                    width: Math.min(label.implicitWidth + 10, parent.width - 12); height: label.implicitHeight + 2
                    radius: 3; visible: cell.badge
                    color: record.deleted ? delegate.theme.redbg : cell.enabledCell && record.enabled || record.state === "added" ? delegate.theme.greenbg : delegate.theme.amberbg
                }
                Label {
                    id: label
                    anchors.fill: parent; anchors.leftMargin: cell.badge ? 11 : 7; anchors.rightMargin: 6
                    verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                    text: cell.stateCell ? (record.deleted ? bridge.trText(bridge.state.zh,"− 待删除","− Deleted") : record.state === "added" ? bridge.trText(bridge.state.zh,"+ 新增","+ Added") : record.state === "modified" ? bridge.trText(bridge.state.zh,"● 已修改","● Modified") : record.duplicate ? bridge.trText(bridge.state.zh,"! 重复","! Duplicate") : !record.writable ? bridge.trText(bridge.state.zh,"只读","Read-only") : "—") : cell.enabledCell ? (record.deleted ? "—" : record.enabled ? bridge.trText(bridge.state.zh,"● 启用","● Enabled") : bridge.trText(bridge.state.zh,"Ⅱ 停用","Ⅱ Disabled")) : record[cell.field]
                    color: cell.badge ? cell.tone : record.deleted ? delegate.theme.red : delegate.added ? delegate.theme.green : delegate.theme.ink
                    font.strikeout: record.deleted && !cell.stateCell && !cell.enabledCell
                    font.family: ["value","ip","names"].indexOf(cell.field) >= 0 ? "Consolas" : (bridge.state.zh ? "Microsoft YaHei UI" : "Segoe UI")
                }
                MouseArea {
                    anchors.fill: parent; hoverEnabled: true
                    onEntered: delegate.hovered = true
                    onExited: delegate.hovered = false
                    onClicked: mouse => { if (bridge.selectCell(record.id,mouse.modifiers)) { delegate.cellFocused(cell.field); delegate.ListView.view.focusRows() } }
                    onDoubleClicked: bridge.activateCell(record.id,cell.field)
                }
            }
        }
    }
}
