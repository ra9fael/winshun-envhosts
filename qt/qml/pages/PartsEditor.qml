import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: page
    required property var bridge
    required property var theme
    readonly property var s: bridge.state
    property string editingId: ""
    spacing: 6
    RowLayout {
        Layout.fillWidth: true
        CommandButton { objectName: "addPartButton"; theme: page.theme; text: bridge.trText(bridge.state.zh, "新增项", "Add item"); enabled: !!s.detail.writable && !s.busy; onClicked: bridge.addPart() }
        CommandButton { objectName: "copyPartsButton"; theme: page.theme; text: bridge.trText(s.zh, "复制", "Duplicate"); reason: bridge.trText(s.zh, "复制当前项及选中项，插入当前项下方", "Duplicate the current and selected items below the current item"); enabled: !!s.partCurrent && !!s.detail.writable && !s.busy; onClicked: bridge.duplicateParts() }
        CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "删除所选", "Delete selected"); enabled: s.partSelected > 0 && !!s.detail.writable && !s.busy; onClicked: bridge.deleteSelected(true, true) }
        CommandButton { theme: page.theme; text: bridge.trText(bridge.state.zh, "恢复所选", "Restore selected"); enabled: s.partSelected > 0 && !!s.detail.writable && !s.busy; onClicked: bridge.deleteSelected(false, true) }
        CommandButton { theme: page.theme; text: "↑"; reason: bridge.trText(bridge.state.zh, "上移所选 Alt+↑", "Move selected up Alt+↑"); enabled: s.partSelected > 0 && !!s.detail.writable; onClicked: bridge.moveParts(-1) }
        CommandButton { theme: page.theme; text: "↓"; reason: bridge.trText(bridge.state.zh, "下移所选 Alt+↓", "Move selected down Alt+↓"); enabled: s.partSelected > 0 && !!s.detail.writable; onClicked: bridge.moveParts(1) }
        Item { Layout.fillWidth: true }
        Label { text: bridge.trText(bridge.state.zh, "%1 项有效 · 选中 %2 项", "%1 effective · %2 selected").arg(s.partActive).arg(s.partSelected); color: theme.muted }
    }
    ListSurface {
        theme: page.theme
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 100
        RowLayout {
            id: partHeader
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.margins: 1; height: theme.headerHeight; spacing: 6
            CompactCheckBox {
                theme: page.theme
                objectName: "selectVisibleParts"
                checkState: s.partSelected === 0 ? Qt.Unchecked : s.partSelected === list.count ? Qt.Checked : Qt.PartiallyChecked
                Accessible.name: bridge.trText(s.zh,"全选分隔项","Select all parts")
                onClicked: { if (s.partSelected === list.count) bridge.clearSelection(true); else bridge.selectAll(true) }
            }
            Label { text: "#"; Layout.preferredWidth: 22; color: theme.muted }
            Label { text: bridge.trText(s.zh,"值","Value"); Layout.fillWidth: true; color: theme.muted }
            Label { text: bridge.trText(s.zh,"状态","State"); Layout.preferredWidth: 72; color: theme.muted }
        }
    ListView {
        id: list
        objectName: "partsList"
        function focusRows() {
            const previous = Window.window.activeFocusItem
            if (previous && previous !== list) previous.focus = false
            list.forceActiveFocus()
        }
        property bool partList: true
        anchors.fill: parent
        anchors.margins: 1
        anchors.topMargin: partHeader.height + 1
        clip: true
        model: bridge.parts
        spacing: 0
        ScrollBar.vertical: ScrollBar {}
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                if (event.modifiers & Qt.AltModifier) bridge.moveParts(event.key === Qt.Key_Up ? -1 : 1)
                else bridge.moveFocus(event.key === Qt.Key_Up ? -1 : 1, event.modifiers, true)
                event.accepted = true
            } else if (event.key === Qt.Key_Delete) { bridge.deleteSelected(true, true); event.accepted = true }
            else if (event.key === Qt.Key_Insert) { bridge.addPart(); event.accepted = true }
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_F2) {
                for (let i = 0; i < list.count; ++i) {
                    const part = bridge.parts.get(i)
                    if (part.id === s.partCurrent && part.writable && bridge.finishEdit()) page.editingId = part.id
                }
                event.accepted = true
            }
            else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) { bridge.selectAll(true); event.accepted = true }
            else if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier)) { bridge.copySelected(true); event.accepted = true }
            else if (event.key === Qt.Key_Escape) { bridge.clearSelection(true); event.accepted = true }
        }
        delegate: RowSurface {
            id: item
            function startEditing() {
                if (!record.writable) return
                page.editingId = record.id
                Qt.callLater(() => editor.focusEditor(true))
            }
            theme: page.theme
            required property var record
            required property int index
            width: list.width
            objectName: "part_" + record.id
            height: Math.max(theme.rowHeight, partLabel.implicitHeight + 10)
            deleted: record.deleted
            added: record.isNew
            selected: record.selected
            focused: record.focused && list.activeFocus
            Accessible.role: Accessible.ListItem
            Accessible.name: record.value
            Accessible.selected: record.selected
            MouseArea {
                anchors.fill: parent
                onClicked: mouse => { if (bridge.selectCell(record.id, mouse.modifiers, true)) { page.editingId = ""; list.focusRows() } }
                onDoubleClicked: { if (record.writable && bridge.selectCell(record.id, 0, true)) page.editingId = record.id }
            }
            RowLayout {
                anchors.fill: parent; anchors.rightMargin: 6; spacing: 6
                CompactCheckBox { theme: page.theme; checked: record.selected; Accessible.name: bridge.trText(bridge.state.zh, "选择项", "Select item"); onClicked: bridge.select(record.id, Qt.ControlModifier, true) }
                Label { text: record.deleted ? "−" : record.isNew ? "+" : String(index + 1); color: record.deleted ? theme.red : record.isNew ? theme.green : theme.muted; Layout.preferredWidth: 22 }
                Label { id: partLabel; visible: page.editingId !== record.id; Layout.fillWidth: true; text: record.value || bridge.trText(bridge.state.zh, "（空项）", "(empty item)"); color: record.deleted ? theme.red : record.isNew ? theme.green : theme.ink; font.family: "Consolas"; font.strikeout: record.deleted; elide: Text.ElideRight }
                EditField {
                    id: editor
                    visible: page.editingId === record.id
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.inputHeight
                    bridge: page.bridge; theme: page.theme; rowId: record.row; partId: record.id
                    modelText: record.value; readOnly: !record.writable
                    textColor: record.isNew ? theme.green : theme.ink
                    fieldColor: record.isNew ? theme.greenbg : theme.field
                    onVisibleChanged: if (visible) Qt.callLater(focusEditor)
                }
                Label { visible: record.deleted || record.isNew; text: record.deleted ? bridge.trText(s.zh,"− 待删除","− Deleted") : bridge.trText(s.zh,"+ 新增","+ Added"); color: record.deleted ? theme.red : theme.green }
            }
        }
    }
    }
    RowLayout {
        objectName: "partsDelimiterControls"
        Label { text: bridge.trText(bridge.state.zh, "分隔符", "Delimiter"); color: theme.muted }
        CompactComboBox {
            id: delimiterBox
            theme: page.theme
            model: [";", ",", ":", "|", "\\t", bridge.trText(bridge.state.zh, "空格", "Space"), bridge.trText(bridge.state.zh, "自定义", "Custom")]
            currentIndex: [";", ",", ":", "|", "\t", " "].indexOf(s.delimiter) < 0 ? 6 : [";", ",", ":", "|", "\t", " "].indexOf(s.delimiter)
            enabled: !!s.detail.writable && !s.busy
            onActivated: if (currentIndex < 6) bridge.setDelimiter(currentIndex === 4 ? "\t" : currentIndex === 5 ? " " : currentText)
        }
        CompactTextField {
            theme: page.theme
            visible: delimiterBox.currentIndex === 6
            text: s.delimiter
            placeholderText: bridge.trText(bridge.state.zh, "输入后按 Enter", "Enter delimiter and press Enter")
            onAccepted: bridge.setDelimiter(text)
        }
        Label { text: bridge.trText(bridge.state.zh, "双击 / F2 编辑 · Ctrl 点选 · Shift 连选", "Double-click / F2 to edit · Ctrl / Shift to select"); color: theme.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
    }
    Connections {
        target: bridge
        function onFocusField(row, field, part) {
            if (!part.length) return
            page.editingId = part
            Qt.callLater(() => {
                list.forceLayout()
                for (let i = 0; i < list.count; ++i) if (bridge.parts.get(i).id === part) {
                    list.positionViewAtIndex(i, ListView.Contain)
                    const delegate = list.itemAtIndex(i)
                    if (delegate) delegate.startEditing()
                    break
                }
            })
        }
    }
}
