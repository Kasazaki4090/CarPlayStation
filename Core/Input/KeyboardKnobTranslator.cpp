#include "Core/Input/KeyboardKnobTranslator.h"

namespace CarPlayStation::Core::Input {

KeyboardKnobTranslator::KeyboardKnobTranslator(IInputEventSink& sink, Config cfg)
    : sink_(sink), cfg_(cfg) {}

void KeyboardKnobTranslator::OnKeyEvent(const KeyEvent& ev) {
    // Automotive knob / button controls are edge-triggered: only a key-down
    // produces an action. Key-ups are ignored.
    if (!ev.isDown) {
        return;
    }

    switch (ev.virtualKey) {
        // Directional navigation -> directional knob press (4-way d-pad).
        // Arrow keys and WASD are equivalent.
        case Key::Up:
        case Key::KeyW:
            lastStatus_ = sink_.SendKnobEvent(KnobAction::PressUp, cfg_.knobStepsPerPress);
            break;

        case Key::Down:
        case Key::KeyS:
            lastStatus_ = sink_.SendKnobEvent(KnobAction::PressDown, cfg_.knobStepsPerPress);
            break;

        case Key::Left:
        case Key::KeyA:
            lastStatus_ = sink_.SendKnobEvent(KnobAction::PressLeft, cfg_.knobStepsPerPress);
            break;

        case Key::Right:
        case Key::KeyD:
            lastStatus_ = sink_.SendKnobEvent(KnobAction::PressRight, cfg_.knobStepsPerPress);
            break;

        // Confirm / select.
        case Key::Enter:
            lastStatus_ = sink_.SendKnobEvent(KnobAction::PushDown, 1);
            break;

        // Back / cancel.
        case Key::Backspace:
        case Key::Escape:
            lastStatus_ = sink_.SendButtonEvent(cfg_.backButton, true);
            break;

        // Media transport.
        case Key::PlayPause:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::PlayPause, true);
            break;
        case Key::NextTrack:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::NextTrack, true);
            break;
        case Key::PrevTrack:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::PrevTrack, true);
            break;

        // Assistant / telephony.
        case Key::Siri:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::Siri, true);
            break;
        case Key::CallAccept:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::CallAccept, true);
            break;
        case Key::CallReject:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::CallReject, true);
            break;

        // Volume.
        case Key::VolumeUp:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::VolumeUp, true);
            break;
        case Key::VolumeDown:
            lastStatus_ = sink_.SendButtonEvent(ButtonType::VolumeDown, true);
            break;

        // Unmapped keys (letters, numbers, etc.) are intentionally ignored:
        // CarPlay has no alphanumeric text-input channel.
        default:
            break;
    }
}

} // namespace CarPlayStation::Core::Input
