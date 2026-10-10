import QtQuick
import QtQuick.Window
import "theme"
import "components"

Window {
    id: appWindow
    width: 1280
    height: 720
    minimumWidth: 800
    minimumHeight: 480
    visible: true
    title: "CarPlayStation"
    color: Theme.background

    // UI state model properties (bridged from Application::IApplicationApi)
    property bool isStreaming: false
    property string statusText: isStreaming ? "Streaming Active" : "Waiting for iPhone..."
    property string songTitle: ""
    property string artistName: ""
    property int callState: 0 // 0: None, 1: Incoming, 2: Active
    property string callerName: ""
    property string callerNumber: ""

    // Shortcuts
    Shortcut {
        sequence: "F1"
        onActivated: console.log("[Shortcut] Trigger Siri")
    }

    Shortcut {
        sequence: "F2"
        onActivated: settingsDrawer.isOpen = !settingsDrawer.isOpen
    }

    Shortcut {
        sequence: "F3"
        onActivated: console.log("[Shortcut] Accept Call (vehicle button event)")
    }

    Shortcut {
        sequence: "F4"
        onActivated: console.log("[Shortcut] Reject Call (vehicle button event)")
    }

    Shortcut {
        sequence: "Ctrl+F4"
        onActivated: logPanel.isOpen = !logPanel.isOpen
    }

    Shortcut {
        sequence: "F11"
        onActivated: {
            appWindow.visibility = (appWindow.visibility === Window.FullScreen) ? Window.Windowed : Window.FullScreen
        }
    }

    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (settingsDrawer.isOpen) {
                settingsDrawer.isOpen = false
            } else if (logPanel.isOpen) {
                logPanel.isOpen = false
            } else if (welcomeCard.visible) {
                welcomeCard.visible = false
            }
        }
    }

    // 1. CarPlay Video Presentation Surface
    CarPlayVideoSurface {
        id: videoSurface
        anchors.fill: parent
        isStreaming: appWindow.isStreaming
    }

    // 2. HUD Overlay Layer
    Item {
        id: hudOverlay
        anchors.fill: parent

        // Top Status Pill
        StatusPill {
            id: statusPill
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 12
            stateText: appWindow.statusText
            songTitle: appWindow.songTitle
            artistName: appWindow.artistName
            isStreaming: appWindow.isStreaming
            onClicked: settingsDrawer.isOpen = true
        }

        // Top-Right Quick Action Buttons
        Row {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 8

            IconButton {
                iconText: "⚙️"
                onClicked: settingsDrawer.isOpen = !settingsDrawer.isOpen
            }

            IconButton {
                iconText: appWindow.visibility === Window.FullScreen ? "🗗" : "⛶"
                onClicked: {
                    appWindow.visibility = (appWindow.visibility === Window.FullScreen) ? Window.Windowed : Window.FullScreen
                }
            }
        }

        // Center Welcome Card (Visible prior to streaming)
        WelcomeCard {
            id: welcomeCard
            anchors.centerIn: parent
            visible: !appWindow.isStreaming
            onCloseClicked: welcomeCard.visible = false
        }

        // Settings Drawer
        SettingsDrawer {
            id: settingsDrawer
            onCloseRequested: settingsDrawer.isOpen = false
        }

        // Diagnostic Log Panel
        LogPanel {
            id: logPanel
            onCloseRequested: logPanel.isOpen = false
            onClearRequested: logPanel.logEntries = []
        }
    }
}

