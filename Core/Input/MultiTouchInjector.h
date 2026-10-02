#pragma once

#include "Core/Input/IInputEventSink.h"
#include "Core/Input/InputEvents.h"
#include <cstdint>
#include <unordered_map>

namespace CarPlayStation::Core::Input {

// ============================================================================
// MultiTouchInjector (Partition 6, Step 4)
//
// Passes native Windows pointer / multi-touch contacts through to CarPlay
// touch events. Used when running on touchscreen hardware (as opposed to the
// mouse-driven MouseTouchTranslator).
//
// The UI layer (Partition 8) feeds a batch of active contacts each frame via
// OnFrame(). The injector diffs the current set against the previous frame to
// emit the correct per-contact TouchAction:
//
//   * contact newly present        -> Touch Down
//   * contact present + moved      -> Touch Move
//   * contact absent (was present) -> Touch Up
//
// Each contact keeps its stable pointerId (transducer id) so multi-finger
// gestures (pinch, two-finger scroll) are preserved. Coordinates are in the
// UI window client pixel space and are normalized to 0.0-1.0.
//
// Pure logic — no Win32 coupling.
// ============================================================================
class MultiTouchInjector {
public:
    struct Config {
        // UI window client size in pixels (the space pointer x/y are in).
        uint32_t windowWidth{1920};
        uint32_t windowHeight{720};
    };

    MultiTouchInjector(IInputEventSink& sink, Config cfg = {});

    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return cfg_; }

    // Feed the set of currently-active contacts for this frame. Each contact
    // carries a stable pointerId. Contacts not in the set are treated as
    // lifted (Touch Up).
    void OnFrame(std::vector<PointerEvent> contacts);

    // Force-lift all active contacts (e.g. on session teardown).
    void CancelAll();

    // Test / introspection accessors.
    [[nodiscard]] size_t ActiveContactCount() const noexcept { return active_.size(); }
    [[nodiscard]] bool IsContactActive(uint32_t pointerId) const noexcept;

    // Status of the most recent sink emission (surfaced, not swallowed).
    [[nodiscard]] Common::Status LastStatus() const noexcept { return lastStatus_; }

private:
    struct Contact {
        float nx{0.0f};
        float ny{0.0f};
    };

    [[nodiscard]] float NormalizeX(float clientX) const noexcept;
    [[nodiscard]] float NormalizeY(float clientY) const noexcept;
    void Emit(uint32_t pointerId, TouchAction action, float nx, float ny);

    IInputEventSink& sink_;
    Config cfg_{};
    std::unordered_map<uint32_t, Contact> active_;
    Common::Status lastStatus_{Common::Status::Ok()};
};

} // namespace CarPlayStation::Core::Input
