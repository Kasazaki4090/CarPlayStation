#include "Core/Input/MultiTouchInjector.h"

namespace CarPlayStation::Core::Input {

MultiTouchInjector::MultiTouchInjector(IInputEventSink& sink, Config cfg)
    : sink_(sink), cfg_(cfg) {}

float MultiTouchInjector::NormalizeX(float clientX) const noexcept {
    if (cfg_.windowWidth == 0) {
        return 0.0f;
    }
    const float v = clientX / static_cast<float>(cfg_.windowWidth);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float MultiTouchInjector::NormalizeY(float clientY) const noexcept {
    if (cfg_.windowHeight == 0) {
        return 0.0f;
    }
    const float v = clientY / static_cast<float>(cfg_.windowHeight);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void MultiTouchInjector::Emit(uint32_t pointerId, TouchAction action, float nx, float ny) {
    TouchPoint point;
    point.id = pointerId;
    point.x_normalized = nx;
    point.y_normalized = ny;
    lastStatus_ = sink_.SendTouchEvent(action, point);
}

void MultiTouchInjector::OnFrame(std::vector<PointerEvent> contacts) {
    // 1. Emit Touch Up for contacts that were active last frame but are gone.
    for (auto& [id, contact] : active_) {
        bool stillPresent = false;
        for (const auto& c : contacts) {
            if (c.pointerId == id) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            Emit(id, TouchAction::Up, contact.nx, contact.ny);
        }
    }

    // 2. Emit Down/Move for contacts in the current frame.
    for (const auto& c : contacts) {
        const float nx = NormalizeX(c.x);
        const float ny = NormalizeY(c.y);
        auto it = active_.find(c.pointerId);
        if (it == active_.end()) {
            active_[c.pointerId] = Contact{nx, ny};
            Emit(c.pointerId, TouchAction::Down, nx, ny);
        } else {
            it->second.nx = nx;
            it->second.ny = ny;
            Emit(c.pointerId, TouchAction::Move, nx, ny);
        }
    }

    // 3. Drop lifted contacts from the active set.
    for (auto it = active_.begin(); it != active_.end();) {
        bool stillPresent = false;
        for (const auto& c : contacts) {
            if (c.pointerId == it->first) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
}

void MultiTouchInjector::CancelAll() {
    for (auto& [id, contact] : active_) {
        Emit(id, TouchAction::Cancel, contact.nx, contact.ny);
    }
    active_.clear();
}

bool MultiTouchInjector::IsContactActive(uint32_t pointerId) const noexcept {
    return active_.find(pointerId) != active_.end();
}

} // namespace CarPlayStation::Core::Input
