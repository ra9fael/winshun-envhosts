// The editor's colour and type scale, from the host's appearance contract.
//
// WinShun sends `appearance.nativeLook` - the style's colours and radii, with
// the user's accent, dark mode and 圆角/直角 already worked out (see
// docs/plugins.md). The editor reads that instead of keeping a palette of its
// own, so it looks like the windows beside it in every 界面风格. The generated
// design tokens stay as the fallback for an older host (or a test harness with
// no appearance), and as the reference the design boards were drawn with.
import QtQuick

QtObject {
    id: root

    // bridge.appearance, or {} in a harness that loads Main.qml directly.
    property var appearance: ({})
    // bridge.state.dark while there is no host look (tests, an older host).
    property bool fallbackDark: false

    readonly property var look: appearance && appearance.nativeLook
        && appearance.nativeLook.background ? appearance.nativeLook : ({})
    readonly property bool hostLook: look.background !== undefined
    readonly property bool dark: hostLook ? !!look.dark : fallbackDark

    readonly property DesignTokens fallback: DesignTokens { dark: root.dark }

    // ---- colours: the host's semantic tokens, fallback palette otherwise
    readonly property color bg: hostLook ? look.background : fallback.bg
    readonly property color panel: hostLook ? look.layer : fallback.panel
    readonly property color field: hostLook ? look.control : fallback.field
    readonly property color ink: hostLook ? look.text : fallback.ink
    readonly property color muted: hostLook ? look.subtext : fallback.muted
    readonly property color line: hostLook ? look.divider : fallback.line
    readonly property color control: hostLook ? look.controlBorder : fallback.control

    // accent: text, lines and focus rings; accentFill: a fill under white text.
    readonly property color accent: hostLook ? look.accentLine : fallback.accent
    readonly property color accentFill: hostLook ? look.accentFill : fallback.accent
    readonly property color onAccent: hostLook ? look.accentText : fallback.onAccent
    readonly property color accentHover: hostLook
        ? (dark ? Qt.lighter(accentFill, 1.12) : Qt.darker(accentFill, 1.12))
        : fallback.accentHover
    // A selected row / hovered button: the accent at a tint, as Theme.accentTint.
    readonly property color soft: hostLook
        ? Qt.rgba(accent.r, accent.g, accent.b, dark ? 0.14 : 0.10)
        : fallback.soft
    readonly property color selection: hostLook ? look.textSelection : fallback.accent

    // The host does not put success/caution in nativeLook (the check and the
    // warning are the same in every style), so the design tokens keep them.
    readonly property color green: fallback.green
    readonly property color greenbg: fallback.greenbg
    readonly property color amber: fallback.amber
    readonly property color amberbg: fallback.amberbg
    readonly property color red: hostLook ? look.danger : fallback.red
    readonly property color redbg: hostLook
        ? Qt.rgba(red.r, red.g, red.b, dark ? 0.18 : 0.10)
        : fallback.redbg

    // ---- radii: already squared by the host's 直角 setting
    readonly property real controlRadius: hostLook ? look.controlRadius : fallback.controlRadius
    readonly property real cardRadius: hostLook ? look.cardRadius : fallback.controlRadius

    // ---- type and metrics: the design's dense table, on the host's 界面大小
    readonly property string uiSize: appearance && appearance.uiSize ? appearance.uiSize : "standard"
    // The host's interface font, when it names one; empty keeps the app font.
    readonly property string uiFontFamily: appearance && appearance.resolvedUiFont
        ? appearance.resolvedUiFont : ""

    readonly property int bodyFontSize: uiSize === "compact" ? 12 : uiSize === "roomy" ? 15 : fallback.bodyFontSize
    readonly property int secondaryFontSize: uiSize === "compact" ? 11 : uiSize === "roomy" ? 13 : fallback.secondaryFontSize
    readonly property int headingFontSize: uiSize === "compact" ? 16 : uiSize === "roomy" ? 20 : fallback.headingFontSize
    readonly property int rowHeight: bodyFontSize + 15
    readonly property int inputHeight: rowHeight
    readonly property int headerHeight: rowHeight
    readonly property int controlMinHeight: bodyFontSize + 13

    // ---- layout values the design contract pins
    readonly property int windowWidth: fallback.windowWidth
    readonly property int windowHeight: fallback.windowHeight
    readonly property int minimumWidth: fallback.minimumWidth
    readonly property int minimumHeight: fallback.minimumHeight
    readonly property int pageMargin: fallback.pageMargin
    readonly property int spacingUnit: fallback.spacingUnit
    readonly property int checkSize: fallback.checkSize
    readonly property int selectionWidth: fallback.selectionWidth
    readonly property int focusWidth: fallback.focusWidth
    readonly property int listMinHeight: fallback.listMinHeight
    readonly property int detailMinHeight: fallback.detailMinHeight
}
