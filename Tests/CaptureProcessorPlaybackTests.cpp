// 实际 OpenTuneAudioProcessor <-> CaptureSession 发布绑定回归。
//
// 覆盖：
//  1) 44100 采集父内容被新 take 裁剪后只剩 4 帧前缀时，CRS 实际发布的
//     canonical 长度是 4，且 native 仍指向 200 帧父 PCM（共享身份）。
//  2) 96000、1 帧、正时长的真实采集能正常 finalize 并发布 native/canonical，
//     canonical 至少 1 帧。
//  3) 已 Edited 的 Capture 在无 usable F0 时，经
//     onContentLocalMutationCompletedSeconds -> requestRenderForLocalMutationRange
//     的 republish 后，CaptureSession 冻结的替换来源随 CRS 新 snapshot 更新，
//     实际播放输出使用新 VolumeEnvelope。
//
// 处理器生命周期遵守消息线程约定：juce::ScopedJuceInitialiser_GUI 之后在主线程
// 构造 / prepare / 编辑 / 析构；跨处理器保持 ProcessRenderRuntime owner，避免
// runtime 反复 shutdown（与现有 Control 处理器测试同一约定）。

#include "../Source/PluginProcessor.h"
#include "../Source/Plugin/Capture/CaptureSession.h"
#include "../Source/Render/ContentRenderService.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "../Source/Utils/AutomationLane.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace OpenTune {

// The application wrapper owns the editor factory; this processor test is headless.
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    return nullptr;
}

} // namespace OpenTune

namespace {

using OpenTune::AutomationLane;
using OpenTune::Capture::CaptureSegment;
using OpenTune::Capture::CaptureSession;
using OpenTune::ContentKey;
using OpenTune::DomainKind;
using OpenTune::OpenTuneAudioProcessor;
using OpenTune::PlaybackReadSource;

constexpr double kCanonicalRate = OpenTune::TimeCoordinate::kRenderSampleRate;

bool fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

// 处理器构造前持有 runtime owner：本测试顺序创建多个 processor，runtime 不在
// 实例之间 shutdown（见 Tests/Control/ControlProcessorSupport.h 的同约定）。
void holdRenderRuntimeForProcess()
{
    static const bool held = [] {
        OpenTune::ProcessRenderRuntime::getInstance().retainOwner();
        return true;
    }();
    (void) held;
}

std::shared_ptr<juce::AudioBuffer<float>> makeMonoRamp(int frames, float slope, float offset)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, frames);
    for (int i = 0; i < frames; ++i)
        buffer->setSample(0, i, offset + slope * static_cast<float>(i));
    return buffer;
}

void fillStereo(juce::AudioBuffer<float>& buffer, float left, float right)
{
    for (int s = 0; s < buffer.getNumSamples(); ++s) {
        buffer.setSample(0, s, left);
        buffer.setSample(1, s, right);
    }
}

ContentKey captureKey(uint64_t id)
{
    return ContentKey{DomainKind::RegularVST3Capture, id, 0};
}

bool getPublishedSource(OpenTuneAudioProcessor& processor,
                        const ContentKey& key,
                        PlaybackReadSource& out)
{
    auto* crs = processor.getContentRenderService();
    return crs != nullptr && crs->getPlaybackReadSource(key, out);
}

// 走真实 processor publish binding：serialize/deserialize 会经
// bindings_.publishPlaybackSource 重建 CRS 源与发布视图。
bool publishInjectedSegment(CaptureSession& session)
{
    const auto archive = session.serialize();
    return session.deserialize(archive);
}

// 1) 新 take 覆盖父内容的 [4, 200)，父内容被裁剪为 4 帧前缀。
//    真实 CRS canonical 长度必须是 4；native 仍与父 200 帧 PCM 共享身份。
bool testTrimmedCaptureKeepsCanonicalLengthAndParentNative()
{
    constexpr int kParentFrames = 200;
    constexpr int kPrefixFrames = 4;

    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    processor->prepareToPlay(44100.0, 512);
    auto* session = processor->getCaptureSession();
    if (session == nullptr)
        return fail("capture session missing after prepareToPlay");

    const auto parentPcm = makeMonoRamp(kParentFrames, 0.001f, -0.1f);
    const auto parentId = session->testInjectEditedSegment(
        0.0, static_cast<double>(kParentFrames) / 44100.0, 9001, parentPcm, 44100.0);
    if (parentId == 0)
        return fail("parent segment injection failed");

    const auto parentKey = captureKey(parentId);
    if (!publishInjectedSegment(*session))
        return fail("parent serialize/deserialize publish failed");

    PlaybackReadSource parentSource;
    if (!getPublishedSource(*processor, parentKey, parentSource))
        return fail("parent CRS source missing");
    if (!parentSource.nativeSource.buffer
        || parentSource.nativeSource.buffer->getNumSamples() != kParentFrames)
        return fail("parent native PCM is not 200 frames");
    if (!parentSource.canonicalBuffer
        || parentSource.canonicalBuffer->getNumSamples() != kParentFrames)
        return fail("parent canonical PCM is not 200 frames");

    // 父源共享身份：裁剪后的 native 必须仍指向这同一块 200 帧父 PCM。
    const auto parentNative = parentSource.nativeSource.buffer;

    if (!session->armNewCapture())
        return fail("armNewCapture failed");
    juce::AudioBuffer<float> take(2, kParentFrames - kPrefixFrames);
    fillStereo(take, 0.25f, -0.5f);
    session->processBlock(take, kPrefixFrames, 44100.0, true);
    session->stopCapture();
    session->tick();

    CaptureSegment* prefix = nullptr;
    for (const auto& segment : session->testSegments()) {
        if (segment->hostStartSample.load(std::memory_order_acquire) == 0
            && segment->hostSampleCount.load(std::memory_order_acquire) == kPrefixFrames) {
            prefix = segment.get();
            break;
        }
    }
    if (prefix == nullptr)
        return fail("trimmed 4-frame prefix segment missing");

    PlaybackReadSource prefixSource;
    if (!getPublishedSource(*processor, prefix->contentKey, prefixSource))
        return fail("trimmed prefix CRS source missing");
    if (!prefixSource.canonicalBuffer
        || prefixSource.canonicalBuffer->getNumSamples() != kPrefixFrames)
        return fail("trimmed canonical is not exactly 4 frames");
    if (prefixSource.nativeSource.buffer != parentNative)
        return fail("trimmed native does not share the 200-frame parent source");
    if (!prefixSource.nativeSource.buffer
        || prefixSource.nativeSource.buffer->getNumSamples() != kParentFrames)
        return fail("trimmed native source is not 200 frames");
    if (prefixSource.firstCanonicalSample != parentSource.firstCanonicalSample)
        return fail("trimmed canonical origin moved");
    for (int i = 0; i < kPrefixFrames; ++i) {
        if (prefixSource.canonicalBuffer->getSample(0, i)
            != parentSource.canonicalBuffer->getSample(0, i))
            return fail("trimmed canonical samples do not match parent prefix");
    }

    return true;
}

// 2) 96000、1 帧、正时长的真实采集：finalize 发布 native 1 帧与 canonical >= 1 帧。
bool testOneFrame96kCapturePublishesNativeAndCanonical()
{
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    processor->prepareToPlay(96000.0, 512);
    auto* session = processor->getCaptureSession();
    if (session == nullptr)
        return fail("capture session missing after 96k prepareToPlay");

    if (!session->armNewCapture())
        return fail("armNewCapture failed at 96k");
    juce::AudioBuffer<float> take(2, 1);
    take.setSample(0, 0, 0.5f);
    take.setSample(1, 0, -0.25f);
    session->processBlock(take, 0, 96000.0, true);
    session->stopCapture();
    session->tick();

    const auto& segments = session->testSegments();
    if (segments.size() != 1)
        return fail("one-frame capture did not finalize to exactly one segment");
    const auto& segment = *segments.front();
    if (segment.captureSampleRate != 96000.0)
        return fail("finalized segment lost the 96k capture rate");
    if (segment.hostSampleCount.load(std::memory_order_acquire) != 1)
        return fail("finalized segment is not one host sample");

    PlaybackReadSource source;
    if (!getPublishedSource(*processor, segment.contentKey, source))
        return fail("one-frame 96k CRS source missing");
    if (!source.contentSnapshot)
        return fail("one-frame 96k published source has no snapshot");
    if (source.nativeSource.sampleRate != 96000.0)
        return fail("one-frame native sample rate changed");
    if (!source.nativeSource.buffer
        || source.nativeSource.buffer->getNumSamples() != 1)
        return fail("one-frame native PCM is not exactly one frame");
    if (source.canonicalSampleRate != kCanonicalRate)
        return fail("one-frame canonical sample rate is not 44.1k");
    if (!source.canonicalBuffer || source.canonicalBuffer->getNumSamples() < 1)
        return fail("one-frame canonical PCM is empty");
    if (!std::isfinite(source.canonicalBuffer->getSample(0, 0)))
        return fail("one-frame canonical PCM is not finite");
    if (!(source.contentSnapshot->sourceWindow.sourceEndSeconds
          > source.contentSnapshot->sourceWindow.sourceStartSeconds))
        return fail("one-frame source window is not positive");

    return true;
}

// 3) 已 Edited、无 usable F0 的 Capture：局部修改经
//    requestRenderForLocalMutationRange republish 后，冻结替换源必须更新。
bool testLocalMutationRefreshesFrozenCaptureSource()
{
    constexpr int kFrames = 100;
    constexpr int kBlockSamples = 16;

    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    processor->prepareToPlay(44100.0, 512);
    auto* session = processor->getCaptureSession();
    if (session == nullptr)
        return fail("capture session missing for local mutation scenario");

    const auto pcm = makeMonoRamp(kFrames, 0.004f, -0.2f);
    const auto id = session->testInjectEditedSegment(
        0.0, static_cast<double>(kFrames) / 44100.0, 9101, pcm, 44100.0);
    if (id == 0)
        return fail("edited segment injection failed");
    const auto key = captureKey(id);

    if (!publishInjectedSegment(*session))
        return fail("edited segment serialize/deserialize publish failed");

    // 修改前：冻结源来自 identity envelope，输出等于原始 PCM。
    juce::AudioBuffer<float> before(2, kBlockSamples);
    before.clear();
    session->processBlock(before, 0, 44100.0, true);
    for (int i = 0; i < kBlockSamples; ++i) {
        const float expected = pcm->getSample(0, i);
        if (before.getSample(0, i) != expected || before.getSample(1, i) != expected)
            return fail("pre-edit frozen source does not play the original PCM");
    }

    // 既有 CaptureSession 公开编辑入口：只改 VolumeEnvelope，无 F0。
    const AutomationLane envelope = AutomationLane::fromSnapshot(
        {{0.0, -60.0f}, {1.0, -60.0f}});
    if (!session->applyVolumeEnvelope(key, envelope))
        return fail("CaptureSession applyVolumeEnvelope failed");

    auto modifiedSnapshot = processor->getContentSnapshot(key);
    if (!modifiedSnapshot)
        return fail("modified content snapshot missing");
    if (modifiedSnapshot->hasUsableOriginalF0())
        return fail("scenario requires a content without usable F0");
    if (modifiedSnapshot->volumeEnvelope.empty())
        return fail("modified snapshot lost the volume envelope");

    processor->onContentLocalMutationCompletedSeconds(
        key, 0.0, static_cast<double>(kFrames) / 44100.0, modifiedSnapshot);

    // CRS 已 republish 新 snapshot。
    PlaybackReadSource updated;
    if (!getPublishedSource(*processor, key, updated))
        return fail("CRS source missing after local mutation republish");
    if (!updated.contentSnapshot || updated.contentSnapshot->volumeEnvelope.empty())
        return fail("CRS source did not receive the modified snapshot");

    // 冻结替换源也必须更新：实际播放输出应用 -60 dB。
    juce::AudioBuffer<float> after(2, kBlockSamples);
    after.clear();
    session->processBlock(after, 0, 44100.0, true);

    const double expectedGain = std::exp(-60.0 * 0.11512925465);
    for (int i = 0; i < kBlockSamples; ++i) {
        const float expected = static_cast<float>(pcm->getSample(0, i) * expectedGain);
        if (std::abs(after.getSample(0, i) - expected) > 1.0e-6f)
            return fail("frozen replacement source was not refreshed after republish");
        if (after.getSample(0, i) == before.getSample(0, i))
            return fail("post-edit playback output is unchanged");
    }

    return true;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    holdRenderRuntimeForProcess();

    if (!testTrimmedCaptureKeepsCanonicalLengthAndParentNative())
        return 1;
    if (!testOneFrame96kCapturePublishesNativeAndCanonical())
        return 1;
    if (!testLocalMutationRefreshesFrozenCaptureSource())
        return 1;

    std::puts("CaptureProcessorPlayback tests passed.");
    return 0;
}
