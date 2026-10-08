#include "UI/Win32/VideoAdaptationHelper.h"
#include "UI/Win32/D3D11Renderer.h"
#include "UI/Win32/SystemPermissionHelper.h"
#include "UI/Win32/ThemeHelper.h"
#include "UI/Win32/SetupWizard.h"
#include "UI/Win32/Win32InputRouter.h"
#include "UI/Win32/AppController.h"
#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "Core/Hardware/TouchscreenCapabilities.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cmath>

using namespace CarPlayStation;

namespace {

class MockApplicationApi final : public Application::IApplicationApi {
public:
    struct TouchRecord {
        Core::Input::TouchAction action;
        Core::Input::TouchPoint point;
    };

    struct KnobRecord {
        Core::Input::KnobAction action;
        uint32_t steps;
    };

    struct ButtonRecord {
        Core::Input::ButtonType button;
        bool pressed;
    };

    Core::Common::Status StartCarPlay(const Application::AppSettings& settings) override {
        started_ = true;
        settings_ = settings;
        ui_state_.session_state = Core::Session::SessionState::AirPlayStreaming;
        ui_state_.is_streaming = true;
        NotifyState();
        return Core::Common::Status::Ok();
    }

    Core::Common::Status ApplySettings(const Application::AppSettings& newSettings) override {
        settings_ = newSettings;
        return Core::Common::Status::Ok();
    }

    void StopCarPlay() override {
        started_ = false;
        ui_state_.session_state = Core::Session::SessionState::Idle;
        ui_state_.is_streaming = false;
        NotifyState();
    }

    void RegisterUiStateCallback(Application::UiStateChangeCallback callback) override {
        state_cbs_.push_back(std::move(callback));
    }

    void RegisterFrameConsumer(Core::Media::VideoFrameCallback callback) override {
        frame_consumer_ = std::move(callback);
    }

    void RegisterCpuFrameConsumer(Core::Media::CpuFrameCallback callback) override {
        cpu_frame_consumer_ = std::move(callback);
    }

    // The UI layer hands over its D3D11 device/ctx; the mock only records that it happened.
    void SetVideoSharedDevice(void* device, void* context) override {
        device_ = device;
        context_ = context;
    }

    Core::Common::Status ForwardTouchEvent(
        Core::Input::TouchAction action,
        const Core::Input::TouchPoint& point) override {
        touch_records_.push_back({ action, point });
        return Core::Common::Status::Ok();
    }

    Core::Common::Status ForwardKnobEvent(
        Core::Input::KnobAction action,
        uint32_t stepCount) override {
        knob_records_.push_back({ action, stepCount });
        return Core::Common::Status::Ok();
    }

    Core::Common::Status ForwardButtonEvent(
        Core::Input::ButtonType button,
        bool isPressed) override {
        button_records_.push_back({ button, isPressed });
        return Core::Common::Status::Ok();
    }

    Core::Common::Status SetHostNightMode(bool night_mode) override {
        night_mode_records_.push_back(night_mode);
        return Core::Common::Status::Ok();
    }

    Application::AppUiState GetCurrentState() const override {
        return ui_state_;
    }

    void NotifyState() {
        for (auto& cb : state_cbs_) {
            cb(ui_state_);
        }
    }

    bool started_{false};
    Application::AppSettings settings_;
    Application::AppUiState ui_state_;
    std::vector<Application::UiStateChangeCallback> state_cbs_;
    Core::Media::VideoFrameCallback frame_consumer_;
    Core::Media::CpuFrameCallback cpu_frame_consumer_;
    void* device_{nullptr};
    void* context_{nullptr};

    std::vector<TouchRecord> touch_records_;
    std::vector<KnobRecord> knob_records_;
    std::vector<ButtonRecord> button_records_;
    std::vector<bool> night_mode_records_;
};

} // namespace

// ============================================================================
// Test 1: VideoAdaptationHelper
// ============================================================================
void TestVideoAdaptationHelper() {
    std::cout << "[Test 1] VideoAdaptationHelper: stream adaptation strategy... ";

    Application::VideoPreferences prefs;
    // Default baseline check: 720P, 30 FPS, H264
    assert(prefs.resolution_preset == Application::VideoResolutionPreset::Res720P);
    assert(prefs.fps == 30);
    assert(prefs.preferred_codec == Application::VideoCodecPreference::H264);

    auto decision = UI::VideoAdaptationHelper::ResolveStreamParameters(prefs, 1280, 720);
    assert(decision.width == 1280);
    assert(decision.height == 720);
    assert(decision.fps == 30);
    assert(decision.codec == Core::Common::VideoCodec::H264);

    // Custom 1080P with HEVC
    prefs.resolution_preset = Application::VideoResolutionPreset::Res1080P;
    prefs.preferred_codec = Application::VideoCodecPreference::HEVC;
    prefs.fps = 60;

    auto decision1080 = UI::VideoAdaptationHelper::ResolveStreamParameters(prefs, 1920, 1080);
    assert(decision1080.width == 1920);
    assert(decision1080.height == 1080);
    assert(decision1080.fps == 60);
    assert(decision1080.codec == Core::Common::VideoCodec::HEVC);

    // Auto Refresh rate query
    uint32_t rr = UI::VideoAdaptationHelper::GetDisplayRefreshRate();
    assert(rr > 0);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 2: D3D11Renderer Geometry & Viewport Normalization
// ============================================================================
void TestD3D11RendererGeometry() {
    std::cout << "[Test 2] D3D11Renderer: letterbox geometry and normalization... ";

    UI::D3D11Renderer renderer;

    // 16:9 video (1280x720) in exact matching client area (1280x720)
    auto vp = renderer.CalculateLetterbox(1280, 720, 1280, 720);
    assert(std::abs(vp.x - 0.0f) < 0.01f);
    assert(std::abs(vp.y - 0.0f) < 0.01f);
    assert(std::abs(vp.width - 1280.0f) < 0.01f);
    assert(std::abs(vp.height - 720.0f) < 0.01f);

    // 16:9 video (1280x720) in wider client area (1920x720) -> Pillarboxing on sides
    auto vpWide = renderer.CalculateLetterbox(1920, 720, 1280, 720);
    assert(vpWide.height == 720.0f);
    assert(vpWide.width == 1280.0f);
    assert(std::abs(vpWide.x - (1920.0f - 1280.0f) * 0.5f) < 0.01f); // centered (x=320)
    assert(vpWide.y == 0.0f);

    // 16:9 video (1280x720) in taller client area (1280x1080) -> Letterboxing on top/bottom
    auto vpTall = renderer.CalculateLetterbox(1280, 1080, 1280, 720);
    assert(vpTall.width == 1280.0f);
    assert(vpTall.height == 720.0f);
    assert(vpTall.x == 0.0f);
    assert(std::abs(vpTall.y - (1080.0f - 720.0f) * 0.5f) < 0.01f); // centered (y=180)

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 3: SystemPermissionHelper
// ============================================================================
void TestSystemPermissionHelper() {
    std::cout << "[Test 3] SystemPermissionHelper: query microphone & location permissions... ";

    auto micState = UI::SystemPermissionHelper::CheckMicrophonePermission();
    assert(micState == UI::PermissionState::Allowed ||
           micState == UI::PermissionState::Denied ||
           micState == UI::PermissionState::Unknown);

    auto locState = UI::SystemPermissionHelper::CheckLocationPermission();
    assert(locState == UI::PermissionState::Allowed ||
           locState == UI::PermissionState::Denied ||
           locState == UI::PermissionState::Unknown);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 4: SetupWizard Preferences & Modals
// ============================================================================
void TestSetupWizardPreferences() {
    std::cout << "[Test 4] SetupWizard: safety disclaimer & MFi wizard logic... ";

    UI::SetupWizard wizard;
    Application::AppSettings settings;

    // Headless automated execution
    bool accepted = wizard.ShowSafetyDisclaimer(nullptr, /*headless=*/true);
    assert(accepted);
    assert(!wizard.ShouldShowDisclaimer());

    bool mfiDone = wizard.ShowMFiWizard(nullptr, settings, /*headless=*/true);
    assert(mfiDone);
    assert(!wizard.ShouldShowMFiWizard(settings));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 4b: touchscreen capability probe
// ============================================================================
void TestTouchscreenCapabilities() {
    std::cout << "[Test 4b] TouchscreenCapabilities: probe is safe and clamped... ";

    const auto caps = Core::Hardware::DetectTouchscreenCapabilities();
    // Whatever hardware this machine has, the probe must never claim contacts
    // without a touch screen, and the value handed to the RTSP server must be
    // one of the two descriptors we can build.
    assert(!caps.has_touch_screen || caps.max_contacts >= 1);
    if (!caps.has_touch_screen) {
        assert(caps.max_contacts == 0);
    }
    const int contacts = Core::Hardware::DetectMaxTouchContacts();
    assert(contacts == 1 || contacts == 2);
    assert(contacts == (caps.max_contacts >= 2 ? 2 : 1));

    std::cout << "PASSED (maxContacts=" << caps.max_contacts << ")\n";
}

// ============================================================================
// Test 5: Win32InputRouter Mouse to Touch & Buttons
// ============================================================================
void TestWin32InputRouterMouse() {
    std::cout << "[Test 5] Win32InputRouter: mouse messages to touch/back/wheel events... ";

    auto mockApi = std::make_shared<MockApplicationApi>();
    UI::D3D11Renderer renderer;
    UI::Win32InputRouter router(mockApi, &renderer, nullptr);

    HWND dummyHwnd = reinterpret_cast<HWND>(0x1234);

    // Left Button Down at (640, 360) in 1280x720 client space -> (0.5, 0.5)
    LPARAM lparam = MAKELPARAM(640, 360);
    bool handled = router.OnWindowMessage(dummyHwnd, WM_LBUTTONDOWN, 0, lparam);
    assert(handled);
    assert(router.IsMouseDown());
    assert(mockApi->touch_records_.size() == 1);
    assert(mockApi->touch_records_[0].action == Core::Input::TouchAction::Down);
    assert(std::abs(mockApi->touch_records_[0].point.x_normalized - 0.5f) < 0.05f);
    assert(std::abs(mockApi->touch_records_[0].point.y_normalized - 0.5f) < 0.05f);

    // Mouse Move while down
    lparam = MAKELPARAM(700, 360);
    handled = router.OnWindowMessage(dummyHwnd, WM_MOUSEMOVE, 0, lparam);
    assert(handled);
    assert(mockApi->touch_records_.size() == 2);
    assert(mockApi->touch_records_[1].action == Core::Input::TouchAction::Move);

    // Left Button Up
    lparam = MAKELPARAM(700, 360);
    handled = router.OnWindowMessage(dummyHwnd, WM_LBUTTONUP, 0, lparam);
    assert(handled);
    assert(!router.IsMouseDown());
    assert(mockApi->touch_records_.size() == 3);
    assert(mockApi->touch_records_[2].action == Core::Input::TouchAction::Up);

    // Right Button Down / Up -> Automotive Back button
    router.OnWindowMessage(dummyHwnd, WM_RBUTTONDOWN, 0, 0);
    assert(mockApi->button_records_.size() == 1);
    assert(mockApi->button_records_[0].button == Core::Input::ButtonType::Back);
    assert(mockApi->button_records_[0].pressed == true);

    router.OnWindowMessage(dummyHwnd, WM_RBUTTONUP, 0, 0);
    assert(mockApi->button_records_.size() == 2);
    assert(mockApi->button_records_[1].button == Core::Input::ButtonType::Back);
    assert(mockApi->button_records_[1].pressed == false);

    // Mouse Wheel -> Knob rotation
    WPARAM wparamWheel = MAKEWPARAM(0, 120); // scroll up
    router.OnWindowMessage(dummyHwnd, WM_MOUSEWHEEL, wparamWheel, 0);
    assert(mockApi->knob_records_.size() == 1);
    assert(mockApi->knob_records_[0].action == Core::Input::KnobAction::RotateCounterClockwise);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 6: Win32InputRouter Keyboard to Knob & Navigation
// ============================================================================
void TestWin32InputRouterKeyboard() {
    std::cout << "[Test 6] Win32InputRouter: keyboard to car knob/media buttons... ";

    auto mockApi = std::make_shared<MockApplicationApi>();
    UI::D3D11Renderer renderer;
    UI::Win32InputRouter router(mockApi, &renderer, nullptr);

    HWND dummyHwnd = reinterpret_cast<HWND>(0x1234);

    // Arrow keys -> 4-way knob
    router.OnWindowMessage(dummyHwnd, WM_KEYDOWN, VK_UP, 0);
    assert(!mockApi->knob_records_.empty());
    assert(mockApi->knob_records_.back().action == Core::Input::KnobAction::PressUp);

    router.OnWindowMessage(dummyHwnd, WM_KEYDOWN, 'S', 0);
    assert(mockApi->knob_records_.back().action == Core::Input::KnobAction::PressDown);

    // Enter -> Push knob
    router.OnWindowMessage(dummyHwnd, WM_KEYDOWN, VK_RETURN, 0);
    assert(mockApi->knob_records_.back().action == Core::Input::KnobAction::PushDown);

    // Media keys
    router.OnWindowMessage(dummyHwnd, WM_KEYDOWN, VK_SPACE, 0);
    assert(!mockApi->button_records_.empty());
    assert(mockApi->button_records_.back().button == Core::Input::ButtonType::PlayPause);

    router.OnWindowMessage(dummyHwnd, WM_KEYDOWN, VK_F1, 0); // Siri
    assert(mockApi->button_records_.back().button == Core::Input::ButtonType::Siri);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 7: AppController Headless Lifecycle
// ============================================================================
void TestAppControllerHeadlessLifecycle() {
    std::cout << "[Test 7] AppController: headless bootstrap, wireup & shutdown... ";

    auto mockApi = std::make_shared<MockApplicationApi>();
    UI::AppController controller;

    UI::WindowConfig config;
    config.title = L"CarPlayStation Test Window";
    config.allow_single_instance_only = false; // Disable single instance mutex for unit testing

    auto status = controller.Initialize(mockApi, config, /*headless=*/true);
    assert(status.IsOk());
    assert(controller.IsInitialized());
    assert(mockApi->started_);

    // Bootstrapping also pushes the current Windows theme to the phone, so the
    // CarPlay UI starts in the same appearance as the window.
    const UI::UiTheme expected = UI::ThemeHelper::CurrentTheme();
    assert(!mockApi->night_mode_records_.empty());
    assert(mockApi->night_mode_records_.back() == (expected == UI::UiTheme::Dark));

    // Run a step
    controller.Step();

    // Shutdown
    controller.Shutdown();
    assert(!controller.IsInitialized());
    assert(!mockApi->started_);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 8: ThemeHelper (Windows light/dark detection & palettes)
// ============================================================================
void TestThemeHelper() {
    std::cout << "[Test 8] ThemeHelper: system theme detection & palettes... ";

    // The current theme must always resolve to one of the two known values
    // (missing registry value falls back to Dark).
    const UI::UiTheme theme = UI::ThemeHelper::CurrentTheme();
    assert(theme == UI::UiTheme::Dark || theme == UI::UiTheme::Light);
    const char* name = UI::ThemeHelper::ThemeName(theme);
    assert(std::string(name) == "dark" || std::string(name) == "light");

    const UI::UiPalette dark = UI::ThemeHelper::BuildPalette(UI::UiTheme::Dark);
    const UI::UiPalette light = UI::ThemeHelper::BuildPalette(UI::UiTheme::Light);

    // Dark palette reproduces the colours this UI has always hardcoded.
    assert(dark.text_primary.r == 1.0f && dark.text_primary.g == 1.0f && dark.text_primary.b == 1.0f);
    assert(dark.text_secondary.r == 0.70f && dark.text_secondary.g == 0.70f);
    assert(dark.panel_bg.r == 0.08f && dark.panel_bg.a == 0.85f);
    assert(dark.accent.b == 1.0f && dark.accent.g == 0.48f);
    assert(dark.slider_track.r == 0.25f);
    assert(dark.letterbox.r == 0.0f && dark.letterbox.g == 0.0f && dark.letterbox.b == 0.0f);

    // Light palette is the mirror image: dark text on light panels.
    assert(light.text_primary.r < 0.5f && light.text_primary.g < 0.5f);
    assert(light.panel_bg.r > 0.5f && light.text_primary.r < light.panel_bg.r);
    assert(light.letterbox.r > 0.5f);

    // Translucency is preserved in both themes (panels are drawn over video).
    assert(dark.panel_bg.a > 0.0f && dark.panel_bg.a < 1.0f);
    assert(light.panel_bg.a > 0.0f && light.panel_bg.a < 1.0f);

    // Every component stays in [0,1] so Direct2D/D3D11 never rejects a colour.
    const UI::UiPalette* palettes[2] = { &dark, &light };
    for (const UI::UiPalette* p : palettes) {
        const UI::UiColor roles[10] = {
            p->text_primary, p->text_secondary, p->panel_bg, p->accent,
            p->status_green, p->status_orange, p->slider_track,
            p->slider_thumb, p->log_error, p->letterbox
        };
        for (const UI::UiColor& c : roles) {
            assert(c.r >= 0.0f && c.r <= 1.0f);
            assert(c.g >= 0.0f && c.g <= 1.0f);
            assert(c.b >= 0.0f && c.b <= 1.0f);
            assert(c.a >= 0.0f && c.a <= 1.0f);
        }
    }

    std::cout << "PASSED (system theme: " << name << ")\n";
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    std::cout << "========================================================\n";
    std::cout << "  CarPlayStation — Partition 8 UI & Presentation Tests  \n";
    std::cout << "========================================================\n";

    TestVideoAdaptationHelper();
    TestD3D11RendererGeometry();
    TestSystemPermissionHelper();
    TestSetupWizardPreferences();
    TestTouchscreenCapabilities();
    TestWin32InputRouterMouse();
    TestWin32InputRouterKeyboard();
    TestAppControllerHeadlessLifecycle();
    TestThemeHelper();

    std::cout << "\nAll 8 Partition 8 UI test suites passed successfully!\n";
    return 0;
}
