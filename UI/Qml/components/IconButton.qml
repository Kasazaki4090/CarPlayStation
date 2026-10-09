import QtQuick
import QtQuick.Templates as T
import "../theme"

T.Button {
    id: control

    property string iconText: ""
    property color buttonColor: "transparent"
    property color hoverColor: Theme.surfaceVariant
    property color pressColor: Theme.surfaceElevated
    property color iconColor: Theme.textPrimary
    property int iconPixelSize: 16
    property int customRadius: Theme.radiusMedium

    implicitWidth: 36
    implicitHeight: 36

    background: Rectangle {
        implicitWidth: control.implicitWidth
        implicitHeight: control.implicitHeight
        radius: control.customRadius
        color: control.down ? control.pressColor : (control.hovered ? control.hoverColor : control.buttonColor)
        border.color: control.visualFocus ? Theme.primary : "transparent"
        border.width: control.visualFocus ? 2 : 0

        Behavior on color {
            ColorAnimation { duration: 120 }
        }
    }

    contentItem: Text {
        text: control.iconText
        font.family: Theme.fontFamily
        font.pixelSize: control.iconPixelSize
        color: control.iconColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}

