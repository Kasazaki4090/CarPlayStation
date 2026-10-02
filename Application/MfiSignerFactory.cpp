#include "Application/MfiSignerFactory.h"

#include "Core/Hardware/FileMfiSigner.h"
#include "Core/Hardware/NetworkMfiClient.h"
#include "Core/Hardware/I2cMfiSigner.h"
#include "Core/Hardware/Ch341I2cAdapter.h"
#include "Core/Hardware/Ch347I2cAdapter.h"
#include "Core/Hardware/FtdiI2cAdapter.h"
#include "Core/Hardware/MockI2cAdapter.h"

#include <memory>
#include <iomanip>
#include <sstream>
#include <string>

namespace CarPlayStation::Application {

namespace {

// Build the concrete II2cAdapter for a requested driver type. For Auto, probe
// the physical bridges in order (CH341 -> CH347 -> FTDI) and pick the first
// whose vendor driver is present. Returns nullptr when nothing is available.
std::shared_ptr<Core::Hardware::II2cAdapter> BuildI2cAdapter(HardwareI2cDriverType driver) {
    using namespace Core::Hardware;

    auto try_ch341 = []() -> std::shared_ptr<II2cAdapter> {
        auto a = std::make_shared<Ch341I2cAdapter>();
        return a->IsDriverLoaded() ? a : nullptr;
    };
    auto try_ch347 = []() -> std::shared_ptr<II2cAdapter> {
        auto a = std::make_shared<Ch347I2cAdapter>();
        return a->IsDriverLoaded() ? a : nullptr;
    };
    auto try_ftdi = []() -> std::shared_ptr<II2cAdapter> {
        auto a = std::make_shared<FtdiI2cAdapter>();
        return a->IsDriverLoaded() ? a : nullptr;
    };

    switch (driver) {
        case HardwareI2cDriverType::CH341: return try_ch341();
        case HardwareI2cDriverType::CH347: return try_ch347();
        case HardwareI2cDriverType::FTDI:  return try_ftdi();
        case HardwareI2cDriverType::Mock:   return std::make_shared<MockI2cAdapter>();
        case HardwareI2cDriverType::Auto:
        default: {
            if (auto a = try_ch341()) return a;
            if (auto a = try_ch347()) return a;
            if (auto a = try_ftdi())  return a;
            return nullptr;
        }
    }
}

} // namespace

std::shared_ptr<Core::Hardware::IMfiSigner> MfiSignerFactory::Build(const MFiPreferences& prefs) {
    using namespace Core::Hardware;

    switch (prefs.backend_type) {
        case MFiBackendType::LocalFile: {
            // Offline software signer (DEC-006). The cert/key files must exist;
            // the signer self-initializes lazily on first use.
            return std::make_shared<FileMfiSigner>(
                prefs.certificate_path, prefs.private_key_path);
        }

        case MFiBackendType::NetworkDaemon: {
            // TCP signing daemon (DEC-003).
            return std::make_shared<NetworkMfiClient>(prefs.daemon_endpoint);
        }

        case MFiBackendType::HardwareI2C: {
            // Physical USB-to-I2C bridge driving an MFi coprocessor (DEC-003/DEC-007).
            auto adapter = BuildI2cAdapter(prefs.hardware_i2c.driver_type);
            if (!adapter) {
                return nullptr; // No usable bridge driver present.
            }
            I2cMfiSigner::Options opts;
            opts.reset_gpio_pin = prefs.hardware_i2c.reset_gpio_pin;
            return std::make_shared<I2cMfiSigner>(
                adapter, prefs.hardware_i2c.slave_address, opts);
        }

        default:
            return nullptr;
    }
}

std::string MfiSignerFactory::Describe(const MFiPreferences& prefs) {
    switch (prefs.backend_type) {
        case MFiBackendType::LocalFile:
            return "LocalFile: " + prefs.certificate_path + " / " + prefs.private_key_path;
        case MFiBackendType::NetworkDaemon:
            return "NetworkDaemon: " + prefs.daemon_endpoint.ToString();
        case MFiBackendType::HardwareI2C: {
            std::ostringstream address;
            address << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(prefs.hardware_i2c.slave_address);
            return "HardwareI2C: driver=" + std::to_string(static_cast<int>(prefs.hardware_i2c.driver_type))
             + " slave=" + address.str();
        }
        default:
            return "Unknown";
    }
}

} // namespace CarPlayStation::Application
