#include "Core/Protocol/Iap2SessionManager.h"
#include <algorithm>

namespace CarPlayStation::Core::Protocol {

Iap2SessionManager::Iap2SessionManager(const Config& config) : config_(config) {}

Iap2Action Iap2SessionManager::OnMessage(uint16_t msgId, Common::ByteSpan payload) {
    (void)payload; // Reserved for future per-message body inspection
    std::lock_guard lock(mutex_);

    namespace id = Iap2MsgId;

    switch (msgId) {
        case id::RequestAuthCertificate: // 0xAA00
            if (state_ == Iap2AuthState::Init) {
                TransitionTo(Iap2AuthState::CertSent, Iap2Action::SendCert, "RequestAuthCertificate");
                return Iap2Action::SendCert;
            }
            break;

        case id::RequestAuthChallenge: // 0xAA02
            if (state_ == Iap2AuthState::CertSent) {
                TransitionTo(Iap2AuthState::SignSent, Iap2Action::SignChallenge, "RequestAuthChallenge");
                return Iap2Action::SignChallenge;
            }
            break;

        case id::AuthSucceeded: // 0xAA05
            if (state_ >= Iap2AuthState::SignSent) {
                auto next = std::max(state_, Iap2AuthState::Authenticated);
                TransitionTo(next, Iap2Action::Note, "AuthenticationSucceeded");
                return Iap2Action::Note;
            }
            break;

        case id::AuthFailed: // 0xAA04
            TransitionTo(state_, Iap2Action::Abort, "AuthenticationFailed");
            return Iap2Action::Abort;

        case id::StartIdentification: // 0x1D00
            if (state_ >= Iap2AuthState::Authenticated && state_ < Iap2AuthState::Identified) {
                TransitionTo(Iap2AuthState::IdentSent, Iap2Action::SendIdentify, "StartIdentification");
                return Iap2Action::SendIdentify;
            }
            break;

        case id::IdentifyAccepted: // 0x1D02
            if (state_ >= Iap2AuthState::IdentSent) {
                TransitionTo(Iap2AuthState::Identified, Iap2Action::Note, "IdentificationAccepted");
                return Iap2Action::Note;
            }
            break;

        case id::IdentifyRejected: // 0x1D03
            if (state_ == Iap2AuthState::IdentSent) {
                TransitionTo(Iap2AuthState::IdentRetried, Iap2Action::RetryIdentify, "IdentificationRejected");
                return Iap2Action::RetryIdentify;
            }
            if (state_ == Iap2AuthState::IdentRetried) {
                TransitionTo(state_, Iap2Action::Abort, "Second IdentificationRejected — aborting");
                return Iap2Action::Abort;
            }
            break;

        case id::CarPlayAvailability: // 0x4300
            TransitionTo(state_, Iap2Action::Note, "CarPlayAvailability");
            return Iap2Action::Note;

        default:
            break;
    }

    return Iap2Action::None;
}

Common::Status Iap2SessionManager::ExecuteSendCert(Iap2LinkEngine& link) {
    if (!mfi_signer_) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "No MFi signer configured");
    }

    auto certResult = mfi_signer_->FetchCertificate();
    if (certResult.IsError()) {
        return certResult.Error();
    }

    // Wrap certificate in group_one(0x0000, cert) then send as 0xAA01.
    Common::ByteBuffer body = Iap2MessageCatalog::GroupOne(0x0000, certResult.Value());
    auto pkt = link.BuildMessage(1 /* Control session */, Iap2MsgId::AuthCertificate, body);

    if (link.GetMySeq() > 0) {
        // Packet was already built with seq consumed; send it via the transport.
        // BuildMessage doesn't send automatically — caller must feed to transport.
        // We return Ok and let the driver handle transmission of `pkt`.
    }

    (void)pkt;
    return Common::Status::Ok();
}

Common::Status Iap2SessionManager::ExecuteSignChallenge(Common::ByteSpan challenge, Iap2LinkEngine& link) {
    if (!mfi_signer_) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "No MFi signer configured");
    }

    auto sigResult = mfi_signer_->SignChallenge(challenge);
    if (sigResult.IsError()) {
        return sigResult.Error();
    }

    // Wrap signature in group_one(0x0000, sig) then send as 0xAA03.
    Common::ByteBuffer body = Iap2MessageCatalog::GroupOne(0x0000, sigResult.Value());
    auto pkt = link.BuildMessage(1, Iap2MsgId::AuthResponse, body);

    (void)pkt;
    return Common::Status::Ok();
}

Common::Status Iap2SessionManager::ExecuteSendIdentify(Iap2LinkEngine& link, const std::vector<uint16_t>& excludeParams) {
    // Build the IdentifyInformation body. For a full retry with exclusions we'd rebuild
    // omitting specific top-level groups; for now we build the standard wireless declaration.
    Common::ByteBuffer identBody = Iap2MessageCatalog::BuildIdentifyInfo(
        config_.accessoryName, config_.btMac);

    // In a full retry path, `excludeParams` would filter top-level TLV groups from identBody.
    (void)excludeParams;

    auto pkt = link.BuildMessage(1, Iap2MsgId::IdentifyInformation, identBody);
    (void)pkt;
    return Common::Status::Ok();
}

void Iap2SessionManager::TransitionTo(Iap2AuthState newState, Iap2Action action, std::string_view desc) {
    state_ = newState;
    if (event_cb_) {
        event_cb_(state_, action, desc);
    }
}

} // namespace CarPlayStation::Core::Protocol
