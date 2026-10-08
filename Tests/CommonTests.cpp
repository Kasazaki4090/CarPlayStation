#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Logging.h"
#include "Core/Common/Config.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Transport/ITransportChannel.h"
#include "Core/Protocol/IIap2Link.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Service/ICarPlayCoreApi.h"
#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include <cassert>
#include <iostream>

using namespace CarPlayStation::Core::Common;

void TestStatusAndResult() {
    Status okStatus = Status::Ok();
    assert(okStatus.IsOk());
    assert(!okStatus.IsError());
    assert(okStatus.Code() == StatusCode::Ok);

    Status errStatus = Status::Error(StatusCode::Timeout, "Operation timed out");
    assert(errStatus.IsError());
    assert(errStatus.Code() == StatusCode::Timeout);
    assert(errStatus.Message() == "Operation timed out");

    Result<int> goodResult = 42;
    assert(goodResult.IsOk());
    assert(goodResult.Value() == 42);
    assert(*goodResult == 42);

    Result<std::string> badResult = Status::Error(StatusCode::NotFound, "Resource missing");
    assert(badResult.IsError());
    assert(badResult.Error().Code() == StatusCode::NotFound);

    std::cout << "[PASS] TestStatusAndResult\n";
}

void TestEndianConversions() {
    uint8_t buffer[8] = {0};

    // Big Endian 16
    Endian::WriteBe16(buffer, 0x1234);
    assert(buffer[0] == 0x12 && buffer[1] == 0x34);
    assert(Endian::ReadBe16(buffer) == 0x1234);

    // Big Endian 32
    Endian::WriteBe32(buffer, 0x12345678);
    assert(buffer[0] == 0x12 && buffer[1] == 0x34 && buffer[2] == 0x56 && buffer[3] == 0x78);
    assert(Endian::ReadBe32(buffer) == 0x12345678);

    // Big Endian 64
    Endian::WriteBe64(buffer, 0x0102030405060708ULL);
    assert(Endian::ReadBe64(buffer) == 0x0102030405060708ULL);

    // Little Endian 16 & 32
    Endian::WriteLe16(buffer, 0x1234);
    assert(buffer[0] == 0x34 && buffer[1] == 0x12);
    assert(Endian::ReadLe16(buffer) == 0x1234);

    Endian::WriteLe32(buffer, 0x12345678);
    assert(buffer[0] == 0x78 && buffer[1] == 0x56 && buffer[2] == 0x34 && buffer[3] == 0x12);
    assert(Endian::ReadLe32(buffer) == 0x12345678);

    std::cout << "[PASS] TestEndianConversions\n";
}

void TestTypesAndFormatting() {
    MacAddress mac{0x00, 0x11, 0x22, 0xAA, 0xBB, 0xCC};
    assert(mac.ToString() == "00:11:22:aa:bb:cc");

    IpEndpoint ep{"192.168.1.100", 5000};
    assert(ep.ToString() == "192.168.1.100:5000");

    std::vector<uint8_t> data = {0xFF, 0x5A, 0x00, 0x10};
    assert(ToHexString(data) == "ff 5a 00 10");

    std::cout << "[PASS] TestTypesAndFormatting\n";
}

void TestConfigSeparation() {
    // Core layer: concrete negotiated video spec only (no UI concepts like presets or window mode)
    VideoStreamSpec spec;
    assert(spec.width == 1920 && spec.height == 1080 && spec.fps == 60);
    assert(spec.codec == VideoCodec::H264);
    spec.codec = VideoCodec::HEVC;
    assert(VideoCodecToString(spec.codec) == "HEVC");

    // Core layer: concrete audio output format (no volumes or bitrate concepts)
    AudioOutputFormat fmt;
    fmt.sample_rate = 44100;
    fmt.channels = 2;
    fmt.bit_depth = 16;
    assert(fmt.sample_rate == 44100);

    // Core layer: environment config (identity + network endpoints)
    CoreEnvironmentConfig env;
    env.identity.device_name = "TestReceiver";
    env.network.rtsp_port = 5001;
    assert(env.network.rtsp_port == 5001);

    // Application layer: user-tunable preferences stay out of Core and resolve to concrete values
    using namespace CarPlayStation::Application;
    AppSettings settings;
    assert(settings.mfi.backend_type != MFiBackendType::HardwareI2C); // Default to local file/network per DEC-003/DEC-006
    settings.mfi.remote_mfi.host = "192.168.2.50";
    settings.mfi.remote_mfi.port = 9999;
    assert(settings.mfi.remote_mfi.host == "192.168.2.50" && settings.mfi.remote_mfi.port == 9999);

    settings.video.resolution_preset = VideoResolutionPreset::Res720P;
    auto [w720, h720] = settings.video.GetEffectiveDimensions();
    assert(w720 == 1280 && h720 == 720);

    settings.video.resolution_preset = VideoResolutionPreset::Auto;
    auto [wAuto, hAuto] = settings.video.GetEffectiveDimensions(3440, 1440);
    assert(wAuto == 3440 && hAuto == 1440);

    settings.video.display_mode = WindowDisplayMode::BorderlessFullscreen;
    settings.video.preferred_codec = VideoCodecPreference::HEVC;
    // Audio settings are the three lane volumes and ducking attenuation ratio.
    settings.audio.media_volume = 0.5f;
    settings.audio.navigation_volume = 0.4f;
    settings.audio.telephony_volume = 0.6f;
    settings.audio.ducking_ratio = 0.25f;
    assert(settings.audio.media_volume == 0.5f);
    assert(settings.audio.ducking_ratio == 0.25f);
    assert(settings.video.preferred_codec == VideoCodecPreference::HEVC);

    std::cout << "[PASS] TestConfigSeparation\n";
}

#include "Core/Session/Pairing/Crypto.h"

void TestCryptoPrimitives() {
    std::cout << "[Test] Running Crypto RFC 8439 test vectors...\n";

    // 1. RFC 8439 Section 2.5.2 Poly1305 test vector
    const char* msg = "Cryptographic Forum Research Group";
    uint8_t poly_key[32] = {
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
        0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
        0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
        0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
    };
    uint8_t expected_poly_tag[16] = {
        0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
        0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9
    };
    uint8_t poly_tag[16] = {};
    pairing::Poly1305(poly_key, reinterpret_cast<const uint8_t*>(msg), strlen(msg), poly_tag);
    for (int i = 0; i < 16; ++i) {
        if (poly_tag[i] != expected_poly_tag[i]) {
            std::cerr << "Poly1305 mismatch at " << i << ": got " << std::hex << (int)poly_tag[i]
                      << " expected " << (int)expected_poly_tag[i] << std::dec << "\n";
            assert(false);
        }
    }
    std::cout << "[PASS] RFC 8439 Poly1305 test vector\n";

    // 2. RFC 8439 Section 2.8.2 ChaCha20-Poly1305 AEAD test vector
    uint8_t aead_key[32];
    for (int i = 0; i < 32; ++i) aead_key[i] = static_cast<uint8_t>(0x80 + i);

    uint8_t aead_nonce[12] = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
    };
    uint8_t aead_aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
    };
    const char* plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t pt_len = strlen(plaintext);

    auto sealed = pairing::ChaCha20Poly1305SealWithAad(
        aead_key, aead_nonce, aead_aad, sizeof(aead_aad), reinterpret_cast<const uint8_t*>(plaintext), pt_len);
    assert(sealed.size() == pt_len + 16);

    uint8_t expected_tag[16] = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
    };
    for (int i = 0; i < 16; ++i) {
        if (sealed[pt_len + i] != expected_tag[i]) {
            std::cerr << "AEAD Tag mismatch at " << i << ": got " << std::hex << (int)sealed[pt_len + i]
                      << " expected " << (int)expected_tag[i] << std::dec << "\n";
            assert(false);
        }
    }
    std::cout << "[PASS] RFC 8439 ChaCha20-Poly1305 Seal test vector\n";

    std::vector<uint8_t> decrypted;
    bool open_ok = pairing::ChaCha20Poly1305OpenWithAad(
        aead_key, aead_nonce, aead_aad, sizeof(aead_aad), sealed.data(), sealed.size(), decrypted);
    assert(open_ok);
    assert(decrypted.size() == pt_len);
    assert(memcmp(decrypted.data(), plaintext, pt_len) == 0);
    std::cout << "[PASS] RFC 8439 ChaCha20-Poly1305 Open test vector\n";

    // 3. PS-Msg05 label round-trip test
    uint8_t ps_key[32];
    for (int i = 0; i < 32; ++i) ps_key[i] = static_cast<uint8_t>(0x10 + i);
    std::vector<uint8_t> test_subtlv = { 0x01, 0x06, 'f', 'o', 'o', 'b', 'a', 'r' };
    auto ps_sealed = pairing::ChaCha20Poly1305Seal(
        ps_key, "PS-Msg05", test_subtlv.data(), test_subtlv.size());
    assert(ps_sealed.size() == test_subtlv.size() + 16);
    auto ps_opened = pairing::ChaCha20Poly1305Open(
        ps_key, "PS-Msg05", ps_sealed.data(), ps_sealed.size());
    assert(ps_opened.has_value());
    assert(*ps_opened == test_subtlv);
    std::cout << "[PASS] PS-Msg05 round-trip test\n";
}

int main() {
    std::cout << "--- Running CarPlayStation Foundation Unit Tests ---\n";
    TestCryptoPrimitives();
    TestStatusAndResult();
    TestEndianConversions();
    TestTypesAndFormatting();
    TestConfigSeparation();
    std::cout << "--- All Foundation Unit Tests Passed Successfully! ---\n";
    return 0;
}
