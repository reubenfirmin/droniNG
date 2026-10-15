import QtQuick 2.0

Item {
    id: welcomeButton
    SystemPalette { id: appPalette }
    width: Math.max(116, icon.width)
    height: icon.height
    z: 0

    property string baseIconName
    property alias label : labelText.text

    signal clicked

    Image {
        id: icon
        source: "images/"+baseIconName+"-off.png"
        anchors.centerIn: parent
    }

    Image {
        id: hoveredIcon
        source: "images/"+baseIconName+"-on.png"
        anchors.centerIn: parent
        opacity: 0
    }

    Rectangle {
        id: labelImage
        property bool displayBelowButton: false
        width: labelText.implicitWidth + 20
        height: labelText.implicitHeight + 12
        radius: 4
        color: appPalette.button
        border.color: appPalette.mid
        opacity: 0
        visible: labelText.text.length > 0 //don't show label bg without text


        anchors.horizontalCenter: parent.horizontalCenter
        y: displayBelowButton ? parent.height-8 : -height+8

        Text {
            id: labelText
            anchors.centerIn: parent

            font {
                weight: Font.DemiBold
                pointSize: 14
            }
            color: appPalette.windowText
        }
    }

    states: State {
        name: "hovered"
        //update the tooltip position before it's displayed,
        //since we don't have a property to bind directly
        StateChangeScript {
            name: "updateLabelPosition"
            script: labelImage.displayBelowButton = welcomeButton.mapToItem(null,0,0).y < labelImage.height
        }
        PropertyChanges { target: hoveredIcon; opacity: 1.0 }
        PropertyChanges { target: labelImage; opacity: 1.0 }
        //raise this button, so tooltip is not obscured by the next items in the grid
        PropertyChanges { target: welcomeButton; z: 1 }
    }

    transitions: Transition {
        from: ""; to: "hovered"; reversible: true
        NumberAnimation { targets: [hoveredIcon, labelImage]; property: "opacity"; duration: 150; easing.type: Easing.InOutQuad }
    }

    MouseArea {
        id: mouseArea
        hoverEnabled: true
        anchors.fill: parent

        onEntered: welcomeButton.state = "hovered"
        onExited: welcomeButton.state = ""
        onClicked: welcomeButton.clicked()
    }
}
