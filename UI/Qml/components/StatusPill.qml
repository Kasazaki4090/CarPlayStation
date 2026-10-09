import QtQuick
import "../theme"

Rectangle {
    id: root

    property string stateText: "CarPlay Ready"
    property string songTitle: ""
    property string artistName: ""
    property bool isStreaming: false
    property bool isNightMode: true

    signal clicked()

    implicitWidth: contentRow.implicitWidth + 24
    implicitHeight: 34
    radius: Theme.radiusPill
    color: Theme.surfaceVariant
    border.color: Theme.border
    border.width: 1

    Behavior on color { ColorAnimation { duration: 150 } }

    Row {
        id: contentRow
        anchors.centerIn: parent
        spacing: 8

        // Status Indicator Dot
        Rectangle {
            width: 8
            height: 8
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: root.isStreaming ? Theme.success : Theme.warning

            SequentialAnimation on opacity {
                running: !root.isStreaming
                loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 0.3; duration: 800 }
                NumberAnimation { from: 0.3; to: 1.0; duration: 800 }
            }
        }

        // Connection State
        Text {
            text: root.stateText
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            font.bold: true
            color: Theme.textPrimary
            anchors.verticalCenter: parent.verticalCenter
        }

        // Now Playing separator and text (if present)
        Item {
            visible: root.songTitle.length > 0
            width: 1
            height: 12
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.fill: parent
                color: Theme.divider
            }
        }

        Text {
            visible: root.songTitle.length > 0
            text: "🎵 " + root.songTitle + (root.artistName.length > 0 ? " — " + root.artistName : "")
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
            elide: Text.ElideRight
            maximumLineCount: 1
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}

