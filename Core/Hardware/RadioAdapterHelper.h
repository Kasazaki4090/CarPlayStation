#pragma once

#include "Core/Hardware/IRadioState.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <optional>
#include <mutex>

namespace CarPlayStation::Core::Hardware {

// Wi-Fi adapter capability profile
struct WifiCapabilities {
    bool has_wifi_adapter{false};
    bool is_radio_on{false};
    bool is_5ghz_supported{false};
    bool is_softap_supported{false};
    std::string adapter_description{};
    std::string interface_state{};
};

// Bluetooth adapter capability profile
struct BluetoothCapabilities {
    bool has_bt_adapter{false};
    bool is_radio_on{false};
    std::string adapter_name{};
    std::string address_string{};
};

// Summary report of hardware readiness for Wireless CarPlay
struct WirelessCarPlayReadiness {
    bool bt_ready{false};
    bool wifi_ready{false};
    bool supports_5ghz{false};
    bool supports_softap{false};
    bool overall_ready{false};
    std::string diagnostics{};
};

// Hardware radio state helper implementing IRadioState.
// Detects Windows Native Wi-Fi (WlanAPI) and Bluetooth Radio capabilities,
// specifically checking for 5GHz and SoftAP requirements mandatory for Wireless CarPlay.
class RadioAdapterHelper : public IRadioState {
public:
    RadioAdapterHelper();
    ~RadioAdapterHelper() override;

    // IRadioState implementation
    [[nodiscard]] RadioState GetState(std::string_view radio) const noexcept override;
    [[nodiscard]] Common::Status SetEnabled(std::string_view radio, bool enabled) override;

    // Advanced hardware queries for Wireless CarPlay bringup
    [[nodiscard]] WifiCapabilities DetectWifiCapabilities() const;
    [[nodiscard]] BluetoothCapabilities DetectBluetoothCapabilities() const;
    [[nodiscard]] WirelessCarPlayReadiness EvaluateReadiness() const;

    // --- Test & Simulation Harness ---
    void SetSimulatedBluetooth(RadioState state, bool has_adapter = true, std::string name = "Mock-BT");
    void SetSimulatedWifi(RadioState state, bool has_5ghz = true, bool has_softap = true, std::string desc = "Mock-WLAN");
    void ClearSimulation();

private:
    class WlanNativeBinding;
    class BluetoothNativeBinding;

    std::unique_ptr<WlanNativeBinding> wlan_binding_;
    std::unique_ptr<BluetoothNativeBinding> bt_binding_;

    mutable std::recursive_mutex mutex_;
    std::optional<RadioState> sim_bt_state_;
    std::optional<BluetoothCapabilities> sim_bt_caps_;
    std::optional<RadioState> sim_wifi_state_;
    std::optional<WifiCapabilities> sim_wifi_caps_;
};

} // namespace CarPlayStation::Core::Hardware
