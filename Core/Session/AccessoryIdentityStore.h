#ifndef CARPLAYSESSION_ACCESSORY_IDENTITY_STORE_H
#define CARPLAYSESSION_ACCESSORY_IDENTITY_STORE_H

#include "Core/Common/Types.h"
#include "Core/Session/Pairing/PairSetup.h"
#include <array>
#include <cstdint>
#include <string>

namespace CarPlayStation::Core::Session {

// The accessory's long term Ed25519 identity, persisted so it survives restarts.
//
// Both consumers of this key need it to be stable:
//   - RTSP pair-setup/pair-verify (carplayd docs 24_PAIR_SETUP_VERIFY.md)
//   - iAP2 wirelessCarPlayStartSession (0x4301) publicKey
// Regenerating it on every start makes the iPhone treat us as a brand new
// accessory each time.
struct StoredAccessoryIdentity {
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 32> secret_key{};

    Pairing::AccessoryIdentity ToAccessoryIdentity() const;
};

class AccessoryIdentityStore {
public:
    static constexpr const char* kDefaultPath = "carplay_accessory_identity.json";

    explicit AccessoryIdentityStore(std::string path = kDefaultPath);

    // Loads the stored key pair, or generates (and saves) a fresh one.
    const StoredAccessoryIdentity& Identity() const { return identity_; }

    bool IsGenerated() const { return generated_; }

    std::string GetStoragePath() const { return path_; }

private:
    bool Load();
    bool Save() const;

    std::string path_;
    StoredAccessoryIdentity identity_;
    bool generated_ = false;
};

}  // namespace CarPlayStation::Core::Session

#endif  // CARPLAYSESSION_ACCESSORY_IDENTITY_STORE_H
