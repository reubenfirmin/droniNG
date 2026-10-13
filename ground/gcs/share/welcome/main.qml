import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: page
    width: 1024
    height: 768
    SystemPalette { id: appPalette }
    SystemPalette { id: mutedPalette; colorGroup: SystemPalette.Disabled }
    color: appPalette.window
    property color foreground: appPalette.windowText
    property color muted: mutedPalette.windowText
    property color accent: appPalette.highlight
    property color surface: appPalette.button
    property color outline: appPalette.mid

    ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: content.implicitHeight + 64
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
            id: content
            x: (scroll.availableWidth - width) / 2
            y: 32
            width: Math.min(1000, scroll.availableWidth - 64)
            spacing: 28

            RowLayout {
                width: parent.width
                spacing: 20
                Image {
                    source: "qrc:/core/gcs_logo_64"
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 64
                    smooth: true
                    Accessible.ignored: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: qsTr("Welcome to DroniNG")
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                        color: page.foreground
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        text: qsTr("Modern drone configuration")
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                        color: page.muted
                        font.pixelSize: 15
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                }
            }

            UsbSetupCard {
                width: parent.width
                setup: usbAccessSetup
                controllerConnected: welcomePlugin.controllerConnected
                controllerSetupRequired: welcomePlugin.controllerSetupRequired
                onConnectRequested: welcomePlugin.openPage("Firmware")
                onSetupRequested: welcomePlugin.launchSetupWizard()
                onConfigureRequested: welcomePlugin.openPage("Configuration")
                foreground: page.foreground
                muted: page.muted
                accent: page.accent
                surface: page.surface
                outline: page.outline
            }

            Column {
                width: parent.width
                spacing: 14
                Text {
                    text: qsTr("Your workspaces")
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    color: page.foreground
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                GridLayout {
                    id: modeButtons
                    objectName: "modeButtons"
                    property var modeNames: []
                    width: parent.width
                    columns: width >= 760 ? 3 : 1
                    columnSpacing: 16
                    rowSpacing: 12

                    WelcomeAction {
                        text: qsTr("Map")
                        description: qsTr("View your aircraft position and GPS map.")
                        foreground: page.foreground; muted: page.muted
                        accent: page.accent; surface: page.surface; outline: page.outline
                        Layout.fillWidth: true
                        visible: modeButtons.modeNames.indexOf("Map") >= 0
                        Layout.fillHeight: true
                        onClicked: welcomePlugin.openPage("Map")
                    }
                    WelcomeAction {
                        text: qsTr("Configuration")
                        description: qsTr("Set up controls, sensors and aircraft settings.")
                        foreground: page.foreground; muted: page.muted
                        accent: page.accent; surface: page.surface; outline: page.outline
                        Layout.fillWidth: true
                        visible: modeButtons.modeNames.indexOf("Configuration") >= 0
                        Layout.fillHeight: true
                        onClicked: welcomePlugin.openPage("Configuration")
                    }
                    WelcomeAction {
                        text: qsTr("Firmware")
                        description: qsTr("Connect your controller and manage its firmware.")
                        foreground: page.foreground; muted: page.muted
                        accent: page.accent; surface: page.surface; outline: page.outline
                        Layout.fillWidth: true
                        visible: modeButtons.modeNames.indexOf("Firmware") >= 0
                        Layout.fillHeight: true
                        onClicked: welcomePlugin.openPage("Firmware")
                    }
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    WelcomeLink {
                        text: qsTr("System")
                        accent: page.accent
                        visible: modeButtons.modeNames.indexOf("System") >= 0
                        onClicked: welcomePlugin.openPage("System")
                    }
                    WelcomeLink {
                        text: qsTr("Scopes")
                        accent: page.accent
                        visible: modeButtons.modeNames.indexOf("Scopes") >= 0
                        onClicked: welcomePlugin.openPage("Scopes")
                    }
                    WelcomeLink {
                        text: qsTr("Advanced")
                        accent: page.accent
                        visible: modeButtons.modeNames.indexOf("Advanced") >= 0
                        onClicked: welcomePlugin.openPage("Advanced")
                    }
                    WelcomeLink {
                        text: qsTr("Vehicle setup wizard")
                        accent: page.accent
                        visible: modeButtons.modeNames.length > 0 && welcomePlugin.hasAction("SetupWizardPlugin.ShowSetupWizard")
                        onClicked: welcomePlugin.triggerAction("SetupWizardPlugin.ShowSetupWizard")
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: page.outline }

            Flow {
                width: parent.width
                spacing: 12
                WelcomeLink {
                    text: qsTr("Documentation ↗")
                    accent: page.muted
                    onClicked: Qt.openUrlExternally("https://github.com/reubenfirmin/droniNG#readme")
                }
                WelcomeLink {
                    id: communityToggle
                    text: checked ? qsTr("Hide community & updates") : qsTr("Community & updates")
                    accent: page.muted
                    checkable: true
                }
            }
            Loader {
                width: parent.width
                height: width >= 880 ? 300 : 740
                active: communityToggle.checked
                visible: active
                source: "CommunityPanel.qml"
            }
        }
    }
}
