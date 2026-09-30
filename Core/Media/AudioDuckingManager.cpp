#include "Core/Media/AudioDuckingManager.h"

namespace CarPlayStation::Core::Media {

float AudioDuckingManager::Tick(uint32_t dtMs) {
    const float target = TargetGain();
    if (currentGain_ == target) {
        return currentGain_;
    }

    const float span = cfg_.fullGain - cfg_.duckedGain;
    if (span <= 0.0f) {
        currentGain_ = target;
        return currentGain_;
    }

    // Moving down (toward the ducked level) uses the attack curve; moving up
    // (restoring) uses the release curve.
    const bool movingDown = target < currentGain_;
    const uint32_t rampMs = movingDown ? cfg_.attackMs : cfg_.releaseMs;
    const float perMs = span / static_cast<float>(rampMs > 0 ? rampMs : 1u);
    const float delta = perMs * static_cast<float>(dtMs);

    // Move toward the target; snap exactly to it once the step reaches or
    // passes it (within a small epsilon) to avoid 1-ULP float drift at the
    // end of the ramp.
    const float eps = span * 1e-4f;
    if (movingDown) {
        const float next = currentGain_ - delta;
        currentGain_ = (next <= target + eps) ? target : next;
    } else {
        const float next = currentGain_ + delta;
        currentGain_ = (next >= target - eps) ? target : next;
    }
    return currentGain_;
}

bool AudioDuckingManager::IsSettled() const noexcept {
    return currentGain_ == TargetGain();
}

} // namespace CarPlayStation::Core::Media
