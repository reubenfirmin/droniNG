import QtQuick 2.0
import DroniNG.Welcome 1.0

Item {
    id: container
    SystemPalette { id: appPalette }
    width: 100
    height: 62
    property url feedSource: "https://github.com/reubenfirmin/droniNG/commits.atom"

    signal clicked(string url)

    Text {
        id: header
        text: "GitHub Activity"
        width: parent.width
        color: appPalette.windowText
        font {
            pointSize: 14
            weight: Font.Bold
        }
    }

    ListView {
        id: view
        width: parent.width - 2
        anchors { top: header.bottom; topMargin: 14; bottom: parent.bottom }
        model: xmlModel
        delegate: listDelegate
        clip: true
    }

    ScrollDecorator {
        flickableItem: view
    }

    Text {
        anchors.top: header.bottom
        anchors.topMargin: 14
        text: xmlModel.errorString
        color: appPalette.windowText
        visible: text.length > 0
    }

    AtomFeedModel {
        id: xmlModel
        objectName: "newsFeed"
        source: container.feedSource
    }

    Component {
        id: listDelegate
        Item  {
            width: view.width - 2
            height: column.height + 8

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: Qt.openUrlExternally(link)
            }

            Row {
                id: column
                spacing: 4
                Image {
                    id: recipeImage
                    width: 30; height: 30
                    source: thumbnail
                    MouseArea {
                        anchors.fill: parent  //...to cover the whole image
                        onClicked: Qt.openUrlExternally(authoruri)
                    }
                }
                Text {
                    text: author
                    font.italic: true
                    width: view.width * 0.15
                    textFormat: text.indexOf("&") > 0 ? Text.StyledText : Text.PlainText
                    elide: Text.ElideRight
                    color: mouseArea.containsMouse ? appPalette.link : appPalette.text
                    MouseArea {
                        anchors.fill: parent  //...to cover the whole image
                        onClicked: Qt.openUrlExternally(authoruri)
                    }
                }
                Text {
                    text: title.trim()
                    font.bold: true
                    elide: Text.ElideRight
                    color: mouseArea.containsMouse ? appPalette.link : appPalette.text
                    MouseArea {
                        anchors.fill: parent  //...to cover the whole image
                        onClicked: Qt.openUrlExternally(link)
                    }
                }
            }
        }
    }
}
