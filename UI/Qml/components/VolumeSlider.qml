import QtQuick
import QtQuick.Templates as T
import "../theme"

Item {
    id: root

    property string label: "Volume"
    property alias value: sliderControl.value
    property alias from: sliderControl.from
    property alias to: sliderControl.to
    property alias stepSize: sliderControl.stepSize
    signal valueModified(real newValue)

    implicitWidth: 260
    implicitHeight: 48

    Column {
        anchors.fill: parent
        spacing: 4

        Row {
            width: parent.width
            Text {
                text: root.label
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }
            Item { width: 1; height: 1; Layout.fillWidth: true }
            Text {
                text: Math.round(sliderControl.value) + "%"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textPrimary
                anchors.right: parent.right
            }
        }

        T.Slider {
            id: sliderControl
            width: parent.width
            height: 24
            from: 0
            to: 100
            stepSize: 1
            value: 80

            onMoved: root.valueModified(value)

            background: Rectangle {
                x: sliderControl.leftPadding
                y: sliderControl.topPadding + sliderControl.availableHeight / 2 - height / 2
                implicitWidth: 200
                implicitHeight: 6
                width: sliderControl.availableWidth
                height: implicitHeight
                radius: 3
                color: Theme.surfaceVariant

                Rectangle {
                    width: sliderControl.visualPosition * parent.width
                    height: parent.height
                    color: Theme.primary
                    radius: 3
                }
            }

            handle: Rectangle {
                x: sliderControl.leftPadding + sliderControl.visualPosition * (sliderControl.availableWidth - width)
                y: sliderControl.topPadding + sliderControl.availableHeight / 2 - height / 2
                implicitWidth: 16
                implicitHeight: 16
                radius: 8
                color: sliderControl.pressed ? Theme.primaryHover : Theme.textOnPrimary
                border.color: Theme.primary
                border.width: 2

                Behavior on scale {
                    NumberAnimation { duration: 100 }
                }
                scale: sliderControl.pressed ? 1.2 : (sliderControl.hovered ? 1.1 : 1.0)
            }
        }
    }
}

