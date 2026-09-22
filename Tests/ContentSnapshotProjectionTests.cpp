// 测试 target 未注入 ARA SDK include 路径，而 juce_audio_processors 在
// JucePlugin_Enable_ARA=1 时会拉入 <ARA_Library/...>。本文件只使用
// OpenTunePlaybackRenderer.h 中 ARA 守卫之外的纯逻辑 span 辅助函数，
// 故在本 TU 内关闭该宏；生产 target 的配置不受影响。
#undef JucePlugin_Enable_ARA

#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Content/StandaloneClipContent.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Utils/TimeCoordinate.h"
#include "../Source/Utils/F0Timeline.h"
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
        && nearlyEqual(snapshot.sourceWindow.sourceStartSeconds, 0.25)
        && nearlyEqual(snapshot.sourceWindow.sourceEndSeconds, 1.5)
        && snapshot.audioBuffer == source.audioBuffer
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
        && restored.audioBuffer == source.audioBuffer
        && nearlyEqual(restored.sampleRate, 48000.0)
        && restored.analysis.originalF0State == source.analysis.originalF0State
        && restored.analysis.detectedKey.root == source.analysis.detectedKey.root
        && restored.analysis.silentGaps.size() == source.analysis.silentGaps.size()
        && restored.notes.size() == source.notes.size()
        && restored.volumeEnvelope.points().size() == source.volumeEnvelope.points().size()
        // 反向投影构造新 ContentState（新内容身份），不继承 snapshot 的运行时 revision。
        && restored.contentRevision == 1;
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
    state.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 8);
    state.sampleRate = 44100.0;
    state.timeGrid = identity;
    state.contentRevision = 1;

    auto snapshot = std::make_shared<const OpenTune::EditableContentSnapshot>(
        OpenTune::makeContentSnapshot(state));
    if (!snapshot->timeGrid || !snapshot->timeGrid->isIdentity())
        return false;

    OpenTune::PlaybackReadSource source;
    source.contentSnapshot = snapshot;
    source.audioBuffer = snapshot->audioBuffer;
    source.audioSampleRate = snapshot->audioSampleRate;
    source.preparedDry.buffer = snapshot->audioBuffer;
    source.preparedDry.sampleRate = 44100.0;
    source.preparedDry.canonicalBuffer = snapshot->audioBuffer;

    juce::AudioBuffer<float> destination(1, 4);
    OpenTune::PlaybackReadRequest request(source, 0, 44100.0, 4);
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

// computeRegionBlockRenderSpan: 部分重叠时 destination 起点 floor、终点 ceil。
bool testComputeRegionBlockRenderSpanPartialOverlap()
{
    // playhead block [0, 1000/44100≈0.02268)s，playback 仅覆盖 [0.0005, 0.015)。
    const auto partial = OpenTune::computeRegionBlockRenderSpan(
        /*blockStartSeconds=*/0.0,
        /*blockSamples=*/1000,
        /*hostSampleRate=*/44100.0,
        /*playbackStartSeconds=*/0.0005,
        /*playbackEndSeconds=*/0.015);
    if (!partial)
        return false;
    if (partial->destinationStartSample != 22)   // floor(0.0005 * 44100 = 22.05)
        return false;
    if (partial->samplesToCopy != 640)           // ceil(0.015 * 44100 = 661.5) - 22
        return false;
    if (!nearlyEqual(partial->overlapStartSeconds, 0.0005))
        return false;

    // block 起点晚于 playback 起点：左侧部分重叠，destination 起点 clamp 到 0、终点 ceil。
    const auto leftPartial = OpenTune::computeRegionBlockRenderSpan(1.0, 512, 48000.0, 0.5, 1.0005);
    if (!leftPartial)
        return false;
    if (leftPartial->destinationStartSample != 0)
        return false;
    if (leftPartial->samplesToCopy != 24)        // ceil((1.0005 - 1.0) * 48000)
        return false;

    return true;
}

// computeRegionBlockRenderSpan: 完全覆盖时 clamp 到整块；无重叠/仅边界相接返回 nullopt。
bool testComputeRegionBlockRenderSpanFullAndNoOverlap()
{
    // playback 完全覆盖 block：destination 覆盖整块 [0, blockSamples)。
    const auto full = OpenTune::computeRegionBlockRenderSpan(1.0, 512, 48000.0, 0.5, 2.0);
    if (!full)
        return false;
    if (full->destinationStartSample != 0 || full->samplesToCopy != 512)
        return false;
    if (!nearlyEqual(full->overlapStartSeconds, 1.0))
        return false;

    // 无重叠：playback 完全在 block 之前。
    if (OpenTune::computeRegionBlockRenderSpan(0.0005, 100, 44100.0, 0.0, 0.0004).has_value())
        return false;
    // 无重叠：playback 完全在 block 之后。
    if (OpenTune::computeRegionBlockRenderSpan(0.0, 100, 44100.0, 0.003, 0.004).has_value())
        return false;
    // playhead 与 playback 仅边界相接（零长度重叠）也为 nullopt。
    return !OpenTune::computeRegionBlockRenderSpan(1.0, 512, 48000.0, 0.5, 1.0).has_value();
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

} // namespace

int main()
{
    if (!testCommonFieldsRoundTrip())
    {
        std::fputs("FAIL: ContentState snapshot round-trip\n", stderr);
        return 1;
    }

    if (!testContentStateProjectionStartsNewRuntimeRevision())
    {
        std::fputs("FAIL: contentStateFromSnapshot new-ContentState projection revision\n", stderr);
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

    if (!testComputeRegionBlockRenderSpanPartialOverlap())
    {
        std::fputs("FAIL: computeRegionBlockRenderSpan partial overlap floor/ceil\n", stderr);
        return 1;
    }

    if (!testComputeRegionBlockRenderSpanFullAndNoOverlap())
    {
        std::fputs("FAIL: computeRegionBlockRenderSpan full/no overlap\n", stderr);
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

    std::puts("ContentSnapshotProjection tests passed.");
    return 0;
}
