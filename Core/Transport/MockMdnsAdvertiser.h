#pragma once

#include "Core/Transport/IMdnsAdvertiser.h"
#include <mutex>

namespace CarPlayStation::Core::Transport {

/**
 * @brief In-memory mock mDNS advertiser for protocol testing.
 */
class MockMdnsAdvertiser : public IMdnsAdvertiser {
public:
    MockMdnsAdvertiser() = default;
    ~MockMdnsAdvertiser() override = default;

    Common::Status Start(const MdnsServiceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        m_running = true;
        m_broadcastCount++;
        return Common::Status::Ok();
    }

    void Stop() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_running;
    }

    [[nodiscard]] MdnsServiceConfig GetConfig() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    [[nodiscard]] Common::Status BroadcastAnnouncement() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return Common::Status::Error(Common::StatusCode::InvalidState, "Advertiser is not running");
        }
        m_broadcastCount++;
        return Common::Status::Ok();
    }

    [[nodiscard]] size_t GetBroadcastCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_broadcastCount;
    }

private:
    mutable std::mutex m_mutex;
    bool m_running{false};
    size_t m_broadcastCount{0};
    MdnsServiceConfig m_config{};
};

} // namespace CarPlayStation::Core::Transport
