#pragma once

#include "Core/Input/IInputEventSink.h"
#include <mutex>
#include <vector>

namespace CarPlayStation::Core::Input {

// ============================================================================
// MockInputEventSink (Partition 6, Step 5)
//
// Records every input event emitted by the translators so unit tests can
// assert on the exact sequence of CarPlay touch / knob / button events
// without a real Session layer. Thread-safe.
// ============================================================================
class MockInputEventSink final : public IInputEventSink {
public:
    struct TouchRecord {
        TouchAction action;
        TouchPoint point;
    };
    struct KnobRecord {
        KnobAction action;
        uint32_t stepCount;
    };
    struct ButtonRecord {
        ButtonType button;
        bool isPressed;
    };

    Common::Status SendTouchEvent(TouchAction action, const TouchPoint& point) override {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_events_.push_back(TouchRecord{action, point});
        return Common::Status::Ok();
    }

    Common::Status SendKnobEvent(KnobAction action, uint32_t stepCount = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        knob_events_.push_back(KnobRecord{action, stepCount});
        return Common::Status::Ok();
    }

    Common::Status SendButtonEvent(ButtonType button, bool isPressed) override {
        std::lock_guard<std::mutex> lock(mutex_);
        button_events_.push_back(ButtonRecord{button, isPressed});
        return Common::Status::Ok();
    }

    // Introspection (thread-safe copies).
    [[nodiscard]] std::vector<TouchRecord> TouchEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return touch_events_;
    }
    [[nodiscard]] std::vector<KnobRecord> KnobEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return knob_events_;
    }
    [[nodiscard]] std::vector<ButtonRecord> ButtonEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return button_events_;
    }

    [[nodiscard]] size_t TouchCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return touch_events_.size();
    }
    [[nodiscard]] size_t KnobCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return knob_events_.size();
    }
    [[nodiscard]] size_t ButtonCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return button_events_.size();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_events_.clear();
        knob_events_.clear();
        button_events_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<TouchRecord> touch_events_;
    std::vector<KnobRecord> knob_events_;
    std::vector<ButtonRecord> button_events_;
};

} // namespace CarPlayStation::Core::Input
