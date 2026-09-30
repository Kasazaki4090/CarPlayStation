#include "Core/Common/Types.h"
#include "Core/Common/Config.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Media/IMicrophoneUplink.h"
#include "Core/Media/MockVideoDecoder.h"
#include "Core/Media/MockAudioPipeline.h"
#include "Core/Media/MockMicrophoneUplink.h"
#include "Core/Media/AirPlayVideoDemuxer.h"
#include "Core/Media/MfVideoDecoder.h"
#include "Core/Media/FFmpegVideoDecoder.h"
#include "Core/Media/AudioDuckingManager.h"
#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Media/WasapiMicrophoneUplink.h"
#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <type_traits>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Media;

// Print a value to a stream. Enums are printed as their underlying integer so
// the ASSERT macros can report them without requiring operator<< overloads.
template <typename T>
void PrintValue(std::ostream& os, const T& v) {
    if constexpr (std::is_enum_v<T>) {
        os << static_cast<std::underlying_type_t<T>>(v);
    } else {
        os << v;
    }
}

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
            std::cerr << "Assertion failed: " #a " == " #b " ("; \
            PrintValue(std::cerr, (a)); \
            std::cerr << " != "; \
            PrintValue(std::cerr, (b)); \
            std::cerr << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
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

// ---------------------------------------------------------------------------
// Step 2 helpers
// ---------------------------------------------------------------------------

// Append a 4-byte Big-Endian length-prefixed NALU to a payload buffer.
void AppendLengthPrefixedNalu(std::vector<uint8_t>& payload, const uint8_t* nalu, size_t naluSize) {
    const uint32_t len = static_cast<uint32_t>(naluSize);
    payload.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(len & 0xFF));
    payload.insert(payload.end(), nalu, nalu + naluSize);
}

// Test 5: AirPlayVideoDemuxer NALU demuxing, length-prefix -> Annex-B
// conversion, SPS/PPS/VPS parsing, and IDR keyframe detection.
void TestAirPlayVideoDemuxer() {
    std::cout << "[RUN] TestAirPlayVideoDemuxer" << std::endl;

    AirPlayVideoDemuxer demuxer;
    demuxer.SetStreamGeometry(1920, 720, Common::VideoCodec::H264);
    ASSERT_EQ(demuxer.GetStreamWidth(), 1920U);
    ASSERT_EQ(demuxer.GetStreamHeight(), 720U);

    // H.264 NALU first bytes: SPS=0x67, PPS=0x68, IDR slice=0x65, non-IDR=0x41.
    const uint8_t spsNalu[] = {0x67, 0x42, 0x00, 0x1E, 0xEB};
    const uint8_t ppsNalu[] = {0x68, 0xCE, 0x3C, 0x80};
    const uint8_t idrSlice[] = {0x65, 0x88, 0x84, 0x00};
    const uint8_t nonIdrSlice[] = {0x41, 0x99, 0x22, 0x33, 0x44};

    DemuxedFrame lastFrame{};
    bool gotFrame = false;
    demuxer.SetFrameCallback([&](const DemuxedFrame& f) {
        lastFrame = f;
        gotFrame = true;
    });

    // Keyframe payload: SPS + PPS + IDR slice (length-prefixed).
    std::vector<uint8_t> keyPayload;
    AppendLengthPrefixedNalu(keyPayload, spsNalu, sizeof(spsNalu));
    AppendLengthPrefixedNalu(keyPayload, ppsNalu, sizeof(ppsNalu));
    AppendLengthPrefixedNalu(keyPayload, idrSlice, sizeof(idrSlice));

    auto status = demuxer.FeedPayload(Common::ByteSpan{keyPayload.data(), keyPayload.size()}, 1000000ULL);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(gotFrame);
    ASSERT_TRUE(lastFrame.isKeyFrame);
    ASSERT_TRUE(lastFrame.containsSps);
    ASSERT_TRUE(lastFrame.containsPps);
    ASSERT_EQ(lastFrame.firstNaluType, NalUnitType::SPS);
    ASSERT_EQ(lastFrame.ptsUs, 1000000ULL);

    // The Annex-B output must begin with a 00 00 00 01 start code.
    ASSERT_TRUE(lastFrame.annexBData.size() >= 4);
    ASSERT_EQ(lastFrame.annexBData[0], 0x00);
    ASSERT_EQ(lastFrame.annexBData[1], 0x00);
    ASSERT_EQ(lastFrame.annexBData[2], 0x00);
    ASSERT_EQ(lastFrame.annexBData[3], 0x01);

    // The demuxer must have captured the latest SPS / PPS (Annex-B encoded).
    ASSERT_TRUE(demuxer.GetLatestSps().size() >= 4);
    ASSERT_TRUE(demuxer.GetLatestPps().size() >= 4);
    ASSERT_EQ(demuxer.GetLatestSps()[0], 0x00);
    ASSERT_EQ(demuxer.GetLatestSps()[3], 0x01);

    // Non-IDR frame: no parameter sets, not a keyframe.
    gotFrame = false;
    std::vector<uint8_t> nonIdrPayload;
    AppendLengthPrefixedNalu(nonIdrPayload, nonIdrSlice, sizeof(nonIdrSlice));
    status = demuxer.FeedPayload(Common::ByteSpan{nonIdrPayload.data(), nonIdrPayload.size()}, 1016666ULL);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(gotFrame);
    ASSERT_TRUE(!lastFrame.isKeyFrame);
    ASSERT_TRUE(!lastFrame.containsSps);
    ASSERT_EQ(lastFrame.firstNaluType, NalUnitType::SliceNonIdr);

    // A keyframe that omits its own SPS/PPS must be prefixed with the latest
    // captured parameter sets so the decoder has full stream context.
    gotFrame = false;
    std::vector<uint8_t> bareIdrPayload;
    AppendLengthPrefixedNalu(bareIdrPayload, idrSlice, sizeof(idrSlice));
    status = demuxer.FeedPayload(Common::ByteSpan{bareIdrPayload.data(), bareIdrPayload.size()}, 1033333ULL);
    ASSERT_TRUE(status.IsOk());
    ASSERT_TRUE(gotFrame);
    ASSERT_TRUE(lastFrame.isKeyFrame);
    ASSERT_TRUE(!lastFrame.containsSps); // the frame itself carried no SPS
    // But the assembled Annex-B must still contain the prefixed SPS/PPS.
    ASSERT_TRUE(lastFrame.annexBData.size() > (4 + sizeof(idrSlice)));

    ASSERT_EQ(demuxer.GetDeliveredFrameCount(), 3ULL);
    ASSERT_EQ(demuxer.GetDroppedFrameCount(), 0ULL);

    std::cout << "[PASS] TestAirPlayVideoDemuxer" << std::endl;
}

// Test 6: AirPlayVideoDemuxer frame-loss detection and resync to next IDR.
void TestAirPlayVideoDemuxerResync() {
    std::cout << "[RUN] TestAirPlayVideoDemuxerResync" << std::endl;

    AirPlayVideoDemuxer demuxer;
    demuxer.SetStreamGeometry(2560, 720, Common::VideoCodec::H264);

    const uint8_t spsNalu[] = {0x67, 0x42, 0x00, 0x1E, 0xEB};
    const uint8_t ppsNalu[] = {0x68, 0xCE, 0x3C, 0x80};
    const uint8_t idrSlice[] = {0x65, 0x88, 0x84, 0x00};
    const uint8_t nonIdrSlice[] = {0x41, 0x99, 0x22, 0x33, 0x44};

    DemuxedFrame lastFrame{};
    uint32_t delivered = 0;
    demuxer.SetFrameCallback([&](const DemuxedFrame& f) {
        lastFrame = f;
        delivered++;
    });

    // Establish a keyframe first.
    std::vector<uint8_t> keyPayload;
    AppendLengthPrefixedNalu(keyPayload, spsNalu, sizeof(spsNalu));
    AppendLengthPrefixedNalu(keyPayload, ppsNalu, sizeof(ppsNalu));
    AppendLengthPrefixedNalu(keyPayload, idrSlice, sizeof(idrSlice));
    ASSERT_TRUE(demuxer.FeedPayload(Common::ByteSpan{keyPayload.data(), keyPayload.size()}, 0).IsOk());
    ASSERT_EQ(delivered, 1U);

    // Truncated payload: a length prefix that claims more bytes than remain.
    std::vector<uint8_t> truncated;
    truncated.push_back(0x00);
    truncated.push_back(0x00);
    truncated.push_back(0x00);
    truncated.push_back(0x64); // claims 100 bytes, but only 2 follow
    truncated.push_back(0x41);
    truncated.push_back(0x99);
    ASSERT_TRUE(demuxer.FeedPayload(Common::ByteSpan{truncated.data(), truncated.size()}, 1000).IsOk());
    ASSERT_EQ(demuxer.GetDroppedFrameCount(), 1ULL);
    ASSERT_EQ(demuxer.GetResyncCount(), 1ULL);
    ASSERT_EQ(delivered, 1U); // truncated frame was not delivered

    // A non-IDR frame while awaiting a keyframe must be dropped.
    std::vector<uint8_t> nonIdrPayload;
    AppendLengthPrefixedNalu(nonIdrPayload, nonIdrSlice, sizeof(nonIdrSlice));
    ASSERT_TRUE(demuxer.FeedPayload(Common::ByteSpan{nonIdrPayload.data(), nonIdrPayload.size()}, 2000).IsOk());
    ASSERT_EQ(demuxer.GetDroppedFrameCount(), 2ULL);
    ASSERT_EQ(delivered, 1U);

    // The next IDR keyframe resynchronizes the stream.
    std::vector<uint8_t> resyncPayload;
    AppendLengthPrefixedNalu(resyncPayload, idrSlice, sizeof(idrSlice));
    ASSERT_TRUE(demuxer.FeedPayload(Common::ByteSpan{resyncPayload.data(), resyncPayload.size()}, 3000).IsOk());
    ASSERT_EQ(delivered, 2U);
    ASSERT_TRUE(lastFrame.isKeyFrame);

    std::cout << "[PASS] TestAirPlayVideoDemuxerResync" << std::endl;
}

// Test 7: MfVideoDecoder and FFmpegVideoDecoder graceful degradation.
// On a host without the hardware MFT / FFmpeg runtime, both decoders must
// report NotSupported (or a clean capability) rather than crash, so the
// pipeline can fall back to the CPU / mock path.
void TestHardwareDecoderGracefulFallback() {
    std::cout << "[RUN] TestHardwareDecoderGracefulFallback" << std::endl;

    Common::VideoStreamSpec spec{
        .codec = Common::VideoCodec::H264,
        .width = 1920,
        .height = 1080,
        .fps = 60
    };

    // FFmpeg decoder: dynamic binding. If the runtime is absent, Init must
    // return NotSupported (not crash) and QueryCapabilities must be safe.
    FFmpegVideoDecoder ffmpeg;
    auto caps = ffmpeg.QueryCapabilities();
    ASSERT_TRUE(caps.supported_codecs.size() >= 2U);
    auto ffStatus = ffmpeg.Init(spec);
    if (ffStatus.IsError()) {
        // Expected on a host without the FFmpeg runtime DLLs.
        ASSERT_EQ(ffStatus.Code(), Common::StatusCode::NotSupported);
    }
    ffmpeg.Shutdown();

    // Media Foundation decoder: on non-Windows (or without a GPU MFT) Init
    // must return NotSupported rather than crash.
    MfVideoDecoder mf;
    auto mfCaps = mf.QueryCapabilities();
    ASSERT_TRUE(mfCaps.supported_codecs.size() >= 2U);
    auto mfStatus = mf.Init(spec);
    if (mfStatus.IsError()) {
        ASSERT_EQ(mfStatus.Code(), Common::StatusCode::NotSupported);
    }
    mf.Shutdown();

    std::cout << "[PASS] TestHardwareDecoderGracefulFallback" << std::endl;
}

// ---------------------------------------------------------------------------
// Step 3 helpers & tests
// ---------------------------------------------------------------------------

// Test 8: AudioDuckingManager attack/release gain curve and timing.
void TestAudioDuckingManager() {
    std::cout << "[RUN] TestAudioDuckingManager" << std::endl;

    AudioDuckingManager duck;
    duck.SetConfig(AudioDuckingManager::Config{
        .fullGain = 1.0f,
        .duckedGain = 0.20f,
        .attackMs = 100,
        .releaseMs = 400
    });

    // Idle: gain is full (100%).
    ASSERT_EQ(duck.CurrentGain(), 1.0f);
    ASSERT_TRUE(duck.IsSettled());

    // Start a navigation prompt: attack fades 100% -> 20% over 100ms.
    duck.SetNavigationActive(true);
    ASSERT_TRUE(duck.IsDucked());
    ASSERT_TRUE(!duck.IsSettled());

    // After 50ms (half the attack), gain should be ~60%.
    float g = duck.Tick(50);
    ASSERT_TRUE(g > 0.55f && g < 0.65f);

    // After the full 100ms attack, gain reaches the 20% floor.
    g = duck.Tick(50);
    ASSERT_EQ(g, 0.20f);
    ASSERT_TRUE(duck.IsSettled());

    // End the prompt: release restores 20% -> 100% over 400ms.
    duck.SetNavigationActive(false);
    ASSERT_TRUE(!duck.IsDucked());
    ASSERT_TRUE(!duck.IsSettled());

    // After 200ms (half the release), gain should be ~60%.
    g = duck.Tick(200);
    ASSERT_TRUE(g > 0.55f && g < 0.65f);

    // After the full 400ms release, gain is back to 100%.
    g = duck.Tick(200);
    ASSERT_EQ(g, 1.0f);
    ASSERT_TRUE(duck.IsSettled());

    // Telephony prompt also ducks.
    duck.SetTelephonyActive(true);
    ASSERT_TRUE(duck.IsDucked());
    g = duck.Tick(100);
    ASSERT_EQ(g, 0.20f);
    duck.SetTelephonyActive(false);
    g = duck.Tick(400);
    ASSERT_EQ(g, 1.0f);

    std::cout << "[PASS] TestAudioDuckingManager" << std::endl;
}

// Test 9: WasapiAudioPipeline 3-lane software mixer (mathematical addition)
// and soft-limiter clamping.
void TestWasapiAudioPipelineMixer() {
    std::cout << "[RUN] TestWasapiAudioPipelineMixer" << std::endl;

    WasapiAudioPipeline pipeline;
    Common::AudioOutputFormat format{
        .sample_rate = 48000,
        .channels = 2,
        .bit_depth = 16,
        .sample_type = Common::AudioSampleType::PcmInt16
    };
    ASSERT_TRUE(pipeline.Start(format).IsOk());
    ASSERT_TRUE(pipeline.IsRunning());

    // Feed each lane a constant 0.5f across 4 samples.
    const float a[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    pipeline.PushLaneSamples(AudioLane::MainMedia, a, 4);
    pipeline.PushLaneSamples(AudioLane::AltNavigation, a, 4);
    pipeline.PushLaneSamples(AudioLane::SiriTelephony, a, 4);

    // 3 lanes * 0.5 = 1.5 per sample, which exceeds the 0.95 limiter threshold.
    auto mixed = pipeline.RenderMixed(4);
    ASSERT_EQ(mixed.size(), 4U);
    ASSERT_TRUE(mixed[0] > 0.9f && mixed[0] <= 0.95f); // soft-limited to threshold
    ASSERT_TRUE(pipeline.GetLimiterClampCount() > 0U);

    // Ducking attenuates the MainMedia lane.
    pipeline.ClearMixBuffers();
    pipeline.SetDuckingRatio(0.25f);
    pipeline.PushLaneSamples(AudioLane::MainMedia, a, 4); // 0.5 * 0.25 = 0.125
    pipeline.SetDuckingRatio(1.0f);
    auto ducked = pipeline.RenderMixed(4);
    ASSERT_TRUE(ducked[0] > 0.10f && ducked[0] < 0.15f);

    // Lane volume multiplier.
    pipeline.ClearMixBuffers();
    pipeline.SetLaneVolume(AudioLane::MainMedia, 0.5f);
    pipeline.PushLaneSamples(AudioLane::MainMedia, a, 4); // 0.5 * 0.5 = 0.25
    auto vol = pipeline.RenderMixed(4);
    ASSERT_TRUE(vol[0] > 0.20f && vol[0] < 0.30f);

    // WriteAudio path: 16-bit PCM bytes are converted and counted.
    std::vector<uint8_t> pcm(48000 * 4, 0x00); // 1s of 16-bit stereo
    ASSERT_TRUE(pipeline.WriteAudio(AudioLane::MainMedia, Common::ByteSpan{pcm.data(), pcm.size()}).IsOk());
    ASSERT_EQ(pipeline.GetRenderedSampleCount(), 48000ULL);

    pipeline.Shutdown();
    ASSERT_TRUE(!pipeline.IsRunning());

    std::cout << "[PASS] TestWasapiAudioPipelineMixer" << std::endl;
}

// Test 10: WasapiAudioPipeline multi-rate resampler.
void TestWasapiAudioPipelineResampler() {
    std::cout << "[RUN] TestWasapiAudioPipelineResampler" << std::endl;

    // A 48kHz ramp of 48 samples (0..47) resampled to 24kHz -> 24 samples.
    std::vector<float> src(48);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<float>(i);
    }
    auto dst = WasapiAudioPipeline::ResampleLinear(src.data(), src.size(), 48000, 24000, 24);
    ASSERT_EQ(dst.size(), 24U);
    // Linear interpolation: dst[i] ~= 2*i (each 24kHz sample spans 2 48kHz samples).
    ASSERT_TRUE(dst[0] < 1.0f);
    ASSERT_TRUE(dst[10] > 18.0f && dst[10] < 22.0f);
    ASSERT_TRUE(dst[23] > 44.0f && dst[23] < 48.0f);

    std::cout << "[PASS] TestWasapiAudioPipelineResampler" << std::endl;
}

// Test 11: WasapiMicrophoneUplink resampling to 16kHz mono and packet dispatch.
void TestWasapiMicrophoneUplink() {
    std::cout << "[RUN] TestWasapiMicrophoneUplink" << std::endl;

    WasapiMicrophoneUplink mic;
    Common::AudioOutputFormat micFormat{
        .sample_rate = 48000,
        .channels = 1,
        .bit_depth = 16,
        .sample_type = Common::AudioSampleType::PcmInt16
    };
    ASSERT_TRUE(mic.Start(micFormat).IsOk());
    ASSERT_TRUE(mic.IsCapturing());

    uint32_t receivedBytes = 0;
    uint64_t receivedTs = 0;
    uint32_t callbackCount = 0;
    mic.SetUplinkCallback([&](Common::ByteSpan data, uint64_t ts) {
        receivedBytes = static_cast<uint32_t>(data.size());
        receivedTs = ts;
        callbackCount++;
    });

    // 48000 samples (1s) at 48kHz -> resampled to 16kHz (16000 samples),
    // dispatched in 160-sample (10ms) packets = 100 packets.
    std::vector<float> src(48000, 0.25f);
    mic.ProcessCapturedSamples(src.data(), src.size(), 48000, 16000, 160, 5000000ULL);

    ASSERT_EQ(mic.GetCapturedSampleCount(), 16000ULL);
    ASSERT_EQ(mic.GetPacketDispatchCount(), 100U);
    ASSERT_EQ(callbackCount, 100U);
    ASSERT_EQ(receivedTs, 5000000ULL);
    // Each packet is 160 float32 samples = 640 bytes.
    ASSERT_EQ(receivedBytes, 640U);

    // Input gain is applied to the dispatched samples.
    mic.SetInputGain(2.0f);
    ASSERT_EQ(mic.GetInputGain(), 2.0f);

    ASSERT_TRUE(mic.Stop().IsOk());
    ASSERT_TRUE(!mic.IsCapturing());

    std::cout << "[PASS] TestWasapiMicrophoneUplink" << std::endl;
}

// Test 12: MockVideoDecoder + MockAudioPipeline end-to-end integration.
void TestMockEndToEndIntegration() {
    std::cout << "[RUN] TestMockEndToEndIntegration" << std::endl;

    MockVideoDecoder video;
    MockAudioPipeline audio;

    Common::VideoStreamSpec spec{
        .codec = Common::VideoCodec::H264,
        .width = 1920,
        .height = 720,
        .fps = 60
    };
    ASSERT_TRUE(video.Init(spec).IsOk());

    Common::AudioOutputFormat audioFormat{
        .sample_rate = 48000,
        .channels = 2,
        .bit_depth = 16,
        .sample_type = Common::AudioSampleType::PcmInt16
    };
    ASSERT_TRUE(audio.Start(audioFormat).IsOk());

    // Simulate a frame callback that also drives the audio lane (A/V sync).
    uint32_t frames = 0;
    video.SetFrameCallback([&](uintptr_t, uint32_t, uint32_t, uint64_t) {
        frames++;
        std::vector<uint8_t> pcm(48000 * 4, 0x01); // 1s of audio per frame
        (void)audio.WriteAudio(AudioLane::MainMedia, Common::ByteSpan{pcm.data(), pcm.size()});
    });

    const uint8_t nalu[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(video.DecodeAccessUnit(Common::ByteSpan{nalu, sizeof(nalu)},
                                           static_cast<uint64_t>(i) * 16666ULL).IsOk());
    }

    ASSERT_EQ(video.GetDecodedFrameCount(), 3ULL);
    ASSERT_EQ(frames, 3U);
    ASSERT_EQ(audio.GetRenderedSampleCount(), 3ULL * 48000ULL);
    ASSERT_EQ(audio.GetLaneWriteCalls(AudioLane::MainMedia), 3U);

    video.Shutdown();
    audio.Shutdown();

    std::cout << "[PASS] TestMockEndToEndIntegration" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  CarPlayStation Media Tests (Step 1-4) " << std::endl;
    std::cout << "========================================" << std::endl;

    TestVideoCodecAndNaluEnums();
    TestMockVideoDecoderLifecycle();
    TestMockAudioPipelineLanesAndDucking();
    TestMockMicrophoneUplink();

    // Step 2: video demuxer + hardware decoder fallback.
    TestAirPlayVideoDemuxer();
    TestAirPlayVideoDemuxerResync();
    TestHardwareDecoderGracefulFallback();

    // Step 3: 3-lane audio mixer, ducking manager & microphone uplink.
    TestAudioDuckingManager();
    TestWasapiAudioPipelineMixer();
    TestWasapiAudioPipelineResampler();
    TestWasapiMicrophoneUplink();

    // Step 4: end-to-end mock integration.
    TestMockEndToEndIntegration();

    std::cout << "========================================" << std::endl;
    std::cout << "  ALL MEDIA TESTS PASSED!               " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
