import QtQuick
import "../theme"

Item {
    id: root

    property int streamWidth: 1280
    property int streamHeight: 720
    property bool isStreaming: false

    // Rect where video is actually rendered within client bounds (preserving aspect ratio)
    readonly property real streamAspect: streamWidth > 0 && streamHeight > 0 ? streamWidth / streamHeight : 16 / 9
    readonly property real viewportAspect: width > 0 && height > 0 ? width / height : 16 / 9

    readonly property real renderWidth: viewportAspect > streamAspect ? height * streamAspect : width
    readonly property real renderHeight: viewportAspect > streamAspect ? height : width / streamAspect
    readonly property real renderX: (width - renderWidth) / 2
    readonly property real renderY: (height - renderHeight) / 2

    // Background letterbox area
    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    // Video Presentation Viewport
    Item {
        id: videoContainer
        x: root.renderX
        y: root.renderY
        width: root.renderWidth
        height: root.renderHeight

        // Placeholder for QQuickItem / QQuickFramebufferObject backed by libmpv or D3D11 shared texture
        Rectangle {
            anchors.fill: parent
            color: root.isStreaming ? "#000000" : Qt.darker(Theme.surface, 1.2)
            border.color: root.isStreaming ? "transparent" : Theme.border
            border.width: root.isStreaming ? 0 : 1

            Text {
                visible: !root.isStreaming
                anchors.centerIn: parent
                text: "Waiting for CarPlay Video Stream (" + root.streamWidth + "x" + root.streamHeight + ")"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textSecondary
            }
        }
    }
}

