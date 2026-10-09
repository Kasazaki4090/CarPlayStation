import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import "../theme"

Rectangle {
    id: root

    property bool isOpen: false
    property var logEntries: []

    signal closeRequested()
    signal clearRequested()

    width: parent.width * 0.75
    height: parent.height * 0.55
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom
    anchors.bottomMargin: isOpen ? 16 : -height - 20
    radius: Theme.radiusLarge
    color: Theme.surface
    border.color: Theme.border
    border.width: 1

    Behavior on anchors.bottomMargin { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Header
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "📋 In-App Diagnostic Logs"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
                color: Theme.textPrimary
            }

            Item { Layout.fillWidth: true }

            IconButton {
                iconText: "🗑️"
                implicitWidth: 32
                implicitHeight: 32
                onClicked: root.clearRequested()
            }

            IconButton {
                iconText: "✕"
                implicitWidth: 32
                implicitHeight: 32
                onClicked: root.closeRequested()
            }
        }

        // Log Console View
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusSmall
            color: "#08080A"
            border.color: Theme.border
            border.width: 1

            C.ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true

                ListView {
                    id: logListView
                    model: root.logEntries
                    spacing: 4

                    delegate: Text {
                        width: logListView.width
                        text: modelData
                        font.family: "Consolas, 'Cascadia Code', monospace"
                        font.pixelSize: 11
                        color: modelData.indexOf("[WARN]") !== -1 ? Theme.warning :
                               (modelData.indexOf("[ERROR]") !== -1 ? Theme.danger :
                               (modelData.indexOf("[INFO]") !== -1 ? Theme.primary : "#A0A0A0"))
                        wrapMode: Text.WrapAnywhere
                    }

                    onCountChanged: {
                        logListView.positionViewAtEnd()
                    }
                }
            }
        }
    }
}

