#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Content/CaptureSegmentContent.h"
#include "../Source/Content/StandaloneClipContent.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Render/PlaybackReadSource.h"

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

    std::puts("ContentSnapshotProjection tests passed.");
    return 0;
}
