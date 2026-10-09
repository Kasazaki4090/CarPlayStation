pragma Singleton
import QtQuick

QtObject {
    id: theme

    // Mode: "dark" or "light" (synced with host system theme)
    property bool isDark: true

    // Backgrounds
    readonly property color background: isDark ? "#121214" : "#F4F4F6"
    readonly property color surface: isDark ? "#1C1C1E" : "#FFFFFF"
    readonly property color surfaceVariant: isDark ? "#2C2C2E" : "#E5E5EA"
    readonly property color surfaceElevated: isDark ? "#3A3A3C" : "#D1D1D6"
    readonly property color overlayBackground: isDark ? Qt.rgba(0, 0, 0, 0.75) : Qt.rgba(255, 255, 255, 0.75)

    // Primary Accents
    readonly property color primary: "#0A84FF"
    readonly property color primaryHover: "#0071E3"
    readonly property color success: "#30D158"
    readonly property color warning: "#FF9F0A"
    readonly property color danger: "#FF453A"
    readonly property color dangerHover: "#D70015"

    // Text & Content
    readonly property color textPrimary: isDark ? "#FFFFFF" : "#000000"
    readonly property color textSecondary: isDark ? "#8E8E93" : "#6C6C70"
    readonly property color textTertiary: isDark ? "#636366" : "#8E8E93"
    readonly property color textOnPrimary: "#FFFFFF"

    // Borders & Dividers
    readonly property color border: isDark ? Qt.rgba(255, 255, 255, 0.12) : Qt.rgba(0, 0, 0, 0.12)
    readonly property color divider: isDark ? Qt.rgba(255, 255, 255, 0.08) : Qt.rgba(0, 0, 0, 0.08)

    // Typography
    readonly property string fontFamily: "Segoe UI, -apple-system, Roboto, sans-serif"
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeNormal: 14
    readonly property int fontSizeMedium: 16
    readonly property int fontSizeLarge: 20
    readonly property int fontSizeHeader: 24

    // Radii & Metrics
    readonly property int radiusSmall: 6
    readonly property int radiusMedium: 10
    readonly property int radiusLarge: 16
    readonly property int radiusPill: 9999

    // Padding & Margins
    readonly property int spaceSmall: 8
    readonly property int spaceMedium: 16
    readonly property int spaceLarge: 24
}

