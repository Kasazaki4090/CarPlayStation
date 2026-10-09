import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as C
import "../theme"

Rectangle {
    id: root

    property bool isOpen: false
    property real mediaVolume: 80
    property real navVolume: 85
    property real telephonyVolume: 90

    property int resolutionIndex: 1 // 0: 720p, 1: 1080p, 2: 1440p
    property int fpsIndex: 1        // 0: 30, 1: 60
    property int codecIndex: 0      // 0: H.264, 1: HEVC

    signal closeRequested()
    signal settingsChanged()

    width: 320
    height: parent.height
    color: Theme.surface
    border.color: Theme.border
    border.width: 1

    // Slide-in animation from the right
    x: isOpen ? parent.width - width : parent.width
    Behavior on x { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

    MouseArea {
        // Prevent click-through
        anchors.fill: parent
        onClicked: {}
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        // Drawer Header
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "⚙️ CarPlay Settings"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLarge
                font.bold: true
                color: Theme.textPrimary
            }

            Item { Layout.fillWidth: true }

            IconButton {
                iconText: "✕"
                onClicked: root.closeRequested()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.divider
        }

        C.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: root.width - 40
                spacing: 20

                // Audio Mixer Section
                Text {
                    text: "AUDIO MIXER (WASAPI)"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.primary
                }

                VolumeSlider {
                    Layout.fillWidth: true
                    label: "Media Lane"
                    value: root.mediaVolume
                    onValueModified: function(v) { root.mediaVolume = v; root.settingsChanged(); }
                }

                VolumeSlider {
                    Layout.fillWidth: true
                    label: "Navigation Lane"
                    value: root.navVolume
                    onValueModified: function(v) { root.navVolume = v; root.settingsChanged(); }
                }

                VolumeSlider {
                    Layout.fillWidth: true
                    label: "Telephony / Siri Lane"
                    value: root.telephonyVolume
                    onValueModified: function(v) { root.telephonyVolume = v; root.settingsChanged(); }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.divider
                }

                // Video Section
                Text {
                    text: "VIDEO & STREAMING"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.primary
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "Resolution Preset:"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondary
                    }

                    RowLayout {
                        spacing: 8
                        Repeater {
                            model: ["720p", "1080p", "1440p"]
                            IconButton {
                                property int idx: index
                                implicitWidth: 70
                                implicitHeight: 32
                                iconText: modelData
                                buttonColor: root.resolutionIndex === idx ? Theme.primary : Theme.surfaceVariant
                                iconColor: root.resolutionIndex === idx ? Theme.textOnPrimary : Theme.textPrimary
                                onClicked: { root.resolutionIndex = idx; root.settingsChanged(); }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "Target Frame Rate:"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondary
                    }

                    RowLayout {
                        spacing: 8
                        Repeater {
                            model: ["30 FPS", "60 FPS"]
                            IconButton {
                                property int idx: index
                                implicitWidth: 90
                                implicitHeight: 32
                                iconText: modelData
                                buttonColor: root.fpsIndex === idx ? Theme.primary : Theme.surfaceVariant
                                iconColor: root.fpsIndex === idx ? Theme.textOnPrimary : Theme.textPrimary
                                onClicked: { root.fpsIndex = idx; root.settingsChanged(); }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "Video Codec:"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondary
                    }

                    RowLayout {
                        spacing: 8
                        Repeater {
                            model: ["H.264", "HEVC (H.265)"]
                            IconButton {
                                property int idx: index
                                implicitWidth: 110
                                implicitHeight: 32
                                iconText: modelData
                                buttonColor: root.codecIndex === idx ? Theme.primary : Theme.surfaceVariant
                                iconColor: root.codecIndex === idx ? Theme.textOnPrimary : Theme.textPrimary
                                onClicked: { root.codecIndex = idx; root.settingsChanged(); }
                            }
                        }
                    }
                }
            }
        }
    }
}

