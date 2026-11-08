import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

ColumnLayout {
    id: entries
    required property var bridge
    required property var theme
    readonly property var s: bridge.state
    readonly property var columns: bridge.columns
    property string currentField: s.kind === "env" ? "name" : "ip"
    property string shownScope: ""
    property bool restoring: false
    readonly property var widths: {
        let sizes = columns.map(c => c.width)
        let total = theme.selectionWidth + sizes.reduce((a,b) => a+b,0)
        let extra = Math.max(0,list.width - total - 12)
        let fill = s.kind === "env" ? 1 : 2
        if (sizes.length > fill && !columns[fill].explicit) sizes[fill] += extra
        return sizes
    }
    readonly property real tableWidth: theme.selectionWidth + widths.reduce((a,b) => a+b,0)
    function focusList() { list.focusRows() }
    function focusFilter() { filter.forceActiveFocus(); filter.selectAll() }
    spacing: theme.spacingUnit
    RowLayout {
        Layout.fillWidth: true; visible: s.kind === "env"
        CommandButton { objectName: "userScopeButton"; theme: entries.theme; primary: s.scope === "user"; text: bridge.trText(s.zh,"用户变量","User variables") + (s.userChanges ? " · " + s.userChanges : ""); enabled: !s.busy; onClicked: bridge.setScope("user") }
        CommandButton { objectName: "systemScopeButton"; theme: entries.theme; primary: s.scope === "system"; visible: s.showSystem; text: bridge.trText(s.zh,"系统变量","System variables") + (s.systemChanges ? " · " + s.systemChanges : ""); enabled: !s.busy; onClicked: bridge.setScope("system") }
        Item { Layout.fillWidth: true }
        Label { text: s.scope === "system" ? (s.systemWritable ? bridge.trText(s.zh,"系统范围","System scope") : bridge.trText(s.zh,"系统范围 · 只读","System scope · read-only")) : bridge.trText(s.zh,"用户范围","User scope"); color: theme.muted }
    }
    RowLayout {
        Layout.fillWidth: true
        CompactTextField {
            id: filter; objectName: "filterField"; theme: entries.theme
            Layout.fillWidth: true
            placeholderText: bridge.trText(s.zh,"筛选当前列表 · Ctrl+F","Filter this list · Ctrl+F")
            text: s.filter
            onTextEdited: if (!inputMethodComposing && !bridge.setFilter(text)) text = s.filter
            onInputMethodComposingChanged: if (!inputMethodComposing && !bridge.setFilter(text)) text = s.filter
        }
        Label { text: bridge.trText(s.zh,"选中 %1 / %2","%1 / %2 selected").arg(s.selected).arg(s.count); color: theme.muted }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        readonly property bool canChange: !s.busy && (s.kind === "env" ? (s.scope === "system" ? s.systemWritable : s.userWritable) : s.hostsWritable)
        CommandButton { objectName: "addRowButton"; theme: entries.theme; text: bridge.trText(s.zh,"新增","Add"); enabled: parent.canChange; onClicked: bridge.addRow(s.scope) }
        CommandButton { objectName: "deleteRowsButton"; theme: entries.theme; text: bridge.trText(s.zh,"删除所选","Delete selected"); enabled: s.selected > 0 && parent.canChange; onClicked: bridge.deleteSelected() }
        CommandButton { theme: entries.theme; text: bridge.trText(s.zh,"恢复所选","Restore selected"); enabled: s.selected > 0 && parent.canChange; onClicked: bridge.deleteSelected(false) }
        CommandButton { theme: entries.theme; text: bridge.trText(s.zh,"启用","Enable"); visible: s.kind === "hosts"; enabled: s.selected > 0 && parent.canChange; onClicked: bridge.enableSelected(true) }
        CommandButton { theme: entries.theme; text: bridge.trText(s.zh,"停用","Disable"); visible: s.kind === "hosts"; enabled: s.selected > 0 && parent.canChange; onClicked: bridge.enableSelected(false) }
        Item { Layout.fillWidth: true }
        CommandButton { theme: entries.theme; text: bridge.trText(s.zh,"复制","Copy"); enabled: s.selected > 0; onClicked: bridge.copySelected() }
    }
    ListSurface {
        theme: entries.theme
        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 100
        ColumnHeader { id: header; x: 1 - list.contentX; y: 1; bridge: entries.bridge; theme: entries.theme; columns: entries.columns; widths: entries.widths }
        ListView {
            id: list; objectName: "rowsList"
            function focusRows() {
                const previous = Window.window.activeFocusItem
                if (previous && previous !== list) previous.focus = false
                list.forceActiveFocus()
            }
            anchors.fill: parent; anchors.margins: 1; anchors.topMargin: header.height + 1
            clip: true; model: bridge.rows
            contentWidth: entries.tableWidth
            flickableDirection: Flickable.AutoFlickIfNeeded
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}
            onContentYChanged: if (!entries.restoring && entries.shownScope === s.scope) bridge.rememberScroll(contentY)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) { bridge.moveFocus(event.key === Qt.Key_Up ? -1 : 1,event.modifiers); event.accepted = true }
                else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) { bridge.moveFocus(event.key === Qt.Key_Home ? -s.count : s.count,event.modifiers); event.accepted = true }
                else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) { bridge.selectAll(); event.accepted = true }
                else if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier)) { bridge.copySelected(); event.accepted = true }
                else if (event.key === Qt.Key_Delete) { bridge.deleteSelected(); event.accepted = true }
                else if (event.key === Qt.Key_Insert) { bridge.addRow(s.scope); event.accepted = true }
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_F2) { bridge.activateCell(s.currentId,entries.currentField); event.accepted = true }
                else if (event.key === Qt.Key_Escape) { bridge.clearSelection(); event.accepted = true }
                else if (event.key === Qt.Key_Space && s.kind === "hosts") { bridge.toggleSelected(); event.accepted = true }
            }
            delegate: RowDelegate { width: entries.tableWidth; bridge: entries.bridge; theme: entries.theme; columns: entries.columns; widths: entries.widths; onCellFocused: field => entries.currentField = field }
            Label { anchors.centerIn: parent; visible: list.count === 0; text: s.ready ? (s.filter ? bridge.trText(s.zh,"没有匹配的条目","No matching entries") : bridge.trText(s.zh,"没有可显示的映射或变量","No mappings or variables")) : bridge.trText(s.zh,"正在读取…","Loading…"); color: theme.muted }
            Connections {
                target: bridge
                function onScopeChanging() { entries.restoring = true }
                function onFocusField(row, field, part) {
                    if (part.length) return
                    for (let i = 0; i < list.count; ++i) if (bridge.rows.get(i).id === row) { list.positionViewAtIndex(i,ListView.Contain); break }
                }
                function onChanged() {
                    if (entries.shownScope !== s.scope) {
                        const y = s.scroll
                        entries.restoring = true; entries.shownScope = s.scope
                        Qt.callLater(() => { list.contentY = Math.max(0,Math.min(y,list.contentHeight-list.height)); entries.restoring = false })
                    }
                    for (let i = 0; i < list.count; ++i) if (bridge.rows.get(i).id === s.currentId) { if (list.currentIndex !== i) list.currentIndex = i; break }
                }
            }
        }
    }
}
