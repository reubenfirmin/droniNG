import QtQuick 2.0

Item {
    id: container
    SystemPalette { id: appPalette }
    width: 100
    height: 62

    signal clicked(string url)

    Text {
        id: header
        text: "droniNG Links"
        width: parent.width
        color: appPalette.windowText
        font {
            pointSize: 14
            weight: Font.Bold
        }
    }

    ListModel {
        id: sitesModel
        ListElement { title: "Documentation"; link: "https://github.com/reubenfirmin/droniNG#readme" }
        ListElement { title: "Issue Tracker"; link: "https://github.com/reubenfirmin/droniNG/issues" }
        ListElement { title: "Releases"; link: "https://github.com/reubenfirmin/droniNG/releases" }
        ListElement { title: "GitHub Repository"; link: "https://github.com/reubenfirmin/droniNG" }
    }

    ListView {
        id: view
        width: parent.width
        anchors { top: header.bottom; topMargin: 14; bottom: parent.bottom }
        model: sitesModel
        spacing: 8
        clip: true

        delegate: Text {
            text: title
            width: view.width
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere

            font {
                pointSize: 12
                weight: Font.Bold
            }

            color: mouseArea.containsMouse ? appPalette.highlight : appPalette.text

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: Qt.openUrlExternally(link)
            }
        }
    }
}
