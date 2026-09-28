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

void TestDynamicConfig() {
    VideoConfig video;
    video.resolution_preset = VideoResolutionPreset::Res720P;
    auto [w720, h720] = video.GetEffectiveDimensions();
    assert(w720 == 1280 && h720 == 720);

    video.resolution_preset = VideoResolutionPreset::Res1080P;
    auto [w1080, h1080] = video.GetEffectiveDimensions();
    assert(w1080 == 1920 && h1080 == 1080);

    video.resolution_preset = VideoResolutionPreset::Res1440P;
    auto [w1440, h1440] = video.GetEffectiveDimensions();
    assert(w1440 == 2560 && h1440 == 1440);

    video.resolution_preset = VideoResolutionPreset::Res2160P;
    auto [w2160, h2160] = video.GetEffectiveDimensions();
    assert(w2160 == 3840 && h2160 == 2160);

    video.resolution_preset = VideoResolutionPreset::Auto;
    auto [wAuto, hAuto] = video.GetEffectiveDimensions(3440, 1440);
    assert(wAuto == 3440 && hAuto == 1440);

    video.display_mode = WindowDisplayMode::BorderlessFullscreen;
    video.preferred_codec = VideoCodecPreference::HEVC;
    assert(video.preferred_codec == VideoCodecPreference::HEVC);

    AudioConfig audio;
    audio.sample_rate = 44100;
    audio.bitrate_kbps = 320;
    assert(audio.sample_rate == 44100);
    assert(audio.bitrate_kbps == 320);

    CarPlayStationConfig fullConfig;
    fullConfig.network.mfi_daemon_endpoint = IpEndpoint{"192.168.2.50", 9999};
    assert(fullConfig.network.mfi_daemon_endpoint.ToString() == "192.168.2.50:9999");

    std::cout << "[PASS] TestDynamicConfig\n";
}

int main() {
    std::cout << "--- Running CarPlayStation Foundation Unit Tests ---\n";
    TestStatusAndResult();
    TestEndianConversions();
    TestTypesAndFormatting();
    TestDynamicConfig();
    std::cout << "--- All Foundation Unit Tests Passed Successfully! ---\n";
    return 0;
}
