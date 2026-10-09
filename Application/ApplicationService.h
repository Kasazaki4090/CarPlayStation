#pragma once

#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "Core/Service/ICarPlayCoreApi.h"
#include "Core/Service/CoreEventBus.h"
#include <memory>
#include <mutex>
#include <functional>
#include <vector>

namespace CarPlayStation::Application {

// ============================================================================
// ApplicationService — implements IApplicationApi.
//
// Sits between the UI layer and the Core Service Facade (DEC-002).
// Responsibilities:
//   1. Application state management (AppUiState)
//   2. Command relay & dispatch (UI commands → Core calls)
//   3. Event observation (Core events → UI state updates)
//   4. AppSettings → Core parameter resolution
//
// Thread-safety:
//   - All public methods are thread-safe (mutex-guarded state).
//   - UI state callbacks are invoked outside the lock.
// ============================================================================
class ApplicationService final : public IApplicationApi {
public:
    explicit ApplicationService(std::shared_ptr<Core::Service::ICarPlayCoreApi> core_api);
    ~ApplicationService() override;

    ApplicationService(const ApplicationService&) = delete;
    ApplicationService& operator=(const ApplicationService&) = delete;

    // --- IApplicationApi Implementation ---
    [[nodiscard]] Core::Common::Status StartCarPlay(const AppSettings& settings) override;
    [[nodiscard]] Core::Common::Status ApplySettings(const AppSettings& newSettings) override;
    void StopCarPlay() override;

    void RegisterUiStateCallback(UiStateChangeCallback callback) override;
    void RegisterFrameConsumer(Core::Media::VideoFrameCallback callback) override;
    void RegisterCpuFrameConsumer(Core::Media::CpuFrameCallback callback) override;
    void SetVideoSharedDevice(void* device, void* context) override;

    [[nodiscard]] Core::Common::Status ForwardTouchEvent(
        Core::Input::TouchAction action,
        const Core::Input::TouchPoint& point) override;
    [[nodiscard]] Core::Common::Status ForwardKnobEvent(
        Core::Input::KnobAction action,
        uint32_t stepCount = 1) override;
    [[nodiscard]] Core::Common::Status ForwardButtonEvent(
        Core::Input::ButtonType button,
        bool isPressed) override;

    [[nodiscard]] Core::Common::Status AcceptCall(std::string_view callUuid = {}) override;
    [[nodiscard]] Core::Common::Status EndCall(std::string_view callUuid = {}) override;
    [[nodiscard]] Core::Common::Status TriggerSiri(bool isPressed) override;

    [[nodiscard]] Core::Common::Status SetHostNightMode(bool night_mode) override;

    [[nodiscard]] AppUiState GetCurrentState() const override;

    // --- Additional accessors ---
    [[nodiscard]] std::shared_ptr<Core::Service::ICarPlayCoreApi> GetCoreApi() const { return core_api_; }

private:
    void ApplyMfiSigner(const AppSettings& settings);
    // Resolve the UI's video preferences into the concrete stream spec Core needs.
    static Core::Common::VideoStreamSpec ResolveVideoSpec(const AppSettings& settings);
    // Fill the read-only video facts in a UI state snapshot from Core, so the HUD
    // reports the spec actually in effect instead of the settings' guess. Takes no
    // lock: callers must not hold mutex_ across it.
    void SnapshotVideoFacts(AppUiState& state) const;
    void OnCoreEvent(const Core::Service::CoreEvent& event);
    void NotifyUiState(const AppUiState& state);
    void ResolveSettingsToCore(const AppSettings& settings);

    // Build the MFi signer from the settings' MFi preferences and inject it
    // into the Core service so the selected backend (LocalFile / Network /
    // HardwareI2C) is actually used for MFi authentication.


    // Ask a hardware-backed signer once whether its coprocessor answers on the
    // bus, so a wiring/address problem is visible at startup instead of only
    // after a phone has failed to authenticate. Runs off the calling thread.
    void ProbeMfiHardwareAsync(const std::shared_ptr<Core::Hardware::IMfiSigner>& signer);

    std::shared_ptr<Core::Service::ICarPlayCoreApi> core_api_;
    AppUiState ui_state_;
    AppSettings current_settings_;
    bool is_running_{false};

    // UI state callbacks (invoked outside the lock).
    std::vector<UiStateChangeCallback> ui_state_callbacks_;
    Core::Media::VideoFrameCallback frame_consumer_;
    Core::Media::CpuFrameCallback cpu_frame_consumer_;

    // UI's D3D11 device/context, injected into the video decoder so its shared
    // texture is created on the same device the renderer samples from.
    void* video_shared_device_{nullptr};
    void* video_shared_context_{nullptr};

    // Core event subscription token.
    uint64_t core_event_token_{0};

    mutable std::mutex mutex_;
};

} // namespace CarPlayStation::Application
