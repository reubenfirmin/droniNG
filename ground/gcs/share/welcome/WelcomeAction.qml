import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property string description
    property color foreground
    property color muted
    property color accent
    property color surface
    property color outline
    implicitHeight: Math.max(112, contentItem.implicitHeight + topPadding + bottomPadding)
    implicitWidth: 280
    padding: 20
    hoverEnabled: true
    Accessible.name: text
    Accessible.description: description
    background: Rectangle {
        radius: 12
        color: control.hovered ? Qt.tint(control.surface,
            Qt.rgba(control.accent.r, control.accent.g, control.accent.b, 0.06)) : control.surface
        border.color: control.hovered || control.visualFocus ? control.accent : control.outline
        border.width: control.visualFocus ? 2 : 1
    }
    contentItem: Column {
        spacing: 10
        Text {
            width: parent.width
            text: control.text + "  →"
            color: control.foreground
            font.pixelSize: 18
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
            Accessible.ignored: true
        }
        Text {
            width: parent.width
            text: control.description
            color: control.muted
            font.pixelSize: 14
            wrapMode: Text.WordWrap
            Accessible.ignored: true
        }
    }
}
