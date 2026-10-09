import QtQuick
import QtQuick.Layouts
import "../theme"

Rectangle {
    id: root

    property string accessoryName: "CarPlayStation"
    property string bluetoothState: "Pairing Active"
    property string wifiState: "Hotspot Ready"
    property bool isStreaming: false

    signal closeClicked()

    implicitWidth: 420
    implicitHeight: 280
    radius: Theme.radiusLarge
    color: Theme.surface
    border.color: Theme.border
    border.width: 1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        // Header Row
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "🚗 Wireless CarPlay"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLarge
                font.bold: true
                color: Theme.textPrimary
            }

            Item { Layout.fillWidth: true }

            IconButton {
                iconText: "✕"
                iconPixelSize: 14
                implicitWidth: 30
                implicitHeight: 30
                onClicked: root.closeClicked()
            }
        }

        Text {
            text: "To start, open Settings → General → CarPlay on your iPhone and select this PC:"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeNormal
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // Accessory Badge
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: Theme.radiusMedium
            color: Theme.surfaceVariant
            border.color: Theme.border
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.margins: 14

                Text {
                    text: "Accessory Name:"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                Text {
                    text: root.accessoryName
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMedium
                    font.bold: true
                    color: Theme.primary
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: "📶 Ready"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.success
                }
            }
        }

        // Status Indicators
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Text {
                text: "Bluetooth: " + root.bluetoothState
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }

            Text {
                text: "•"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textTertiary
            }

            Text {
                text: "Wi-Fi: " + root.wifiState
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }
        }

        Item { Layout.fillHeight: true }
    }
}

