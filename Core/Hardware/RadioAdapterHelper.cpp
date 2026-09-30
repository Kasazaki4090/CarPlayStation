#include "Core/Hardware/RadioAdapterHelper.h"
#include <vector>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#endif

namespace CarPlayStation::Core::Hardware {

#ifdef _WIN32
// Function pointer definitions for WlanAPI
using PFN_WlanOpenHandle       = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
using PFN_WlanCloseHandle      = DWORD(WINAPI*)(HANDLE, PVOID);
using PFN_WlanEnumInterfaces   = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
using PFN_WlanQueryInterface   = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
using PFN_WlanGetInterfaceCapability = DWORD(WINAPI*)(HANDLE, const GUID*, PVOID, PWLAN_INTERFACE_CAPABILITY*);
using PFN_WlanFreeMemory       = VOID(WINAPI*)(PVOID);

// Function pointer definitions for BluetoothAPIs (bthprops.cpl)
using PFN_BluetoothFindFirstRadio  = HBLUETOOTH_RADIO_FIND(WINAPI*)(const BLUETOOTH_FIND_RADIO_PARAMS*, HANDLE*);
using PFN_BluetoothFindRadioClose = BOOL(WINAPI*)(HBLUETOOTH_RADIO_FIND);
using PFN_BluetoothGetRadioInfo   = DWORD(WINAPI*)(HANDLE, PBLUETOOTH_RADIO_INFO);
#endif

class RadioAdapterHelper::WlanNativeBinding {
public:
    WlanNativeBinding() {
#ifdef _WIN32
        h_dll_ = LoadLibraryA("wlanapi.dll");
        if (h_dll_) {
            fn_open_   = reinterpret_cast<PFN_WlanOpenHandle>(GetProcAddress(h_dll_, "WlanOpenHandle"));
            fn_close_  = reinterpret_cast<PFN_WlanCloseHandle>(GetProcAddress(h_dll_, "WlanCloseHandle"));
            fn_enum_   = reinterpret_cast<PFN_WlanEnumInterfaces>(GetProcAddress(h_dll_, "WlanEnumInterfaces"));
            fn_query_  = reinterpret_cast<PFN_WlanQueryInterface>(GetProcAddress(h_dll_, "WlanQueryInterface"));
            fn_get_cap_ = reinterpret_cast<PFN_WlanGetInterfaceCapability>(GetProcAddress(h_dll_, "WlanGetInterfaceCapability"));
            fn_free_   = reinterpret_cast<PFN_WlanFreeMemory>(GetProcAddress(h_dll_, "WlanFreeMemory"));
        }
#endif
    }

    ~WlanNativeBinding() {
#ifdef _WIN32
        if (h_dll_) {
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool IsAvailable() const noexcept {
#ifdef _WIN32
        return h_dll_ && fn_open_ && fn_close_ && fn_enum_ && fn_free_;
#else
        return false;
#endif
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    PFN_WlanOpenHandle     fn_open_{nullptr};
    PFN_WlanCloseHandle    fn_close_{nullptr};
    PFN_WlanEnumInterfaces fn_enum_{nullptr};
    PFN_WlanQueryInterface fn_query_{nullptr};
    PFN_WlanGetInterfaceCapability fn_get_cap_{nullptr};
    PFN_WlanFreeMemory     fn_free_{nullptr};
#endif
};

class RadioAdapterHelper::BluetoothNativeBinding {
public:
    BluetoothNativeBinding() {
#ifdef _WIN32
        h_dll_ = LoadLibraryA("bthprops.cpl");
        if (h_dll_) {
            fn_find_first_ = reinterpret_cast<PFN_BluetoothFindFirstRadio>(GetProcAddress(h_dll_, "BluetoothFindFirstRadio"));
            fn_find_close_ = reinterpret_cast<PFN_BluetoothFindRadioClose>(GetProcAddress(h_dll_, "BluetoothFindRadioClose"));
            fn_get_info_   = reinterpret_cast<PFN_BluetoothGetRadioInfo>(GetProcAddress(h_dll_, "BluetoothGetRadioInfo"));
        }
#endif
    }

    ~BluetoothNativeBinding() {
#ifdef _WIN32
        if (h_dll_) {
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool IsAvailable() const noexcept {
#ifdef _WIN32
        return h_dll_ && fn_find_first_ && fn_find_close_ && fn_get_info_;
#else
        return false;
#endif
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    PFN_BluetoothFindFirstRadio  fn_find_first_{nullptr};
    PFN_BluetoothFindRadioClose fn_find_close_{nullptr};
    PFN_BluetoothGetRadioInfo   fn_get_info_{nullptr};
#endif
};

RadioAdapterHelper::RadioAdapterHelper()
    : wlan_binding_(std::make_unique<WlanNativeBinding>())
    , bt_binding_(std::make_unique<BluetoothNativeBinding>()) {
}

RadioAdapterHelper::~RadioAdapterHelper() = default;

void RadioAdapterHelper::SetSimulatedBluetooth(RadioState state, bool has_adapter, std::string name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sim_bt_state_ = state;
    BluetoothCapabilities caps;
    caps.has_bt_adapter = has_adapter;
    caps.is_radio_on = (state == RadioState::On);
    caps.adapter_name = std::move(name);
    caps.address_string = "00:11:22:33:44:55";
    sim_bt_caps_ = caps;
}

void RadioAdapterHelper::SetSimulatedWifi(RadioState state, bool has_5ghz, bool has_softap, std::string desc) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sim_wifi_state_ = state;
    WifiCapabilities caps;
    caps.has_wifi_adapter = (state != RadioState::Unavailable);
    caps.is_radio_on = (state == RadioState::On);
    caps.is_5ghz_supported = has_5ghz;
    caps.is_softap_supported = has_softap;
    caps.adapter_description = std::move(desc);
    caps.interface_state = (state == RadioState::On) ? "Connected/Ready" : "Disabled";
    sim_wifi_caps_ = caps;
}

void RadioAdapterHelper::ClearSimulation() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sim_bt_state_.reset();
    sim_bt_caps_.reset();
    sim_wifi_state_.reset();
    sim_wifi_caps_.reset();
}

RadioState RadioAdapterHelper::GetState(std::string_view radio) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (radio == "bluetooth" || radio == "bt") {
        if (sim_bt_state_.has_value()) {
            return *sim_bt_state_;
        }
        auto caps = DetectBluetoothCapabilities();
        if (!caps.has_bt_adapter) return RadioState::Unavailable;
        return caps.is_radio_on ? RadioState::On : RadioState::Off;
    }

    if (radio == "wifi" || radio == "wlan") {
        if (sim_wifi_state_.has_value()) {
            return *sim_wifi_state_;
        }
        auto caps = DetectWifiCapabilities();
        if (!caps.has_wifi_adapter) return RadioState::Unavailable;
        return caps.is_radio_on ? RadioState::On : RadioState::Off;
    }

    return RadioState::Unavailable;
}

Common::Status RadioAdapterHelper::SetEnabled(std::string_view radio, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (radio == "bluetooth" || radio == "bt") {
        if (sim_bt_state_.has_value()) {
            sim_bt_state_ = enabled ? RadioState::On : RadioState::Off;
            if (sim_bt_caps_.has_value()) sim_bt_caps_->is_radio_on = enabled;
            return Common::Status::Ok();
        }
    } else if (radio == "wifi" || radio == "wlan") {
        if (sim_wifi_state_.has_value()) {
            sim_wifi_state_ = enabled ? RadioState::On : RadioState::Off;
            if (sim_wifi_caps_.has_value()) sim_wifi_caps_->is_radio_on = enabled;
            return Common::Status::Ok();
        }
    }

    // Changing system radio state programmatically requires elevated privileges
    // or Windows.Devices.Radios UWP broker. Return informative status:
    return Common::Status::Error(Common::StatusCode::PermissionDenied,
        "Toggling system physical radio requires administrative privileges or Windows Settings");
}

WifiCapabilities RadioAdapterHelper::DetectWifiCapabilities() const {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (sim_wifi_caps_.has_value()) {
            return *sim_wifi_caps_;
        }
    }

    WifiCapabilities caps{};

#ifdef _WIN32
    if (!wlan_binding_->IsAvailable()) {
        return caps;
    }

    HANDLE hClient = nullptr;
    DWORD negotiatedVersion = 0;
    DWORD res = wlan_binding_->fn_open_(2, nullptr, &negotiatedVersion, &hClient);
    if (res != ERROR_SUCCESS || !hClient) {
        return caps;
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    res = wlan_binding_->fn_enum_(hClient, nullptr, &pIfList);
    if (res == ERROR_SUCCESS && pIfList && pIfList->dwNumberOfItems > 0) {
        caps.has_wifi_adapter = true;
        // Take the primary interface
        const auto& ifInfo = pIfList->InterfaceInfo[0];
        
        // Convert description wide string to utf-8
        int len = WideCharToMultiByte(CP_UTF8, 0, ifInfo.strInterfaceDescription, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            caps.adapter_description.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, ifInfo.strInterfaceDescription, -1, caps.adapter_description.data(), len, nullptr, nullptr);
        }

        if (ifInfo.isState == wlan_interface_state_connected ||
            ifInfo.isState == wlan_interface_state_disconnected) {
            caps.is_radio_on = true;
        }

        switch (ifInfo.isState) {
            case wlan_interface_state_connected:    caps.interface_state = "Connected"; break;
            case wlan_interface_state_disconnected: caps.interface_state = "Disconnected"; break;
            default: caps.interface_state = "Other"; break;
        }

        // Query physical capabilities (5GHz detection)
        if (wlan_binding_->fn_get_cap_) {
            PWLAN_INTERFACE_CAPABILITY pCap = nullptr;
            DWORD qRes = wlan_binding_->fn_get_cap_(hClient, &ifInfo.InterfaceGuid, nullptr, &pCap);
            if (qRes == ERROR_SUCCESS && pCap) {
                for (DWORD i = 0; i < pCap->dwNumberOfSupportedPhys; ++i) {
                    DOT11_PHY_TYPE phy = pCap->dot11PhyTypes[i];
                    // dot11_phy_type_ofdm (802.11a), dot11_phy_type_ht (802.11n 5GHz),
                    // dot11_phy_type_vht (802.11ac 5GHz), dot11_phy_type_he (802.11ax Wi-Fi 6),
                    // or dot11_phy_type_eht = 11 (802.11be Wi-Fi 7 in Win11 24H2 SDK)
                    constexpr uint32_t PHY_TYPE_EHT_WIFI7 = 11;
                    if (phy == dot11_phy_type_ofdm || phy == dot11_phy_type_ht ||
                        phy == dot11_phy_type_vht || phy == dot11_phy_type_he ||
                        static_cast<uint32_t>(phy) == PHY_TYPE_EHT_WIFI7) {
                        caps.is_5ghz_supported = true;
                        break;
                    }
                }
                wlan_binding_->fn_free_(pCap);
            }
        }

        // Windows modern Wi-Fi adapters inherently support SoftAP / HostedNetwork or Wi-Fi Direct
        caps.is_softap_supported = true;
    }

    if (pIfList) {
        wlan_binding_->fn_free_(pIfList);
    }
    wlan_binding_->fn_close_(hClient, nullptr);
#endif

    return caps;
}

BluetoothCapabilities RadioAdapterHelper::DetectBluetoothCapabilities() const {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (sim_bt_caps_.has_value()) {
            return *sim_bt_caps_;
        }
    }

    BluetoothCapabilities caps{};

#ifdef _WIN32
    if (!bt_binding_->IsAvailable()) {
        return caps;
    }

    BLUETOOTH_FIND_RADIO_PARAMS params{};
    params.dwSize = sizeof(BLUETOOTH_FIND_RADIO_PARAMS);

    HANDLE hRadio = nullptr;
    HBLUETOOTH_RADIO_FIND hFind = bt_binding_->fn_find_first_(&params, &hRadio);
    if (hFind && hRadio) {
        caps.has_bt_adapter = true;
        caps.is_radio_on = true;

        BLUETOOTH_RADIO_INFO radioInfo{};
        radioInfo.dwSize = sizeof(BLUETOOTH_RADIO_INFO);
        DWORD res = bt_binding_->fn_get_info_(hRadio, &radioInfo);
        if (res == ERROR_SUCCESS) {
            std::wstring ws(radioInfo.szName);
            caps.adapter_name = std::string(ws.begin(), ws.end());

            std::ostringstream ss;
            ss << std::hex << std::setfill('0');
            for (int i = 5; i >= 0; --i) {
                ss << std::setw(2) << static_cast<int>(radioInfo.address.rgBytes[i]);
                if (i > 0) ss << ":";
            }
            caps.address_string = ss.str();
        }

        CloseHandle(hRadio);
        bt_binding_->fn_find_close_(hFind);
    }
#endif

    return caps;
}

WirelessCarPlayReadiness RadioAdapterHelper::EvaluateReadiness() const {
    auto bt = DetectBluetoothCapabilities();
    auto wifi = DetectWifiCapabilities();

    WirelessCarPlayReadiness r{};
    r.bt_ready = bt.has_bt_adapter && bt.is_radio_on;
    r.wifi_ready = wifi.has_wifi_adapter && wifi.is_radio_on;
    r.supports_5ghz = wifi.is_5ghz_supported;
    r.supports_softap = wifi.is_softap_supported;

    std::ostringstream diag;
    if (!r.bt_ready) {
        diag << "[Bluetooth: NOT READY (no adapter or turned off)] ";
    } else {
        diag << "[Bluetooth: OK (" << bt.adapter_name << ")] ";
    }

    if (!r.wifi_ready) {
        diag << "[Wi-Fi: NOT READY (no adapter or turned off)] ";
    } else {
        diag << "[Wi-Fi: OK (" << wifi.adapter_description << ")] ";
        if (!r.supports_5ghz) {
            diag << "[WARNING: 5GHz band not detected - Apple Wireless CarPlay requires 5GHz!] ";
        } else {
            diag << "[5GHz: Supported] ";
        }
    }

    r.overall_ready = r.bt_ready && r.wifi_ready && r.supports_5ghz;
    if (r.overall_ready) {
        diag << "-> All hardware prerequisites for Wireless CarPlay are satisfied.";
    } else {
        diag << "-> Incomplete hardware capabilities for Wireless CarPlay.";
    }

    r.diagnostics = diag.str();
    return r;
}

} // namespace CarPlayStation::Core::Hardware
