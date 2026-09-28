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
    env.network.mfi_daemon_endpoint = IpEndpoint{"192.168.2.50", 9999};
    assert(env.network.mfi_daemon_endpoint.ToString() == "192.168.2.50:9999");

    // Application layer: user-tunable preferences stay out of Core and resolve to concrete values
    using namespace CarPlayStation::Application;
    AppSettings settings;

    settings.video.resolution_preset = VideoResolutionPreset::Res720P;
    auto [w720, h720] = settings.video.GetEffectiveDimensions();
    assert(w720 == 1280 && h720 == 720);

    settings.video.resolution_preset = VideoResolutionPreset::Auto;
    auto [wAuto, hAuto] = settings.video.GetEffectiveDimensions(3440, 1440);
    assert(wAuto == 3440 && hAuto == 1440);

    settings.video.display_mode = WindowDisplayMode::BorderlessFullscreen;
    settings.video.preferred_codec = VideoCodecPreference::HEVC;
    settings.audio.sample_rate = 48000;
    settings.audio.ducking_ratio = 0.25f;
    assert(settings.video.preferred_codec == VideoCodecPreference::HEVC);

    std::cout << "[PASS] TestConfigSeparation\n";
}

int main() {
    std::cout << "--- Running CarPlayStation Foundation Unit Tests ---\n";
    TestStatusAndResult();
    TestEndianConversions();
    TestTypesAndFormatting();
    TestConfigSeparation();
    std::cout << "--- All Foundation Unit Tests Passed Successfully! ---\n";
    return 0;
}
