#pragma once

#include "Application/AppSettings.h"
#include <windows.h>
#include <string>
#include <cstdint>

namespace CarPlayStation::UI {

struct UiPreferences {
    bool safety_disclaimer_dismissed{false};
    bool first_run_completed{false};
    std::wstring config_file_path;
};

class SetupWizard {
public:
    SetupWizard();
    ~SetupWizard() = default;

    /**
     * @brief Load UI preferences from disk.
     */
    void LoadPreferences();

    /**
     * @brief Save UI preferences to disk.
     */
    void SavePreferences();

    /**
     * @brief Load persisted MFi backend settings from disk into the given settings.
     *
     * Only overrides the in-memory defaults for keys that actually exist in the
     * INI, so a fresh install keeps its documented defaults until the user
     * explicitly configures a backend.
     */
    void LoadMFiSettings(Application::AppSettings& settings) const;

    /**
     * @brief Persist the MFi backend settings from the given settings to disk.
     */
    void SaveMFiSettings(const Application::AppSettings& settings) const;

    /**
     * @brief Check whether the safety disclaimer modal should be displayed.
     */
    [[nodiscard]] bool ShouldShowDisclaimer() const noexcept {
        return !preferences_.safety_disclaimer_dismissed;
    }

    /**
     * @brief Check whether first-run MFi configuration wizard should be shown.
     */
    [[nodiscard]] bool ShouldShowMFiWizard(const Application::AppSettings& settings) const noexcept;

    /**
     * @brief Show the safety disclaimer modal dialog.
     * @param parent Window handle of main window.
     * @param headless If true, programmatically accept for testing.
     * @return true if accepted, false if canceled/closed without agreement.
     */
    bool ShowSafetyDisclaimer(HWND parent, bool headless = false);

    /**
     * @brief Show the MFi authentication first-run configuration wizard.
     * @param parent Window handle of main window.
     * @param settings Mutable settings to update with user choice.
     * @param headless If true, programmatically accept for testing.
     * @return true if configured and accepted, false otherwise.
     */
    bool ShowMFiWizard(HWND parent, Application::AppSettings& settings, bool headless = false);

    // --- Accessors ---
    [[nodiscard]] const UiPreferences& GetPreferences() const noexcept { return preferences_; }
    void SetDisclaimerDismissed(bool dismissed) noexcept { preferences_.safety_disclaimer_dismissed = dismissed; }
    void SetFirstRunCompleted(bool completed) noexcept { preferences_.first_run_completed = completed; }

private:
    [[nodiscard]] static std::wstring GetDefaultConfigPath();

    UiPreferences preferences_;
};

} // namespace CarPlayStation::UI
