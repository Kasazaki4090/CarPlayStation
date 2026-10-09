import QtQuick
import QtQuick.Layouts
import "../theme"

Rectangle {
    id: root

    property string callerName: "Unknown Caller"
    property string callerNumber: ""
    property int callState: 0 // 0: None, 1: Incoming, 2: Active, 3: Held
    property bool bannerVisible: callState !== 0

    signal acceptTriggered()
    signal declineTriggered()

    implicitWidth: 380
    implicitHeight: 72
    radius: Theme.radiusLarge
    color: Theme.surface
    border.color: Theme.border
    border.width: 1

    opacity: bannerVisible ? 1.0 : 0.0
    scale: bannerVisible ? 1.0 : 0.85
    visible: opacity > 0.0

    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        // Call State Icon Avatar
        Rectangle {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            radius: 22
            color: root.callState === 1 ? Qt.rgba(48/255, 209/255, 88/255, 0.2) : Theme.surfaceVariant

            Text {
                anchors.centerIn: parent
                text: root.callState === 1 ? "📞" : "📱"
                font.pixelSize: 20
            }
        }

        // Caller Details
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Text {
                text: root.callState === 1 ? "Incoming Call" : "Active Call"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }

            Text {
                text: root.callerName.length > 0 ? root.callerName : (root.callerNumber.length > 0 ? root.callerNumber : "CarPlay Call")
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
                color: Theme.textPrimary
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        // Action Buttons
        RowLayout {
            spacing: 8

            // Accept Call Button (visible when incoming)
            IconButton {
                visible: root.callState === 1
                iconText: "📞"
                iconPixelSize: 18
                buttonColor: Theme.success
                hoverColor: Qt.lighter(Theme.success, 1.1)
                pressColor: Qt.darker(Theme.success, 1.1)
                customRadius: 18
                implicitWidth: 38
                implicitHeight: 38
                onClicked: root.acceptTriggered()
            }

            // Decline / End Call Button
            IconButton {
                iconText: "✖"
                iconPixelSize: 16
                buttonColor: Theme.danger
                hoverColor: Theme.dangerHover
                pressColor: Qt.darker(Theme.danger, 1.2)
                customRadius: 18
                implicitWidth: 38
                implicitHeight: 38
                onClicked: root.declineTriggered()
            }
        }
    }
}

