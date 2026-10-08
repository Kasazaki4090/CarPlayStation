#pragma once

#include <cstdint>

namespace CarPlayStation::UI {

/**
 * @brief Which appearance the UI should render in.
 *
 * Follows the Windows "Choose your mode" app setting (AppsUseLightTheme).
 * There is no in-app override: the HUD flips with the system by design.
 */
enum class UiTheme : uint8_t {
    Dark = 0,
    Light = 1
};

/**
 * @brief One RGBA colour with components in [0,1].
 *
 * Plain floats on purpose: the Direct2D overlay and the Direct3D 11 letterbox
 * clear both consume the palette, so neither graphics API leaks into the other.
 */
struct UiColor {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};
};

/**
 * @brief Every colour the Win32 UI paints with, keyed by role.
 *
 * Alpha is part of the colour because the panels are translucent; individual
 * draw calls may still raise or lower it with ID2D1Brush::SetOpacity.
 */
struct UiPalette {
    UiColor text_primary;     // titles, values, active-button labels
    UiColor text_secondary;   // hints, inactive-button labels
    UiColor panel_bg;         // status pill, drawer, cards, log panel fill
    UiColor accent;           // active buttons, slider fill, section headers
    UiColor status_green;     // streaming / OK indicator
    UiColor status_orange;    // pairing / warning indicator
    UiColor slider_track;     // unfilled track, inactive button fill, borders
    UiColor slider_thumb;     // slider knob
    UiColor log_error;        // Error level log lines
    UiColor letterbox;        // D3D11 backbuffer clear (bars around the video)
};

class ThemeHelper {
public:
    /**
     * @brief Read the current Windows app light/dark setting.
     *
     * HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize with an
     * HKLM fallback, mirroring SystemPermissionHelper. Defaults to Dark when the
     * value is missing or unreadable (the historical appearance of this UI).
     */
    static UiTheme CurrentTheme();

    /**
     * @brief The colour set for a theme.
     */
    static UiPalette BuildPalette(UiTheme theme);

    /**
     * @brief Short stable name used in log lines.
     */
    static const char* ThemeName(UiTheme theme);
};

} // namespace CarPlayStation::UI
