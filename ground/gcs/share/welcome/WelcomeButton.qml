import QtQuick
import QtQuick.Controls.Basic

Button {
    id: button
    property color accent
    property color foreground
    SystemPalette { id: appPalette }
    Accessible.name: text
    padding: 12
    leftPadding: 18; rightPadding: 18
    implicitWidth: Math.max(184, contentItem.implicitWidth + leftPadding + rightPadding)
    hoverEnabled: true
    background: Rectangle {
        radius: 8
        color: button.down ? Qt.darker(button.accent, 1.15) : button.hovered ? Qt.lighter(button.accent, 1.1) : button.accent
        border.width: button.visualFocus ? 2 : 0
        border.color: button.foreground
    }
    contentItem: Text {
        text: button.text
        font.pixelSize: 14
        font.weight: Font.DemiBold
        color: appPalette.highlightedText
        Accessible.ignored: true
    }
}
