import QtQuick 2.0
import QtQuick.Layouts

Item {
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Community updates")
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

    GridLayout {
        anchors.fill: parent
        anchors.margins: 24
        columns: parent.width >= 880 ? 3 : 1
        columnSpacing: 24
        rowSpacing: 24
        NewsPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 420
            Layout.preferredHeight: 220
        }
        AutoTunePanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 280
            Layout.preferredHeight: 220
        }
        SitesPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            Layout.preferredHeight: 220
        }
    }
}
