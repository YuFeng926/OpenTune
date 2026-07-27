#include "../Source/Render/PlaybackSourcePublisher.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Inference/RenderCache.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/PluginProcessor.h"
#include "../Source/Utils/TimeCoordinate.h"
#include "../Source/Content/ContentKey.h"
#include "../Source/StandaloneArrangement.h"
#include "../Source/Render/ContentRenderService.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

using namespace OpenTune;

namespace OpenTune {
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    return nullptr;
}
} // namespace OpenTune

namespace OpenTune {

// ============================================================================
// PluginProcessorTransportTestAccessor
//
// Friend-accessor for testing transport internals. The production side
// (PluginProcessor.h) declares this struct as a friend and provides the
// private fields: audioReadCursor_, transportCursor_, currentOutputGain_,
// targetOutputGain_, rampSamplesRemaining_, transitionCompletionCursor_,
// transitionCompletionPhase_, transitionActive_, phase_, playHeadState_.
//
// The accessor allows:
//  - setProjection(P): write timeInSeconds=P, bump presentationEpoch,
//    publish epoch-matched projection with horizon=P.
//  - Reading the transport state fields listed above.
// ============================================================================
struct PluginProcessorTransportTestAccessor
{
    OpenTuneAudioProcessor& proc;

    explicit PluginProcessorTransportTestAccessor(OpenTuneAudioProcessor& p) : proc(p) {}

    // -- Transport state readers --

    int64_t audioReadCursor() const { return proc.audioReadCursor_; }
    int64_t transportCursor() const { return proc.transportCursor_; }
    float currentOutputGain() const { return proc.currentOutputGain_; }
    float targetOutputGain() const { return proc.targetOutputGain_; }
    int rampSamplesRemaining() const { return proc.rampSamplesRemaining_; }
    int64_t transitionCompletionCursor() const { return proc.transitionCompletionCursor_; }
    bool transitionActive() const { return proc.transitionActive_; }
    OpenTuneAudioProcessor::RuntimePhase phase() const { return proc.phase_; }
    const PlayHeadState& playHeadState() const { return proc.playHeadState_; }

    // -- Projection writer --
    // Sets the UI projection to P (seconds): writes timeInSeconds, bumps
    // presentationEpoch, publishes epoch-matched projection with horizon=P.
    void setProjection(double PSeconds, double nowClockSec)
    {
        auto& state = proc.playHeadState_;
        state.timeInSeconds.store(PSeconds, std::memory_order_relaxed);
        const uint64_t newEpoch = state.presentationEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        state.presentationProjection.publish(PSeconds, nowClockSec, PSeconds, newEpoch);
    }
};

} // namespace OpenTune

// ============================================================================
// RenderCacheTestAccessor — global namespace (matches RenderCache.h:16 friend decl).
// Returns shared_ptr to first prepared chunk audio, avoiding exposure of
// private nested types as function signatures.
// ============================================================================
struct RenderCacheTestAccessor
{
    static auto getFirstPreparedChunkAudio(const OpenTune::RenderCache& cache)
        -> std::shared_ptr<const std::vector<float>>
    {
        const auto* snap = cache.preparedSnapshot_.get();
        if (!snap || snap->chunks.empty())
            return {};
        return snap->chunks[0].audio;
    }

    static double getPreparedSampleRate(const OpenTune::RenderCache& cache)
    {
        const auto* snap = cache.preparedSnapshot_.get();
        return snap ? snap->sampleRate : 0.0;
    }

    static bool hasPreparedChunks(const OpenTune::RenderCache& cache)
    {
        const auto* snap = cache.preparedSnapshot_.get();
        return snap && !snap->chunks.empty();
    }
};

namespace {

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cout << "[FAIL] " << message << "\n";
    }
}

std::filesystem::path sourcePath(std::string_view relative)
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR) / std::filesystem::path(relative);
}

std::string readText(std::string_view relative)
{
    const auto path = sourcePath(relative);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool contains(std::string_view text, std::string_view token)
{
    return text.find(token) != std::string_view::npos;
}

// ============================================================================
// Test 1: PlaybackSourcePublisher 默认 44.1kHz publish → preparedDry alias canonical;
//         切 48k/96k 后 source 有正确 sampleRate 和长度；
//         metadata republish 复用同一 prepared buffer shared_ptr。
// ============================================================================

void playbackSourcePublisherDefault441PublishAndRateSwitch()
{
    PlaybackSourcePublisher publisher;

    // 构造一段 44100-sample 的 44.1kHz 测试数据
    constexpr int canonicalSamples = 44100;
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, canonicalSamples);
    {
        auto* data = buffer->getWritePointer(0);
        for (int i = 0; i < canonicalSamples; ++i)
            data[i] = static_cast<float>(i) * 0.0001f;
    }

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 1001;

    PlaybackReadSource source;
    source.audioBuffer = buffer;
    source.audioSampleRate = 44100.0;
    source.pitchRevision = 1;
    source.timeGridRevision = 1;
    source.pitchShiftRevision = 1;
    source.timeGridIsIdentity = true;

    // --- 默认 44.1kHz publish: preparedDry alias canonical ---
    publisher.publish(key, source);

    {
        PlaybackReadSource out;
        expect(publisher.get(key, out), "get after publish");
        expect(out.preparedDry.buffer != nullptr, "preparedDry.buffer non-null at 44.1k");
        expect(std::abs(out.preparedDry.sampleRate - 44100.0) < 1.0, "preparedDry.sampleRate == 44100");
        // Alias: same shared_ptr as canonical buffer
        expect(out.preparedDry.buffer == buffer, "preparedDry.buffer aliases canonical audioBuffer at 44.1k");
        expect(out.preparedDry.canonicalBuffer == buffer, "preparedDry.canonicalBuffer == audioBuffer");
    }

    // --- 切换到 48kHz ---
    publisher.setPlaybackSampleRate(48000.0);

    {
        PlaybackReadSource out;
        expect(publisher.get(key, out), "get after 48k switch");
        expect(out.preparedDry.buffer != nullptr, "preparedDry.buffer non-null at 48k");
        expect(std::abs(out.preparedDry.sampleRate - 48000.0) < 1.0, "preparedDry.sampleRate == 48000");
        expect(out.preparedDry.buffer != buffer, "preparedDry.buffer != canonical at 48k (resampled)");
        expect(out.preparedDry.canonicalBuffer == buffer, "preparedDry.canonicalBuffer still == audioBuffer");

        const int expectedLen = static_cast<int>(std::round(44100.0 * 48000.0 / 44100.0));
        expect(out.preparedDry.buffer->getNumSamples() == expectedLen,
               "48k prepared length = round(44100 * 48000 / 44100) = 48000");
    }

    // --- 切换到 96kHz ---
    publisher.setPlaybackSampleRate(96000.0);

    {
        PlaybackReadSource out;
        expect(publisher.get(key, out), "get after 96k switch");
        expect(out.preparedDry.buffer != nullptr, "preparedDry.buffer non-null at 96k");
        expect(std::abs(out.preparedDry.sampleRate - 96000.0) < 1.0, "preparedDry.sampleRate == 96000");
        const int expectedLen = static_cast<int>(std::round(44100.0 * 96000.0 / 44100.0));
        expect(out.preparedDry.buffer->getNumSamples() == expectedLen,
               "96k prepared length = round(44100 * 96000 / 44100) = 96000");
    }

    // --- Metadata republish: same canonical shared_ptr, only revision changes ---
    {
        // Grab the current prepared buffer shared_ptr
        PlaybackReadSource before;
        publisher.get(key, before);
        auto prepBufBefore = before.preparedDry.buffer;

        // Republish with new revisions
        PlaybackReadSource source2;
        source2.audioBuffer = buffer;               // same canonical shared_ptr
        source2.audioSampleRate = 44100.0;
        source2.pitchRevision = 2;                   // changed
        source2.timeGridRevision = 2;                // changed
        source2.pitchShiftRevision = 2;             // changed
        source2.timeGridIsIdentity = true;
        publisher.publish(key, source2);

        PlaybackReadSource after;
        publisher.get(key, after);
        // canonical buffer unchanged (shared_ptr identity)
        expect(after.audioBuffer == buffer, "metadata republish preserves canonical audioBuffer");
        // preparedDry should be reused (same canonical buffer + same target rate)
        expect(after.preparedDry.buffer == prepBufBefore,
               "metadata republish reuses same prepared buffer shared_ptr");
        // Revisions updated
        expect(after.pitchRevision == 2, "pitchRevision updated");
        expect(after.timeGridRevision == 2, "timeGridRevision updated");
        expect(after.pitchShiftRevision == 2, "pitchShiftRevision updated");
    }
}

// ============================================================================
// Test 2: readPlaybackAudio 相邻整数 readStartSample 分块读取
//         逐样本等于 preparedDry 对应区间，证明 direct-copy 连续无重叠/跳样。
// ============================================================================

void readPlaybackAudioAdjacentChunkDirectCopyContinuity()
{
    PlaybackSourcePublisher publisher;
    constexpr double testRate = 44100.0;

    // 构造 1 秒 44.1kHz 单声道 ramp: sample[i] = i * 0.0001f
    constexpr int totalSamples = 44100;
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, totalSamples);
    {
        auto* data = buffer->getWritePointer(0);
        for (int i = 0; i < totalSamples; ++i)
            data[i] = static_cast<float>(i) * 0.0001f;
    }

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 2001;

    PlaybackReadSource source;
    source.audioBuffer = buffer;
    source.audioSampleRate = testRate;
    source.pitchRevision = 1;
    source.timeGridRevision = 1;
    source.pitchShiftRevision = 1;
    source.timeGridIsIdentity = true;
    publisher.publish(key, source);

    PlaybackReadSource srcOut;
    publisher.get(key, srcOut);

    // 分两段相邻读取: [0, 1000) 和 [1000, 2000)
    juce::AudioBuffer<float> dest(2, 2000);
    dest.clear();

    PlaybackReadRequest req1(srcOut, 0, testRate, 1000);
    readPlaybackAudio(req1, dest, 0);

    PlaybackReadRequest req2(srcOut, 1000, testRate, 1000);
    readPlaybackAudio(req2, dest, 1000);

    // 逐样本验证 dest 值 == preparedDry 对应位置值
    const auto& prepared = srcOut.preparedDry;
    for (int i = 0; i < 2000; ++i) {
        const float expected = prepared.buffer->getSample(0, i);
        const float actual0 = dest.getSample(0, i);
        const float actual1 = dest.getSample(1, i);
        expect(std::abs(actual0 - expected) < 1.0e-9f,
               "ch0[" + std::to_string(i) + "] == preparedDry");
        expect(std::abs(actual1 - expected) < 1.0e-9f,
               "ch1[" + std::to_string(i) + "] == preparedDry (multi-channel copy)");
    }
}

// ============================================================================
// Test 3: active prepared rate 切到 96k 后，readCanonicalAudio 仍逐样本
//         返回原始 44.1kHz canonical buffer，证明导出/Stage2 不依赖 active rate。
// ============================================================================

void readCanonicalAudioIndependentOfActivePreparedRate()
{
    PlaybackSourcePublisher publisher;
    constexpr double canonicalRate = 44100.0;

    constexpr int canonicalSamples = 4410;
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, canonicalSamples);
    {
        auto* data = buffer->getWritePointer(0);
        for (int i = 0; i < canonicalSamples; ++i)
            data[i] = static_cast<float>(i) * 0.001f;
    }

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 3001;

    PlaybackReadSource source;
    source.audioBuffer = buffer;
    source.audioSampleRate = canonicalRate;
    source.pitchRevision = 1;
    source.timeGridRevision = 1;
    source.pitchShiftRevision = 1;
    source.timeGridIsIdentity = true;
    publisher.publish(key, source);

    // 切换到 96kHz prepared rate
    publisher.setPlaybackSampleRate(96000.0);

    PlaybackReadSource srcOut;
    publisher.get(key, srcOut);

    // preparedDry 已是 96kHz
    expect(std::abs(srcOut.preparedDry.sampleRate - 96000.0) < 1.0,
           "preparedDry at 96k after switch");

    // readCanonicalAudio 仍返回 canonical 44.1kHz 逐样本值
    juce::AudioBuffer<float> dest(1, 500);
    dest.clear();

    CanonicalReadRequest cReq(srcOut, 100, 500);
    const int wrote = readCanonicalAudio(cReq, dest, 0);
    expect(wrote == 500, "readCanonicalAudio wrote 500 samples at 96k prepared rate");

    for (int i = 0; i < 500; ++i) {
        const float expected = buffer->getSample(0, 100 + i);
        const float actual = dest.getSample(0, i);
        expect(std::abs(actual - expected) < 1.0e-9f,
               "canonical[" + std::to_string(100 + i) + "] == " + std::to_string(expected) +
               " (got " + std::to_string(actual) + ") at 96k prepared rate");
    }
}

// ============================================================================
// Test 4: RenderCache public API 建立 canonical chunk，
//         44.1kHz prepared overlay 替换 dry：
//         dry=0.25, render=0.75, readPlaybackAudio 重叠区严格为 0.75。
// ============================================================================

void renderCacheCanonicalChunkAndPreparedOverlayReplacesDry()
{
    PlaybackSourcePublisher publisher;

    constexpr int canonicalSamples = 44100;
    auto dryBuf = std::make_shared<juce::AudioBuffer<float>>(1, canonicalSamples);
    {
        auto* data = dryBuf->getWritePointer(0);
        for (int i = 0; i < canonicalSamples; ++i)
            data[i] = 0.25f;   // dry = 0.25
    }

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 4001;

    // 创建 RenderCache 并建立 canonical chunk
    auto renderCache = std::make_shared<RenderCache>();

    // requestRenderPending → getNextPendingJob
    constexpr int64_t chunkStart = 1000;
    constexpr int64_t chunkEndExclusive = 3000;
    constexpr int chunkSamples = static_cast<int>(chunkEndExclusive - chunkStart);

    renderCache->requestRenderPending(chunkStart, chunkEndExclusive);

    RenderCache::PendingJob job;
    expect(renderCache->getNextPendingJob(job), "getNextPendingJob returns a job");
    expect(job.startSample == chunkStart, "job.startSample correct");
    expect(job.endSampleExclusive == chunkEndExclusive, "job.endSampleExclusive correct");
    expect(job.targetRevision > 0, "job.targetRevision > 0");

    // completeChunkRenderWithAudio: render audio all = 0.75
    std::vector<float> renderAudio(chunkSamples, 0.75f);

    const auto result = renderCache->completeChunkRenderWithAudio(
        chunkStart, chunkEndExclusive, std::move(renderAudio), job.targetRevision);
    expect(result == RenderCache::ChunkRenderResult::Published, "completeChunkRenderWithAudio published");

    // Prepare for 44.1kHz (aliased path)
    renderCache->prepareForPlaybackSampleRate(44100.0);

    // 构造 source 引用该 RenderCache
    PlaybackReadSource source;
    source.contentKey = key;
    source.audioBuffer = dryBuf;
    source.audioSampleRate = 44100.0;
    source.renderCache = renderCache;
    source.pitchRevision = 1;
    source.timeGridRevision = 1;
    source.pitchShiftRevision = 1;
    source.timeGridIsIdentity = true;

    publisher.publish(key, source);

    PlaybackReadSource srcOut;
    publisher.get(key, srcOut);

    // readPlaybackAudio 读取重叠区 [1500, 2500)
    juce::AudioBuffer<float> dest(1, 1000);
    dest.clear();

    PlaybackReadRequest req(srcOut, 1500, 44100.0, 1000);
    const int wrote = readPlaybackAudio(req, dest, 0);
    expect(wrote == 1000, "readPlaybackAudio wrote 1000 samples");

    // 重叠区全部为 0.75（render overlay replaced dry）
    for (int i = 0; i < 1000; ++i) {
        const float actual = dest.getSample(0, i);
        expect(std::abs(actual - 0.75f) < 1.0e-6f,
               "overlay[" + std::to_string(1500 + i) + "] == 0.75, not 0.25 (got " + std::to_string(actual) + ")");
    }

    // 非重叠区（chunk 外的前缀）应保持 dry = 0.25
    juce::AudioBuffer<float> destPrefix(1, 500);
    destPrefix.clear();
    PlaybackReadRequest reqPrefix(srcOut, 0, 44100.0, 500);
    readPlaybackAudio(reqPrefix, destPrefix, 0);
    for (int i = 0; i < 500; ++i) {
        expect(std::abs(destPrefix.getSample(0, i) - 0.25f) < 1.0e-6f,
               "non-overlay prefix[" + std::to_string(i) + "] == 0.25");
    }
}

// ============================================================================
// Test 5: TimeStretchCache 默认 44.1kHz store 后 prepared slice 逐样本
//         等于 canonical；切 48k 后 prepared 可读，长度/相邻切片连续；
//         canonical slice 仍与原 canonical 一致。
// ============================================================================

void timeStretchCacheDefault441PreparedEqualsCanonicalAndRateSwitch()
{
    TimeStretchCache cache;

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 5001;

    constexpr int audioLen = 44100;
    std::vector<float> canonical(audioLen);
    for (int i = 0; i < audioLen; ++i)
        canonical[i] = static_cast<float>(i) * 0.00001f;

    // store at 44.1kHz
    uint32_t gen = cache.beginBuild(key);
    expect(gen == 0, "beginBuild returns generation 0 for first build");

    cache.store(key, canonical, 1, 1, 1, 44100.0, gen);

    // --- 默认 44.1kHz: prepared slice 逐样本等于 canonical ---
    {
        juce::AudioBuffer<float> dest(1, 500);
        dest.clear();
        int wrote = cache.sliceForOutputRange(key, 1, 1, 1, 100, dest, 0, 500, 44100);
        expect(wrote == 500, "sliceForOutputRange at 44.1k wrote 500");

        for (int i = 0; i < 500; ++i) {
            expect(std::abs(dest.getSample(0, i) - canonical[100 + i]) < 1.0e-9f,
                   "prepared slice[" + std::to_string(100 + i) + "] == canonical at 44.1k");
        }
    }

    // --- 切到 48kHz: prepared 可读，长度和相邻切片连续 ---
    cache.prepareForPlaybackSampleRate(48000.0);

    {
        // 读第一段
        juce::AudioBuffer<float> dest1(1, 500);
        dest1.clear();
        int wrote1 = cache.sliceForOutputRange(key, 1, 1, 1, 0, dest1, 0, 500, 48000);
        expect(wrote1 == 500, "sliceForOutputRange at 48k first chunk wrote 500");

        // 读第二段（连续紧接第一段）
        juce::AudioBuffer<float> dest2(1, 500);
        dest2.clear();
        int wrote2 = cache.sliceForOutputRange(key, 1, 1, 1, 500, dest2, 0, 500, 48000);
        expect(wrote2 == 500, "sliceForOutputRange at 48k second chunk wrote 500");

        // 第二段第一样本不应等于第一段最后样本（连续信号）
        expect(std::abs(dest1.getSample(0, 499) - dest2.getSample(0, 0)) > 1.0e-7f,
               "adjacent prepared slices at 48k are different (continuous signal)");
    }

    // --- 48k prepared 长度正确 ---
    {
        const int expected48kLen = static_cast<int>(std::round(44100.0 * 48000.0 / 44100.0));
        juce::AudioBuffer<float> dest(1, expected48kLen);
        int wrote = cache.sliceForOutputRange(key, 1, 1, 1, 0, dest, 0, expected48kLen, 48000);
        expect(wrote == expected48kLen, "prepared total length at 48k = round(44100 * 48000 / 44100) = 48000");
    }

    // --- Canonical slice 仍与原 canonical 一致（不受 prepared rate 影响）---
    {
        juce::AudioBuffer<float> dest(1, 500);
        dest.clear();
        int wrote = cache.sliceCanonicalForOutputRange(key, 1, 1, 1, 200, dest, 0, 500);
        expect(wrote == 500, "sliceCanonicalForOutputRange wrote 500 after 48k switch");

        for (int i = 0; i < 500; ++i) {
            expect(std::abs(dest.getSample(0, i) - canonical[200 + i]) < 1.0e-9f,
                   "canonical slice[" + std::to_string(200 + i) + "] == original canonical after 48k switch");
        }
    }
}

// ============================================================================
// Shared test fixture: create a Standalone-wrapped processor with a ramp
// source published and a placement inserted. Returns processor + accessor.
// ============================================================================

struct TransportTestFixture
{
    static constexpr double kDeviceRate = 48000.0;
    static constexpr double kSourceDurationSec = 10.0;
    static constexpr int kSourceSamples = static_cast<int>(kDeviceRate * kSourceDurationSec); // 480000

    std::unique_ptr<OpenTuneAudioProcessor> proc;
    std::unique_ptr<PluginProcessorTransportTestAccessor> acc;
    std::shared_ptr<juce::AudioBuffer<float>> sourceBuf;
    ContentKey sourceKey;
    int blockSize_;

    // Saved wrapper state for teardown
    juce::AudioProcessor::WrapperType savedJucePlugInClientCurrentWrapperType =
        juce::AudioProcessor::wrapperType_Undefined;

    TransportTestFixture(int blockSize = 64)
        : blockSize_(blockSize)
    {
        // Save and set wrapper type to Standalone
        savedJucePlugInClientCurrentWrapperType =
            juce::PluginHostType::jucePlugInClientCurrentWrapperType;
        juce::PluginHostType::jucePlugInClientCurrentWrapperType =
            juce::AudioProcessor::wrapperType_Standalone;
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_Standalone);

        proc = std::make_unique<OpenTuneAudioProcessor>();
        acc = std::make_unique<PluginProcessorTransportTestAccessor>(*proc);

        // Restore wrapper type: reset next-plugin hint, then restore global
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_Undefined);
        juce::PluginHostType::jucePlugInClientCurrentWrapperType =
            savedJucePlugInClientCurrentWrapperType;

        proc->prepareToPlay(kDeviceRate, blockSize_);

        // Create source: source[i] = 0.1f + i*1e-6f — each position is uniquely identifiable
        sourceBuf = std::make_shared<juce::AudioBuffer<float>>(1, kSourceSamples);
        {
            auto* data = sourceBuf->getWritePointer(0);
            for (int i = 0; i < kSourceSamples; ++i)
                data[i] = 0.1f + static_cast<float>(i) * 1.0e-6f;
        }

        sourceKey.domainKind = DomainKind::StandaloneClip;
        sourceKey.objectId = 1;

        // Publish playback source
        {
            PlaybackReadSource src;
            src.contentKey = sourceKey;
            src.audioBuffer = sourceBuf;
            src.audioSampleRate = kDeviceRate;
            src.renderCache = proc->getContentRenderService()->getOrCreateRenderCache(sourceKey);
            src.timeStretchCache = &proc->getContentRenderService()->getTimeStretchCache();
            src.pitchRevision = 1;
            src.timeGridRevision = 1;
            src.pitchShiftRevision = 1;
            src.timeGridIsIdentity = true;
            proc->getContentRenderService()->publishPlaybackSource(sourceKey, std::move(src));
        }

        // Insert placement covering the full source
        {
            StandaloneArrangement::Placement placement;
            placement.contentKey = sourceKey;
            placement.timelineStartSeconds = 0.0;
            placement.durationSeconds = kSourceDurationSec;
            placement.gain = 1.0f;
            proc->getStandaloneArrangement()->insertPlacement(/*trackId=*/0, placement);
        }
    }

    ~TransportTestFixture()
    {
        if (proc) {
            proc->releaseResources();
        }
    }

    // Process one block and return the output buffer (2 channels, or as configured).
    juce::AudioBuffer<float> pushBlock(int blockSamples = -1)
    {
        const int bs = (blockSamples > 0) ? blockSamples : blockSize_;
        const int outCh = std::max(2, proc->getTotalNumOutputChannels());
        juce::AudioBuffer<float> buf(outCh, bs);
        buf.clear();
        juce::MidiBuffer midi;
        proc->processBlock(buf, midi);
        return buf;
    }

    // Total number of blocks for a 0.2s fade at the current block size.
    static int fadeBlocksForSize(int blockSize)
    {
        return static_cast<int>(kDeviceRate * 0.2) / blockSize;
    }
};

// ============================================================================
// Test 6: Transport behavior tests (scenarios 1–7)
// Replaces the old audioProcessorRuntimeTransportCursorAdvanceAndFade().
//
// Scenario 1: Pause — first output sample from real frontier F, not UI P.
// Scenario 2: Stop — same source fidelity; final cursors=0, phase=Stopped.
// Scenario 3: PauseAtPosition — source fidelity; final cursors=target.
// Scenario 4: Play — first block does 0→1 fade-in, no hard full-gain start.
// Scenario 5: Seek during Play — fade-out from F, commit target, fade-in.
// Scenario 6: Consecutive Pause→Stop — no gain jump, strict tolerance.
// Scenario 7: Reverse ramp fade-in→Stop — no gain jump on direction change.
// ============================================================================

void audioProcessorRuntimeTransportCursorAdvanceAndFade()
{
    constexpr int blockSize = 64;
    constexpr int fadeTotal = static_cast<int>(TransportTestFixture::kDeviceRate * 0.2); // 9600
    constexpr int fadeBlocks = fadeTotal / blockSize; // 150

    // ========================================================================
    // Scenario 1: Pause
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        // Play and reach full gain
        proc.setPosition(0.0);
        fix.pushBlock(); // consume Seek
        proc.play();
        fix.pushBlock(); // consume Play → enter transition (fade-in)

        // Push through fade-in
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // Now in Playing phase with full gain
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing, "S1: phase == Playing after fade-in");
        expect(!acc.transitionActive(), "S1: transitionActive false after fade-in");
        expect(std::abs(acc.currentOutputGain() - 1.0f) < 0.001f,
               "S1: currentOutputGain == 1.0 after fade-in");

        // Record real frontier F
        int64_t F = acc.audioReadCursor();
        expect(F > 0, "S1: audioReadCursor > 0 after play");

        // Set UI projection P within the previous block [F-blockSize, F)
        int64_t P = F - blockSize / 2; // mid-block position
        double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
        acc.setProjection(TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate),
                          nowClock);

        // Pause — UI thinks we're at P, audio frontier is F
        proc.pause();

        // First fade-out block: audio must read from F, not P
        auto buf = fix.pushBlock();

        // Verify first output sample: must match source[F], not source[P]
        const float ch0Sample0 = buf.getSample(0, 0);
        const float sourceAtF = fix.sourceBuf->getSample(0, static_cast<int>(F));
        const float sourceAtP = fix.sourceBuf->getSample(0, static_cast<int>(P));

        expect(std::abs(ch0Sample0 - sourceAtF) < 1.0e-6f,
               "S1: first output sample == source[F] (got " +
               std::to_string(ch0Sample0) + " expected " + std::to_string(sourceAtF) + ")");
        expect(std::abs(sourceAtF - sourceAtP) > 0.0f,
               "S1: source[F] != source[P] (F=" + std::to_string(F) +
               " P=" + std::to_string(P) + ")");

        // PlayHeadState::timeInSeconds must still reflect P (frozen during fade)
        {
            double pSec = TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate);
            double timeSec = acc.playHeadState().timeInSeconds.load(std::memory_order_relaxed);
            expect(std::abs(timeSec - pSec) < 0.001,
                   "S1: timeInSeconds still at P=" + std::to_string(pSec) +
                   " (got " + std::to_string(timeSec) + ")");
        }

        // First sample gain should be 1.0 (continuous from Playing)
        const float firstGain = ch0Sample0 / sourceAtF;
        expect(std::abs(firstGain - 1.0f) < 0.01f,
               "S1: first sample gain continuous at 1.0 (got " + std::to_string(firstGain) + ")");

        // During fade: audioReadCursor advances, transportCursor frozen at P
        int64_t readCursorDuringFade = acc.audioReadCursor();
        int64_t transportDuringFade = acc.transportCursor();
        expect(readCursorDuringFade > F,
               "S1: audioReadCursor advances during fade (from " + std::to_string(F) +
               " to " + std::to_string(readCursorDuringFade) + ")");
        expect(transportDuringFade == P,
               "S1: transportCursor frozen at P=" + std::to_string(P) +
               " (got " + std::to_string(transportDuringFade) + ")");

        // Push remaining fade blocks
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // After fade completes: both cursors = P, phase = Paused, gain = 0
        expect(acc.audioReadCursor() == P,
               "S1: audioReadCursor == P after fade (got " +
               std::to_string(acc.audioReadCursor()) + " expected " + std::to_string(P) + ")");
        expect(acc.transportCursor() == P,
               "S1: transportCursor == P after fade");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Paused,
               "S1: phase == Paused after fade");
        expect(!acc.transitionActive(),
               "S1: transitionActive false after fade");
        expect(std::abs(acc.currentOutputGain()) < 0.001f,
               "S1: currentOutputGain == 0 after fade");
        expect(!acc.playHeadState().isPlaying.load(std::memory_order_acquire),
               "S1: isPlaying false after Pause");
    }

    // ========================================================================
    // Scenario 2: Stop
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        proc.setPosition(0.0);
        fix.pushBlock();
        proc.play();
        fix.pushBlock();
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        int64_t F = acc.audioReadCursor();
        int64_t P = F - blockSize / 2;
        double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
        acc.setProjection(TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate),
                          nowClock);

        proc.stop();

        auto buf = fix.pushBlock();
        const float ch0Sample0 = buf.getSample(0, 0);
        const float sourceAtF = fix.sourceBuf->getSample(0, static_cast<int>(F));
        const float sourceAtP = fix.sourceBuf->getSample(0, static_cast<int>(P));

        expect(std::abs(ch0Sample0 - sourceAtF) < 1.0e-6f,
               "S2: first output sample == source[F] (Stop)");
        expect(std::abs(sourceAtF - sourceAtP) > 0.0f,
               "S2: source[F] != source[P]");
        // PlayHeadState::timeInSeconds must still reflect P (frozen during fade)
        {
            double pSec = TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate);
            double timeSec = acc.playHeadState().timeInSeconds.load(std::memory_order_relaxed);
            expect(std::abs(timeSec - pSec) < 0.001,
                   "S2: timeInSeconds still at P=" + std::to_string(pSec) +
                   " (got " + std::to_string(timeSec) + ")");
        }
        expect(acc.transportCursor() == P,
               "S2: transportCursor frozen at P during Stop fade (got " +
               std::to_string(acc.transportCursor()) + ")");

        // Push remaining fade blocks
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // After Stop fade: both cursors = 0, phase = Stopped
        expect(acc.audioReadCursor() == 0,
               "S2: audioReadCursor == 0 after Stop fade (got " +
               std::to_string(acc.audioReadCursor()) + ")");
        expect(acc.transportCursor() == 0,
               "S2: transportCursor == 0 after Stop fade");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Stopped,
               "S2: phase == Stopped after Stop");
        expect(!acc.transitionActive(),
               "S2: transitionActive false after Stop");
        expect(std::abs(acc.currentOutputGain()) < 0.001f,
               "S2: gain == 0 after Stop");
    }

    // ========================================================================
    // Scenario 3: PauseAtPosition
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        proc.setPosition(0.0);
        fix.pushBlock();
        proc.play();
        fix.pushBlock();
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        int64_t F = acc.audioReadCursor();
        int64_t P = F - blockSize / 2;
        double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
        acc.setProjection(TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate),
                          nowClock);

        // Target: 2.5 seconds = 120000 samples at 48kHz
        constexpr double targetSec = 2.5;
        const int64_t targetCursor = TimeCoordinate::secondsToSamples(targetSec, TransportTestFixture::kDeviceRate);

        proc.pauseAtPosition(targetSec);

        auto buf = fix.pushBlock();
        const float ch0Sample0 = buf.getSample(0, 0);
        const float sourceAtF = fix.sourceBuf->getSample(0, static_cast<int>(F));
        const float sourceAtP = fix.sourceBuf->getSample(0, static_cast<int>(P));

        expect(std::abs(ch0Sample0 - sourceAtF) < 1.0e-6f,
               "S3: first output sample == source[F] (PauseAtPosition)");
        expect(std::abs(sourceAtF - sourceAtP) > 0.0f,
               "S3: source[F] != source[P]");
        // PlayHeadState::timeInSeconds must still reflect P (frozen during fade)
        {
            double pSec = TimeCoordinate::samplesToSeconds(P, TransportTestFixture::kDeviceRate);
            double timeSec = acc.playHeadState().timeInSeconds.load(std::memory_order_relaxed);
            expect(std::abs(timeSec - pSec) < 0.001,
                   "S3: timeInSeconds still at P=" + std::to_string(pSec) +
                   " (got " + std::to_string(timeSec) + ")");
        }
        expect(acc.transportCursor() == P,
               "S3: transportCursor frozen at P during fade (got " +
               std::to_string(acc.transportCursor()) + ")");

        // Push remaining fade blocks
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // After fade: both cursors = targetCursor, phase = Paused
        expect(acc.audioReadCursor() == targetCursor,
               "S3: audioReadCursor == targetCursor after PauseAtPosition (got " +
               std::to_string(acc.audioReadCursor()) + " expected " + std::to_string(targetCursor) + ")");
        expect(acc.transportCursor() == targetCursor,
               "S3: transportCursor == targetCursor after PauseAtPosition");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Paused,
               "S3: phase == Paused after PauseAtPosition");
        expect(!acc.transitionActive(),
               "S3: transitionActive false after PauseAtPosition");
        expect(std::abs(acc.currentOutputGain()) < 0.001f,
               "S3: gain == 0 after PauseAtPosition");

        // Verify timeInSeconds matches target
        double finalTime = acc.playHeadState().timeInSeconds.load(std::memory_order_relaxed);
        expect(std::abs(finalTime - targetSec) < 0.001,
               "S3: timeInSeconds == target 2.5s (got " + std::to_string(finalTime) + ")");
    }

    // ========================================================================
    // Scenario 4: Play from Paused/Stopped — first block does 0→1 fade-in
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        // Start from Stopped (cursor=0)
        proc.setPosition(0.0);
        fix.pushBlock(); // consume Seek
        fix.pushBlock(); // one more to ensure phase settles

        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Paused
            || acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Stopped,
               "S4: phase is Paused/Stopped before Play");

        // Play
        proc.play();
        auto buf = fix.pushBlock();

        // First output sample should be silent (gain=0 at start of fade-in)
        const float firstSample = buf.getSample(0, 0);
        expect(std::abs(firstSample) < 1.0e-6f,
               "S4: first sample silent during fade-in (got " + std::to_string(firstSample) + ")");

        // At end of first block, currentOutputGain is between 0 and 1 (not full-gain)
        float gainEnd = acc.currentOutputGain();
        expect(gainEnd > 0.0f && gainEnd < 1.0f,
               "S4: currentOutputGain at end of first Play block is between 0 and 1 (got " +
               std::to_string(gainEnd) + "), no hard full-gain start");

        // rampSamplesRemaining should be positive (fade-in still in progress)
        expect(acc.rampSamplesRemaining() > 0,
               "S4: rampSamplesRemaining > 0 after first Play block");

        // Push through remaining fade-in
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        expect(std::abs(acc.currentOutputGain() - 1.0f) < 0.001f,
               "S4: currentOutputGain == 1.0 after full fade-in");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing,
               "S4: phase == Playing after fade-in");
    }

    // ========================================================================
    // Scenario 5: Seek during Play — fade-out from F, commit target, fade-in
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        // Build up to Playing
        proc.setPosition(0.0);
        fix.pushBlock();
        proc.play();
        fix.pushBlock();
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // Play some more blocks to get away from position 0
        for (int s = 0; s < 10; ++s)
            fix.pushBlock();

        int64_t F_beforeSeek = acc.audioReadCursor();
        expect(F_beforeSeek > fadeTotal,
               "S5: audioReadCursor well past fade-in before Seek");

        // Seek to target position (0.5 seconds = 24000 samples)
        constexpr double seekTargetSec = 0.5;
        const int64_t seekTargetCursor =
            TimeCoordinate::secondsToSamples(seekTargetSec, TransportTestFixture::kDeviceRate);

        proc.setPosition(seekTargetSec);

        // First fade-out block: still reads from current F
        auto fadeOutBuf = fix.pushBlock();
        const float firstSampleAfterSeek = fadeOutBuf.getSample(0, 0);
        const float sourceAtF = fix.sourceBuf->getSample(0, static_cast<int>(F_beforeSeek));

        expect(std::abs(firstSampleAfterSeek - sourceAtF) < 1.0e-6f,
               "S5: first fade-out sample == source[F] (Seek, F=" +
               std::to_string(F_beforeSeek) + " got " + std::to_string(firstSampleAfterSeek) + ")");

        // Push through fade-out
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();

        // After fade-out completes: silence point.
        // audioReadCursor and transportCursor must both be at seekTargetCursor,
        // currentGain ramp reached 0, targetGain set for fade-in (1.0),
        // transitionActive still true (fade-in phase upcoming), phase = Playing.
        expect(acc.audioReadCursor() == seekTargetCursor,
               "S5: audioReadCursor == seekTargetCursor at silence point (got " +
               std::to_string(acc.audioReadCursor()) + " expected " +
               std::to_string(seekTargetCursor) + ")");
        expect(acc.transportCursor() == seekTargetCursor,
               "S5: transportCursor == seekTargetCursor at silence point (got " +
               std::to_string(acc.transportCursor()) + ")");
        expect(std::abs(acc.currentOutputGain()) < 0.001f,
               "S5: currentGain == 0 at silence point (got " +
               std::to_string(acc.currentOutputGain()) + ")");
        expect(std::abs(acc.targetOutputGain() - 1.0f) < 0.001f,
               "S5: targetGain == 1 at silence point (got " +
               std::to_string(acc.targetOutputGain()) + ")");
        expect(acc.transitionActive(),
               "S5: transitionActive == true at silence point (fade-in upcoming)");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing,
               "S5: phase == Playing at silence point");

        // Push through fade-in
        for (int b = 0; b < fadeBlocks; ++b)
            fix.pushBlock();

        // After fade-in: transition complete, both cursors equal and advanced past target
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing,
               "S5: phase == Playing after Seek transition");
        expect(!acc.transitionActive(),
               "S5: transitionActive false after Seek transition");
        expect(acc.audioReadCursor() == acc.transportCursor(),
               "S5: audioReadCursor == transportCursor after transition (audio=" +
               std::to_string(acc.audioReadCursor()) + " transport=" +
               std::to_string(acc.transportCursor()) + ")");
        expect(acc.audioReadCursor() > seekTargetCursor,
               "S5: audioReadCursor advanced past seek target (cursor=" +
               std::to_string(acc.audioReadCursor()) + " target=" +
               std::to_string(seekTargetCursor) + ")");
    }

    // ========================================================================
    // Scenario 6: Consecutive commands during transition — no gain jump
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        // Reach Playing state
        proc.setPosition(0.0);
        fix.pushBlock();
        proc.play();
        fix.pushBlock();
        for (int b = 1; b < fadeBlocks; ++b)
            fix.pushBlock();
        for (int s = 0; s < 5; ++s)
            fix.pushBlock();

        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing, "S6: Playing before command");

        // Issue Pause (starts fade-out)
        proc.pause();

        // Push one fade-out block
        auto firstFadeBlock = fix.pushBlock();
        float gainBeforeSecondCmd = acc.currentOutputGain();
        int64_t frontierBeforeStop = acc.audioReadCursor();
        expect(gainBeforeSecondCmd > 0.0f && gainBeforeSecondCmd < 1.0f,
               "S6: mid-fade gain after first Pause block (got " +
               std::to_string(gainBeforeSecondCmd) + ")");
        expect(acc.transitionActive(), "S6: transition active during Pause fade");

        // During mid-fade, issue Stop — new command with different completion intent
        proc.stop();

        // Next block's first sample: divide by source[frontier] to get exact pre-command gain
        auto secondFadeBlock = fix.pushBlock();
        const float firstSampleSecondBlock = secondFadeBlock.getSample(0, 0);
        const float sourceAtFrontier = fix.sourceBuf->getSample(0, static_cast<int>(frontierBeforeStop));
        const float gainFromAudio = firstSampleSecondBlock / sourceAtFrontier;
        expect(std::abs(gainFromAudio - gainBeforeSecondCmd) < 1.0e-5f,
               "S6: gain from audio/source[frontier] matches pre-command gain (audioGain=" +
               std::to_string(gainFromAudio) + " expected " + std::to_string(gainBeforeSecondCmd) + ")");

        // New completion intent: Stop (cursor=0) replaces Pause completion
        expect(acc.transitionCompletionCursor() == 0,
               "S6: transitionCompletionCursor updated to 0 (Stop) after second command (got " +
               std::to_string(acc.transitionCompletionCursor()) + ")");

        // Push exactly rampSamplesRemaining blocks to complete the fade-out
        int remainingAfterStop = acc.rampSamplesRemaining();
        expect(remainingAfterStop > 0,
               "S6: rampSamplesRemaining > 0 after Stop command (got " +
               std::to_string(remainingAfterStop) + ")");
        // Push blocks covering remaining ramp samples
        while (acc.rampSamplesRemaining() > 0) {
            fix.pushBlock();
        }
        // After ramp completes: phase must be Stopped, both cursors 0
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Stopped,
               "S6: phase == Stopped after ramp completion (got " +
               std::to_string(static_cast<int>(acc.phase())) + ")");
        expect(acc.audioReadCursor() == 0,
               "S6: audioReadCursor == 0 after Stop ramp (got " +
               std::to_string(acc.audioReadCursor()) + ")");
        expect(acc.transportCursor() == 0,
               "S6: transportCursor == 0 after Stop ramp (got " +
               std::to_string(acc.transportCursor()) + ")");
    }

    // ========================================================================
    // Scenario 7: Reverse ramp — fade-in → Stop (no gain jump)
    // ========================================================================
    {
        TransportTestFixture fix(blockSize);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        // Start from Paused at 0
        proc.setPosition(0.0);
        fix.pushBlock(); // consume Seek
        fix.pushBlock(); // settle

        // Play → starts fade-in
        proc.play();

        // Push exactly one fade-in block
        auto firstPlayBlock = fix.pushBlock();

        // Record current gain and audio frontier after first fade-in block
        float gainBeforeStop = acc.currentOutputGain();
        int64_t frontierBeforeStop = acc.audioReadCursor();
        expect(gainBeforeStop > 0.0f && gainBeforeStop < 1.0f,
               "S7: fade-in gain after first Play block (got " +
               std::to_string(gainBeforeStop) + ")");
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Playing,
               "S7: phase == Playing during fade-in");
        expect(acc.transitionActive(),
               "S7: transitionActive true during fade-in");
        expect(acc.targetOutputGain() == 1.0f,
               "S7: targetGain == 1 during fade-in");

        // Immediately Stop — reverses ramp direction (fade-in → fade-out)
        proc.stop();

        // First sample of next block: divide by source[frontier] to get exact
        // pre-command gain — must match gainBeforeStop (no gain jump).
        auto stopBlock = fix.pushBlock();
        const float firstSampleAfterStop = stopBlock.getSample(0, 0);
        const float sourceAtFrontier = fix.sourceBuf->getSample(0, static_cast<int>(frontierBeforeStop));
        const float gainFromAudio = firstSampleAfterStop / sourceAtFrontier;
        expect(std::abs(gainFromAudio - gainBeforeStop) < 1.0e-5f,
               "S7: first sample gain after Stop equals pre-command gain (audioGain=" +
               std::to_string(gainFromAudio) + " expected " + std::to_string(gainBeforeStop) + ")");

        // targetGain must be flipped to 0 (fade-out)
        expect(std::abs(acc.targetOutputGain()) < 0.001f,
               "S7: targetGain flipped to 0 after Stop (got " +
               std::to_string(acc.targetOutputGain()) + ")");

        // rampSamplesRemaining must be 0.2s worth minus one block already consumed
        // by the stopBlock push above (the ramp continued from where fade-in left off)
        expect(acc.transitionActive(), "S7: transitionActive true during fade-out");

        // Push remaining ramp blocks until completion
        while (acc.rampSamplesRemaining() > 0) {
            fix.pushBlock();
        }

        // Final state: Stopped, both cursors 0
        expect(acc.phase() == OpenTuneAudioProcessor::RuntimePhase::Stopped,
               "S7: phase == Stopped after reverse ramp (got " +
               std::to_string(static_cast<int>(acc.phase())) + ")");
        expect(!acc.transitionActive(),
               "S7: transitionActive false after completion");
        expect(acc.audioReadCursor() == 0,
               "S7: audioReadCursor == 0 after reverse ramp completion (got " +
               std::to_string(acc.audioReadCursor()) + ")");
        expect(acc.transportCursor() == 0,
               "S7: transportCursor == 0 after reverse ramp completion");
    }
}

// ============================================================================
// Test 7: Block size variants (64, 960, 1024, 2560) — ramp remaining
//          decrements exactly by block size, finishes at ramp-end block.
// ============================================================================

void audioProcessorRampBlockSizeVariants()
{
    auto testBlockSize = [](int bs) {
        constexpr double rampDurationSec = 0.2;
        TransportTestFixture fix(bs);
        auto& proc = *fix.proc;
        auto& acc = *fix.acc;

        const int rampTotalSamples = static_cast<int>(TransportTestFixture::kDeviceRate * rampDurationSec);

        // Start from Paused at 0
        proc.setPosition(0.0);
        fix.pushBlock(bs);
        fix.pushBlock(bs);

        // Play — starts fade-in
        proc.play();
        fix.pushBlock(bs); // first fade-in block, consumed bs samples

        // After first Play block: remaining == rampTotal - bs
        {
            int remaining = acc.rampSamplesRemaining();
            int expected = rampTotalSamples - bs;
            expect(remaining == expected,
                   "S7 bs=" + std::to_string(bs) +
                   " after first Play block: rampRemaining == " + std::to_string(expected) +
                   " (got " + std::to_string(remaining) + ")");
        }

        int processed = bs;
        const int fullBlocks = rampTotalSamples / bs;

        for (int b = 1; b < fullBlocks; ++b) {
            fix.pushBlock(bs);
            processed += bs;
            int remaining = acc.rampSamplesRemaining();
            int expected = std::max(0, rampTotalSamples - processed);
            expect(remaining == expected,
                   "S7 bs=" + std::to_string(bs) +
                   " block " + std::to_string(b) +
                   ": rampRemaining == " + std::to_string(expected) +
                   " (got " + std::to_string(remaining) + ")");
        }

        // Any remainder block covering the ramp end point
        if (rampTotalSamples % bs != 0) {
            fix.pushBlock(bs);
            processed += bs;
        }

        // After ramp end: remaining == 0, transition false, gain == 1
        expect(acc.rampSamplesRemaining() == 0,
               "S7 bs=" + std::to_string(bs) +
               ": rampSamplesRemaining == 0 after completion (got " +
               std::to_string(acc.rampSamplesRemaining()) + ")");
        expect(!acc.transitionActive(),
               "S7 bs=" + std::to_string(bs) +
               ": transitionActive == false after ramp");
        expect(std::abs(acc.currentOutputGain() - 1.0f) < 0.001f,
               "S7 bs=" + std::to_string(bs) +
               ": currentOutputGain == 1.0 after ramp (got " +
               std::to_string(acc.currentOutputGain()) + ")");
    };

    testBlockSize(64);
    testBlockSize(960);
    testBlockSize(1024);
    testBlockSize(2560);
}

// ============================================================================
// Test 8: Sample rate switch — transport absolute time preserved,
//          cursor projected to new rate; pending API preserves second semantics.
// ============================================================================

void audioProcessorSampleRateSwitchPreservesAbsoluteTime()
{
    constexpr double rate1 = 48000.0;
    constexpr double rate2 = 44100.0;
    constexpr double positionSec = 1.5;

    TransportTestFixture fix(64);
    auto& proc = *fix.proc;
    auto& acc = *fix.acc;

    // Set a known position then Play to advance slightly
    proc.setPosition(positionSec);
    fix.pushBlock();
    proc.play();
    fix.pushBlock();
    for (int b = 1; b < fix.fadeBlocksForSize(64); ++b)
        fix.pushBlock();

    // Record the absolute position time
    int64_t cursorBefore = acc.audioReadCursor();
    double timeBefore = TimeCoordinate::samplesToSeconds(cursorBefore, rate1);
    expect(timeBefore > positionSec,
           "S8: time advanced past seek position before rate switch");

    // Switch to 44.1kHz
    proc.prepareToPlay(rate2, 64);

    // Cursor should be projected: same absolute time, new sample count
    int64_t cursorAfter = acc.audioReadCursor();
    double timeAfter = TimeCoordinate::samplesToSeconds(cursorAfter, rate2);
    int64_t expectedProjected = TimeCoordinate::sampleRateProject(cursorBefore, rate1, rate2);

    expect(cursorAfter == expectedProjected,
           "S8: cursor projected from rate1 to rate2 (got " +
           std::to_string(cursorAfter) + " expected " + std::to_string(expectedProjected) + ")");
    expect(std::abs(timeAfter - timeBefore) < 0.001,
           "S8: absolute time preserved after rate switch (before=" +
           std::to_string(timeBefore) + " after=" + std::to_string(timeAfter) + ")");

    // Play at new rate to advance, then switch back
    proc.play();
    fix.pushBlock();
    int64_t cursorAtRate2 = acc.audioReadCursor();

    proc.prepareToPlay(rate1, 64);
    int64_t cursorBackToRate1 = acc.audioReadCursor();
    int64_t expectedBack = TimeCoordinate::sampleRateProject(cursorAtRate2, rate2, rate1);
    expect(cursorBackToRate1 == expectedBack,
           "S8: cursor projected back to rate1 (got " +
           std::to_string(cursorBackToRate1) + " expected " + std::to_string(expectedBack) + ")");
}

// ============================================================================
// Test 9: Same-rate prepareToPlay with different blockSize preserves
//          RenderCache prepared chunk audio shared_ptr identity.
//          Uses processor's CRS (no direct preparePlaybackSampleRate).
// ============================================================================

void processorPreparedCacheIdentityAcrossSameRateBlockSizeChange()
{
    constexpr double rate = 48000.0;
    constexpr int oldBs = 64;
    constexpr int newBs = 1024;

    // Save and set wrapper to Standalone
    const juce::AudioProcessor::WrapperType savedWrapper =
        juce::PluginHostType::jucePlugInClientCurrentWrapperType;
    juce::PluginHostType::jucePlugInClientCurrentWrapperType =
        juce::AudioProcessor::wrapperType_Standalone;
    juce::AudioProcessor::setTypeOfNextNewPlugin(
        juce::AudioProcessor::wrapperType_Standalone);

    auto proc = std::make_unique<OpenTuneAudioProcessor>();

    // Reset next-plugin hint
    juce::AudioProcessor::setTypeOfNextNewPlugin(
        juce::AudioProcessor::wrapperType_Undefined);
    juce::PluginHostType::jucePlugInClientCurrentWrapperType = savedWrapper;

    // Prepare at first block size
    proc->prepareToPlay(rate, oldBs);

    // Create a canonical source buffer
    constexpr int canonicalSamples = 44100;
    auto dryBuf = std::make_shared<juce::AudioBuffer<float>>(1, canonicalSamples);
    {
        auto* data = dryBuf->getWritePointer(0);
        for (int i = 0; i < canonicalSamples; ++i)
            data[i] = static_cast<float>(i) * 0.0001f;
    }

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 9001;

    // Get or create a RenderCache through CRS, publish a canonical chunk
    auto renderCache = proc->getContentRenderService()->getOrCreateRenderCache(key);

    constexpr int64_t chunkStart = 0;
    constexpr int64_t chunkEndExclusive = canonicalSamples;
    constexpr int chunkLen = static_cast<int>(chunkEndExclusive - chunkStart);

    renderCache->requestRenderPending(chunkStart, chunkEndExclusive);

    RenderCache::PendingJob job;
    expect(renderCache->getNextPendingJob(job), "S9: getNextPendingJob returns job");

    std::vector<float> renderAudio(chunkLen);
    for (int i = 0; i < chunkLen; ++i)
        renderAudio[i] = dryBuf->getSample(0, i);

    const auto result = renderCache->completeChunkRenderWithAudio(
        chunkStart, chunkEndExclusive, std::move(renderAudio), job.targetRevision);
    expect(result == RenderCache::ChunkRenderResult::Published, "S9: canonical chunk published");

    // Publish the source through CRS
    {
        PlaybackReadSource src;
        src.contentKey = key;
        src.audioBuffer = dryBuf;
        src.audioSampleRate = 44100.0;
        src.renderCache = renderCache;
        src.timeStretchCache = &proc->getContentRenderService()->getTimeStretchCache();
        src.pitchRevision = 1;
        src.timeGridRevision = 1;
        src.pitchShiftRevision = 1;
        src.timeGridIsIdentity = true;
        proc->getContentRenderService()->publishPlaybackSource(key, std::move(src));
    }

    // RenderCache was created at the registry's current 48k rate (prepareToPlay above),
    // and canonical chunk publish triggers prepared rebuild automatically.
    // The second prepareToPlay below (same rate, different block size) determines
    // identity preservation.

    // Capture prepared audio identity via RenderCacheTestAccessor
    expect(RenderCacheTestAccessor::hasPreparedChunks(*renderCache),
           "S9: preparedSnapshot has chunks after canonical publish");
    expect(std::abs(RenderCacheTestAccessor::getPreparedSampleRate(*renderCache) - rate) < 1.0,
           "S9: prepared sampleRate == 48k");

    const auto audioPtr1 = RenderCacheTestAccessor::getFirstPreparedChunkAudio(*renderCache);
    expect(audioPtr1 != nullptr, "S9: first prepared chunk audio non-null");

    // Same rate, different block size: prepareToPlay again
    proc->prepareToPlay(rate, newBs);

    expect(RenderCacheTestAccessor::hasPreparedChunks(*renderCache),
           "S9: preparedSnapshot has chunks after block change");
    expect(std::abs(RenderCacheTestAccessor::getPreparedSampleRate(*renderCache) - rate) < 1.0,
           "S9: prepared sampleRate still 48k");

    const auto audioPtr2 = RenderCacheTestAccessor::getFirstPreparedChunkAudio(*renderCache);

    // Same rate → same prepared audio shared_ptr (no resample triggered)
    expect(audioPtr1 == audioPtr2,
           "S9: prepared chunk audio shared_ptr identity preserved across same-rate block-size change");

    proc->releaseResources();
}

// ============================================================================
// Test 10: Bus layout — Standalone total input=0/output=2, VST3 input=2/output=2.
// ============================================================================

void audioProcessorBusLayoutStandaloneVsVst3()
{
    // --- Standalone ---
    {
        const juce::AudioProcessor::WrapperType savedClientType =
            juce::PluginHostType::jucePlugInClientCurrentWrapperType;

        juce::PluginHostType::jucePlugInClientCurrentWrapperType =
            juce::AudioProcessor::wrapperType_Standalone;
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_Standalone);

        auto standaloneProc = std::make_unique<OpenTuneAudioProcessor>();

        // Reset next-plugin hint after construction
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_Undefined);

        expect(standaloneProc->getTotalNumInputChannels() == 0,
               "S10: Standalone total input channels == 0 (got " +
               std::to_string(standaloneProc->getTotalNumInputChannels()) + ")");
        expect(standaloneProc->getTotalNumOutputChannels() == 2,
               "S10: Standalone total output channels == 2 (got " +
               std::to_string(standaloneProc->getTotalNumOutputChannels()) + ")");

        // Destruct processor before restoring wrapper
        standaloneProc->releaseResources();
        standaloneProc.reset();

        juce::PluginHostType::jucePlugInClientCurrentWrapperType = savedClientType;
    }

    // --- VST3 ---
    {
        const juce::AudioProcessor::WrapperType savedClientType =
            juce::PluginHostType::jucePlugInClientCurrentWrapperType;

        juce::PluginHostType::jucePlugInClientCurrentWrapperType =
            juce::AudioProcessor::wrapperType_VST3;
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_VST3);

        auto vst3Proc = std::make_unique<OpenTuneAudioProcessor>();

        // Reset next-plugin hint after construction
        juce::AudioProcessor::setTypeOfNextNewPlugin(
            juce::AudioProcessor::wrapperType_Undefined);

        expect(vst3Proc->getTotalNumInputChannels() == 2,
               "S10: VST3 total input channels == 2 (got " +
               std::to_string(vst3Proc->getTotalNumInputChannels()) + ")");
        expect(vst3Proc->getTotalNumOutputChannels() == 2,
               "S10: VST3 total output channels == 2 (got " +
               std::to_string(vst3Proc->getTotalNumOutputChannels()) + ")");

        // Destruct processor before restoring wrapper
        vst3Proc->releaseResources();
        vst3Proc.reset();

        juce::PluginHostType::jucePlugInClientCurrentWrapperType = savedClientType;
    }
}

// ============================================================================
// Test A: PlaybackSourcePublisher 生命周期令牌测试
// ============================================================================

void playbackSourcePublisherLifecycleTokenTest()
{
    struct DestructRecord {
        std::thread::id threadId;
        std::atomic<bool> called{false};
    };
    auto record = std::make_shared<DestructRecord>();
    const auto mainThreadId = std::this_thread::get_id();

    constexpr int canonicalSamples = 44100;
    auto canonBuf = std::shared_ptr<juce::AudioBuffer<float>>(
        new juce::AudioBuffer<float>(1, canonicalSamples),
        [record](juce::AudioBuffer<float>* p) {
            record->threadId = std::this_thread::get_id();
            record->called.store(true, std::memory_order_release);
            delete p;
        });
    {
        auto* data = canonBuf->getWritePointer(0);
        for (int i = 0; i < canonicalSamples; ++i)
            data[i] = static_cast<float>(i) * 0.0001f;
    }

    PlaybackSourcePublisher publisher;

    ContentKey keyA;
    keyA.domainKind = DomainKind::StandaloneClip;
    keyA.objectId = 1001;

    {
        PlaybackReadSource src;
        src.audioBuffer = canonBuf;
        src.audioSampleRate = 44100.0;
        src.pitchRevision = 1;
        src.timeGridRevision = 1;
        src.pitchShiftRevision = 1;
        src.timeGridIsIdentity = true;
        publisher.publish(keyA, src);
    }

    PlaybackReadSource reader;
    expect(publisher.get(keyA, reader), "reader get after publish");
    expect(reader.audioBuffer == canonBuf,
           "reader sees canonical buffer (shared_ptr identity)");

    publisher.setPlaybackSampleRate(48000.0);

    {
        auto otherBuf = std::make_shared<juce::AudioBuffer<float>>(1, 100);
        {
            auto* d = otherBuf->getWritePointer(0);
            for (int i = 0; i < 100; ++i) d[i] = 0.5f;
        }
        PlaybackReadSource otherSrc;
        otherSrc.audioBuffer = otherBuf;
        otherSrc.audioSampleRate = 44100.0;
        otherSrc.pitchRevision = 1;
        otherSrc.timeGridRevision = 1;
        otherSrc.pitchShiftRevision = 1;
        otherSrc.timeGridIsIdentity = true;
        publisher.publish(keyA, otherSrc);
    }

    std::thread audioThread([r = std::move(reader)]() mutable {
    });
    audioThread.join();

    canonBuf.reset();
    expect(!record->called.load(std::memory_order_acquire),
           "canonical buffer NOT destructed after audio thread release and local reset");

    {
        ContentKey dummyKey;
        dummyKey.domainKind = DomainKind::StandaloneClip;
        dummyKey.objectId = 9999;
        auto dummyBuf = std::make_shared<juce::AudioBuffer<float>>(1, 10);
        dummyBuf->clear();
        PlaybackReadSource dummySrc;
        dummySrc.audioBuffer = dummyBuf;
        dummySrc.audioSampleRate = 44100.0;
        dummySrc.pitchRevision = 1;
        dummySrc.timeGridRevision = 1;
        dummySrc.pitchShiftRevision = 1;
        dummySrc.timeGridIsIdentity = true;
        publisher.publish(dummyKey, dummySrc);
    }

    expect(record->called.load(std::memory_order_acquire),
           "canonical buffer deleter called after prune mutation");
    expect(record->threadId == mainThreadId,
           "deleter executed on main/writer thread, not on audio thread");

    publisher.clear();
    record.reset();
}

// ============================================================================
// Test B: prepared active-cache 内存计数测试
// ============================================================================

void preparedActiveCacheMemoryCountingTest()
{
    // B1: PlaybackSourcePublisher
    {
        const size_t baseline = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);

        PlaybackSourcePublisher publisher;

        constexpr int canonicalSamples = 44100;
        constexpr int channels = 1;
        auto canonBuf = std::make_shared<juce::AudioBuffer<float>>(channels, canonicalSamples);
        {
            auto* data = canonBuf->getWritePointer(0);
            for (int i = 0; i < canonicalSamples; ++i)
                data[i] = static_cast<float>(i) * 0.0001f;
        }

        ContentKey key;
        key.domainKind = DomainKind::StandaloneClip;
        key.objectId = 2001;

        {
            PlaybackReadSource src;
            src.audioBuffer = canonBuf;
            src.audioSampleRate = 44100.0;
            src.pitchRevision = 1;
            src.timeGridRevision = 1;
            src.pitchShiftRevision = 1;
            src.timeGridIsIdentity = true;
            publisher.publish(key, src);
        }
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline,
               "PSP 44.1k alias adds 0 bytes to global current");

        publisher.setPlaybackSampleRate(48000.0);

        const int64_t preparedSamples = TimeCoordinate::sampleRateProject(
            canonicalSamples, 44100.0, 48000.0);
        const size_t expectedPreparedBytes =
            static_cast<size_t>(channels) * static_cast<size_t>(preparedSamples) * sizeof(float);
        const size_t after48k = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);
        expect(after48k == baseline + expectedPreparedBytes,
               "PSP 48k prepared adds channels*preparedSamples*sizeof(float)");

        publisher.clear();
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline,
               "PSP clear returns to baseline");
    }

    // B2: RenderCache
    {
        const size_t baseline = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);

        auto renderCache = std::make_shared<RenderCache>();

        constexpr int64_t chunkStart = 1000;
        constexpr int64_t chunkEndExclusive = 3000;
        constexpr int chunkSamples = static_cast<int>(chunkEndExclusive - chunkStart);

        renderCache->requestRenderPending(chunkStart, chunkEndExclusive);

        RenderCache::PendingJob job;
        expect(renderCache->getNextPendingJob(job), "RC getNextPendingJob");
        std::vector<float> renderAudio(chunkSamples, 0.5f);
        const auto result = renderCache->completeChunkRenderWithAudio(
            chunkStart, chunkEndExclusive, std::move(renderAudio), job.targetRevision);
        expect(result == RenderCache::ChunkRenderResult::Published,
               "RC canonical chunk published");

        const size_t canonicalBytes = static_cast<size_t>(chunkSamples) * sizeof(float);
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline + canonicalBytes,
               "RC canonical chunk bytes added");

        renderCache->prepareForPlaybackSampleRate(44100.0);
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline + canonicalBytes,
               "RC 44.1k alias does not double-count prepared");

        renderCache->prepareForPlaybackSampleRate(48000.0);

        const int64_t prepStart = TimeCoordinate::sampleRateProject(
            chunkStart, 44100.0, 48000.0);
        const int64_t prepEnd = TimeCoordinate::sampleRateProject(
            chunkEndExclusive, 44100.0, 48000.0);
        const int outputLength = static_cast<int>(prepEnd - prepStart);
        const size_t expectedPreparedBytes = static_cast<size_t>(outputLength) * sizeof(float);

        const size_t after48kRC = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);
        expect(after48kRC == baseline + canonicalBytes + expectedPreparedBytes,
               "RC 48k prepared adds projected prepared length*sizeof(float)");

        renderCache->clear();
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline,
               "RC clear returns to baseline");
    }

    // B3: TimeStretchCache
    {
        const size_t baseline = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);

        TimeStretchCache cache;

        ContentKey key;
        key.domainKind = DomainKind::StandaloneClip;
        key.objectId = 5001;

        constexpr int audioLen = 44100;
        std::vector<float> canonical(audioLen);
        for (int i = 0; i < audioLen; ++i)
            canonical[i] = static_cast<float>(i) * 0.00001f;

        uint32_t gen = cache.beginBuild(key);
        cache.store(key, canonical, 1, 1, 1, 44100.0, gen);

        const size_t canonicalBytes = static_cast<size_t>(audioLen) * sizeof(float);
        const size_t afterStore = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);
        expect(afterStore == baseline + canonicalBytes,
               "TSC store 44.1k alias counted once");

        cache.prepareForPlaybackSampleRate(48000.0);

        const int64_t preparedLen = TimeCoordinate::sampleRateProject(
            audioLen, 44100.0, 48000.0);
        const size_t preparedBytes = static_cast<size_t>(preparedLen) * sizeof(float);

        const size_t after48k = RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed);
        expect(after48k == baseline + canonicalBytes + preparedBytes,
               "TSC 48k adds non-alias prepared bytes");

        cache.clear();
        expect(RenderCache::globalCacheCurrentBytes().load(std::memory_order_relaxed) == baseline,
               "TSC clear returns to baseline");
    }
}

// ============================================================================
// Test C: 零残留合同 — 源码中禁止旧字段/旧路径 token，并新增
//         新架构中禁止出现的旧 token。
// ============================================================================

void zeroResidueSourceContract()
{
    const auto processorHeader = readText("Source/PluginProcessor.h");
    const auto processorSource = readText("Source/PluginProcessor.cpp");
    const auto publisherHeader = readText("Source/Render/PlaybackSourcePublisher.h");
    const auto publisherSource = readText("Source/Render/PlaybackSourcePublisher.cpp");
    const auto playbackReader = readText("Source/Utils/PlaybackAudioReader.h");

    // 禁止 isFadingOut_ / fadeOutSampleCount_ 旧 fade 字段
    expect(!contains(processorHeader, "isFadingOut_"),
           "PluginProcessor.h must not contain isFadingOut_");
    expect(!contains(processorSource, "isFadingOut_"),
           "PluginProcessor.cpp must not contain isFadingOut_");
    expect(!contains(processorHeader, "fadeOutSampleCount_"),
           "PluginProcessor.h must not contain fadeOutSampleCount_");
    expect(!contains(processorSource, "fadeOutSampleCount_"),
           "PluginProcessor.cpp must not contain fadeOutSampleCount_");

    // 禁止 overlayPublishedAudioForRate（旧 RenderCache 路径）
    const auto renderCacheHeader = readText("Source/Inference/RenderCache.h");
    const auto renderCacheSource = readText("Source/Inference/RenderCache.cpp");
    expect(!contains(renderCacheHeader, "overlayPublishedAudioForRate"),
           "RenderCache.h must not contain overlayPublishedAudioForRate");
    expect(!contains(renderCacheSource, "overlayPublishedAudioForRate"),
           "RenderCache.cpp must not contain overlayPublishedAudioForRate");

    // 禁止旧 realtime interpolation token
    const auto allProduction = processorHeader + processorSource
        + publisherHeader + publisherSource + playbackReader
        + renderCacheHeader + renderCacheSource;

    expect(!contains(allProduction, "realTimeInterpolation"),
           "production code must not contain realTimeInterpolation");
    expect(!contains(allProduction, "linearInterpolate"),
           "production code must not contain linearInterpolate");
    expect(!contains(allProduction, "interpolatePrepared"),
           "production code must not contain interpolatePrepared");

    // ==== New: forbid old transport tokens in the new architecture ====
    // These tokens must not appear in PluginProcessor.h/.cpp (the new
    // transport uses audioReadCursor_, transportCursor_, etc. instead).

    const auto procHeaderAndSource = processorHeader + processorSource;

    expect(!contains(procHeaderAndSource, "sampleCursor"),
           "PluginProcessor.h/.cpp must not contain sampleCursor (replaced by audioReadCursor_/transportCursor_)");
    expect(!contains(procHeaderAndSource, "pendingMainCursor_"),
           "PluginProcessor.h/.cpp must not contain pendingMainCursor_");
    expect(!contains(procHeaderAndSource, "pendingTargetCursor_"),
           "PluginProcessor.h/.cpp must not contain pendingTargetCursor_");
    expect(!contains(procHeaderAndSource, "pendingIsPlaying_"),
           "PluginProcessor.h/.cpp must not contain pendingIsPlaying_");
    expect(!contains(procHeaderAndSource, "fadeActive_"),
           "PluginProcessor.h/.cpp must not contain fadeActive_");
    expect(!contains(procHeaderAndSource, "fadeElapsed_"),
           "PluginProcessor.h/.cpp must not contain fadeElapsed_");
    expect(!contains(procHeaderAndSource, "fadeReadCursor_"),
           "PluginProcessor.h/.cpp must not contain fadeReadCursor_");
    expect(!contains(procHeaderAndSource, "fadeCompletionCursor_"),
           "PluginProcessor.h/.cpp must not contain fadeCompletionCursor_");
    expect(!contains(procHeaderAndSource, "fadeCompletionPhase_"),
           "PluginProcessor.h/.cpp must not contain fadeCompletionPhase_");

    // RuntimePhase::Fading must not appear (replaced by transitionActive_)
    expect(!contains(processorHeader, "RuntimePhase::Fading"),
           "PluginProcessor.h must not contain RuntimePhase::Fading");
    expect(!contains(processorSource, "RuntimePhase::Fading"),
           "PluginProcessor.cpp must not contain RuntimePhase::Fading");
}

} // anonymous namespace

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "[Test] PlaybackDeviceTests\n\n";

    playbackSourcePublisherDefault441PublishAndRateSwitch();
    readPlaybackAudioAdjacentChunkDirectCopyContinuity();
    readCanonicalAudioIndependentOfActivePreparedRate();
    renderCacheCanonicalChunkAndPreparedOverlayReplacesDry();
    timeStretchCacheDefault441PreparedEqualsCanonicalAndRateSwitch();
    audioProcessorRuntimeTransportCursorAdvanceAndFade();
    audioProcessorRampBlockSizeVariants();
    audioProcessorSampleRateSwitchPreservesAbsoluteTime();
    processorPreparedCacheIdentityAcrossSameRateBlockSizeChange();
    audioProcessorBusLayoutStandaloneVsVst3();
    zeroResidueSourceContract();
    playbackSourcePublisherLifecycleTokenTest();
    preparedActiveCacheMemoryCountingTest();

    std::cout << "\n[Result] " << failures << " failure(s)\n";
    return (failures == 0) ? 0 : 1;
}
