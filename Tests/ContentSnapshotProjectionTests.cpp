// 测试 target 未注入 ARA SDK include 路径，而 juce_audio_processors 在
// JucePlugin_Enable_ARA=1 时会拉入 <ARA_Library/...>。本文件只使用
// OpenTunePlaybackRenderer.h 中 ARA 守卫之外的实时门控函数，
// 故在本 TU 内关闭该宏；生产 target 的配置不受影响。
#undef JucePlugin_Enable_ARA

#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Content/StandaloneClipContent.h"
#include "../Source/Services/HnsepAudio.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Utils/PlaybackSampleGeometry.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Render/StretcherPool.h"
#include "../Source/Inference/SoundTouchStretcher.h"
#include "../Source/Utils/TimeCoordinate.h"
#include "../Source/Utils/F0Timeline.h"
#include "../Source/Utils/SourceWindow.h"
#include "../Source/Utils/PlacementFade.h"
#include "../Source/ARA/OpenTunePlaybackRenderer.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= 1.0e-9;
}

std::shared_ptr<OpenTune::PitchCurve> makeUsableOriginalF0Curve()
{
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setHopSize(256);
    curve->setSampleRate(16000.0);
    curve->setOriginalF0({440.0f, 441.0f, 442.0f});
    return curve;
}

bool testErasedRangesSurviveContentSnapshotProjection()
{
    auto curve = makeUsableOriginalF0Curve();
    curve->replaceErasedRangesInRange(0, 3, {{1, 2}});
    OpenTune::StandaloneClipContent clip(12);
    clip.applyOriginalF0(curve);
    const auto snapshot = clip.snapshotContent();
    return snapshot->pitchCurve != nullptr
        && snapshot->pitchCurve->getErasedRanges().size() == 1
        && snapshot->pitchCurve->getErasedRanges().front().startFrame == 1
        && snapshot->pitchCurve->getErasedRanges().front().endFrameExclusive == 2;
}

bool testErasedRangesSurviveF0CurveRebuild()
{
    auto previous = makeUsableOriginalF0Curve();
    previous->replaceErasedRangesInRange(0, 3, {{1, 2}});

    auto rebuilt = std::make_shared<OpenTune::PitchCurve>();
    rebuilt->setOriginalF0({450.0f, 451.0f, 452.0f, 453.0f});
    rebuilt->replaceErasedRangesInRange(
        0, static_cast<int>(rebuilt->size()),
        previous->getSnapshot()->getErasedRanges());

    const auto ranges = rebuilt->getSnapshot()->getErasedRanges();
    return ranges.size() == 1
        && ranges.front().startFrame == 1
        && ranges.front().endFrameExclusive == 2;
}

bool testCommonFieldsRoundTrip()
{
    // 新 owner/content 的运行时 revision 从 1 开始。
    if (OpenTune::ContentState{}.contentRevision != 1
        || OpenTune::EditableContentSnapshot{}.contentRevision != 1
        || !OpenTune::EditableContentSnapshot{}.timeGrid
        || !OpenTune::EditableContentSnapshot{}.timeGrid->isIdentity())
        return false;

    OpenTune::ContentState source;
    source.sourceWindow = OpenTune::SourceWindow{17, {}, 0.25, 1.5};
    source.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 4);
    source.sampleRate = 48000.0;
    auto nativePcm = std::make_shared<const juce::AudioBuffer<float>>(2, 6);
    source.nativeSource = OpenTune::NativePcmSource{nativePcm, 48000.0, 12345, 8};
    source.firstCanonicalSample = 456;
    source.analysis.hnsepHarmonicAudio =
        std::make_shared<const std::vector<float>>(std::vector<float>{0.1f, 0.2f});
    source.analysis.hnsepResidualAudio =
        std::make_shared<const std::vector<float>>(std::vector<float>{0.3f, 0.4f});
    source.analysis.originalF0State = OpenTune::OriginalF0State::Ready;
    source.analysis.detectedKey.root = OpenTune::Key::A;
    source.analysis.detectedKey.scale = OpenTune::Scale::Minor;
    source.analysis.detectedKey.confidence = 0.75f;
    source.analysis.detectedKey.origin = OpenTune::Origin::Automatic;
    source.analysis.silentGaps.push_back({100, 200, -48.0f});
    source.notes.push_back({});
    source.pitchShiftSettings = OpenTune::PitchShiftSettings{3, 25};
    source.volumeEnvelope = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{{0.0, -1.0f}, {1.0, -3.0f}});
    source.timeGrid = OpenTune::TimeGridSnapshot::makeIdentity(
        source.sourceWindow.durationSeconds());
    source.notesRevision = 2;
    source.noteTopologyInitialized = true;
    source.pitchRevision = 3;
    source.timeGridRevision = 4;
    source.contentRevision = 7;
    source.audioRevision = 8;

    // 默认/空 ContentState 必须携带非空 identity grid（identity 不用 nullptr 表达）。
    if (!source.timeGrid || !source.timeGrid->isIdentity())
        return false;

    const auto snapshot = OpenTune::makeContentSnapshot(source);
    const auto restored = OpenTune::contentStateFromSnapshot(snapshot);

    return snapshot.sourceWindow.sourceId == 17
        && snapshot.sourceWindow.sourceStartSeconds == 0.25
        && snapshot.sourceWindow.sourceEndSeconds == 1.5
        && snapshot.audioBuffer == source.audioBuffer
        && snapshot.nativeSource == source.nativeSource
        && snapshot.firstCanonicalSample == 456
        && snapshot.hnsepHarmonicAudio == source.analysis.hnsepHarmonicAudio
        && snapshot.hnsepResidualAudio == source.analysis.hnsepResidualAudio
        && nearlyEqual(snapshot.audioSampleRate, 48000.0)
        && snapshot.originalF0State == OpenTune::OriginalF0State::Ready
        && snapshot.detectedKey.root == OpenTune::Key::A
        && snapshot.detectedKey.scale == OpenTune::Scale::Minor
        && nearlyEqual(snapshot.detectedKey.confidence, 0.75)
        && snapshot.detectedKey.origin == OpenTune::Origin::Automatic
        && snapshot.silentGaps.size() == 1
        && snapshot.notes.size() == 1
        && snapshot.pitchShiftSettings == source.pitchShiftSettings
        && snapshot.volumeEnvelope.points().size() == 2
        && snapshot.notesRevision == 2
        && snapshot.noteTopologyInitialized
        && snapshot.pitchRevision == 3
        && snapshot.timeGridRevision == 4
        && snapshot.contentRevision == 7
        && snapshot.audioRevision == 8
        && snapshot.timeGrid == source.timeGrid
        && restored.timeGrid == snapshot.timeGrid
        && restored.sourceWindow.sourceId == source.sourceWindow.sourceId
        && restored.sourceWindow.sourceStartSeconds == source.sourceWindow.sourceStartSeconds
        && restored.sourceWindow.sourceEndSeconds == source.sourceWindow.sourceEndSeconds
        && restored.audioBuffer == source.audioBuffer
        && restored.nativeSource == source.nativeSource
        && restored.firstCanonicalSample == 456
        && restored.analysis.hnsepHarmonicAudio == source.analysis.hnsepHarmonicAudio
        && restored.analysis.hnsepResidualAudio == source.analysis.hnsepResidualAudio
        && nearlyEqual(restored.sampleRate, 48000.0)
        && restored.analysis.originalF0State == source.analysis.originalF0State
        && restored.analysis.detectedKey.root == source.analysis.detectedKey.root
        && restored.analysis.silentGaps.size() == source.analysis.silentGaps.size()
        && restored.notes.size() == source.notes.size()
        && restored.volumeEnvelope.points().size() == source.volumeEnvelope.points().size()
        // 反向投影构造新 ContentState（新内容身份），不继承 snapshot 的运行时 revision。
        && restored.contentRevision == 1;
}

bool testHnsepChunkStitchResidualAndSampleSlice()
{
    const std::vector<OpenTune::HnsepSilentGap> hnsepGaps{
        {4 * 44100 + 30000, 5 * 44100 + 30000},
        {9 * 44100 + 30000, 10 * 44100 + 30000}
    };
    const auto hnsepBoundaries = OpenTune::buildHnsepChunkBoundaries(
        131 * 44100 / 10, hnsepGaps);
    if (hnsepBoundaries.size() != 4
        || hnsepBoundaries[0] != 0
        || hnsepBoundaries[1] != ((5 * 44100 + 256) / 512) * 512
        || hnsepBoundaries[2] != (((5 * 44100 + 256) / 512) * 512
            + 5 * 44100 + 256) / 512 * 512
        || hnsepBoundaries[3] != 131 * 44100 / 10)
        return false;

    const auto unsplitBoundaries = OpenTune::buildHnsepChunkBoundaries(12 * 44100, {});
    if (unsplitBoundaries != std::vector<int64_t>{0, 12 * 44100})
        return false;

    const std::vector<OpenTune::HnsepChunkOutput> chunks{
        {0, {10.0f, 10.0f, 10.0f, 10.0f}},
        {2, {20.0f, 20.0f, 20.0f, 20.0f}}
    };
    std::vector<float> harmonic;
    if (!OpenTune::stitchHnsepChunkOutputs(6, chunks, harmonic)
        || harmonic.size() != 6
        || std::abs(harmonic[0] - 10.0f) > 1.0e-6f
        || std::abs(harmonic[2] - (40.0f / 3.0f)) > 1.0e-5f
        || std::abs(harmonic[3] - (50.0f / 3.0f)) > 1.0e-5f
        || std::abs(harmonic[5] - 20.0f) > 1.0e-6f)
        return false;

    const std::vector<float> dry{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    std::vector<float> residual;
    std::vector<float> slice;
    return OpenTune::makeHnsepResidual(dry, harmonic, residual)
        && residual.size() == dry.size()
        && std::abs((harmonic[3] + residual[3]) - dry[3]) <= 1.0e-6f
        && OpenTune::sliceHnsepSamples(harmonic, 2, 5, slice)
        && slice.size() == 5
        && slice[0] == harmonic[2]
        && slice[3] == harmonic[5]
        && slice[4] == 0.0f;
}

// contentStateFromSnapshot 是"new ContentState projection"：即使 snapshot 是
// revision=7 的运行时身份，新 state 也从默认 1 开始，而可编辑字段照常投影。
bool testContentStateProjectionStartsNewRuntimeRevision()
{
    OpenTune::EditableContentSnapshot snapshot;
    snapshot.contentRevision = 7;
    snapshot.notesRevision = 3;
    snapshot.notes.push_back({});
    snapshot.timeGrid = OpenTune::TimeGridSnapshot::bootstrapIdentity();

    const auto state = OpenTune::contentStateFromSnapshot(snapshot);

    return state.contentRevision == 1
        && state.notesRevision == 3
        && state.notes.size() == 1
        && state.timeGrid == snapshot.timeGrid;
}

// owner 负责按权威 sourceWindow 建立精确 TimeGrid；投影与发布只传递完整快照，
// 不补造、不修剪、也不按派生 buffer 长度重写。
bool testTimeGridIsNotRepairedAtProjectionOrPublicationBoundaries()
{
    OpenTune::ContentState state;
    state.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    state.sampleRate = 44100.0;
    state.timeGrid.reset();
    const auto projected = OpenTune::makeContentSnapshot(state);
    if (projected.timeGrid != nullptr)
        return false;
    if (OpenTune::contentStateFromSnapshot(projected).timeGrid != nullptr)
        return false;

    state.sourceWindow = OpenTune::SourceWindow{7, "persistent-7", 2.0, 3.25};
    const auto ownerGrid = OpenTune::TimeGridSnapshot::makeIdentity(0.75);
    state.timeGrid = ownerGrid;
    const auto snapshot = std::make_shared<const OpenTune::EditableContentSnapshot>(
        OpenTune::makeContentSnapshot(state));
    if (snapshot->timeGrid != ownerGrid
        || !nearlyEqual(snapshot->timeGrid->totalDurationSeconds(), 0.75)
        || snapshot->sourceWindow.sourceStartSeconds != 2.0
        || snapshot->sourceWindow.sourceEndSeconds != 3.25)
        return false;

    OpenTune::TimeStretchCache timeStretchCache;
    const auto native = OpenTune::NativePcmSource{snapshot->audioBuffer, 44100.0, 0, 0};
    const auto source = OpenTune::makePlaybackReadSource(
        OpenTune::ContentKey{OpenTune::DomainKind::StandaloneClip, 1, 0},
        snapshot,
        native,
        snapshot->audioBuffer,
        44100.0,
        snapshot->firstCanonicalSample,
        std::make_shared<OpenTune::RenderCache>(),
        timeStretchCache);

    return source.contentSnapshot != nullptr
        && source.contentSnapshot->timeGrid == ownerGrid
        && nearlyEqual(source.contentSnapshot->timeGrid->totalDurationSeconds(), 0.75)
        && source.contentSnapshot->sourceWindow.sourceStartSeconds == 2.0
        && source.contentSnapshot->sourceWindow.sourceEndSeconds == 3.25
        && source.contentSnapshot->audioBuffer == snapshot->audioBuffer
        && source.nativeSource == native;
}

bool testStretcherHandleSurvivesPoolRemoval()
{
    OpenTune::StretcherPool pool;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 77, 0};
    auto oldHandle = pool.getOrCreate(key, 44100.0, 1);
    if (oldHandle == nullptr)
        return false;

    pool.remove(key);
    auto newHandle = pool.getOrCreate(key, 44100.0, 1);
    return newHandle != nullptr && newHandle != oldHandle;
}

// Capture retire 重置 active content 为默认 bootstrap（revision 1、timeGrid 非空），
// revive 恢复记录并推进一次 contentRevision。
bool testCaptureRetireReviveKeepsTimeGridAndAdvancesRevision()
{
    OpenTune::CaptureSegmentContent segment(7);
    if (segment.content().contentRevision != 1)
        return false;

    juce::AudioBuffer<float> buffer(1, 64);
    buffer.clear();
    segment.applyAudioBuffer(buffer, 48000.0);

    const uint64_t revisionBeforeRetire = segment.content().contentRevision;
    if (revisionBeforeRetire == 0 || !segment.content().timeGrid)
        return false;

    segment.retireContent(segment.contentKey());

    if (!segment.content().timeGrid || !segment.content().timeGrid->isIdentity())
        return false;
    if (segment.content().contentRevision != 1)
        return false;

    segment.reviveContent(segment.contentKey());

    return segment.content().timeGrid != nullptr
        && segment.content().contentRevision == revisionBeforeRetire + 1;
}

// zero-sample（duration 非正）音频不得把 timeGrid 写成 nullptr；
// bootstrap identity 保留且 timeGridRevision 不推进，真实 audio/content bump 保留。
bool testZeroSampleAudioKeepsBootstrapTimeGrid()
{
    juce::AudioBuffer<float> empty(2, 0);

    OpenTune::CaptureSegmentContent capture(5);
    capture.applyAudioBuffer(empty, 48000.0);
    if (!capture.content().timeGrid || !capture.content().timeGrid->isIdentity())
        return false;
    if (capture.content().timeGridRevision != 0)
        return false;
    if (capture.content().audioRevision != 1 || capture.content().contentRevision != 2)
        return false;

    OpenTune::StandaloneClipContent clip(9);
    clip.applyAudioBuffer(std::make_shared<const juce::AudioBuffer<float>>(2, 0), 48000.0);
    return clip.content().timeGrid != nullptr
        && clip.content().timeGrid->isIdentity()
        && clip.content().timeGridRevision == 0
        && clip.content().audioRevision == 1
        && clip.content().contentRevision == 2;
}

// Owner F0 成功提交必须一次性收口：state=Ready、analysisRevision/contentRevision
// 各前进一次；提交后的 Ready 幂等 setter 不得二次推进 contentRevision。
bool testStandaloneOriginalF0CommitAdvancesRevisionsOnce()
{
    OpenTune::StandaloneClipContent clip(11);
    const auto& before = clip.content();
    const uint64_t analysisBefore = before.analysis.analysisRevision;
    const uint64_t pitchBefore = before.pitchRevision;
    const uint64_t contentBefore = before.contentRevision;

    clip.applyOriginalF0(makeUsableOriginalF0Curve());

    const auto snapshot = clip.snapshotContent();
    if (snapshot->originalF0State != OpenTune::OriginalF0State::Ready)
        return false;
    if (!snapshot->hasUsableOriginalF0())
        return false;
    if (clip.content().analysis.analysisRevision != analysisBefore + 1)
        return false;
    if (clip.content().pitchRevision != pitchBefore + 1)
        return false;
    if (clip.content().contentRevision != contentBefore + 1)
        return false;

    // ContentRefresh 调用方随后仍会 setContentOriginalF0State(Ready)：同状态不得再 bump。
    clip.applyOriginalF0State(OpenTune::OriginalF0State::Ready);
    return clip.content().contentRevision == contentBefore + 1
        && clip.content().analysis.analysisRevision == analysisBefore + 1;
}

bool testCaptureSegmentOriginalF0CommitAdvancesRevisionsOnce()
{
    OpenTune::CaptureSegmentContent segment(7);
    segment.content().sourceWindow = OpenTune::SourceWindow{23, {}, 1.0, 2.0};
    const uint64_t analysisBefore = segment.content().analysis.analysisRevision;
    const uint64_t pitchBefore = segment.content().pitchRevision;
    const uint64_t contentBefore = segment.content().contentRevision;

    segment.applyOriginalF0(makeUsableOriginalF0Curve());

    const auto snapshot = segment.snapshotContent();
    if (snapshot->sourceWindow.sourceId != 23
        || !nearlyEqual(snapshot->sourceWindow.sourceStartSeconds, 1.0)
        || !nearlyEqual(snapshot->sourceWindow.sourceEndSeconds, 2.0))
        return false;
    if (snapshot->originalF0State != OpenTune::OriginalF0State::Ready)
        return false;
    if (!snapshot->hasUsableOriginalF0())
        return false;
    if (segment.content().analysis.analysisRevision != analysisBefore + 1)
        return false;
    if (segment.content().pitchRevision != pitchBefore + 1)
        return false;
    if (segment.content().contentRevision != contentBefore + 1)
        return false;

    // 失败/无曲线路径的 setter 保持幂等，不改变失败语义。
    segment.applyOriginalF0State(OpenTune::OriginalF0State::Ready);
    return segment.content().contentRevision == contentBefore + 1;
}

bool testIdentityTimeGridIsAlwaysPublishedAndNonIdentityMissIsSilent()
{
    auto identity = OpenTune::TimeGridSnapshot::makeIdentity(1.0);
    if (!identity)
        return false;

    OpenTune::ContentState state;
    state.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    state.sampleRate = 44100.0;
    state.timeGrid = identity;
    state.contentRevision = 1;

    auto snapshot = std::make_shared<const OpenTune::EditableContentSnapshot>(
        OpenTune::makeContentSnapshot(state));
    if (!snapshot->timeGrid || !snapshot->timeGrid->isIdentity())
        return false;

    OpenTune::PlaybackReadSource source;
    source.contentSnapshot = snapshot;
    const auto audio = snapshot->audioBuffer;
    const auto native = OpenTune::NativePcmSource{audio, 44100.0, 0, 1};
    source.nativeSource = native;
    source.canonicalBuffer = audio;
    source.canonicalSampleRate = 44100.0;
    source.firstCanonicalSample = snapshot->firstCanonicalSample;
    source.preparedDry.buffer = audio;
    source.preparedDry.sampleRate = 44100.0;
    source.preparedDry.firstPreparedSample = 0;
    source.preparedDry.nativeSource = native;

    juce::AudioBuffer<float> destination(1, 4);
    OpenTune::PlaybackReadRequest request(source, 0, 44100.0, 4, 0.0);
    if (OpenTune::readPlaybackAudio(request, destination, 0) != 4)
        return false;

    std::vector<OpenTune::TimeHandle> handles = snapshot->timeGrid->handles();
    handles.insert(handles.begin() + 1, OpenTune::TimeHandle{
        99, 0.5, 0.75, OpenTune::HandleKind::UserAdded, OpenTune::Confidence::Default});
    auto warped = OpenTune::TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (!warped || warped->isIdentity())
        return false;

    state.timeGrid = warped;
    snapshot = std::make_shared<const OpenTune::EditableContentSnapshot>(
        OpenTune::makeContentSnapshot(state));
    source.contentSnapshot = snapshot;
    request.source = source;
    destination.clear();
    return OpenTune::readPlaybackAudio(request, destination, 0) == 0;
}

// TimeStretchCache 的双版本键：(contentRevision, timeGridRevision) 必须同时匹配；
// 任一为旧值都必须 miss（返回 0），不得用旧内容/旧时间网格的切片。
bool testTimeStretchCacheDualRevisionKey()
{
    OpenTune::TimeStretchCache cache;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 42, 0};

    const uint32_t buildGeneration = cache.beginBuild(key);
    cache.store(key, {0.25f, -0.5f, 0.75f, -1.0f}, 5, 7, 44100.0, buildGeneration);

    juce::AudioBuffer<float> destination(1, 4);
    destination.clear();

    // 同 key、同 contentRevision/timeGridRevision：命中并返回全部样本。
    if (cache.sliceForOutputRange(key, 5, 7, 0, destination, 0, 4, 44100) != 4)
        return false;
    if (!nearlyEqual(destination.getSample(0, 0), 0.25f)
        || !nearlyEqual(destination.getSample(0, 3), -1.0f))
        return false;

    // 旧 contentRevision（timeGridRevision 相同）：miss。
    destination.clear();
    if (cache.sliceForOutputRange(key, 4, 7, 0, destination, 0, 4, 44100) != 0)
        return false;

    // 旧 timeGridRevision（contentRevision 相同）：miss。
    destination.clear();
    if (cache.sliceForOutputRange(key, 5, 6, 0, destination, 0, 4, 44100) != 0)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// 纯逻辑时间坐标/时间轴/区块渲染跨度契约（不触碰生产公式）
// ---------------------------------------------------------------------------

// secondsToSamples 是截断（truncation）语义，不是四舍五入。
bool testTimeCoordinateSecondsToSamplesTruncation()
{
    const double sampleRate = 44100.0;

    // 0.001s 在 44.1kHz 下精确为 44.1 样本，必须截断到 44。
    if (OpenTune::TimeCoordinate::secondsToSamples(0.001, sampleRate) != 44)
        return false;
    if (OpenTune::TimeCoordinate::secondsToSamples(1.0, sampleRate) != 44100)
        return false;
    return true;
}

bool testTimeCoordinateNearestPointProjection()
{
    const double sampleRate = 44100.0;
    return OpenTune::TimeCoordinate::secondsToSamplesNearest(0.001, sampleRate) == 44
        && OpenTune::TimeCoordinate::secondsToSamplesNearest(0.0011, sampleRate) == 49;
}

// Floor/Ceil 是时间区间端点的向外取整：floor 起点 <= 精确样本 <= ceil 终点。
bool testTimeCoordinateFloorCeilIntervalEndpoints()
{
    const double sampleRate = 44100.0;
    const double startSeconds = 0.0005;   // 22.05 样本
    const double endSeconds = 0.0015;     // 66.15 样本
    const double exactStart = startSeconds * sampleRate;
    const double exactEnd = endSeconds * sampleRate;

    const int64_t floorStart = OpenTune::TimeCoordinate::secondsToSamplesFloor(startSeconds, sampleRate);
    const int64_t ceilStart = OpenTune::TimeCoordinate::secondsToSamplesCeil(startSeconds, sampleRate);
    const int64_t floorEnd = OpenTune::TimeCoordinate::secondsToSamplesFloor(endSeconds, sampleRate);
    const int64_t ceilEnd = OpenTune::TimeCoordinate::secondsToSamplesCeil(endSeconds, sampleRate);

    if (floorStart != 22 || ceilStart != 23 || floorEnd != 66 || ceilEnd != 67)
        return false;

    // 非整数端点必须 floor != ceil；区间 [floor(start), ceil(end)) 覆盖 [start, end]。
    return floorStart != ceilStart
        && floorEnd != ceilEnd
        && static_cast<double>(floorStart) <= exactStart
        && exactStart <= static_cast<double>(ceilStart)
        && static_cast<double>(floorEnd) <= exactEnd
        && exactEnd <= static_cast<double>(ceilEnd);
}

// 跨采样率投影按未取整的精确秒数计算：同一时间边界从不同源采样率投影到同一目标样本，
// 相邻 chunk 共享边界投影后首尾相接（无缝隙/无重叠）。
bool testTimeCoordinateSampleRateProjectBoundaries()
{
    namespace TC = OpenTune::TimeCoordinate;

    // 0.25s 边界：44100Hz -> 11025 号样本，48000Hz -> 12000 号样本；投影到 96kHz 均为 24000。
    if (TC::sampleRateProject(11025, 44100.0, 96000.0) != 24000)
        return false;
    if (TC::sampleRateProject(12000, 48000.0, 96000.0) != 24000)
        return false;

    // 相邻 chunk: [0, 22050) + [22050, 44100) 投影到 48kHz 后与整段 [0, 44100) 长度守恒。
    const int64_t projectedStart = TC::sampleRateProject(0, 44100.0, 48000.0);
    const int64_t projectedBoundary = TC::sampleRateProject(22050, 44100.0, 48000.0);
    const int64_t projectedEnd = TC::sampleRateProject(44100, 44100.0, 48000.0);
    if (projectedBoundary != 24000)
        return false;
    if (projectedBoundary - projectedStart + (projectedEnd - projectedBoundary) != projectedEnd - projectedStart)
        return false;

    // 非整数比率走 round 语义：44100 -> 48000 的 100 号样本 = 108.84 -> 109。
    return TC::sampleRateProject(100, 44100.0, 48000.0) == 109;
}

// F0Timeline: 负/超范围 clamp、rangeForTimes 的 floor 起点 + ceil 排他终点、半开区间。
bool testF0TimelineClampAndHalfOpenRange()
{
    // hopSize=256 @ 16kHz => 0.016s/frame，共 10 帧（帧 0..9，排他终点 10）。
    OpenTune::F0Timeline timeline(256, 16000.0, 10);
    if (timeline.isEmpty() || timeline.endFrameExclusive() != 10)
        return false;

    // 负/超范围 clamp 到合法帧区间。
    if (timeline.frameAtOrBefore(-1.0) != 0 || timeline.frameAtOrBefore(1000.0) != 9)
        return false;
    if (timeline.exclusiveFrameAt(-1.0) != 0 || timeline.exclusiveFrameAt(1000.0) != 10)
        return false;

    // 起点 floor、终点 ceil：0.0201s -> 1.256 帧 -> 1；0.0495s -> 3.094 帧 -> 4。
    const auto midRange = timeline.rangeForTimes(0.0201, 0.0495);
    if (midRange.startFrame != 1 || midRange.endFrameExclusive != 4 || midRange.isEmpty())
        return false;

    // 半开区间: [2*spf, 4*spf) 恰好覆盖帧 2、3，不含帧 4。
    const auto exactRange = timeline.rangeForTimes(timeline.timeAtFrame(2), timeline.timeAtFrame(4));
    if (exactRange.startFrame != 2 || exactRange.endFrameExclusive != 4)
        return false;

    // 空半开区间（start == end）映射为空帧范围。
    const auto emptyRange = timeline.rangeForTimes(timeline.timeAtFrame(2), timeline.timeAtFrame(2));
    if (!emptyRange.isEmpty() || emptyRange.startFrame != 2 || emptyRange.endFrameExclusive != 2)
        return false;

    // 整体越界区间 clamp 到整条时间轴。
    const auto clamped = timeline.rangeForTimes(-1.0, 1000.0);
    return clamped.startFrame == 0 && clamped.endFrameExclusive == 10;
}

bool testF0TimelineNearestSplitBoundary()
{
    OpenTune::F0Timeline timeline(160, 16000.0, 100);
    if (timeline.nearestFrameBoundary(-1.0) != 0
        || timeline.nearestFrameBoundary(100.0) != 100)
        return false;

    if (timeline.nearestFrameBoundary(timeline.timeAtFrame(17)) != 17
        || timeline.nearestFrameBoundary(0.005) != 1     // 半帧 tie 归后一帧
        || timeline.nearestFrameBoundary(0.015) != 2
        || timeline.nearestFrameBoundary(0.024999) != 2
        || timeline.nearestFrameBoundary(0.025) != 3     // tie 归后一帧
        || timeline.nearestFrameBoundary(0.025001) != 3)
        return false;

    // frame time 由 double 乘法得到，与 sample 网格的 round-trip 只断言数值等价。
    for (int boundary = 1; boundary < timeline.endFrameExclusive(); ++boundary) {
        const double seconds = timeline.timeAtFrame(boundary);
        const int64_t sample = OpenTune::TimeCoordinate::secondsToSamplesNearest(
            seconds, OpenTune::TimeCoordinate::kRenderSampleRate);
        if (!nearlyEqual(OpenTune::TimeCoordinate::samplesToSeconds(
                sample, OpenTune::TimeCoordinate::kRenderSampleRate), seconds))
            return false;
    }

    // split 的 F0 分区按 frame index 半开：[0, boundary) + [boundary, N)；
    // nearest 在真实 hop/rate 的 frame time 上稳定，两个区间无缝无重叠。
    OpenTune::F0Timeline realistic(160, 16000.0, 1000);
    for (int frame = 1; frame < realistic.endFrameExclusive(); ++frame) {
        const int boundary = realistic.nearestFrameBoundary(realistic.timeAtFrame(frame));
        if (boundary != frame)
            return false;

        const auto leading = realistic.rangeForFrames(0, boundary);
        const auto trailing = realistic.rangeForFrames(boundary, realistic.endFrameExclusive());
        if (leading.startFrame != 0
            || leading.endFrameExclusive != boundary
            || trailing.startFrame != boundary
            || trailing.endFrameExclusive != realistic.endFrameExclusive())
            return false;

        if ((leading.endFrameExclusive - leading.startFrame)
                + (trailing.endFrameExclusive - trailing.startFrame)
            != realistic.endFrameExclusive())
            return false;
    }

    OpenTune::F0Timeline singleFrame(160, 16000.0, 1);
    return singleFrame.nearestFrameBoundary(0.004) == 0
        && singleFrame.nearestFrameBoundary(0.006) == 1;
}

// 固定映射建立后，权威窗口秒值保持原值；host 区间与 prepared 区间是同一个
// 半开整数映射，block 划分、seek 和端点边界只切地址，不移动映射。
bool testFixedGeometryKeepsAuthoritativeWindowSecondsAndHalfOpenSpans()
{
    namespace PSG = OpenTune::PlaybackSampleGeometry;
    constexpr double hostRate = 48000.0;

    const OpenTune::SourceWindow window{31, "persistent-31", 0.0011, 1.2345};
    const auto placement = PSG::makePlacement(hostRate,
                                              window.sourceStartSeconds,
                                              window.sourceEndSeconds,
                                              0.0);
    if (window.sourceStartSeconds != 0.0011
        || window.sourceEndSeconds != 1.2345
        || window.durationSeconds() != 1.2345 - 0.0011)
        return false;

    const auto host = PSG::hostPlaybackSpan(placement);
    const auto prepared = PSG::requiredPreparedSpan(placement);
    if (host.empty() || prepared.empty()
        || prepared.end - prepared.begin != host.end - host.begin
        || prepared.begin != host.begin - placement.offsetSamples
        || prepared.end != host.end - placement.offsetSamples)
        return false;

    // 两个相邻半开 block 公共边界只出现一次；终点 Z 不属于播放区间。
    const int64_t sharedBoundary = host.begin + (host.end - host.begin) / 2;
    const auto leading = PSG::intersect(host, {host.begin, sharedBoundary});
    const auto trailing = PSG::intersect(host, {sharedBoundary, host.end});
    if (leading.begin != host.begin || leading.end != sharedBoundary
        || trailing.begin != sharedBoundary || trailing.end != host.end)
        return false;
    if (!PSG::intersect({host.end, host.end + 512}, host).empty())
        return false;

    // host 区间内每个样本恰好映射到一个 prepared local 地址。
    const int64_t length = host.end - host.begin;
    for (const int64_t offset : {int64_t{0}, length / 3, length - 1})
    {
        const auto address = PSG::mapHostSampleToPrepared(
            placement, host.begin + offset, prepared.begin);
        if (address.bufferLocalIndex != offset)
            return false;
    }

    // 权威终点直接投影，不由起点与时长重建；投影不回写窗口秒值。
    const auto endProjection = PSG::intersect(
        {placement.playbackEndSample, placement.playbackEndSample + 1}, host);
    return endProjection.empty()
        && window.sourceEndSeconds == 1.2345;
}

bool testARAPlaybackRendererRealtimeStopGate()
{
    using Realtime = juce::AudioProcessor::Realtime;
    return !OpenTune::shouldRenderPlaybackBlock(Realtime::yes, false)
        && OpenTune::shouldRenderPlaybackBlock(Realtime::yes, true)
        && OpenTune::shouldRenderPlaybackBlock(Realtime::no, false);
}

// TimeGrid makeFromHandles: 总时长守恒、tauForward/tauInverse 锚点位精确、越界 clamp。
bool testTimeGridMakeFromHandlesDurationAndAnchors()
{
    std::vector<OpenTune::TimeHandle> handles{
        {1, 0.0, 0.0, OpenTune::HandleKind::ClipStart, OpenTune::Confidence::Default},
        {2, 0.5, 0.25, OpenTune::HandleKind::UserAdded, OpenTune::Confidence::Default},
        {3, 1.0, 1.0, OpenTune::HandleKind::ClipEnd, OpenTune::Confidence::Default}};

    const auto grid = OpenTune::TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (!grid)
        return false;

    // 总时长守恒：output 总长 == source 总长（端点不变量）。
    if (!nearlyEqual(grid->totalDurationSeconds(), 1.0))
        return false;
    if (!nearlyEqual(grid->handles().back().output_seconds, grid->handles().back().source_seconds))
        return false;

    // 锚点位精确（非 epsilon 比较）：每个 handle 上正向/逆向映射都命中其 output/source。
    if (grid->tauForward(0.0) != 0.0 || grid->tauForward(0.5) != 0.25 || grid->tauForward(1.0) != 1.0)
        return false;
    if (grid->tauInverse(0.0) != 0.0 || grid->tauInverse(0.25) != 0.5 || grid->tauInverse(1.0) != 1.0)
        return false;

    // 越界 clamp 到两端锚点。
    if (grid->tauForward(-0.5) != 0.0 || grid->tauForward(2.0) != 1.0)
        return false;
    if (grid->tauInverse(-0.5) != 0.0 || grid->tauInverse(2.0) != 1.0)
        return false;

    // 段内插值往返一致（第二段中点 0.75s <-> 0.625s）。
    return nearlyEqual(grid->tauInverse(grid->tauForward(0.75)), 0.75);
}

// makeFromHandles 拒绝破坏总时长守恒的 handles（不做兜底/修正）。
bool testTimeGridMakeFromHandlesRejectsDurationViolation()
{
    std::vector<OpenTune::TimeHandle> handles{
        {1, 0.0, 0.0, OpenTune::HandleKind::ClipStart, OpenTune::Confidence::Default},
        {2, 1.0, 1.5, OpenTune::HandleKind::ClipEnd, OpenTune::Confidence::Default}};

    return OpenTune::TimeGridSnapshot::makeFromHandles(std::move(handles)) == nullptr;
}

bool testTimeGridSourceSpacingUsesAbsoluteSeconds()
{
    using OpenTune::TimeGridSnapshot;
    return !TimeGridSnapshot::hasMinimumSourceSpacing(0.0, 0.149999)
        && TimeGridSnapshot::hasMinimumSourceSpacing(0.0, 0.150000)
        && !TimeGridSnapshot::hasMinimumSourceSpacing(1.0, 1.149999)
        && TimeGridSnapshot::hasMinimumSourceSpacing(1.0, 1.150000);
}

bool testPlacementFadeUsesAbsoluteSeconds()
{
    const double duration = 1.0;
    const double fadeIn = 0.25;
    const double fadeOut = 0.25;

    return nearlyEqual(OpenTune::placementFadeGain(0.0, duration, fadeIn, fadeOut), 0.0)
        && nearlyEqual(OpenTune::placementFadeGain(0.125, duration, fadeIn, fadeOut), 0.5)
        && nearlyEqual(OpenTune::placementFadeGain(0.5, duration, fadeIn, fadeOut), 1.0)
        && nearlyEqual(OpenTune::placementFadeGain(0.875, duration, fadeIn, fadeOut), 0.5)
        && nearlyEqual(OpenTune::placementFadeGain(1.0, duration, fadeIn, fadeOut), 0.0);
}

// AutomationLane slice/merge 纯逻辑契约。容差 1e-3：fromSnapshot 归一化含 1e-4 简化阈值，
// 逐点 evalAt 对比不要求位精确。
bool laneEvalNear(const OpenTune::AutomationLane& lane, double t, float expected)
{
    return std::abs(lane.evalAt(t) - expected) <= 1.0e-3;
}

// slice：取父 lane [start, end] 并平移为 child local 秒，两端以父插值值锚定；
// 逐点 evalAt 与父一致；空 lane 与 end<=start 返回空。
bool testAutomationLaneSliceToLocalRange()
{
    const auto parent = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{{0.0, 0.0f}, {1.0, 4.0f}, {2.0, 0.0f}});

    const auto sliced = OpenTune::AutomationLane::sliceToLocalRange(parent, 0.5, 1.5);

    // 端点锚定：{0, evalAt(0.5)=2}、内部点平移 {0.5, 4}、{1, evalAt(1.5)=2}。
    if (sliced.points().size() != 3)
        return false;
    if (!nearlyEqual(sliced.points().front().timeSeconds, 0.0)
        || !nearlyEqual(sliced.points().front().gainDb, 2.0f)
        || !nearlyEqual(sliced.points()[1].timeSeconds, 0.5)
        || !nearlyEqual(sliced.points()[1].gainDb, 4.0f)
        || !nearlyEqual(sliced.points().back().timeSeconds, 1.0)
        || !nearlyEqual(sliced.points().back().gainDb, 2.0f))
        return false;

    // 子区间 evalAt 与父区间一致（local t ↔ 父 t+0.5）。
    for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        if (!laneEvalNear(sliced, t, parent.evalAt(t + 0.5)))
            return false;
    }

    // 空 lane 与 end<=start 返回空（单位增益）。
    const OpenTune::AutomationLane emptyLane;
    if (!OpenTune::AutomationLane::sliceToLocalRange(emptyLane, 0.0, 1.0).empty())
        return false;
    return OpenTune::AutomationLane::sliceToLocalRange(parent, 1.0, 1.0).empty();
}

// merge：split→merge 往返逐点无损。
bool testAutomationLaneMergeContiguousRoundTrip()
{
    const auto parent = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{
            {0.0, 0.0f}, {1.0, 3.0f}, {2.0, 1.0f}, {3.0, 0.0f}});
    const double b = 1.7;

    const auto merged = OpenTune::AutomationLane::mergeContiguous(
        OpenTune::AutomationLane::sliceToLocalRange(parent, 0.0, b),
        OpenTune::AutomationLane::sliceToLocalRange(parent, b, 3.0),
        b);

    for (const double t : {0.0, 0.4, 1.0, 1.7, 2.2, 2.9, 3.0}) {
        if (!laneEvalNear(merged, t, parent.evalAt(t)))
            return false;
    }
    return true;
}

// merge 接缝语义与空侧：空 trailing = 0 dB 单位增益；空 leading 时接缝锚定 trailing.evalAt(0)。
bool testAutomationLaneMergeSeamAndEmptySides()
{
    const auto parent = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{
            {0.0, 0.0f}, {1.0, 3.0f}, {2.0, 1.0f}, {3.0, 0.0f}});
    const OpenTune::AutomationLane emptyLane;

    // 双空：单点 0 dB 被归一化为空 lane。
    if (!OpenTune::AutomationLane::mergeContiguous(emptyLane, emptyLane, 2.0).empty())
        return false;

    // 空 trailing：接缝之后为 0 dB（单位增益）。
    const auto leadingOnly = OpenTune::AutomationLane::mergeContiguous(parent, emptyLane, 2.0);
    if (!laneEvalNear(leadingOnly, 2.5, 0.0f))
        return false;

    // 空 leading：接缝处与之后均为 trailing.evalAt(0) = -6 dB。
    const auto trailing = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{{0.5, -6.0f}});
    const auto trailingOnly = OpenTune::AutomationLane::mergeContiguous(emptyLane, trailing, 2.0);
    return laneEvalNear(trailingOnly, 2.0, -6.0f)
        && laneEvalNear(trailingOnly, 2.5, -6.0f)
        && laneEvalNear(trailingOnly, 3.0, -6.0f);
}

} // namespace

int main()
{
    if (!testErasedRangesSurviveContentSnapshotProjection())
    {
        std::fputs("FAIL: erased ranges content snapshot projection\n", stderr);
        return 1;
    }

    if (!testErasedRangesSurviveF0CurveRebuild())
    {
        std::fputs("FAIL: erased ranges F0 curve rebuild\n", stderr);
        return 1;
    }

    if (!testCommonFieldsRoundTrip())
    {
        std::fputs("FAIL: ContentState snapshot round-trip\n", stderr);
        return 1;
    }

    if (!testHnsepChunkStitchResidualAndSampleSlice())
    {
        std::fputs("FAIL: HNSEP chunk stitching/residual/sample slicing\n", stderr);
        return 1;
    }

    if (!testContentStateProjectionStartsNewRuntimeRevision())
    {
        std::fputs("FAIL: contentStateFromSnapshot new-ContentState projection revision\n", stderr);
        return 1;
    }

    if (!testTimeGridIsNotRepairedAtProjectionOrPublicationBoundaries())
    {
        std::fputs("FAIL: owner TimeGrid and window preserved at projection/publication\n", stderr);
        return 1;
    }

    if (!testStretcherHandleSurvivesPoolRemoval())
    {
        std::fputs("FAIL: stretcher handle lifetime across pool removal\n", stderr);
        return 1;
    }

    if (!testCaptureRetireReviveKeepsTimeGridAndAdvancesRevision())
    {
        std::fputs("FAIL: Capture retire/revive TimeGrid and revision contract\n", stderr);
        return 1;
    }

    if (!testZeroSampleAudioKeepsBootstrapTimeGrid())
    {
        std::fputs("FAIL: zero-sample audio bootstrap TimeGrid contract\n", stderr);
        return 1;
    }

    if (!testStandaloneOriginalF0CommitAdvancesRevisionsOnce())
    {
        std::fputs("FAIL: StandaloneClipContent OriginalF0 commit revisions\n", stderr);
        return 1;
    }

    if (!testCaptureSegmentOriginalF0CommitAdvancesRevisionsOnce())
    {
        std::fputs("FAIL: CaptureSegmentContent OriginalF0 commit revisions\n", stderr);
        return 1;
    }

    if (!testIdentityTimeGridIsAlwaysPublishedAndNonIdentityMissIsSilent())
    {
        std::fputs("FAIL: Playback TimeGrid identity/cache miss contract\n", stderr);
        return 1;
    }

    if (!testTimeStretchCacheDualRevisionKey())
    {
        std::fputs("FAIL: TimeStretchCache dual-revision key contract\n", stderr);
        return 1;
    }

    if (!testTimeCoordinateSecondsToSamplesTruncation())
    {
        std::fputs("FAIL: TimeCoordinate secondsToSamples truncation\n", stderr);
        return 1;
    }

    if (!testTimeCoordinateFloorCeilIntervalEndpoints())
    {
        std::fputs("FAIL: TimeCoordinate floor/ceil interval endpoints\n", stderr);
        return 1;
    }

    if (!testTimeCoordinateNearestPointProjection())
    {
        std::fputs("FAIL: TimeCoordinate nearest point projection\n", stderr);
        return 1;
    }

    if (!testTimeCoordinateSampleRateProjectBoundaries())
    {
        std::fputs("FAIL: TimeCoordinate sampleRateProject cross-rate boundaries\n", stderr);
        return 1;
    }

    if (!testF0TimelineClampAndHalfOpenRange())
    {
        std::fputs("FAIL: F0Timeline clamp and half-open range\n", stderr);
        return 1;
    }

    if (!testF0TimelineNearestSplitBoundary())
    {
        std::fputs("FAIL: F0Timeline nearest split boundary\n", stderr);
        return 1;
    }

    if (!testFixedGeometryKeepsAuthoritativeWindowSecondsAndHalfOpenSpans())
    {
        std::fputs("FAIL: fixed geometry authority and half-open identity\n", stderr);
        return 1;
    }

    if (!testARAPlaybackRendererRealtimeStopGate())
    {
        std::fputs("FAIL: ARA playback renderer realtime stop gate\n", stderr);
        return 1;
    }

    if (!testTimeGridMakeFromHandlesDurationAndAnchors())
    {
        std::fputs("FAIL: TimeGrid makeFromHandles duration/anchor contract\n", stderr);
        return 1;
    }

    if (!testTimeGridMakeFromHandlesRejectsDurationViolation())
    {
        std::fputs("FAIL: TimeGrid makeFromHandles duration preservation rejection\n", stderr);
        return 1;
    }

    if (!testTimeGridSourceSpacingUsesAbsoluteSeconds())
    {
        std::fputs("FAIL: TimeGrid source spacing absolute-seconds contract\n", stderr);
        return 1;
    }

    if (!testPlacementFadeUsesAbsoluteSeconds())
    {
        std::fputs("FAIL: placement fade absolute-seconds contract\n", stderr);
        return 1;
    }

    if (!testAutomationLaneSliceToLocalRange())
    {
        std::fputs("FAIL: AutomationLane sliceToLocalRange contract\n", stderr);
        return 1;
    }

    if (!testAutomationLaneMergeContiguousRoundTrip())
    {
        std::fputs("FAIL: AutomationLane mergeContiguous split->merge round-trip\n", stderr);
        return 1;
    }

    if (!testAutomationLaneMergeSeamAndEmptySides())
    {
        std::fputs("FAIL: AutomationLane mergeContiguous seam and empty sides\n", stderr);
        return 1;
    }

    std::puts("ContentSnapshotProjection tests passed.");
    return 0;
}
