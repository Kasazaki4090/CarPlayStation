#pragma once

#include "Application/AppSettings.h"
#include "Core/Hardware/IMfiSigner.h"
#include <memory>

namespace CarPlayStation::Application {

// ============================================================================
// MfiSignerFactory — resolves an Application::MFiPreferences selection into a
// concrete Core::Hardware::IMfiSigner implementation (DEC-003 / DEC-006 / DEC-007).
//
// The Application layer is the only place that knows about the user's backend
// choice (local files, network daemon, or physical USB-to-I2C bridge). Core
// never hardcodes a file path, daemon address, or bridge driver.
// ============================================================================
class MfiSignerFactory {
public:
    // Build the signer matching the configured backend. Returns nullptr if the
    // backend cannot be constructed (e.g. an unknown driver type).
    [[nodiscard]] static std::shared_ptr<Core::Hardware::IMfiSigner> Build(const MFiPreferences& prefs);

    // Human-readable description of the resolved backend (for UI diagnostics).
    [[nodiscard]] static std::string Describe(const MFiPreferences& prefs);
};

} // namespace CarPlayStation::Application
