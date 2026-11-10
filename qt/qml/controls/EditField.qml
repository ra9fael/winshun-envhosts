import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: field
    required property var bridge
    required property var theme
    property string label: ""
    property string rowId: ""
    property string partId: ""
    property string fieldName: "value"
    property string modelText: ""
    property bool multiline: false
    property bool readOnly: false
    property bool idleCommit: false
    property color textColor: theme.ink
    property color fieldColor: theme.field
    property string token: ""
    property bool syncing: false
    property bool localDirty: false
    property alias editor: input
    spacing: 3
    implicitHeight: (labelItem.visible ? labelItem.implicitHeight + spacing : 0) + (multiline ? 100 : Math.max(theme.inputHeight, input.font.pixelSize + 12))
    function syncText() {
        syncing = true
        let cursor = input.cursorPosition
        input.text = ""
        input.text = bridge.editText(rowId, fieldName, partId)
        input.cursorPosition = Math.min(cursor, input.length)
        localDirty = input.text !== modelText
        syncing = false
    }
    function finish() {
        idle.stop()
        if (!token.length) return true
        bridge.updateEdit(token, input.text, input.inputMethodComposing)
        if (!bridge.finishEdit()) return false
        token = ""
        syncText()
        return true
    }
    function focusEditor(selectText = false) {
        token = readOnly ? "" : bridge.beginEdit(rowId, fieldName, partId)
        syncText()
        input.forceActiveFocus(); input.cursorPosition = input.length
        if (selectText) input.selectAll()
    }
    onRowIdChanged: { token = ""; syncText() }
    onPartIdChanged: { token = ""; syncText() }
    onModelTextChanged: if (!input.activeFocus || !localDirty) syncText()
    Component.onCompleted: syncText()
    Label { id: labelItem; text: field.label; visible: text.length > 0; color: theme.muted; font.pixelSize: theme.secondaryFontSize }
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: field.multiline
        implicitHeight: field.multiline ? 100 : Math.max(theme.inputHeight, input.font.pixelSize + 12)
        color: field.fieldColor
        border.color: input.activeFocus ? theme.accent : theme.control
        border.width: input.activeFocus ? theme.focusWidth : 1
        radius: theme.controlRadius
        ScrollView {
            anchors.fill: parent
            clip: true
            TextArea {
                id: input
                objectName: field.fieldName + "Field"
                property bool editingText: true
                color: field.readOnly ? theme.muted : field.textColor
                selectionColor: theme.accent
                selectedTextColor: theme.onAccent
                font.family: field.fieldName === "name" ? "Segoe UI" : "Consolas"
                font.pixelSize: theme.bodyFontSize
                wrapMode: field.multiline ? TextEdit.Wrap : TextEdit.NoWrap
                selectByMouse: true
                readOnly: field.readOnly
                background: null
                topPadding: 4; bottomPadding: 4; leftPadding: 6; rightPadding: 6
                Accessible.name: field.label || field.fieldName
                onActiveFocusChanged: {
                    if (activeFocus && !readOnly) field.token = bridge.beginEdit(field.rowId, field.fieldName, field.partId)
                    else if (!activeFocus && !field.syncing) field.finish()
                }
                onTextChanged: {
                    if (!activeFocus || field.syncing || readOnly) return
                    if (!field.token.length) field.token = bridge.beginEdit(field.rowId, field.fieldName, field.partId)
                    field.localDirty = text !== field.modelText
                    bridge.updateEdit(field.token, text, inputMethodComposing)
                    if (field.idleCommit && !inputMethodComposing) idle.restart()
                }
                onInputMethodComposingChanged: {
                    bridge.updateEdit(field.token, text, inputMethodComposing)
                    if (!inputMethodComposing && field.idleCommit && field.localDirty) idle.restart()
                }
                Keys.onPressed: event => {
                    if (inputMethodComposing) return
                    if (event.key === Qt.Key_Escape) {
                        bridge.cancelEdit(); field.token = ""; field.syncText(); event.accepted = true
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !field.multiline) {
                        field.finish(); event.accepted = true
                    } else if ((event.key === Qt.Key_Z || event.key === Qt.Key_Y) && (event.modifiers & Qt.ControlModifier) && !field.localDirty) {
                        bridge.history(event.key === Qt.Key_Y || (event.modifiers & Qt.ShiftModifier)); field.syncText(); event.accepted = true
                    } else if (event.key === Qt.Key_Tab && !field.finish()) event.accepted = true
                }
            }
        }
    }
    Timer { id: idle; interval: 400; onTriggered: field.finish() }
    Connections {
        target: bridge
        function onFocusField(row, name, part) {
            if (row === field.rowId && name === field.fieldName && part === field.partId && field.visible) field.focusEditor(true)
        }
    }
}
