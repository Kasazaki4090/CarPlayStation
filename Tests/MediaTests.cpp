#include "Core/Common/Types.h"
#include "Core/Common/Config.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Media/IMicrophoneUplink.h"
#include "Core/Media/MockVideoDecoder.h"
#include "Core/Media/MockAudioPipeline.h"
#include "Core/Media/MockMicrophoneUplink.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Media;

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "Assertion failed: " #expr " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "Assertion failed: " #a " == " #b " (" << (a) << " != " << (b) << ") at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

// Test 1: Codec and NALU string conversions and enum values
void TestVideoCodecAndNaluEnums() {
    std::cout << "[RUN] TestVideoCodecAndNaluEnums" << std::endl;

    ASSERT_EQ(Common::VideoCodecToString(Common::VideoCodec::H264), "H264");
    ASSERT_EQ(Common::VideoCodecToString(Common::VideoCodec::HEVC), "HEVC");
    ASSERT_EQ(Common::VideoCodecToString(Common::VideoCodec::AV1),  "AV1");
    ASSERT_EQ(Common::VideoCodecToString(Common::VideoCodec::VP9),  "VP9");
    ASSERT_EQ(Common::VideoCodecToString(Common::VideoCodec::VVC),  "VVC");

    ASSERT_EQ(NalUnitTypeToString(NalUnitType::SliceNonIdr), "SliceNonIdr");
    ASSERT_EQ(NalUnitTypeToString(NalUnitType::SliceIdr),    "SliceIdr");
    ASSERT_EQ(NalUnitTypeToString(NalUnitType::SPS),         "SPS");
    ASSERT_EQ(NalUnitTypeToString(NalUnitType::PPS),         "PPS");
    ASSERT_EQ(NalUnitTypeToString(NalUnitType::VPS),         "VPS");

    ASSERT_EQ(Common::AudioSampleTypeToString(Common::AudioSampleType::PcmInt16),   "PcmInt16");
    ASSERT_EQ(Common::AudioSampleTypeToString(Common::AudioSampleType::PcmInt24),   "PcmInt24");
    ASSERT_EQ(Common::AudioSampleTypeToString(Common::AudioSampleType::PcmFloat32), "PcmFloat32");

    std::cout << "[PASS] TestVideoCodecAndNaluEnums" << std::endl;
}

// Test 2: MockVideoDecoder lifecycle, PTS tracking, and callbacks
void TestMockVideoDecoderLifecycle() {
    std::cout << "[RUN] TestMockVideoDecoderLifecycle" << std::endl;

    MockVideoDecoder decoder;
    ASSERT_TRUE(!decoder.IsInitialized());

    Common::VideoStreamSpec spec{
        .codec = Common::VideoCodec::HEVC,
        .width = 2560,
        .height = 1440,
        .fps = 60
    };

    auto status = decoder.Init(spec);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(decoder.IsInitialized());
    ASSERT_EQ(decoder.GetCurrentSpec().width, 2560);
    ASSERT_EQ(decoder.GetCurrentSpec().height, 1440);

    // Frame callback verification
    uintptr_t deliveredHandle = 0;
    uint32_t deliveredW = 0, deliveredH = 0;
    uint64_t deliveredPts = 0;
    uint32_t callbackCount = 0;

    decoder.SetFrameCallback([&](uintptr_t handle, uint32_t w, uint32_t h, uint64_t pts) {
        deliveredHandle = handle;
        deliveredW = w;
        deliveredH = h;
        deliveredPts = pts;
        callbackCount++;
    });

    uint8_t dummyNalu[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF}; // Dummy HEVC IDR
    Common::ByteSpan span{dummyNalu, sizeof(dummyNalu)};

    status = decoder.DecodeAccessUnit(span, 1000000ULL);
    ASSERT_TRUE(status.IsOk());
    ASSERT_EQ(decoder.GetDecodedFrameCount(), 1ULL);
    ASSERT_EQ(decoder.GetTotalDecodedBytes(), sizeof(dummyNalu));
    ASSERT_EQ(decoder.GetLastPtsUs(), 1000000ULL);
    ASSERT_EQ(callbackCount, 1U);
    ASSERT_EQ(deliveredHandle, 0xDEADBEEF);
    ASSERT_EQ(deliveredW, 2560U);
    ASSERT_EQ(deliveredH, 1440U);
    ASSERT_EQ(deliveredPts, 1000000ULL);

    // Reconfigure test
    Common::VideoStreamSpec newSpec{
        .codec = Common::VideoCodec::H264,
        .width = 1920,
        .height = 1080,
        .fps = 60
    };
    status = decoder.Reconfigure(newSpec);
    ASSERT_TRUE(status.IsOk());
    ASSERT_EQ(decoder.GetCurrentSpec().width, 1920);
    ASSERT_EQ(decoder.GetReconfigureCount(), 1U);

    decoder.Flush();
    ASSERT_EQ(decoder.GetFlushCount(), 1U);

    decoder.Shutdown();
    ASSERT_TRUE(!decoder.IsInitialized());

    std::cout << "[PASS] TestMockVideoDecoderLifecycle" << std::endl;
}

// Test 3: MockAudioPipeline 3-Lane writes, volume multipliers, and ducking ratio
void TestMockAudioPipelineLanesAndDucking() {
    std::cout << "[RUN] TestMockAudioPipelineLanesAndDucking" << std::endl;

    MockAudioPipeline audio;
    ASSERT_TRUE(!audio.IsRunning());

    Common::AudioOutputFormat format{
        .sample_rate = 48000,
        .channels = 2,
        .bit_depth = 16,
        .sample_type = Common::AudioSampleType::PcmInt16
    };

    auto status = audio.Start(format);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(audio.IsRunning());

    // Write to MainMedia
    std::vector<uint8_t> mediaPcm(48000 * 4, 0x11); // 1 second of 16-bit stereo PCM
    status = audio.WriteAudio(AudioLane::MainMedia, Common::ByteSpan{mediaPcm.data(), mediaPcm.size()});
    ASSERT_TRUE(status.IsOk());
    ASSERT_EQ(audio.GetLaneWrittenBytes(AudioLane::MainMedia), mediaPcm.size());
    ASSERT_EQ(audio.GetRenderedSampleCount(), 48000ULL);

    // Navigation speech activates ducking
    audio.SetDuckingRatio(0.25f);
    ASSERT_EQ(audio.GetDuckingRatio(), 0.25f);

    // Write to AltNavigation
    std::vector<uint8_t> navPcm(24000 * 4, 0x22);
    status = audio.WriteAudio(AudioLane::AltNavigation, Common::ByteSpan{navPcm.data(), navPcm.size()});
    ASSERT_TRUE(status.IsOk());
    ASSERT_EQ(audio.GetLaneWrittenBytes(AudioLane::AltNavigation), navPcm.size());
    ASSERT_EQ(audio.GetRenderedSampleCount(), 72000ULL);

    // Lane volume adjustments
    audio.SetLaneVolume(AudioLane::MainMedia, 0.8f);
    ASSERT_EQ(audio.GetLaneVolume(AudioLane::MainMedia), 0.8f);

    audio.StopLane(AudioLane::AltNavigation);
    ASSERT_EQ(audio.GetLaneStopCount(AudioLane::AltNavigation), 1U);

    audio.Shutdown();
    ASSERT_TRUE(!audio.IsRunning());

    std::cout << "[PASS] TestMockAudioPipelineLanesAndDucking" << std::endl;
}

// Test 4: MockMicrophoneUplink injection and sample count
void TestMockMicrophoneUplink() {
    std::cout << "[RUN] TestMockMicrophoneUplink" << std::endl;

    MockMicrophoneUplink mic;
    ASSERT_TRUE(!mic.IsCapturing());

    Common::AudioOutputFormat micFormat{
        .sample_rate = 16000,
        .channels = 1,
        .bit_depth = 16,
        .sample_type = Common::AudioSampleType::PcmInt16
    };

    auto status = mic.Start(micFormat);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(mic.IsCapturing());

    uint32_t receivedBytes = 0;
    uint64_t receivedTimestamp = 0;
    mic.SetUplinkCallback([&](Common::ByteSpan data, uint64_t ts) {
        receivedBytes = static_cast<uint32_t>(data.size());
        receivedTimestamp = ts;
    });

    std::vector<uint8_t> voiceData(320, 0x33); // 10ms of 16kHz 16-bit mono PCM (160 samples = 320 bytes)
    mic.InjectAudioData(Common::ByteSpan{voiceData.data(), voiceData.size()}, 5000000ULL);

    ASSERT_EQ(receivedBytes, 320U);
    ASSERT_EQ(receivedTimestamp, 5000000ULL);
    ASSERT_EQ(mic.GetCapturedSampleCount(), 160ULL);
    ASSERT_EQ(mic.GetCallbackDispatchCount(), 1U);

    mic.SetInputGain(1.5f);
    ASSERT_EQ(mic.GetInputGain(), 1.5f);

    status = mic.Stop();
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(!mic.IsCapturing());

    std::cout << "[PASS] TestMockMicrophoneUplink" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  CarPlayStation Media Tests (Step 1)   " << std::endl;
    std::cout << "========================================" << std::endl;

    TestVideoCodecAndNaluEnums();
    TestMockVideoDecoderLifecycle();
    TestMockAudioPipelineLanesAndDucking();
    TestMockMicrophoneUplink();

    std::cout << "========================================" << std::endl;
    std::cout << "  ALL MEDIA STEP 1 TESTS PASSED!        " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
