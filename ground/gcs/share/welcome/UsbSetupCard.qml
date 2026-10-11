import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: card
    objectName: "usbSetupCard"
    required property var setup
    property color foreground
    property color muted
    property color accent
    property color surface
    property color outline
    property bool expanded: false
    property bool controllerConnected: false
    property bool controllerSetupRequired: false
    signal connectRequested()
    signal setupRequested()
    signal configureRequested()
    readonly property bool needsSetup: !setup.required && controllerConnected && controllerSetupRequired
    readonly property bool hasCommand: setup.command.length > 0
    implicitHeight: details.implicitHeight + 48
    radius: 14
    color: Qt.tint(surface, Qt.rgba(accent.r, accent.g, accent.b, 0.03))
    border.color: Qt.tint(outline, Qt.rgba(accent.r, accent.g, accent.b, 0.3))

    Connections {
        target: card.setup
        function onStatusChanged() { card.expanded = false }
    }

    ColumnLayout {
        id: details
        x: 24; y: 24
        width: parent.width - 48
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Text {
                text: card.needsSetup ? qsTr("Set up controller?")
                    : !card.setup.required && card.controllerConnected ? qsTr("Controller connected") : card.setup.title
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                color: card.foreground
                font.pixelSize: 21
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Rectangle {
                implicitWidth: badge.implicitWidth + 20
                implicitHeight: badge.implicitHeight + 10
                radius: 6
                color: Qt.rgba(card.accent.r, card.accent.g, card.accent.b, 0.1)
                Text {
                    id: badge
                    anchors.centerIn: parent
                    text: card.hasCommand ? qsTr("One-time setup") : card.setup.required ? qsTr("Setup") : qsTr("USB ready")
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    color: card.accent
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: card.needsSetup
                ? qsTr("The setup wizard guides you through your aircraft, receiver and motor output settings.")
                : !card.setup.required && card.controllerConnected
                ? qsTr("Open your controller for tuning, configuration and live diagnostics.") : card.setup.message
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            color: card.muted
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: card.hasCommand
            text: qsTr("Copy the command, run it in a terminal, then reconnect your controller.")
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            color: card.foreground
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }
        Flow {
            Layout.fillWidth: true
            visible: card.hasCommand
            spacing: 12
            WelcomeButton {
                id: copy
                objectName: "copyUsbAccessCommand"
                text: card.setup.copied ? qsTr("Command copied") : qsTr("Copy setup command")
                Accessible.name: text
                Accessible.description: qsTr("Copy the command to install the Linux USB access rules.")
                accent: card.accent
                foreground: card.foreground
                onClicked: card.setup.copyCommand()
            }
            WelcomeLink {
                text: card.expanded ? qsTr("Hide command") : qsTr("View command")
                accent: card.accent
                onClicked: card.expanded = !card.expanded
            }
        }
        WelcomeButton {
            objectName: "welcomeConnectController"
            visible: !card.setup.required
            text: card.needsSetup ? qsTr("Launch setup wizard")
                : card.controllerConnected ? qsTr("Tune & configure") : qsTr("Connect controller")
            accent: card.accent
            foreground: card.foreground
            onClicked: {
                if (card.needsSetup) card.setupRequested()
                else if (card.controllerConnected) card.configureRequested()
                else card.connectRequested()
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(140, command.contentHeight + 28)
            visible: card.hasCommand && card.expanded
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            TextArea {
                id: command
                objectName: "usbAccessCommand"
                text: card.setup.command
                Accessible.name: text
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                padding: 14
                color: card.foreground
                font.family: "monospace"
                font.pixelSize: 12
                background: Rectangle {
                    color: card.surface
                    radius: 8
                    border.color: card.outline
                }
            }
        }
    }
}
