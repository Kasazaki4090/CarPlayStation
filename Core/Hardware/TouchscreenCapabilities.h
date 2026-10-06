#pragma once

#include <cstdint>
#include <string>

namespace CarPlayStation::Core::Hardware {

// ============================================================================
// Touchscreen capability probe
//
// CarPlay has no knob device: the only HID pointer we may advertise is the
// touchscreen, and how many contacts its report may carry is fixed by the
// descriptor we send in /info. That number has to match what the host can
// actually produce, so it is read from the hardware instead of being a user
// setting:
//
//   * a display-attached digitizer reporting 2+ simultaneous contacts
//     (Surface, all-in-one touch panels)  -> advertise 2 contacts
//   * mouse / touchpad / pen-only hosts   -> advertise 1 contact
//
// A touchpad is deliberately excluded: its input arrives as PT_TOUCHPAD, which
// the UI pointer router ignores, so advertising two contacts for it would
// change the descriptor without ever producing a second contact.
// ============================================================================

struct TouchscreenCapabilities {
    bool has_touch_screen{false};
    int max_contacts{0};
    std::string device_description{};
};

// Probe the host once. Safe to call from any thread; no caching (called once
// during service initialization).
TouchscreenCapabilities DetectTouchscreenCapabilities();

// 2 when the host has a multi-contact touch screen, otherwise 1.
int DetectMaxTouchContacts();

} // namespace CarPlayStation::Core::Hardware
