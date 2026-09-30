#pragma once

#include "Core/Session/IPairingStore.h"
#include <mutex>
#include <algorithm>

namespace CarPlayStation::Core::Session {

/**
 * @brief Thread-safe in-memory mock implementation of IPairingStore for unit tests and CI.
 */
class MockPairingStore : public IPairingStore {
public:
    MockPairingStore() = default;

    Common::Result<void> SaveDevice(const PairedDevice& device) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_saves_) {
            return Common::Status::Error(Common::StatusCode::IoError, "Injected save failure in MockPairingStore");
        }
        if (!device.IsValid()) {
            return Common::Status::Error(Common::StatusCode::InvalidArgument, "Cannot save invalid PairedDevice");
        }

        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const PairedDevice& d) {
            return d.device_id == device.device_id;
        });

        if (it != devices_.end()) {
            *it = device;
        } else {
            devices_.push_back(device);
        }
        return Common::Result<void>::Ok();
    }

    Common::Result<PairedDevice> GetDevice(std::string_view device_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const PairedDevice& d) {
            return d.device_id == device_id;
        });
        if (it != devices_.end()) {
            return *it;
        }
        return Common::Status::Error(Common::StatusCode::NotFound, "Paired device not found: " + std::string(device_id));
    }

    Common::Result<PairedDevice> FindByMac(const Common::MacAddress& mac) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const PairedDevice& d) {
            return d.bluetooth_mac == mac || d.wifi_mac == mac;
        });
        if (it != devices_.end()) {
            return *it;
        }
        return Common::Status::Error(Common::StatusCode::NotFound, "Device with matching MAC not found");
    }

    std::vector<PairedDevice> ListDevices() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PairedDevice> sorted = devices_;
        std::sort(sorted.begin(), sorted.end(), [](const PairedDevice& a, const PairedDevice& b) {
            return a.last_connected_timestamp > b.last_connected_timestamp;
        });
        return sorted;
    }

    Common::Result<void> RemoveDevice(std::string_view device_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::remove_if(devices_.begin(), devices_.end(), [&](const PairedDevice& d) {
            return d.device_id == device_id;
        });
        if (it != devices_.end()) {
            devices_.erase(it, devices_.end());
            return Common::Result<void>::Ok();
        }
        return Common::Status::Error(Common::StatusCode::NotFound, "Cannot remove non-existent device: " + std::string(device_id));
    }

    void Clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_.clear();
    }

    size_t Count() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return devices_.size();
    }

    bool IsPaired(std::string_view device_id) const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(devices_.begin(), devices_.end(), [&](const PairedDevice& d) {
            return d.device_id == device_id;
        });
    }

    // --- Test Control Methods ---
    void SetFailSaves(bool fail) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_saves_ = fail;
    }

private:
    mutable std::mutex mutex_;
    std::vector<PairedDevice> devices_;
    bool fail_saves_{false};
};

} // namespace CarPlayStation::Core::Session
