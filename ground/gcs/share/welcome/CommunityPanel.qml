import QtQuick 2.0

Item {
    SystemPalette { id: appPalette }
    width: 900
    height: 300

    Rectangle {
        id: background
        anchors.fill: parent

        radius: 6
        color: appPalette.button
        border.color: appPalette.mid
    }

    NewsPanel {
        id: gitHubNewsPanel
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * 0.5
        anchors.margins: 32
    }

    AutoTunePanel {
        id: autoTunePanel
        anchors.left: gitHubNewsPanel.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * 0.25
        anchors.margins: 32
    }

    //better to use image instead
    Rectangle {
        id: separator
        width: 1
        height: parent.height * 0.7
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: autoTunePanel.right
        anchors.margins: 16
        color: appPalette.mid
    }
    SitesPanel {
        transformOrigin: Item.Center
        anchors.left: autoTunePanel.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 32
    }
}
