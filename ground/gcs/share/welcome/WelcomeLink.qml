import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property color accent
    padding: 8
    hoverEnabled: true
    background: Rectangle {
        radius: 6
        color: control.hovered || control.visualFocus
            ? Qt.rgba(control.accent.r, control.accent.g, control.accent.b, 0.08) : "transparent"
        border.color: control.visualFocus ? control.accent : "transparent"
    }
    contentItem: Text {
        text: control.text
        color: control.accent
        font.pixelSize: 14
        verticalAlignment: Text.AlignVCenter
        Accessible.ignored: true
    }
}
