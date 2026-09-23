#include "PluginProcessor.h"
#if JucePlugin_Build_Standalone
#include "SourceStore.h"
#include "StandaloneArrangement.h"
#endif
#include "Editor/EditorFactory.h"
#include "DSP/ResamplingManager.h"
#include "DSP/MelSpectrogram.h"
#include "DSP/F0KeyDetector.h"
#include "Services/F0ExtractionService.h"
#include "Services/ImportedClipF0Extraction.h"
#include "Runtime/ProcessF0Runtime.h"
#include "Utils/ModelPathResolver.h"
#include "Utils/AppLogger.h"
#include "Utils/ChannelLayoutLogger.h"
#if JucePlugin_Build_VST3
#include "Plugin/Capture/CaptureSession.h"
#include "Plugin/Capture/CapturePersistence.h"
#include "Plugin/Vst3ProcessorStateCodec.h"
#else
#include "DSP/ReferenceAutoAlign.h"
#include "Plugin/StandaloneProcessorStateCodec.h"
#endif
#include "Utils/TimeCoordinate.h"
#include "Utils/PlacementFade.h"
#include "Inference/GameNoteGenerator.h"      // GAME NoteGeneratorInput/Note DTO（进程级 GAME 入口）
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/ScaleUiMapping.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Render/Stage2TimeStretchRebuilder.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif
#include "Content/ContentEditCommands.h"
#include "Content/ContentPatchGeometry.h"
#include "Content/ContentSnapshotProjection.h"

namespace OpenTune {

// ============================================================================
// Export Helper Functions (Anonymous Namespace)
// ============================================================================
namespace {

constexpr double kExportSampleRateHz = 44100.0;
constexpr int kExportNumChannels = 1;
constexpr int kExportMasterNumChannels = 2;
constexpr int kExportBitsPerSample = static_cast<int>(sizeof(float) * 8);

ReferenceFeatureSet makeReferenceFeatureSetFromNotes(
    ReferenceFeatureProducer producer,
    int64_t inputFingerprint,
    int analysisRevision,
    double sourceDurationSeconds,
    std::vector<Note> notes,
    const juce::String& emptyError)
{
    ReferenceFeatureSet result;
    result.producer = producer;
    result.inputFingerprint = inputFingerprint;
    result.analysisRevision = analysisRevision;
    result.sourceDurationSeconds = sourceDurationSeconds;
    result.pitch.notes = normalizeStoredNotes(std::move(notes));

    uint64_t nextAnchorId = 1;
    double lastAcceptedSourceSeconds = 0.0;
    for (const auto& note : result.pitch.notes) {
        const double sourceSeconds = juce::jlimit(0.0, sourceDurationSeconds, note.startTime);
        if (sourceSeconds <= 0.0 || sourceSeconds >= sourceDurationSeconds)
            continue;
        if (!result.timing.anchors.empty()
            && !TimeGridSnapshot::hasMinimumSourceSpacing(lastAcceptedSourceSeconds, sourceSeconds))
            continue;

        ReferenceTimingAnchor anchor;
        anchor.anchorId = nextAnchorId++;
        anchor.sourceSeconds = sourceSeconds;
        anchor.strength = 1.0f;
        anchor.kind = ReferenceTimingAnchorKind::Onset;
        anchor.confidence = 1.0f;
        result.timing.anchors.push_back(anchor);
        lastAcceptedSourceSeconds = sourceSeconds;
    }

    if (result.pitch.notes.empty() && result.timing.anchors.empty()) {
        result.status = ReferenceFeatureStatus::Failed;
        result.errorMessage = emptyError;
        return result;
    }

    result.status = ReferenceFeatureStatus::Ready;
    return result;
}

#if JucePlugin_Build_Standalone
void publishStandalonePlaybackSource(ContentRenderService& crs,
                                     ContentKey key,
                                     const ContentState& payload)
{
    auto snapshot = std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(payload));
    auto readSource = makePlaybackReadSource(
        key, std::move(snapshot), payload.audioBuffer, payload.sampleRate,
        crs.getOrCreateRenderCache(key), crs.getTimeStretchCache());
    crs.publishPlaybackSource(key, std::move(readSource));
}

ContentKey createStandaloneClipOwner(StandaloneContentRepository& repository,
                                     ContentRenderService& crs,
                                     ContentState payload,
                                     uint64_t forcedId = 0)
{
    const ContentKey key = repository.createClip(forcedId);
    auto* clip = key.isValid() ? repository.findClip(key) : nullptr;
    if (clip == nullptr) {
        return {};
    }

    // owner bootstrap：真实 source/audio duration 已知时，bootstrap identity 或
    // 时长不匹配的 grid（如 split 后沿用原 grid）统一覆盖为真实 duration 的 identity。
    const double contentDuration = payload.sourceWindow.isValid()
        ? payload.sourceWindow.durationSeconds()
        : static_cast<double>(payload.audioBuffer != nullptr ? payload.audioBuffer->getNumSamples() : 0) / payload.sampleRate;
    const bool gridMatchesContent =
        std::abs(payload.timeGrid->totalDurationSeconds() - contentDuration) <= 1e-6;
    if (!gridMatchesContent && contentDuration > 0.0) {
        if (auto realGrid = TimeGridSnapshot::makeIdentity(contentDuration)) {
            payload.timeGrid = std::move(realGrid);
            ++payload.timeGridRevision;
        }
    }

    clip->content() = std::move(payload);
    publishStandalonePlaybackSource(crs, key, clip->content());
    return key;
}

bool standaloneRepositoryReferencesSource(const StandaloneContentRepository& repository,
                                          uint64_t sourceId)
{
    if (sourceId == 0) {
        return false;
    }

    for (const auto key : repository.getAllClips()) {
        const auto* clip = repository.findClip(key);
        if (clip != nullptr && clip->content().sourceWindow.sourceId == sourceId) {
            return true;
        }
    }

    return false;
}
#endif // JucePlugin_Build_Standalone

double contentDurationSeconds(const EditableContentSnapshot& snap)
{
    if (snap.sourceWindow.isValid())
        return snap.sourceWindow.durationSeconds();

    if (snap.audioBuffer != nullptr
        && snap.audioBuffer->getNumSamples() > 0
        && snap.audioSampleRate > 0.0) {
        return static_cast<double>(snap.audioBuffer->getNumSamples()) / snap.audioSampleRate;
    }

    return 0.0;
}

std::optional<F0FrameRange> f0FrameRangeForSeconds(
    int hopSize,
    double sampleRate,
    int frameCount,
    double startSeconds,
    double endSeconds)
{
    if (endSeconds <= startSeconds
        || hopSize <= 0
        || sampleRate <= 0.0
        || frameCount <= 0) {
        return std::nullopt;
    }

    const F0Timeline timeline(hopSize, sampleRate, frameCount);
    const double timelineEndSeconds = timeline.timeAtFrame(frameCount);
    if (endSeconds <= 0.0 || startSeconds >= timelineEndSeconds)
        return std::nullopt;

    return timeline.rangeForTimes(startSeconds, endSeconds);
}

std::optional<F0FrameRange> f0FrameRangeForSeconds(
    const PitchCurveSnapshot& snapshot,
    double startSeconds,
    double endSeconds)
{
    return f0FrameRangeForSeconds(snapshot.getHopSize(),
                                  snapshot.getSampleRate(),
                                  static_cast<int>(snapshot.getOriginalF0().size()),
                                  startSeconds,
                                  endSeconds);
}

#if JucePlugin_Build_Standalone
juce::String diagnosticControlCallToString(OpenTuneAudioProcessor::DiagnosticControlCall controlCall)
{
    switch (controlCall) {
        case OpenTuneAudioProcessor::DiagnosticControlCall::Play: return "play";
        case OpenTuneAudioProcessor::DiagnosticControlCall::Pause: return "pause";
        case OpenTuneAudioProcessor::DiagnosticControlCall::Stop: return "stop";
        case OpenTuneAudioProcessor::DiagnosticControlCall::Seek: return "seek";
        case OpenTuneAudioProcessor::DiagnosticControlCall::None: break;
    }

    return "none";
}

bool findPlacementByIdGlobal(const StandaloneArrangement& arrangement,
                             uint64_t placementId,
                             int& outTrackId,
                             StandaloneArrangement::Placement& outPlacement)
{
    if (placementId == 0) {
        return false;
    }

    for (int trackId = 0; trackId < arrangement.getNumTracks(); ++trackId) {
        if (arrangement.getPlacementById(trackId, placementId, outPlacement)) {
            outTrackId = trackId;
            return true;
        }
    }

    return false;
}

std::shared_ptr<const juce::AudioBuffer<float>> sliceAudioBuffer(const std::shared_ptr<const juce::AudioBuffer<float>>& audioBuffer,
                                                                 int64_t startSample,
                                                                 int64_t endSampleExclusive)
{
    if (audioBuffer == nullptr || endSampleExclusive <= startSample) {
        return {};
    }

    const int64_t clampedStart = juce::jlimit<int64_t>(0, audioBuffer->getNumSamples(), startSample);
    const int64_t clampedEnd = juce::jlimit<int64_t>(clampedStart, audioBuffer->getNumSamples(), endSampleExclusive);
    if (clampedEnd <= clampedStart) {
        return {};
    }

    auto sliced = std::make_shared<juce::AudioBuffer<float>>(audioBuffer->getNumChannels(), static_cast<int>(clampedEnd - clampedStart));
    for (int channel = 0; channel < sliced->getNumChannels(); ++channel) {
        sliced->copyFrom(channel,
                         0,
                         *audioBuffer,
                         channel,
                         static_cast<int>(clampedStart),
                         sliced->getNumSamples());
    }
    return sliced;
}

std::vector<SilentGap> sliceSilentGaps(const std::vector<SilentGap>& silentGaps,
                                       int64_t startSample,
                                       int64_t endSampleExclusive)
{
    std::vector<SilentGap> slicedGaps;
    for (const auto& gap : silentGaps) {
        const int64_t overlapStart = std::max<int64_t>(gap.startSample, startSample);
        const int64_t overlapEnd = std::min<int64_t>(gap.endSampleExclusive, endSampleExclusive);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        SilentGap slicedGap;
        slicedGap.startSample = overlapStart - startSample;
        slicedGap.endSampleExclusive = overlapEnd - startSample;
        slicedGap.minLevel_dB = gap.minLevel_dB;
        slicedGaps.push_back(slicedGap);
    }
    return slicedGaps;
}

std::vector<Note> sliceNotesToLocalRange(const std::vector<Note>& notes,
                                         double startSeconds,
                                         double endSeconds)
{
    std::vector<Note> slicedNotes;
    if (endSeconds <= startSeconds) {
        return slicedNotes;
    }

    for (const auto& note : notes) {
        const double overlapStart = std::max(note.startTime, startSeconds);
        const double overlapEnd = std::min(note.endTime, endSeconds);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        Note slicedNote = note;
        slicedNote.startTime = overlapStart - startSeconds;
        slicedNote.endTime = overlapEnd - startSeconds;
        slicedNotes.push_back(slicedNote);
    }

    return normalizeStoredNotes(slicedNotes);
}

// 音量包络是 content-local 源秒上的分段线性 lane：截取 [startSeconds, endSeconds]
// 并整体平移到 child local 0。两端以父 lane 的插值值锚定，保证子区间 evalAt 与父一致。
AutomationLane sliceVolumeEnvelopeToLocalRange(const AutomationLane& envelope,
                                               double startSeconds,
                                               double endSeconds)
{
    if (envelope.empty() || endSeconds <= startSeconds) {
        return {};
    }

    std::vector<AutomationPoint> points;
    points.reserve(envelope.points().size() + 2);
    points.push_back({0.0, envelope.evalAt(startSeconds)});
    for (const auto& point : envelope.points()) {
        if (point.timeSeconds > startSeconds && point.timeSeconds < endSeconds) {
            points.push_back({point.timeSeconds - startSeconds, point.gainDb});
        }
    }
    points.push_back({endSeconds - startSeconds, envelope.evalAt(endSeconds)});
    return AutomationLane::fromSnapshot(points);
}

// frame index 版：调用方已持有精确 frame 区间时使用，避免 seconds 反算取整
// 造成边界帧同时落入相邻两个半开区间。
std::shared_ptr<PitchCurve> slicePitchCurveToFrameRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
    int startFrame,
    int endFrameExclusive)
{
    if (pitchCurve == nullptr || endFrameExclusive <= startFrame) {
        return nullptr;
    }

    const auto& snapshot = *pitchCurve;
    const int hopSize = snapshot.getHopSize();
    const double sampleRate = snapshot.getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0) {
        return nullptr;
    }

    const auto& originalF0 = snapshot.getOriginalF0();
    if (startFrame < 0 || endFrameExclusive > static_cast<int>(originalF0.size())) {
        return nullptr;
    }

    auto slicedCurve = std::make_shared<PitchCurve>();
    slicedCurve->setHopSize(hopSize);
    slicedCurve->setSampleRate(sampleRate);

    slicedCurve->setOriginalF0(std::vector<float>(originalF0.begin() + startFrame, originalF0.begin() + endFrameExclusive));

    const auto& originalEnergy = snapshot.getOriginalEnergy();
    if (originalEnergy.size() >= static_cast<size_t>(endFrameExclusive)) {
        slicedCurve->setOriginalEnergy(std::vector<float>(originalEnergy.begin() + startFrame, originalEnergy.begin() + endFrameExclusive));
    }

    std::vector<PitchCorrectionSegment> slicedSegments;
    for (const auto& segment : snapshot.getCorrectionSegments()) {
        const int overlapStart = std::max(segment.startFrame, startFrame);
        const int overlapEnd = std::min(segment.endFrame, endFrameExclusive);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        PitchCorrectionSegment slicedSegment = segment;
        const int originalOffsetStart = overlapStart - segment.startFrame;
        const int originalOffsetEnd = overlapEnd - segment.startFrame;
        slicedSegment.startFrame = overlapStart - startFrame;
        slicedSegment.endFrame = overlapEnd - startFrame;
        slicedSegment.f0Data.assign(segment.f0Data.begin() + originalOffsetStart,
                                    segment.f0Data.begin() + originalOffsetEnd);
        slicedSegments.push_back(std::move(slicedSegment));
    }
    slicedCurve->replaceCorrectionSegments(slicedSegments);
    return slicedCurve;
}

// seconds 版：按端点秒数求覆盖 frame range 后委托 frame index 版。
std::shared_ptr<PitchCurve> slicePitchCurveToLocalRange(
    const std::shared_ptr<const PitchCurveSnapshot>& pitchCurve,
                                                        double startSeconds,
                                                        double endSeconds)
{
    if (pitchCurve == nullptr || endSeconds <= startSeconds) {
        return nullptr;
    }

    const auto frameRange = f0FrameRangeForSeconds(*pitchCurve, startSeconds, endSeconds);
    if (!frameRange.has_value()) {
        return nullptr;
    }

    return slicePitchCurveToFrameRange(pitchCurve, frameRange->startFrame, frameRange->endFrameExclusive);
}

bool detectedKeysMatch(const DetectedKey& lhs, const DetectedKey& rhs)
{
    return lhs.root == rhs.root
        && lhs.scale == rhs.scale
        && lhs.origin == rhs.origin
        && std::abs(lhs.confidence - rhs.confidence) <= 1.0e-6f;
}

std::vector<SilentGap> mergeSilentGaps(const std::vector<SilentGap>& leadingGaps,
                                       const std::vector<SilentGap>& trailingGaps,
                                       int64_t trailingOffsetSamples)
{
    std::vector<SilentGap> mergedGaps = leadingGaps;
    mergedGaps.reserve(leadingGaps.size() + trailingGaps.size());
    for (const auto& gap : trailingGaps) {
        SilentGap mergedGap = gap;
        mergedGap.startSample += trailingOffsetSamples;
        mergedGap.endSampleExclusive += trailingOffsetSamples;
        mergedGaps.push_back(mergedGap);
    }
    return mergedGaps;
}

std::shared_ptr<PitchCurve> mergePitchCurves(
                                             const std::shared_ptr<const PitchCurveSnapshot>& leadingCurve,
                                             const std::shared_ptr<const PitchCurveSnapshot>& trailingCurve,
                                             OriginalF0State leadingState,
                                             OriginalF0State trailingState)
{
    if (leadingState != trailingState) {
        return nullptr;
    }

    if (leadingCurve == nullptr || trailingCurve == nullptr) {
        return (leadingCurve == nullptr && trailingCurve == nullptr) ? std::shared_ptr<PitchCurve>{} : nullptr;
    }

    const auto& leadingSnapshot = *leadingCurve;
    const auto& trailingSnapshot = *trailingCurve;
    if (leadingSnapshot.getHopSize() <= 0
        || leadingSnapshot.getHopSize() != trailingSnapshot.getHopSize()
        || std::abs(leadingSnapshot.getSampleRate() - trailingSnapshot.getSampleRate()) > 1.0e-6) {
        return nullptr;
    }

    std::vector<float> mergedOriginalF0 = leadingSnapshot.getOriginalF0();
    const auto& trailingOriginalF0 = trailingSnapshot.getOriginalF0();
    mergedOriginalF0.insert(mergedOriginalF0.end(), trailingOriginalF0.begin(), trailingOriginalF0.end());

    std::vector<float> mergedOriginalEnergy = leadingSnapshot.getOriginalEnergy();
    if (mergedOriginalEnergy.size() < leadingSnapshot.getOriginalF0().size()) {
        mergedOriginalEnergy.resize(leadingSnapshot.getOriginalF0().size(), 0.0f);
    }

    const auto& trailingOriginalEnergy = trailingSnapshot.getOriginalEnergy();
    if (trailingOriginalEnergy.size() < trailingOriginalF0.size()) {
        mergedOriginalEnergy.insert(mergedOriginalEnergy.end(),
                                    trailingOriginalF0.size() - trailingOriginalEnergy.size(),
                                    0.0f);
    } else {
        mergedOriginalEnergy.insert(mergedOriginalEnergy.end(),
                                    trailingOriginalEnergy.begin(),
                                    trailingOriginalEnergy.begin() + static_cast<std::ptrdiff_t>(trailingOriginalF0.size()));
    }

    std::vector<PitchCorrectionSegment> mergedSegments = leadingSnapshot.getCorrectionSegments();
    const int leadingFrameCount = static_cast<int>(leadingSnapshot.getOriginalF0().size());
    for (auto segment : trailingSnapshot.getCorrectionSegments()) {
        segment.startFrame += leadingFrameCount;
        segment.endFrame += leadingFrameCount;
        mergedSegments.push_back(std::move(segment));
    }

    auto mergedCurve = std::make_shared<PitchCurve>();
    mergedCurve->setHopSize(leadingSnapshot.getHopSize());
    mergedCurve->setSampleRate(leadingSnapshot.getSampleRate());
    mergedCurve->setOriginalF0(mergedOriginalF0);
    mergedCurve->setOriginalEnergy(mergedOriginalEnergy);
    mergedCurve->replaceCorrectionSegments(mergedSegments);
    return mergedCurve;
}
#endif // JucePlugin_Build_Standalone

} // anonymous namespace


#if JucePlugin_Build_Standalone
namespace {

void renderPlacementForExport(OpenTuneAudioProcessor& processor,
                              const StandaloneArrangement::PlaybackPlacement& placement,
                              float trackGain,
                              int64_t placementStartInOutput,
                              int64_t placementSampleCount,
                              double outputOriginSeconds,
                              juce::AudioBuffer<float>& out,
                              int64_t totalLen,
                              const PlaybackReadSource& source)
{
    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;

    if (!placement.contentKey.isValid() || placement.durationSeconds <= 0.0 || placementStartInOutput >= totalLen
        || !source.hasAudio()
        || source.contentSnapshot == nullptr) {
        return;
    }

    const int64_t requestedPlacementSamples = juce::jmax<int64_t>(1, placementSampleCount);
    const int64_t remainingOutputSamples = totalLen - placementStartInOutput;
    const int samplesToRender = static_cast<int>(juce::jmin<int64_t>(requestedPlacementSamples, remainingOutputSamples));
    if (samplesToRender <= 0) {
        return;
    }

    juce::AudioBuffer<float> placementBuffer(out.getNumChannels(), samplesToRender);
    placementBuffer.clear();

    ::OpenTune::PlaybackReadRequest readRequest;
    readRequest.source = source;
    readRequest.readStartSample = TimeCoordinate::secondsToSamples(
        source.contentSnapshot->timeGrid->tauForward(placement.clipInSeconds), kExportSr);
    readRequest.targetSampleRate = kExportSr;
    readRequest.numSamples = samplesToRender;

    const int renderedSamples = ::OpenTune::readExportPlaybackAudio(readRequest, placementBuffer, 0);
    if (renderedSamples <= 0) {
        return;
    }

    const float baseGain = trackGain * placement.gain;
    for (int sampleIndex = 0; sampleIndex < renderedSamples; ++sampleIndex) {
        const int64_t dstIndex = placementStartInOutput + sampleIndex;
        if (dstIndex < 0 || dstIndex >= totalLen)
            continue;

        const float fade = placementFadeGain(
            outputOriginSeconds
                + static_cast<double>(dstIndex) / kExportSr
                - placement.timelineStartSeconds,
            placement.durationSeconds,
            placement.fadeInDuration,
            placement.fadeOutDuration);
        const float finalGain = baseGain * fade;
        for (int ch = 0; ch < out.getNumChannels(); ++ch) {
            const float* src = placementBuffer.getReadPointer(ch);
            float* dst = out.getWritePointer(ch);
            dst[static_cast<size_t>(dstIndex)] += src[sampleIndex] * finalGain;
        }
    }
}

// copyContentRange 的 TimeGrid 截断：与 audio slicing 使用同一
// [sourceStartSeconds, sourceStartSeconds + newDurationSeconds] 区间。
// 端点合同保持不变：ClipStart=(0,0)、ClipEnd=(newDuration,newDuration)；
// 区间内原 handle 平移 source，output 用区间 tauForward 跨度归一化到新 clip 轴。
// identity 源直接给新 duration 的 identity；构造失败返回 nullptr，由调用方保留
// owner bootstrap identity。
std::shared_ptr<const TimeGridSnapshot> buildCopiedRangeTimeGrid(
    const TimeGridSnapshot& sourceGrid,
    double sourceStartSeconds,
    double newDurationSeconds)
{
    if (!std::isfinite(sourceStartSeconds) || !std::isfinite(newDurationSeconds)
        || newDurationSeconds <= 0.0) {
        return nullptr;
    }

    if (sourceGrid.isIdentity())
        return TimeGridSnapshot::makeIdentity(newDurationSeconds);

    const double outputStart = sourceGrid.tauForward(sourceStartSeconds);
    const double outputEnd = sourceGrid.tauForward(sourceStartSeconds + newDurationSeconds);
    if (!(outputEnd > outputStart))
        return nullptr;

    const double sourceEndSeconds = sourceStartSeconds + newDurationSeconds;
    const double outputScale = newDurationSeconds / (outputEnd - outputStart);

    std::vector<TimeHandle> newHandles;
    newHandles.reserve(sourceGrid.handles().size());

    TimeHandle clipStart;
    clipStart.id = 0;
    clipStart.source_seconds = 0.0;
    clipStart.output_seconds = 0.0;
    clipStart.kind = HandleKind::ClipStart;
    newHandles.push_back(clipStart);

    for (const auto& handle : sourceGrid.handles()) {
        if (handle.isEndpoint()
            || handle.source_seconds <= sourceStartSeconds
            || handle.source_seconds >= sourceEndSeconds) {
            continue;
        }

        TimeHandle internal = handle;
        internal.source_seconds = handle.source_seconds - sourceStartSeconds;
        internal.output_seconds =
            (sourceGrid.tauForward(handle.source_seconds) - outputStart) * outputScale;
        newHandles.push_back(std::move(internal));
    }

    TimeHandle clipEnd;
    clipEnd.id = 0;
    clipEnd.source_seconds = newDurationSeconds;
    clipEnd.output_seconds = newDurationSeconds;
    clipEnd.kind = HandleKind::ClipEnd;
    newHandles.push_back(clipEnd);

    return TimeGridSnapshot::makeFromHandles(std::move(newHandles));
}

} // anonymous namespace
#endif // JucePlugin_Build_Standalone

// --- Serialization helpers ---
// Each SharedCode target is single-format: the Standalone target owns the OTSS
// settings payload (encoded/decoded by StandaloneProcessorStateCodec), the VST3
// target owns the OTST processor state payload (encoded/decoded by
// Vst3ProcessorStateCodec).

ReferenceFeatureProducer OpenTuneAudioProcessor::resolveReferenceFeatureProducer() const
{
    if (experimentalReferenceAlignMode_ == ExperimentalReferenceAlignMode::StandardAuto)
        return ReferenceFeatureProducer::StandardAuto;

    // GAME 后端只由环境变量与 GAME 模型文件存在性决定：GAME 生成器是进程级
    // 共享单例（ProcessF0Runtime），不存在 per-instance generator 可探测。
    const auto envBackend = juce::SystemStats::getEnvironmentVariable("OPENTUNE_NOTE_BACKEND", {})
                                .trim()
                                .toLowerCase();
    if (envBackend == "legacy")
        return ReferenceFeatureProducer::StandardAuto;

    const auto gameDir = juce::File(juce::String(ModelPathResolver::getModelsDirectory()))
                             .getChildFile("GAME");
    return gameDir.getChildFile("encoder.onnx").existsAsFile()
        ? ReferenceFeatureProducer::Game
        : ReferenceFeatureProducer::StandardAuto;
}

ReferenceFeatureSet OpenTuneAudioProcessor::buildStandardAutoReferenceFeatureSet(
    const EditableContentSnapshot& snapshot, int analysisRevision)
{
    ReferenceFeatureSet failed;
    failed.status = ReferenceFeatureStatus::Failed;
    failed.producer = ReferenceFeatureProducer::StandardAuto;
    failed.inputFingerprint = static_cast<int64_t>(snapshot.audioRevision);
    failed.analysisRevision = analysisRevision;

    if (snapshot.originalF0State != OriginalF0State::Ready || snapshot.pitchCurve == nullptr) {
        failed.errorMessage = "AUTO Ref standard analysis requires Original F0";
        return failed;
    }

    const auto pitchSnapshot = snapshot.pitchCurve;
    const auto& originalF0 = pitchSnapshot->getOriginalF0();
    const auto& originalEnergy = pitchSnapshot->getOriginalEnergy();
    const int hopSize = pitchSnapshot->getHopSize();
    const double sampleRate = pitchSnapshot->getSampleRate();
    if (originalF0.empty() || hopSize <= 0 || sampleRate <= 0.0) {
        failed.errorMessage = "AUTO Ref standard analysis requires valid Original F0";
        return failed;
    }

    NoteGeneratorParams params;
    auto notes = LegacyNoteGenerator::generate(
        originalF0.data(),
        static_cast<int>(originalF0.size()),
        originalEnergy.size() == originalF0.size() ? originalEnergy.data() : nullptr,
        0,
        static_cast<int>(originalF0.size()),
        hopSize,
        sampleRate,
        params);
    LegacyNoteGenerator::validate(notes);

    const double sourceDurationSeconds = static_cast<double>(originalF0.size())
        * static_cast<double>(hopSize) / sampleRate;
    return makeReferenceFeatureSetFromNotes(
        ReferenceFeatureProducer::StandardAuto,
        static_cast<int64_t>(snapshot.audioRevision),
        analysisRevision,
        sourceDurationSeconds,
        std::move(notes),
        "AUTO Ref standard analysis found no notes or timing anchors");
}

void OpenTuneAudioProcessor::analysisFinished(
    ContentKey key,
    const ReferenceFeatureSet& result)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;

    if (result.inputFingerprint != static_cast<int64_t>(snap->audioRevision)
        || result.producer != resolveReferenceFeatureProducer()) {
        return;
    }

    setContentReferenceFeatures(key, result);

    const auto pendingIt = pendingTimeToolSeedKeys_.find(key);
    if (pendingIt == pendingTimeToolSeedKeys_.end())
        return;

    pendingTimeToolSeedKeys_.erase(pendingIt);
    if (result.isReady())
        ensureTimeToolAnchorSeed(key);
}

juce::AudioProcessor::BusesProperties OpenTuneAudioProcessor::makeBuses()
{
#if JucePlugin_Build_Standalone
    return BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true);
#else
    return BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true);
#endif
}

OpenTuneAudioProcessor::OpenTuneAudioProcessor()
    : AudioProcessor(makeBuses()) {
    // Constructor stays scan-safe: no thread, Timer, file lock, logger, or
    // process runtime attachment. All of that is deferred to
    // initializeRuntimeState(), called from prepareToPlay / createEditor /
    // didBindToARA. setStateInformation must NOT trigger runtime init (scanner
    // may do a state round-trip) -- it caches the payload instead.
    editVersionParam_ = new juce::AudioParameterInt("editVersion", "EditVersion", 0, 100000, 0);
    addParameter(editVersionParam_);

    // Pure in-memory state is safe to expose before runtime initialization.
#if JucePlugin_Build_Standalone
    standaloneArrangement_ = std::make_unique<StandaloneArrangement>();
#endif
}

bool OpenTuneAudioProcessor::initializeRuntimeState() noexcept
{
    if (runtimeStateInitialized_.load(std::memory_order_acquire))
        return true;

    try {
        std::call_once(runtimeInitOnce_, [this]() { initializeRuntimeStateOnce(); });
    }
    catch (const std::exception& e) {
        AppLogger::emergencyError("initializeRuntimeState failed (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)) + "): " + juce::String(e.what()));
    }
    catch (...) {
        AppLogger::emergencyError("initializeRuntimeState failed with unknown exception (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)) + ")");
    }
    return runtimeStateInitialized_.load(std::memory_order_acquire);
}

void OpenTuneAudioProcessor::initializeRuntimeStateOnce()
{
    AppLogger::initialize();
    AppLogger::log("OpenTuneAudioProcessor: initializeRuntimeState version=" + juce::String(OPENTUNE_VERSION)
        + " wrapper=" + juce::String(juce::AudioProcessor::getWrapperTypeDescription(wrapperType))
        + " araCompiled="
#if JucePlugin_Enable_ARA
        + "true"
#else
        + "false"
#endif
        + " processor=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)));

    // Construct services that used to be value members. Their constructors
    // start detached worker threads -- must not run during scan-time createInstance.
    auto f0SvcOwner = std::make_unique<F0ExtractionService>(
        1, 64, [] { return ProcessF0Runtime::getInstance().getF0Service(); });
    auto refSvc = std::make_unique<ReferenceAnalysisService>();

    // 资源所有权：ARA 绑定实例的渲染全部走 DC 自己的 CRS（DC 构造时安装自己的
    // ExecutionLease），processor 不得再持有 local CRS / CaptureSession，避免出现
    // 第二个不可达的渲染所有者。未绑定的普通 VST3 / Standalone 实例行为不变。
#if JucePlugin_Enable_ARA
    const bool araBound = isBoundToARA();
#else
    const bool araBound = false;
#endif

    std::shared_ptr<ContentRenderService> crs;
#if JucePlugin_Build_Standalone
    auto srcStore = std::make_shared<SourceStore>();
    auto repo = std::make_unique<StandaloneContentRepository>();
#endif
    // Bind processor's render callback to CRS via ExecutionLease (ARA 重构路由改制)
    if (!araBound)
    {
        crs = std::make_shared<ContentRenderService>();
        ContentRenderService::ExecutionLease lease;
        lease.owner = this;
        lease.renderJobCallback = [this](RenderJob& job) {
            if (job.kind == RenderJob::Kind::Stage2Rebuild) {
                if (job.contentKey.domainKind == DomainKind::ARAAudioModification
                    || !job.contentSnapshot || !job.audioBuffer)
                    return;

                Stage2TimeStretchRebuilder::Request request;
                request.contentKey = job.contentKey;
                request.contentSnapshot = job.contentSnapshot;
                request.audioBuffer = job.audioBuffer;
                request.audioSampleRate = job.audioSampleRate;
                Stage2TimeStretchRebuilder::rebuild(*contentRenderService_, request);
                return;
            }

            if (job.renderCache == nullptr)
                return;

            auto contentSnap = job.contentSnapshot;
            if (!contentSnap) {
                if (job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
                {
#if JucePlugin_Build_VST3
                    auto gate = completionGate_;
                    const auto failedKey = job.contentKey;
                    juce::MessageManager::callAsync(
                        [this, gate, failedKey]()
                        {
                            std::lock_guard<std::mutex> lock(gate->mutex);
                            if (gate->closed)
                                return;
                            if (auto* session = getCaptureSession())
                                session->onRenderFailed(failedKey);
                        });
#endif
                }
                return;
            }
            ProcessRenderRuntime::CompletionContext completion;
            completion.gate = completionGate_;
            const auto completionGate = completionGate_;
            completion.chunkSettled = [this, completionGate](ContentKey key,
                                             std::shared_ptr<const EditableContentSnapshot> snapshot,
                                             std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                             double audioSampleRate) {
 #if JucePlugin_Build_VST3
                juce::MessageManager::callAsync(
                    [this, completionGate, key,
                     snapshot = std::move(snapshot),
                     audioBuffer = std::move(audioBuffer),
                     audioSampleRate]() mutable
                    {
                        std::lock_guard<std::mutex> lock(completionGate->mutex);
                        if (completionGate->closed)
                            return;
                        handleStage1ChunkSettled(key, std::move(snapshot),
                                                 std::move(audioBuffer), audioSampleRate);
                    });
 #else
                juce::ignoreUnused(completionGate);
                handleStage1ChunkSettled(key, std::move(snapshot),
                                         std::move(audioBuffer), audioSampleRate);
 #endif
            };
            completion.chunkFailed = [this, completionGate](ContentKey key) {
#if JucePlugin_Build_VST3
                juce::MessageManager::callAsync(
                    [this, completionGate, key]()
                    {
                        std::lock_guard<std::mutex> lock(completionGate->mutex);
                        if (completionGate->closed)
                            return;
                        if (auto* session = getCaptureSession())
                            session->onRenderFailed(key);
                    });
#else
                juce::ignoreUnused(key);
#endif
            };
            const bool lightPitchEnabled = appPreferences_ != nullptr
                && appPreferences_->getState().shared.lightPitchCorrectionEnabled;
            ProcessRenderRuntime::getInstance().processChunkRenderJob(
                contentRenderService_, job,
                lightPitchEnabled, std::move(completion));
        };
        crs->attachExecutionLease(std::move(lease));
    }
    class ProcessorContentCommands final : public ContentEditCommands
    {
    public:
        explicit ProcessorContentCommands(OpenTuneAudioProcessor& proc) noexcept : proc_(proc) {}

        bool setDetectedKey(ContentKey key, const DetectedKey& detectedKey) override
        {
            return proc_.setContentDetectedKey(key, detectedKey);
        }

        bool applyPitchShiftState(ContentKey key, const PitchShiftEditState& state) override
        {
            return proc_.applyContentPitchShiftState(key, state);
        }

        std::unique_ptr<PitchShiftEditAction> commitPitchShiftEdit(
            ContentKey key,
            const PitchShiftSettings& newSettings) override
        {
            return proc_.commitPitchShiftEdit(key, newSettings);
        }

        bool autoTuneContentRange(ContentKey key,
                                   int startFrame, int endFrameExclusive,
                                   const NoteGeneratorParams& params,
                                   const std::optional<ScaleSnapConfig>& scaleSnap) override
        {
            return proc_.autoTuneContentRangeByContentKey(
                key, startFrame, endFrameExclusive, params, scaleSnap);
        }

        bool generateNotesOnly(ContentKey key, const NoteGeneratorParams& params) override
        {
            return proc_.generateNotesOnlyByContentKey(key, params);
        }

        bool ensureTimeToolAnchorSeed(ContentKey key) override
        {
            return proc_.ensureTimeToolAnchorSeed(key);
        }

        bool replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes) override
        {
            return proc_.replaceContentNotesForFullMutation(key, std::move(notes));
        }

        ContentCommitSnapshot commitNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch) override
        {
            return proc_.commitContentNoteTopologyPatch(key, std::move(patch));
        }

        ContentCommitSnapshot commitVolumeEnvelope(ContentKey key, AutomationLane envelope) override
        {
            return proc_.commitVolumeEnvelope(key, std::move(envelope));
        }

        void republishPlaybackSource(ContentKey key) override
        {
            proc_.republishPlaybackSource(key);
        }

        ContentCommitSnapshot commitNotesAndSegments(ContentKey key,
                                     std::vector<Note> notes,
                                     std::vector<PitchCorrectionSegment> segments,
                                     ContentEditRangeFrames affectedRange) override
        {
            return proc_.commitContentNotesAndSegments(key, std::move(notes), std::move(segments), affectedRange);
        }

        bool setTimeGrid(ContentKey key,
                          std::shared_ptr<const TimeGridSnapshot> grid) override
        {
            return proc_.setContentTimeGrid(key, std::move(grid));
        }

    private:
        OpenTuneAudioProcessor& proc_;
    };
    auto commands = std::make_shared<ProcessorContentCommands>(*this);
    auto resampler = std::make_shared<ResamplingManager>();

#if JucePlugin_Build_VST3
    // Capture session（仅 regular VST3 / ARA 未绑定实例）：先局部构造，attach 成功后发布。
    // ARA 绑定实例的采集由 DC 的 ARA 对象模型承担，不创建 CaptureSession，也不启动 tick timer。
    std::unique_ptr<Capture::CaptureSession> capture;
    if (!araBound)
    {
        Capture::ProcessorBindings bindings;

        bindings.replaceWithRendered = [this](juce::AudioBuffer<float>& buffer,
                                               int destStart, int numSamples,
                                               ContentKey segmentContentKey,
                                               int64_t readStartSample,
                                               double targetSampleRate) {
            // Capture segment contentKey is the CRS content key.
            jassert(contentRenderService_ != nullptr);

            PlaybackReadSource readSource;
            if (!contentRenderService_->getPlaybackReadSource(segmentContentKey, readSource)
                || !readSource.hasAudio()) {
                return;
            }
            ::OpenTune::PlaybackReadRequest req;
            req.source = readSource;
            req.readStartSample = readStartSample;
            req.targetSampleRate = targetSampleRate;
            req.numSamples = numSamples;
            readPlaybackAudio(req, buffer, destStart);
        };

        bindings.retireSegment = [this](ContentKey segmentContentKey) {
            // CaptureSegmentContent owns content; CRS cache is derived.
            if (contentRenderService_) {
                contentRenderService_->removeRenderCache(segmentContentKey);
            }
        };

        bindings.refreshSegment = [this](ContentKey segmentContentKey) {
            if (auto* captureSession = getCaptureSession()) {
                auto* segment = captureSession->findSegmentByContentKey(segmentContentKey);
                if (!segment || !segment->content)
                    return;
                const double duration = segment->durationSeconds;
                if (duration <= 0.0)
                    return;

                const auto snap = segment->content->snapshotContent();
                auto audio = snap->audioBuffer;
                double sr = snap->audioSampleRate > 0.0
                    ? snap->audioSampleRate : segment->captureSampleRate;
                ContentKey segContentKey = segment->contentKey;
                auto gate = completionGate_;

                f0ExtractionService_->submit(
                    F0RequestKey{segContentKey},
                    [audio, sr, segContentKey](const std::shared_ptr<F0RunOwnerState>& runOwnerState) -> F0ExtractionService::Result {
                        F0ExtractionService::Result result;
                        result.contentKey = segContentKey;
                        if (!audio || audio->getNumSamples() == 0) {
                            result.errorMessage = "no_audio_data";
                            return result;
                        }
                        // Lazy initialize F0 runtime and resolve service inside worker.
                        auto& f0Runtime = ProcessF0Runtime::getInstance();
                        if (!f0Runtime.isReady())
                            f0Runtime.initialize(ModelPathResolver::getModelsDirectory());
                        auto f0Svc = f0Runtime.getF0Service();
                        if (!f0Svc) {
                            result.errorMessage = "no_f0_service";
                            return result;
                        }
                        const float* src = audio->getReadPointer(0);
                        const int numSamples = audio->getNumSamples();
                        auto extraction = f0Svc->extractF0(
                            src, static_cast<size_t>(numSamples),
                            static_cast<int>(sr), runOwnerState);
                        if (!extraction.ok() || extraction.value().empty()) {
                            result.errorMessage = "f0_empty_or_unvoiced";
                            return result;
                        }
                        result.f0 = extraction.value();
                        result.hopSize = f0Svc->getF0HopSize();
                        result.f0SampleRate = f0Svc->getF0SampleRate();
                        result.energy = computeFrameEnergy(
                            src, numSamples, static_cast<int>(sr),
                            result.f0, result.f0SampleRate, result.hopSize);
                        result.success = true;
                        return result;
                    },
                    [this, gate, segContentKey](F0ExtractionService::Result&& result) {
                        // 持锁访问 owner：与析构置 closed 互斥。closed 后不再访问 this。
                        std::lock_guard<std::mutex> lk(gate->mutex);
                        if (gate->closed)
                            return;
                        if (auto* session = getCaptureSession()) {
                            if (!result.success) {
                                session->commitSegmentF0Result(
                                    segContentKey, nullptr,
                                    OriginalF0State::Failed);
                                return;
                            }
                            auto pitchCurve = std::make_shared<PitchCurve>();
                            pitchCurve->setHopSize(result.hopSize);
                            pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
                            pitchCurve->setOriginalF0(result.f0);
                            if (!result.energy.empty())
                                pitchCurve->setOriginalEnergy(result.energy);
                            if (session->commitSegmentF0Result(
                                    segContentKey, std::move(pitchCurve),
                                    OriginalF0State::Ready)) {
                                updateContentKeyFromOriginalF0(segContentKey);
                                requestFullContentRender(segContentKey);
                            }
                            if (pendingTimeToolSeedKeys_.count(segContentKey) != 0)
                                ensureTimeToolAnchorSeed(segContentKey);
                        }
                    }
                );
            }
        };

        // Capture 播放源的唯一汇聚点：CaptureSegmentContent 保留宿主率原始 PCM
        // （不回写 owner），此处派生 CRS canonical 播放源，统一固定 44.1kHz。
        // 44.1k 输入直接共享原 audio buffer；非同率经既有 upsampleForHost 重采样，
        // 时长守恒方式与 prepareImport/ARA 完全一致
        // （sampleRateProject(originalLen, sampleRate, targetRate)）。
        bindings.publishPlaybackSource = [this](const ContentKey& key,
                                                 std::shared_ptr<const EditableContentSnapshot> snapshot,
                                                 std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                 double sampleRate) {
            jassert(contentRenderService_ != nullptr);
            if (audio == nullptr || snapshot == nullptr || !key.isValid())
                return;

            const double targetRate = TimeCoordinate::kRenderSampleRate;
            std::shared_ptr<const juce::AudioBuffer<float>> canonicalAudio = std::move(audio);
            if (std::abs(sampleRate - targetRate) > 1.0) {
                const int numChannels = canonicalAudio->getNumChannels();
                const int originalLen = canonicalAudio->getNumSamples();
                const int newLen = juce::jmax(
                    1,
                    static_cast<int>(TimeCoordinate::sampleRateProject(
                        originalLen, sampleRate, targetRate)));
                auto derived = std::make_shared<juce::AudioBuffer<float>>(numChannels, newLen);
                for (int ch = 0; ch < numChannels; ++ch) {
                    auto resampledData = resamplingManager_->upsampleForHost(
                        canonicalAudio->getReadPointer(ch),
                        originalLen,
                        static_cast<int>(sampleRate),
                        static_cast<int>(targetRate));
                    const int toCopy = juce::jmin(newLen, static_cast<int>(resampledData.size()));
                    derived->copyFrom(ch, 0, resampledData.data(), toCopy);
                }
                canonicalAudio = std::move(derived);
            }

            auto readSource = makePlaybackReadSource(
                key, std::move(snapshot), std::move(canonicalAudio), targetRate,
                contentRenderService_->getOrCreateRenderCache(key),
                contentRenderService_->getTimeStretchCache());
            contentRenderService_->publishPlaybackSource(key, readSource);
        };

        bindings.requestFullRender = [this](ContentKey key) {
            requestFullContentRender(key);
        };

        capture = std::make_unique<Capture::CaptureSession>(std::move(bindings));

        // Log before publishing members so a throwing logger cannot leave partial state.
        AppLogger::log("OpenTuneAudioProcessor: regular VST3 capture session created processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)));
    }
#endif // JucePlugin_Build_VST3

    // 完整成功后一次性发布成员（noexcept 移动）。
    f0ExtractionService_ = std::move(f0SvcOwner);
    referenceAnalysisService_ = std::move(refSvc);
#if JucePlugin_Build_Standalone
    sourceStore_ = std::move(srcStore);
#endif
    contentRenderService_ = std::move(crs);
#if JucePlugin_Build_Standalone
    standaloneContentRepository_ = std::move(repo);
#endif
    contentCommands_ = std::move(commands);
    resamplingManager_ = std::move(resampler);
#if JucePlugin_Build_VST3
    if (capture)
    {
        captureSession_ = std::move(capture);
        startTimerHz(30);
    }
#endif

    runtimeStateInitialized_.store(true, std::memory_order_release);
}

OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
    if (!runtimeStateInitialized_.load(std::memory_order_acquire))
        return;

    AppLogger::log("OpenTuneAudioProcessor: dtor processor="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
#if JucePlugin_Enable_ARA
        + " araBound=" + juce::String(isBoundToARA() ? "true" : "false")
#else
        + " araBound=false"
#endif
    );

    // Stop the tick timer first — no more timerCallback after this point.
#if JucePlugin_Build_VST3
    stopTimer();
#endif

    // 析构最前段生命周期动作（此后再无异步任务访问裸 this）：
    // 1) 关闭 completion gate：与持锁进入的 F0 commit / chunkSettled / 模型切换
    //    completion 回调互斥。已进入者完成后才置 closed；此后进入者持锁见
    //    closed 即返回，不访问 owner。
    {
        std::lock_guard<std::mutex> lk(completionGate_->mutex);
        completionGate_->closed = true;
    }

    // 2) 仅当运行时已初始化时才关闭 F0 / Reference owner 服务、解除 CRS execution
    //    lease；未初始化（scanner-only 生命周期）时不得构造进程 runtime 单例 —
    //    getInstance() 会启动 control worker。
    // F0 / Reference shutdown 只关闭 owner、丢弃排队任务、终止本 owner 的
    // 活跃 F0 Run（SetTerminate 加速返回）；不 join worker —— worker 是
    // detached 进程常驻执行器，见 shutdownStarted_ 后自行退出，期间只访问
    // 进程寿命服务与提交时捕获的纯数据。不等待推理。
    f0ExtractionService_->shutdown();
    referenceAnalysisService_->shutdown();

    // Phase 2: 解除 CRS execution lease，取消 pending render jobs
    if (contentRenderService_) {
        contentRenderService_->detachExecutionLease(this);
    }

    // Phase 4: 内部清理
#if JucePlugin_Build_Standalone
    cancelPendingUpdate();
#endif

    // Vocoder / F0 / GAME / AppLogger 全部是进程级资源（ProcessRenderRuntime /
    // ProcessF0Runtime 单例与进程寿命 logger）：实例析构不 shutdown、不 reset、
    // 不等待推理，后续实例直接复用。
}

#if JucePlugin_Build_Standalone
OpenTuneAudioProcessor::AutoRefAvailability
OpenTuneAudioProcessor::queryAutoRefAvailability(uint64_t targetPlacementId) const
{
    AutoRefAvailability availability;
    availability.targetPlacementId = targetPlacementId;

    if (targetPlacementId == 0 || standaloneArrangement_ == nullptr) {
        availability.status = AutoRefAvailability::Status::InvalidSelection;
        availability.message = juce::String("No usable clip is selected; using regular AUTO.");
        return availability;
    }

    StandaloneArrangement::Placement targetPlacement;
    int targetTrackId = -1;
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        if (getPlacementById(trackId, targetPlacementId, targetPlacement)) {
            targetTrackId = trackId;
            break;
        }
    }

    if (targetTrackId < 0) {
        availability.status = AutoRefAvailability::Status::InvalidSelection;
        availability.message = juce::String("No usable clip is selected; using regular AUTO.");
        return availability;
    }

    availability.referencePlacementId =
        standaloneArrangement_->getPlacementReferencePlacement(targetTrackId, targetPlacementId);
    if (availability.referencePlacementId == 0) {
        availability.status = AutoRefAvailability::Status::NoReference;
        availability.message = juce::String("The selected clip has no reference clip bound; using regular AUTO.");
        return availability;
    }

    if (availability.referencePlacementId == targetPlacementId) {
        availability.status = AutoRefAvailability::Status::InvalidSelection;
        availability.message = juce::String("The reference clip binding is invalid; using regular AUTO.");
        return availability;
    }

    StandaloneArrangement::Placement referencePlacement;
    bool referenceFound = false;
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        if (getPlacementById(trackId, availability.referencePlacementId, referencePlacement)) {
            referenceFound = true;
            break;
        }
    }
    if (!referenceFound) {
        availability.status = AutoRefAvailability::Status::InvalidSelection;
        availability.message = juce::String("The reference clip is unavailable; using regular AUTO.");
        return availability;
    }

    availability.status = AutoRefAvailability::Status::Ready;
    availability.message = resolveReferenceFeatureProducer() == ReferenceFeatureProducer::Game
        ? juce::String("AUTO(Ref) will analyze the reference clip with GAME.")
        : juce::String("AUTO(Ref) will analyze the reference clip with standard AUTO.");
    return availability;
}
#endif // JucePlugin_Build_Standalone

void OpenTuneAudioProcessor::resetInferenceBackend(bool forceCpu, std::function<void()> beforeResume)
{
    AppLogger::info("[Processor] Resetting inference backend, forceCpu=" 
        + juce::String(forceCpu ? "true" : "false"));
    
    // 暂停 render worker（本 owner 的 CRS）
    if (contentRenderService_)
        contentRenderService_->pauseRenderWorker();

    // 耗时 reset（Session 销毁、按当前配置重建、AccelerationDetector
    //    resetAndDetect）全部在 ProcessRenderRuntime 的进程寿命 control worker 上
    //    串行执行；UI 线程只投递命令并立即返回。completion 经
    //    MessageManager::callAsync 回消息线程；gate 已关闭时不访问 processor。
    auto gate = completionGate_;
    auto* processor = this;
    ProcessRenderRuntime::getInstance().resetInferenceBackend(forceCpu,
        [processor, gate, beforeResume = std::move(beforeResume)]() mutable {
        std::lock_guard<std::mutex> lk(gate->mutex);
        if (gate->closed)
            return;   // owner 已析构：不访问 processor
        if (beforeResume)
            beforeResume();
        if (processor->contentRenderService_)
            processor->contentRenderService_->resumeRenderWorker();
        AppLogger::info("[Processor] Inference backend reset complete");
        });
}

void OpenTuneAudioProcessor::setVocoderModelWeight(const VocoderModelWeight& weight)
{
    // 1. 暂停 render worker（本 owner 的 CRS）
    if (contentRenderService_)
        contentRenderService_->pauseRenderWorker();

    // 2. 模型切换（严格先销毁旧 Domain/Session，再按当前配置重建）在进程寿命
    //    control worker 上串行执行；UI 线程只投递命令并立即返回。completion 经
    //    MessageManager::callAsync 回消息线程；gate 已关闭时不访问 processor。
    auto gate = completionGate_;
    auto* processor = this;
    ProcessRenderRuntime::getInstance().setVocoderModelWeight(weight, [processor, gate]() {
        std::lock_guard<std::mutex> lk(gate->mutex);
        if (gate->closed)
            return;   // owner 已析构：不访问 processor
        // 3. 模型切换完成后才清 RenderCache/TimeStretchCache 并恢复 render worker
#if JucePlugin_Build_Standalone
        if (processor->contentRenderService_ && processor->standaloneContentRepository_) {
            const auto keys = processor->standaloneContentRepository_->getAllClips();
            for (const auto key : keys) {
                if (auto cache = processor->contentRenderService_->getRenderCache(key))
                    cache->clear();
                processor->requestFullContentRender(key);
            }
            processor->contentRenderService_->getTimeStretchCache().clear();
        }
#endif
#if JucePlugin_Enable_ARA
        if (auto* dc = processor->getDocumentController())
            dc->invalidateAllModificationCaches();
#endif
        if (processor->contentRenderService_)
            processor->contentRenderService_->resumeRenderWorker();
    });
}

void OpenTuneAudioProcessor::invalidateAllContentCaches()
{
#if JucePlugin_Build_Standalone
    if (contentRenderService_ && standaloneContentRepository_) {
        const auto keys = standaloneContentRepository_->getAllClips();
        for (const auto key : keys) {
            if (auto cache = contentRenderService_->getRenderCache(key))
                cache->clear();
            requestFullContentRender(key);
        }
        contentRenderService_->getTimeStretchCache().clear();
    }
#endif
#if JucePlugin_Enable_ARA
    if (auto* dc = getDocumentController())
        dc->invalidateAllModificationCaches();
#endif
}

bool OpenTuneAudioProcessor::setF0ModelType(F0ModelType type)
{
    auto& f0Runtime = ProcessF0Runtime::getInstance();
    if (!f0Runtime.isReady()
        && !f0Runtime.initialize(ModelPathResolver::getModelsDirectory(), type))
        return false;

    auto f0Svc = ProcessF0Runtime::getInstance().getF0Service();
    if (!f0Svc) {
        AppLogger::warn("[Processor] F0 service not ready, cannot switch model");
        return false;
    }
    if (!f0Svc->setF0Model(type)) {
        AppLogger::error("[Processor] Failed to switch F0 model");
        return false;
    }
    return true;
}

// ============================================================================
// JUCE AudioProcessor 标准接口
// ============================================================================

const juce::String OpenTuneAudioProcessor::getName() const {
    return JucePlugin_Name;
}

bool OpenTuneAudioProcessor::acceptsMidi() const {
    return false;
}

bool OpenTuneAudioProcessor::producesMidi() const {
    return false;
}

bool OpenTuneAudioProcessor::isMidiEffect() const {
    return false;
}

double OpenTuneAudioProcessor::getTailLengthSeconds() const {
    return 0.0;
}

int OpenTuneAudioProcessor::getNumPrograms() {
    return 1;
}

int OpenTuneAudioProcessor::getCurrentProgram() {
    return 0;
}

void OpenTuneAudioProcessor::setCurrentProgram(int index) {
    juce::ignoreUnused(index);
}

const juce::String OpenTuneAudioProcessor::getProgramName(int index) {
    if (index == 0) {
        return "Default";
    }
    return {};
}

void OpenTuneAudioProcessor::changeProgramName(int index, const juce::String& newName) {
    juce::ignoreUnused(index, newName);
}

void OpenTuneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    if (!initializeRuntimeState()) {
        AppLogger::emergencyError("prepareToPlay: runtime initialization failed (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + " sr=" + juce::String(sampleRate, 1)
            + " block=" + juce::String(samplesPerBlock) + ")");
        return;
    }
    // Runtime is now ready: restore any state that arrived before init
    // (e.g. a scanner state round-trip, or a host save-state restore order
    // where setStateInformation preceded prepareToPlay).
    replayDeferredState();
    const RuntimePhase entryPhase = phase_;  // snapshot before any mutation
    const bool firstPrepare = (preparedPlaybackSampleRate_ == 0.0);
    const bool realRateChange = (!firstPrepare && preparedPlaybackSampleRate_ != sampleRate);

    AppLogger::log("prepareToPlay: sampleRate=" + juce::String(sampleRate, 2) +
                   " blockSize=" + juce::String(samplesPerBlock) +
                   " firstPrepare=" + juce::String(firstPrepare ? "true" : "false") +
                   " realRateChange=" + juce::String(realRateChange ? "true" : "false"));

    currentSampleRate_ = sampleRate;
    currentBlockSize_ = samplesPerBlock;

    // Real sample rate change: project transportCursor_ from old rate to new rate
    if (realRateChange) {
        const double oldRate = preparedPlaybackSampleRate_;
        const int64_t oldCursor = transportCursor_;
        transportCursor_ = TimeCoordinate::sampleRateProject(oldCursor, oldRate, sampleRate);
        audioReadCursor_ = transportCursor_;
        AppLogger::log("prepareToPlay: transportCursor projected from " + juce::String(oldCursor)
                       + " to " + juce::String(transportCursor_));
    } else {
        audioReadCursor_ = transportCursor_;
    }

    // Cancel any active transition; gain=0
    transitionActive_ = false;
    currentOutputGain_ = 0.0f;
    targetOutputGain_ = 0.0f;
    rampSamplesRemaining_ = 0;

    // Set phase from saved entry: Stopped stays Stopped; Playing/Paused becomes Paused
    phase_ = (entryPhase == RuntimePhase::Stopped) ? RuntimePhase::Stopped : RuntimePhase::Paused;

    // CRS playback rate: call on first prepare or real sample rate change
    if ((firstPrepare || realRateChange) && contentRenderService_) {
        contentRenderService_->preparePlaybackSampleRate(sampleRate);
        preparedPlaybackSampleRate_ = sampleRate;
    }

    // Sync seqlock sequences so audio thread does not replay stale commands on restart
    appliedControlSequence_ = controlSequence_.load(std::memory_order_acquire);

    doublePrecisionScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);
    trackMixScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);
    clipReadScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);

    // Transport reset on (re)prepare: clear play/loop flags; keep last known time/loop range
    playHeadState_.reset();

    // Sync time mirror after projection
    {
        const double posSec = TimeCoordinate::samplesToSeconds(transportCursor_, sampleRate);
        playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
    }

#if JucePlugin_Enable_ARA
    prepareToPlayForARA(sampleRate,
                        samplesPerBlock,
                        getMainBusNumOutputChannels(),
                        getProcessingPrecision());
#endif
    pianoKeyAudition_.loadSamples();

    outputSpectrumAnalyzer_.prepare(sampleRate);

#if JucePlugin_Build_VST3
    if (auto* captureSession = getCaptureSession())
        captureSession->prepareToPlay(sampleRate, samplesPerBlock, getMainBusNumInputChannels());
#endif
}

void OpenTuneAudioProcessor::releaseResources() {
    const RuntimePhase entryPhase = phase_;  // snapshot before any mutation
    // Cancel transition; align cursors, gain=0
    transitionActive_ = false;
    currentOutputGain_ = 0.0f;
    targetOutputGain_ = 0.0f;
    rampSamplesRemaining_ = 0;
    audioReadCursor_ = transportCursor_;

    phase_ = (entryPhase == RuntimePhase::Stopped) ? RuntimePhase::Stopped : RuntimePhase::Paused;

    // Sync seqlock sequences
    appliedControlSequence_ = controlSequence_.load(std::memory_order_acquire);

    // Transport reset on release: clear play/loop flags; keep last known time/loop range
    playHeadState_.reset();

    outputSpectrumAnalyzer_.reset();

#if JucePlugin_Enable_ARA
    releaseResourcesForARA();
#endif

#if JucePlugin_Build_VST3
    if (auto* captureSession = getCaptureSession())
        captureSession->releaseResources();
#endif
}

#if JucePlugin_Build_VST3
Capture::CaptureSession* OpenTuneAudioProcessor::getCaptureSession() noexcept
{
#if JucePlugin_Enable_ARA
    if (isBoundToARA())
        return nullptr;
#endif
    return captureSession_.get();
}

const Capture::CaptureSession* OpenTuneAudioProcessor::getCaptureSession() const noexcept
{
#if JucePlugin_Enable_ARA
    if (isBoundToARA())
        return nullptr;
#endif
    return captureSession_.get();
}
#endif // JucePlugin_Build_VST3

#if JucePlugin_Enable_ARA
OpenTuneDocumentController* OpenTuneAudioProcessor::getDocumentController() const
{
    auto* controller = AudioProcessorARAExtension::getDocumentController();
    if (controller == nullptr)
    {
        return nullptr;
    }

    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(controller);
}

const PlayHeadState& OpenTuneAudioProcessor::getPlayHeadState() const noexcept
{
    if (isBoundToARA())
        if (auto* dc = getDocumentController())
            return dc->getSharedPlayHeadState();
    return playHeadState_;
}
#else
const PlayHeadState& OpenTuneAudioProcessor::getPlayHeadState() const noexcept
{
    return playHeadState_;
}
#endif // JucePlugin_Enable_ARA

#if JucePlugin_Enable_ARA
void OpenTuneAudioProcessor::didBindToARA() noexcept
{
    // 先完成 JUCE 基类绑定，再延迟初始化运行时态；初始化失败则直接返回，
    // 保留后续 ARA 文档控制器逻辑（仅在初始化成功后执行）。
    juce::AudioProcessorARAExtension::didBindToARA();
    if (!initializeRuntimeState()) {
        AppLogger::emergencyError("didBindToARA: runtime initialization failed (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)) + ")");
        return;
    }

    // Runtime ready: restore any deferred pre-init state.
    replayDeferredState();

    // 绑定后渲染由 DC 的 CRS 承担（initializeRuntimeStateOnce 只给未绑定实例创建
    // processor-local 路径）。若 runtime 在绑定前已初始化（prepareToPlay /
    // createEditor 先到），这里一次性拆掉旧 regular 路径；若初始化时就已绑定，
    // 以下成员全为空，清理为 no-op。放在 replay 之后，保持既有 restore 顺序。
#if JucePlugin_Build_VST3
    stopTimer();
#endif
    if (contentRenderService_) {
        // detach 必须先于 clearAll：先摘掉 processor lease，render worker 才不会
        // 在清理中途进入 processor 回调。
        contentRenderService_->detachExecutionLease(this);
        // clearAll 覆盖 playback source / stage1 队列 / render cache / stretcher /
        // time-stretch cache。
        contentRenderService_->clearAll();
    }
#if JucePlugin_Build_VST3
    captureSession_.reset();
#endif
    contentRenderService_.reset();

    if (auto* dc = getDocumentController())
    {
        // F0 不在主线程重初始化；由分析 worker 懒初始化（scheduleAsyncF0Extraction
        // 内的 ProcessF0Runtime::initialize）。

        // The DC installs its own lease on its CRS in its constructor
        // (installDocumentRenderExecution -> processDocumentRenderJob -> ProcessRenderRuntime).
        // The processor does NOT attach an additional lease: doing so would
        // override the DC's lease and break the ARA2 render path. With a local
        // CRS torn down above, the processor owns no render state at all when bound.

        AppLogger::logNoThrow("ARA: didBindToARA - DC owns its CRS lease; processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc))
            + " playbackRenderer=" + juce::String(isPlaybackRenderer() ? "true" : "false")
            + " editorRenderer=" + juce::String(isEditorRenderer() ? "true" : "false")
            + " editorView=" + juce::String(isEditorView() ? "true" : "false")
            + " araBound=true");
    }
}
#endif // JucePlugin_Enable_ARA

bool OpenTuneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo())
        return false;

#if JucePlugin_Build_Standalone
    return in.isDisabled();
#else
    return in == juce::AudioChannelSet::stereo();
#endif
}

bool OpenTuneAudioProcessor::supportsDoublePrecisionProcessing() const {
    return true;
}

// ============================================================================
// 音频处理（processBlock）
// ============================================================================

void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer,
                                          juce::MidiBuffer& midiMessages) {
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Zero-sample block: route through float processBlock for a single host
    // PositionInfo observation (PlayHeadState update), then bail before any
    // ARA / capture / renderer / audio work. No second getPosition() is added;
    // the float path's own numSamples<=0 early return (after its update) does
    // the bail. Mirrors Sample32 zero-data semantics on Sample64 path.
    if (numSamples <= 0) {
        juce::AudioBuffer<float> emptyFloatBuffer;
        processBlock(emptyFloatBuffer, midiMessages);
        return;
    }

    // Non-zero audio with no channels: keep original early return.
    if (numChannels <= 0) {
        return;
    }

    if (numChannels > doublePrecisionScratch_.getNumChannels() || numSamples > doublePrecisionScratch_.getNumSamples()) {
        buffer.clear();
        return;
    }

    for (int ch = 0; ch < numChannels; ++ch) {
        const double* src = buffer.getReadPointer(ch);
        float* dst = doublePrecisionScratch_.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            dst[i] = static_cast<float>(src[i]);
        }
    }

    juce::AudioBuffer<float> floatBuffer(doublePrecisionScratch_.getArrayOfWritePointers(), numChannels, numSamples);
    processBlock(floatBuffer, midiMessages);

    for (int ch = 0; ch < numChannels; ++ch) {
        const float* src = floatBuffer.getReadPointer(ch);
        double* dst = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            dst[i] = static_cast<double>(src[i]);
        }
    }
}

void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages) {
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    // 初始化失败或未完成时，清零输出并绕过所有成员访问：主机仍会调用
    // processBlock，但 lazily 初始化的成员（contentRenderService_ 等）尚未
    // 发布，不能触碰它们。acquire load 保证：见 true -> 所有成员发布已对本音频
    // 线程可见。
    if (!runtimeStateInitialized_.load(std::memory_order_acquire)) {
        buffer.clear();
        return;
    }

    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

#if JucePlugin_Build_VST3
    // Single host PositionInfo read per block (per ARA2 spec). All branches below
    // consume the same Optional / PositionInfo object; empty Optional is never
    // promoted into canonical state and never replaced with a zero fallback.
    juce::Optional<juce::AudioPlayHead::PositionInfo> hostPosOpt = juce::nullopt;
    if (auto* hostPlayHead = getPlayHead())
        hostPosOpt = hostPlayHead->getPosition();

    // 1) Update processor-owned canonical transport truth first (no-op if nullopt).
    //    In ARA mode the shared DC-owned PlayHeadState is the single canonical
    //    truth; processor-local playHeadState_ is unused and must not be written.
#if JucePlugin_Enable_ARA
    if (!isBoundToARA())
#endif
        playHeadState_.update(hostPosOpt);

#if JucePlugin_Enable_ARA
    // REAPER may split ARA roles across processor instances. The playback
    // renderer is the sole publisher of the host's PositionInfo to the shared
    // document-level PlayHeadState; editor renderers must not overwrite it.
    if (hostPosOpt.hasValue() && isBoundToARA() && isPlaybackRenderer())
        if (auto* dc = getDocumentController())
            dc->observeHostPlaybackPosition(hostPosOpt,
                (numSamples > 0)
                    ? static_cast<double>(numSamples) / currentSampleRate_.load(std::memory_order_relaxed)
                    : 0.0);
#endif

    // 2) Publish presentation projection anchor (before any early return).
    //    Only when the host supplied timeInSeconds in this block.
    //    In ARA mode the DC publishes its own projection; skip processor-local.
#if JucePlugin_Enable_ARA
    if (!isBoundToARA())
#endif
    {
        const double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
        const double blockDur = (numSamples > 0)
            ? static_cast<double>(numSamples) / currentSampleRate_.load(std::memory_order_relaxed)
            : 0.0;
        const uint64_t epoch = playHeadState_.presentationEpoch.load(std::memory_order_relaxed);

        if (hostPosOpt.hasValue())
        {
            if (const auto timeSec = hostPosOpt->getTimeInSeconds())
                playHeadState_.presentationProjection.publish(*timeSec, nowClock, *timeSec + blockDur, epoch);
        }
    }
#endif // JucePlugin_Build_VST3

    // Zero-data block: PositionInfo was observed and PlayHeadState updated above.
    // Skip ARA / capture / renderer / standalone audio paths entirely — zero-data
    // must not enter capture/renderer, but the host transport truth is still
    // refreshed once per block.
    if (numSamples <= 0)
        return;

#if JucePlugin_Enable_ARA
    if (isBoundToARA())
    {
        // Forward same PositionInfo to ARA. Build a local empty PositionInfo when the
        // host did not provide one; this never enters playHeadState_ (update above was
        // a no-op for nullopt). Metadata snapshot is only refreshed when the host
        // actually provided a PositionInfo — empty Optional must not overwrite the
        // last valid BPM/PPQ/recording/time-signature with defaults.
        const juce::AudioPlayHead::PositionInfo emptyPositionInfo;
        const auto& araPositionInfo = hostPosOpt.hasValue()
            ? *hostPosOpt
            : emptyPositionInfo;
        if (hostPosOpt.hasValue())
            updateHostTransportSnapshot(araPositionInfo);

        if (processBlockForARA(buffer, isRealtime(), araPositionInfo))
        {
            pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, static_cast<double>(getSampleRate()));
            outputSpectrumAnalyzer_.push(buffer);
            return;
        }
    }
#endif

#if JucePlugin_Build_VST3
    // --- Non-ARA VST3 capture path: dry pass-through + capture session ---
    // Host absolute sample (from PositionInfo::getTimeInSamples()) is the sole source
    // of truth for audio positioning. No fallback from timeInSeconds.
    if (auto* captureSession = getCaptureSession())
    {
        if (hostPosOpt.hasValue())
            updateHostTransportSnapshot(*hostPosOpt);

        int64_t hostAbsoluteSample = -1;
        bool isPlaying = false;
        if (hostPosOpt.hasValue()) {
            if (auto timeInSamples = hostPosOpt->getTimeInSamples())
                hostAbsoluteSample = *timeInSamples;
            isPlaying = hostPosOpt->getIsPlaying();
        }

        captureSession->processBlock(buffer, hostAbsoluteSample, getSampleRate(), isPlaying);
        pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, static_cast<double>(getSampleRate()));
        outputSpectrumAnalyzer_.push(buffer);
        return;
    }

    // ARA-bound instance whose ARA processor path declined this block: silence the
    // output and keep the post-ARA audition/spectrum publication.
    for (int i = 0; i < totalNumOutputChannels; ++i) {
        buffer.clear(i, 0, numSamples);
    }
    pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, static_cast<double>(getSampleRate()));
    outputSpectrumAnalyzer_.push(buffer);
#else
    // --- Standalone state machine ---
    // Clear output buffer
    for (int i = 0; i < totalNumOutputChannels; ++i) {
        buffer.clear(i, 0, numSamples);
    }

    const double deviceSampleRate = currentSampleRate_.load();
    const int rampTotal = static_cast<int>(TimeCoordinate::secondsToSamples(kTransportRampDurationSeconds, deviceSampleRate));

    // Capture the transport generation and command snapshot before consumption.
    // A control write racing this block must not be followed by an old cursor
    // mirror at block end.
    const uint64_t blockEpoch = playHeadState_.presentationEpoch.load(std::memory_order_acquire);
    const uint64_t blockControlSequence = controlSequence_.load(std::memory_order_acquire);
    bool controlSnapshotStable = (blockControlSequence & 1u) == 0;

    // ==== BLOCK START: Consume seqlock command snapshot ====
    {
        const uint64_t seq1 = blockControlSequence;
        if (controlSnapshotStable && seq1 != 0 && seq1 != appliedControlSequence_) {
            TransportCommand cmd = pendingCommand_.load(std::memory_order_relaxed);
            double presTime = pendingPresentationTime_.load(std::memory_order_relaxed);
            double compTime = pendingCompletionTime_.load(std::memory_order_relaxed);
            RuntimePhase termPhase = pendingTerminalPhase_.load(std::memory_order_relaxed);

            uint64_t seq2 = controlSequence_.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                appliedControlSequence_ = seq1;

                const int64_t targetPresSample = TimeCoordinate::secondsToSamples(presTime, deviceSampleRate);
                const int64_t targetCompSample = TimeCoordinate::secondsToSamples(compTime, deviceSampleRate);

                switch (cmd) {
                case TransportCommand::Play:
                    // Fading out: freeze transportCursor_ at play presentation target,
                    // continue fade to 0, then fade in at completion.
                    if (transitionActive_ && targetOutputGain_ == 0.0f) {
                        transportCursor_ = targetPresSample;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Playing;
                    }
                    // Fading in or normal Playing: no duplicate action
                    else if (transitionActive_ || phase_ == RuntimePhase::Playing) {
                    }
                    // Paused/Stopped: start from current mute 0→1
                    else {
                        audioReadCursor_ = targetPresSample;
                        transportCursor_ = targetPresSample;
                        phase_ = RuntimePhase::Playing;
                        targetOutputGain_ = 1.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionActive_ = true;
                    }
                    break;

                case TransportCommand::Pause:
                case TransportCommand::PauseAtPosition:
                    // Same-direction fade-out: update freeze position and completion, keep fading
                    if (transitionActive_ && targetOutputGain_ == 0.0f) {
                        transportCursor_ = targetPresSample;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Paused;
                    }
                    // Fading in: reverse to fade-out
                    else if (transitionActive_) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Paused;
                    }
                    // Playing: freeze transportCursor_ at presentation target,
                    // audioReadCursor_ stays at frontier F, start fade-out
                    else if (phase_ == RuntimePhase::Playing) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Paused;
                        transitionActive_ = true;
                    }
                    // Direct Paused/Stopped: both cursors to completion, gain=0
                    else {
                        audioReadCursor_ = targetCompSample;
                        transportCursor_ = targetCompSample;
                        phase_ = termPhase;
                        currentOutputGain_ = 0.0f;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = 0;
                        transitionActive_ = false;
                    }
                    break;

                case TransportCommand::Stop:
                    // Same-direction fade-out: update freeze, completion to targetCompSample
                    if (transitionActive_ && targetOutputGain_ == 0.0f) {
                        transportCursor_ = targetPresSample;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Stopped;
                    }
                    // Fading in: reverse to fade-out, land at targetCompSample
                    else if (transitionActive_) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Stopped;
                    }
                    // Playing: freeze at presentation target, audioReadCursor_ stays at frontier
                    else if (phase_ == RuntimePhase::Playing) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Stopped;
                        transitionActive_ = true;
                    }
                    // Direct Paused/Stopped: both cursors to completion, gain=0
                    else {
                        audioReadCursor_ = targetCompSample;
                        transportCursor_ = targetCompSample;
                        phase_ = RuntimePhase::Stopped;
                        currentOutputGain_ = 0.0f;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = 0;
                        transitionActive_ = false;
                    }
                    break;

                case TransportCommand::Seek:
                    // Same-direction fade-out: update freeze and landing
                    if (transitionActive_ && targetOutputGain_ == 0.0f) {
                        transportCursor_ = targetPresSample;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = termPhase;
                    }
                    // Fading in: reverse to fade-out, land at seek completion target
                    else if (transitionActive_) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = termPhase;
                    }
                    // Playing→Playing Seek: freeze at presentation target, fade out, land, fade in
                    else if (phase_ == RuntimePhase::Playing && termPhase == RuntimePhase::Playing) {
                        transportCursor_ = targetPresSample;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = rampTotal;
                        transitionCompletionCursor_ = targetCompSample;
                        transitionCompletionPhase_ = RuntimePhase::Playing;
                        transitionActive_ = true;
                    }
                    // Direct Paused/Stopped: both cursors to completion, gain=0
                    else {
                        audioReadCursor_ = targetCompSample;
                        transportCursor_ = targetCompSample;
                        phase_ = termPhase;
                        currentOutputGain_ = 0.0f;
                        targetOutputGain_ = 0.0f;
                        rampSamplesRemaining_ = 0;
                        transitionActive_ = false;
                    }
                    break;

                case TransportCommand::None:
                    break;
                }
            }
            else
            {
                controlSnapshotStable = false;
            }
        }
    }

    // Determine read cursor for this block (fixed from audioReadCursor_)
    const int64_t blockStartSample = audioReadCursor_;

    // Stopped/Paused with no active transition — only piano audition
    if (phase_ != RuntimePhase::Playing && !transitionActive_) {
        pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, deviceSampleRate);
        outputSpectrumAnalyzer_.push(buffer);
        jassert(standaloneArrangement_ != nullptr);
        for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
            standaloneArrangement_->setTrackRmsDb(trackId, -100.0f);
        }
        if (controlSnapshotStable
            && controlSequence_.load(std::memory_order_acquire) == blockControlSequence
            && playHeadState_.presentationEpoch.load(std::memory_order_acquire) == blockEpoch) {
            const double posSec = TimeCoordinate::samplesToSeconds(transportCursor_, deviceSampleRate);
            playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
            playHeadState_.isPlaying.store(false, std::memory_order_release);
        }
        return;
    }

    jassert(trackMixScratch_.getNumChannels() >= totalNumOutputChannels);
    jassert(trackMixScratch_.getNumSamples() >= numSamples);
    jassert(clipReadScratch_.getNumChannels() >= totalNumOutputChannels);
    jassert(clipReadScratch_.getNumSamples() >= numSamples);

    const double blockDurationSeconds = static_cast<double>(numSamples) / deviceSampleRate;
    const double currentPosSeconds = TimeCoordinate::samplesToSeconds(blockStartSample, deviceSampleRate);
    const double blockEndSeconds = currentPosSeconds + blockDurationSeconds;
    const int64_t blockEndSample = blockStartSample + static_cast<int64_t>(numSamples);

    // Publish projection: Playing (including fade-in), NOT during fade-out
    {
        const bool publishProjection = (phase_ == RuntimePhase::Playing)
            && !(transitionActive_ && targetOutputGain_ == 0.0f)
            && controlSnapshotStable
            && controlSequence_.load(std::memory_order_acquire) == blockControlSequence
            && playHeadState_.presentationEpoch.load(std::memory_order_acquire) == blockEpoch;
        if (publishProjection) {
            const double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
            playHeadState_.presentationProjection.publish(currentPosSeconds, nowClock, blockEndSeconds, blockEpoch);
        }
    }

    const auto playbackSnapshot = standaloneArrangement_->loadPlaybackSnapshot();

    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        if (playbackSnapshot == nullptr) {
            continue;
        }

        const auto& track = playbackSnapshot->tracks[static_cast<size_t>(trackId)];
        
        bool shouldPlay = true;
        if (playbackSnapshot->anySoloed) {
            if (!track.isSolo) shouldPlay = false;
        } else {
            if (track.isMuted) shouldPlay = false;
        }

        if (!shouldPlay) {
            jassert(standaloneArrangement_ != nullptr);
            standaloneArrangement_->setTrackRmsDb(trackId, -100.0f);
            continue;
        }

        float trackVolume = track.volume;
        double trackRmsSum = 0.0;
        int trackSampleCount = 0;

        trackMixScratch_.clear();

        bool trackHasOutput = false;

        for (const auto& placement : track.placements) {
            PlaybackReadSource readSource;
            if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, readSource)
                || !readSource.hasAudio()) {
                continue;
            }

            const int64_t placementStartSample = TimeCoordinate::secondsToSamplesFloor(
                placement.timelineStartSeconds, deviceSampleRate);
            const int64_t placementEndSample = TimeCoordinate::secondsToSamplesCeil(
                placement.timelineStartSeconds + placement.durationSeconds, deviceSampleRate);
            if (placementEndSample <= placementStartSample)
                continue;

            if (placementEndSample <= blockStartSample || placementStartSample >= blockEndSample) {
                continue;
            }

            const int64_t overlapStartSample = std::max(blockStartSample, placementStartSample);
            const int64_t overlapEndSample = std::min(blockEndSample, placementEndSample);
            const int64_t samplesToCopy64 = overlapEndSample - overlapStartSample;
            if (samplesToCopy64 <= 0) {
                continue;
            }

            const double overlapAbsoluteSeconds = TimeCoordinate::samplesToSeconds(
                overlapStartSample, deviceSampleRate);
            double timeInPlacement = juce::jlimit(
                0.0,
                placement.durationSeconds,
                overlapAbsoluteSeconds - placement.timelineStartSeconds);
            const double outputStartSeconds =
                readSource.contentSnapshot->timeGrid->tauForward(placement.clipInSeconds);
            const int64_t readStartSample = TimeCoordinate::secondsToSamples(
                outputStartSeconds + timeInPlacement, deviceSampleRate);
            const int offsetInBlock = static_cast<int>(overlapStartSample - blockStartSample);
            const int samplesToCopy = static_cast<int>(samplesToCopy64);

            const float placementGain = placement.gain * trackVolume;
            const double fadeInSeconds = placement.fadeInDuration;
            const double fadeOutSeconds = placement.fadeOutDuration;

            ::OpenTune::PlaybackReadRequest readRequest;
            readRequest.source = readSource;
            readRequest.readStartSample = readStartSample;
            readRequest.targetSampleRate = deviceSampleRate;
            readRequest.numSamples = samplesToCopy;

            clipReadScratch_.clear();
            const int availableReadSamples = readPlaybackAudio(readRequest, clipReadScratch_, 0);

            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const float* src = clipReadScratch_.getReadPointer(ch);
                float* dst = trackMixScratch_.getWritePointer(ch, offsetInBlock);

                const double dt = 1.0 / deviceSampleRate;
                for (int s = 0; s < availableReadSamples; ++s) {
                    float gain = placementGain;

                    gain *= placementFadeGain(timeInPlacement,
                                              placement.durationSeconds,
                                              fadeInSeconds,
                                              fadeOutSeconds);

                    dst[s] += src[s] * gain;
                    timeInPlacement += dt;
                }
            }

            if (availableReadSamples > 0) {
                trackHasOutput = true;
            }
        }

        if (trackHasOutput) {
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const float* src = trackMixScratch_.getReadPointer(ch);
                float* dst = buffer.getWritePointer(ch);

                for (int s = 0; s < numSamples; ++s) {
                    dst[s] += src[s];
                    trackRmsSum += src[s] * src[s];
                }
            }
            trackSampleCount = numSamples * totalNumOutputChannels;
        }

        if (trackSampleCount > 0) {
            float rms = std::sqrt(static_cast<float>(trackRmsSum / trackSampleCount));
            float db = (rms > 1e-9f) ? 20.0f * std::log10(rms) : -100.0f;
            jassert(standaloneArrangement_ != nullptr);
            standaloneArrangement_->setTrackRmsDb(trackId, db);
        } else {
            jassert(standaloneArrangement_ != nullptr);
            standaloneArrangement_->setTrackRmsDb(trackId, -100.0f);
        }
    }

    // ==== Per-sample ramp: multiply by current gain, then step toward target ====
    bool fadeOutCompletedThisBlock = false;
    for (int sample = 0; sample < numSamples; ++sample) {
        const float sampleGain = currentOutputGain_;

        for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
            buffer.setSample(ch, sample, buffer.getSample(ch, sample) * sampleGain);
        }

        if (transitionActive_ && rampSamplesRemaining_ > 0) {
            currentOutputGain_ += (targetOutputGain_ - currentOutputGain_) / static_cast<float>(rampSamplesRemaining_);
            rampSamplesRemaining_--;

            if (rampSamplesRemaining_ == 0) {
                currentOutputGain_ = targetOutputGain_;

                if (targetOutputGain_ == 0.0f) {
                    fadeOutCompletedThisBlock = true;
                } else {
                    transitionActive_ = false;
                }
            }
        }
    }

    pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, deviceSampleRate);

    // Standalone 频谱分析：接收最终输出（含 pianoKeyAudition 混音）
    outputSpectrumAnalyzer_.push(buffer);

    // ==== Block-end: cursor advance (unconditional) ====
    audioReadCursor_ = blockEndSample;

    // Handle fade-out completion (land at completion cursor; start fade-in if terminal is Playing)
    if (fadeOutCompletedThisBlock) {
        audioReadCursor_ = transitionCompletionCursor_;
        transportCursor_ = transitionCompletionCursor_;

        if (transitionCompletionPhase_ == RuntimePhase::Playing) {
            phase_ = RuntimePhase::Playing;
            targetOutputGain_ = 1.0f;
            rampSamplesRemaining_ = rampTotal;
            transitionActive_ = true;
        } else {
            phase_ = transitionCompletionPhase_;
            transitionActive_ = false;
        }
    }

    // Update transportCursor_: frozen during fade-out, follows audioReadCursor_ otherwise
    if (!(transitionActive_ && targetOutputGain_ == 0.0f)) {
        transportCursor_ = audioReadCursor_;
    }

    // UI mirror update (only if epoch unchanged during this block)
    if (controlSnapshotStable
        && controlSequence_.load(std::memory_order_acquire) == blockControlSequence
        && playHeadState_.presentationEpoch.load(std::memory_order_acquire) == blockEpoch) {
        const double posSec = TimeCoordinate::samplesToSeconds(transportCursor_, deviceSampleRate);
        playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);

        // isPlaying during fade-out depends on terminal phase, not just target gain
        bool uiIsPlaying;
        if (transitionActive_ && targetOutputGain_ == 0.0f) {
            uiIsPlaying = (transitionCompletionPhase_ == RuntimePhase::Playing);
        } else {
            uiIsPlaying = (phase_ == RuntimePhase::Playing);
        }
        playHeadState_.isPlaying.store(uiIsPlaying, std::memory_order_release);
    }
#endif // JucePlugin_Build_VST3 / JucePlugin_Build_Standalone
}

#if JucePlugin_Build_VST3
OpenTuneAudioProcessor::HostTransportSnapshot OpenTuneAudioProcessor::getHostTransportSnapshot() const
{
    HostTransportSnapshot snapshot;
    snapshot.bpm = hostTransportBpm_.load(std::memory_order_relaxed);
    snapshot.ppqPosition = hostTransportPpqPosition_.load(std::memory_order_relaxed);
    snapshot.isRecording = hostTransportIsRecording_.load(std::memory_order_relaxed);
    snapshot.timeSignatureNumerator = hostTransportTimeSignatureNumerator_.load(std::memory_order_relaxed);
    snapshot.timeSignatureDenominator = hostTransportTimeSignatureDenominator_.load(std::memory_order_relaxed);
    return snapshot;
}

OpenTuneAudioProcessor::HostTransportSnapshot OpenTuneAudioProcessor::updateHostTransportSnapshot(
    const juce::AudioPlayHead::PositionInfo& positionInfo)
{
    HostTransportSnapshot snapshot = getHostTransportSnapshot();

    snapshot.isRecording = positionInfo.getIsRecording();

    if (const auto bpm = positionInfo.getBpm()) {
        snapshot.bpm = *bpm;
    }

    if (const auto ppq = positionInfo.getPpqPosition()) {
        snapshot.ppqPosition = *ppq;
    }

    if (const auto timeSignature = positionInfo.getTimeSignature()) {
        snapshot.timeSignatureNumerator = timeSignature->numerator;
        snapshot.timeSignatureDenominator = timeSignature->denominator;
    }

    hostTransportBpm_.store(snapshot.bpm, std::memory_order_relaxed);
    hostTransportPpqPosition_.store(snapshot.ppqPosition, std::memory_order_relaxed);
    hostTransportIsRecording_.store(snapshot.isRecording, std::memory_order_relaxed);
    hostTransportTimeSignatureNumerator_.store(snapshot.timeSignatureNumerator, std::memory_order_relaxed);
    hostTransportTimeSignatureDenominator_.store(snapshot.timeSignatureDenominator, std::memory_order_relaxed);
    return snapshot;
}
#endif // JucePlugin_Build_VST3

#if JucePlugin_Build_Standalone
OpenTuneAudioProcessor::DiagnosticInfo OpenTuneAudioProcessor::getDiagnosticInfo(int trackId, uint64_t placementId) const
{
    DiagnosticInfo info;
    jassert(editVersionParam_ != nullptr);
    info.editVersion = editVersionParam_->get();

    const auto controlCall = static_cast<DiagnosticControlCall>(lastControlType_.load(std::memory_order_relaxed));
    info.lastControlCall = diagnosticControlCallToString(controlCall);
    info.lastControlTimestamp = lastControlTimestamp_.load(std::memory_order_relaxed);

    jassert(standaloneArrangement_ != nullptr);
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        return info;
    }

    StandaloneArrangement::Placement targetPlacement;
    bool hasTargetPlacement = false;
    if (placementId != 0) {
        hasTargetPlacement = standaloneArrangement_->getPlacementById(trackId, placementId, targetPlacement);
    } else {
        const uint64_t selectedPlacementId = standaloneArrangement_->getSelectedPlacementId(trackId);
        if (selectedPlacementId != 0) {
            hasTargetPlacement = standaloneArrangement_->getPlacementById(trackId, selectedPlacementId, targetPlacement);
        }
        if (!hasTargetPlacement && standaloneArrangement_->getNumPlacements(trackId) > 0) {
            hasTargetPlacement = standaloneArrangement_->getPlacementByIndex(trackId, 0, targetPlacement);
        }
    }

    if (!hasTargetPlacement) {
        return info;
    }

    info.contentKey = targetPlacement.contentKey;
    info.placementId = targetPlacement.placementId;
    if (auto renderCache = contentRenderService_->getRenderCache(targetPlacement.contentKey)) {
        const auto renderState = renderCache->getStateSnapshot();
        info.chunkStats = renderState.chunkStats;
        info.publishedRevision = 0;
        info.desiredRevision = 0;
    }

    return info;
}

void OpenTuneAudioProcessor::recordControlCall(DiagnosticControlCall controlCall)
{
    lastControlType_.store(static_cast<int>(controlCall), std::memory_order_relaxed);
    lastControlTimestamp_.store(juce::Time::currentTimeMillis(), std::memory_order_relaxed);
}
#endif // JucePlugin_Build_Standalone

namespace {

// Minimal error editor shown when runtime initialization fails.
// hasEditor() == true, so createEditor MUST return a non-null editor.
// This class is intentionally tiny — no second UI path, no normal-editor logic.
class OpenTuneInitFailedEditor final : public juce::AudioProcessorEditor
{
public:
    explicit OpenTuneInitFailedEditor(juce::AudioProcessor& p) : juce::AudioProcessorEditor(p)
    {
        const juce::File logFile = AppLogger::getCurrentLogFile();
        const juce::String logPath = logFile != juce::File()
            ? logFile.getFullPathName()
            : juce::String("(log file not available)");
        message_ = "OpenTune runtime initialization failed.\nSee log: " + logPath;
        setResizable(false, false);
        setSize(520, 120);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::red);
        g.setFont(14.0f);
        g.drawText(message_, getLocalBounds().reduced(12),
                   juce::Justification::centredLeft, true);
    }

private:
    juce::String message_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneInitFailedEditor)
};

} // namespace

juce::AudioProcessorEditor* OpenTuneAudioProcessor::createEditor() {
    if (!initializeRuntimeState()) {
        AppLogger::emergencyError("createEditor: runtime initialization failed (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + "). Returning minimal error editor.");
        return new OpenTuneInitFailedEditor(*this);
    }
    // Runtime ready: restore any deferred pre-init state (scanner round-trip
    // or save-state delivered before first prepareToPlay).
    replayDeferredState();
    return createOpenTuneEditor(*this);
}

bool OpenTuneAudioProcessor::hasEditor() const {
    return true;
}

// ============================================================================
// 项目状态序列化/反序列化
// ============================================================================

void OpenTuneAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
#if JucePlugin_Build_Standalone
    // Standalone owns its project save format; host state carries settings only.
    StandaloneProcessorSettings settings;
    settings.bpm = getBpm();
    settings.timeSigNumerator = getTimeSigNumerator();
    settings.timeSigDenominator = getTimeSigDenominator();
    settings.uiZoomPercent = getUiZoomPercent();
    settings.trackHeight = trackHeight_;
    destData = StandaloneProcessorStateCodec::encode(settings);
#else
    // Per ARA2 spec: ARA AudioModification objects are persisted via
    // doStoreObjectsToStream/doRestoreObjectsFromStream, NOT via VST3 processor state.
    // ARA host owns document archive lifecycle.
    // VST3 processor state carries the outer settings plus the regular-VST3 capture
    // tail; the Standalone arrangement has its own persistence and is not embedded here.
    Vst3ProcessorOuterState outerState;
    outerState.uiZoomPercent = getUiZoomPercent();
    outerState.trackHeight = trackHeight_;

    // Append regular VST3 capture data at end of stream. ARA-bound instances keep
    // their edit state in the ARA document/session archive instead.
    if (const auto* captureSession = getCaptureSession())
        outerState.captureTail = captureSession->serialize();

    destData = Vst3ProcessorStateCodec::encode(outerState);
#endif
}


void OpenTuneAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) {
        return;
    }

    // setStateInformation itself must NOT trigger initializeRuntimeState():
    // the scanner may perform a state round-trip (getStateInformation ->
    // setStateInformation), and runtime init starts worker threads that are
    // forbidden at scan-time.
    //  - runtime ready  -> restore immediately via the shared restore helper;
    //    if the restore fails, cache as deferred so replayDeferredState
    //    retries on the next host entrypoint.
    //  - runtime pending -> cache the complete raw payload as "deferred" and
    //    replay it once prepareToPlay / createEditor / didBindToARA finish init.
    if (runtimeStateInitialized_.load(std::memory_order_acquire)) {
        if (!restoreStatePayload(data, sizeInBytes))
            pendingState_ = juce::MemoryBlock(data, static_cast<size_t>(sizeInBytes));
        return;
    }

    // Runtime not ready -- cache the complete raw payload and defer restore.
    // No logging here: AppLogger::log lazily initialises a FileLogger with an
    // independent write thread, which violates the scan-safe "no thread"
    // contract when the scanner performs a state round-trip before init.
    // The deferred restore is logged by replayDeferredState() post-init.
    pendingState_ = juce::MemoryBlock(data, static_cast<size_t>(sizeInBytes));
}

// --- restoreStatePayload: core restore logic. Caller guarantees runtime
// is initialized (runtimeStateInitialized_ == true). This helper does NOT
// touch initializeRuntimeState(). It is the single restore path used by
// setStateInformation (immediate) and replayDeferredState() (deferred). ---
bool OpenTuneAudioProcessor::restoreStatePayload(const void* data, int sizeInBytes) {
    try {
#if JucePlugin_Build_Standalone
    // Standalone settings-only payload (OTSS). The Standalone project format
    // owns arrangement/content persistence; host state carries settings only.
    StandaloneProcessorSettings restoredSettings;
    juce::String decodeError;
    if (!StandaloneProcessorStateCodec::decode(data, sizeInBytes, restoredSettings, decodeError)) {
        AppLogger::error("StateRestore: " + decodeError);
        return false;
    }
    setBpm(restoredSettings.bpm);
    setTimeSignature(restoredSettings.timeSigNumerator, restoredSettings.timeSigDenominator);
    setUiZoomPercent(restoredSettings.uiZoomPercent);
    trackHeight_ = restoredSettings.trackHeight;
    return true;
#else
    // Per ARA2 spec: ARA AudioModification objects are restored via
    // doRestoreObjectsFromStream, NOT via VST3 processor state.
    // ARA host owns document archive lifecycle.
    // OTST v11 carries outer UI settings plus the regular-VST3 capture tail; the
    // Standalone arrangement is not embedded. Released v9/v10 arrangement
    // sections are skipped by the codec, never restored.

    // VST3 processor state (OTST): outer settings + optional capture tail. The
    // codec accepts current v11 and released v10/v9, skipping the legacy
    // arrangement layout without materialising it.
    Vst3ProcessorOuterState outerState;
    if (!Vst3ProcessorStateCodec::decode(data, sizeInBytes, outerState)) {
        AppLogger::error("StateRestore: unsupported processor state payload sizeBytes="
                         + juce::String(sizeInBytes));
        return false;
    }

    AppLogger::log("StateRestore: VST3 processor state begin sizeBytes=" + juce::String(sizeInBytes)
                   + " version=" + juce::String(outerState.sourceVersion));

    // Capture tail policy: validate current CAPz before committing anything;
    // released OTST v9/v10 payloads may embed an older CAPz archive, which is
    // dropped per the explicit no-migration policy (settings still restored).
    const bool hasCaptureTail = outerState.captureTail.getSize() > 0;
    bool discardCaptureTail = false;
    if (hasCaptureTail) {
        const int captureVersion = Capture::CapturePersistence::peekArchiveVersion(outerState.captureTail);
        if (captureVersion == Capture::CapturePersistence::kArchiveVersion) {
            if (!Capture::CapturePersistence::validate(outerState.captureTail)) {
                AppLogger::error("StateRestore: CaptureSession payload rejected during validation");
                return false;
            }
        } else if (outerState.sourceVersion == Vst3ProcessorStateCodec::kLegacyVersion10
                   || outerState.sourceVersion == Vst3ProcessorStateCodec::kLegacyVersion9) {
            AppLogger::error("StateRestore: legacy OTST capture tail discarded (capture version="
                             + juce::String(captureVersion) + ")");
            discardCaptureTail = true;
        } else {
            AppLogger::error("StateRestore: CaptureSession payload rejected during validation");
            return false;
        }
    }

    setUiZoomPercent(outerState.uiZoomPercent);
    trackHeight_ = outerState.trackHeight;

    // Regular VST3 capture persistence is a separate owner transaction.
    if (hasCaptureTail && !discardCaptureTail) {
        if (auto* captureSession = getCaptureSession()) {
            if (!captureSession->deserialize(outerState.captureTail)) {
                AppLogger::error("StateRestore: CaptureSession payload rejected");
                return false;
            }
            AppLogger::log("CaptureSession: state restored ("
                           + juce::String(captureSession->listSegments().size()) + " segments)");
        }
    }

    return true;
#endif
    }
    catch (const std::exception& e) {
        AppLogger::emergencyError("StateRestore: exception (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + "): " + juce::String(e.what()));
        return false;
    }
    catch (...) {
        AppLogger::emergencyError("StateRestore: unknown exception (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)) + ")");
        return false;
    }
}

void OpenTuneAudioProcessor::replayDeferredState() noexcept {
    if (pendingState_.getSize() == 0)
        return;

    try {
        AppLogger::log("StateRestore: replaying deferred setStateInformation sizeBytes="
                       + juce::String(static_cast<int>(pendingState_.getSize())));
        if (restoreStatePayload(pendingState_.getData(),
                                static_cast<int>(pendingState_.getSize())))
            pendingState_.reset();
        else
            AppLogger::error("StateRestore: deferred payload retained after restore failure");
    }
    catch (const std::exception& e) {
        AppLogger::emergencyError("StateRestore: deferred replay exception (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + "): " + juce::String(e.what()));
    }
    catch (...) {
        AppLogger::emergencyError("StateRestore: deferred replay unknown exception (processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)) + ")");
    }
}

// ============================================================================
// Standalone Arrangement 代理接口
// ============================================================================

void OpenTuneAudioProcessor::setTrackHeight(int height) {
    trackHeight_ = height;
}

#if JucePlugin_Build_Standalone
uint64_t OpenTuneAudioProcessor::getPlacementId(int trackId, int placementIndex) const
{
    return standaloneArrangement_->getPlacementId(trackId, placementIndex);
}

int OpenTuneAudioProcessor::findPlacementIndexById(int trackId, uint64_t placementId) const
{
    return standaloneArrangement_->findPlacementIndexById(trackId, placementId);
}

bool OpenTuneAudioProcessor::getPlacementByIndex(int trackId,
                                                 int placementIndex,
                                                 StandaloneArrangement::Placement& out) const
{
    out = StandaloneArrangement::Placement{};
    jassert(standaloneArrangement_ != nullptr);
    return standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, out);
}

bool OpenTuneAudioProcessor::getPlacementById(int trackId,
                                              uint64_t placementId,
                                              StandaloneArrangement::Placement& out) const
{
    out = StandaloneArrangement::Placement{};
    jassert(standaloneArrangement_ != nullptr);
    return standaloneArrangement_->getPlacementById(trackId, placementId, out);
}

std::optional<SplitOutcome> OpenTuneAudioProcessor::splitPlacementAtSeconds(int trackId, int placementIndex, double splitSeconds)
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("Split rejected: invalid trackId=" + juce::String(trackId));
        return std::nullopt;
    }

    StandaloneArrangement::Placement originalPlacement;
    if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, originalPlacement)) {
        AppLogger::log("Split rejected: invalid placementIndex=" + juce::String(placementIndex)
                       + " for trackId=" + juce::String(trackId));
        return std::nullopt;
    }

    const double requestedOffsetSeconds = splitSeconds - originalPlacement.timelineStartSeconds;
    constexpr double minDurationSeconds = 0.1;
    if (requestedOffsetSeconds <= minDurationSeconds
        || requestedOffsetSeconds >= originalPlacement.durationSeconds - minDurationSeconds) {
        AppLogger::log("Split rejected: split point out of valid placement range");
        return std::nullopt;
    }

    const auto originalSnapshot = getContentSnapshot(originalPlacement.contentKey);
    if (!originalSnapshot || originalSnapshot->audioBuffer == nullptr) {
        return std::nullopt;
    }

    // 绝对时间合同：非 identity grid 的 split 结果会被 owner bootstrap 覆盖成
    // identity，造成时间映射错误；在取得 snapshot 后直接拒绝，不执行旧路径。
    if (!originalSnapshot->timeGrid->isIdentity()) {
        AppLogger::log("Split rejected: content has non-identity time grid");
        return std::nullopt;
    }

    if (!originalSnapshot->hasUsableOriginalF0()) {
        AppLogger::log("Split rejected: content has no usable original F0");
        return std::nullopt;
    }

    const double audioSampleRate = originalSnapshot->audioSampleRate;
    if (!std::isfinite(audioSampleRate) || audioSampleRate <= 0.0) {
        AppLogger::log("Split rejected: content audio sample rate unavailable");
        return std::nullopt;
    }

    const auto& f0Curve = *originalSnapshot->pitchCurve;
    const int f0FrameCount = static_cast<int>(f0Curve.getOriginalF0().size());
    const F0Timeline f0Timeline(f0Curve.getHopSize(), f0Curve.getSampleRate(), f0FrameCount);

    // identity grid：content-local source 秒 == output 秒。
    const double requestedSourceSeconds = originalPlacement.clipInSeconds + requestedOffsetSeconds;
    const int frameBoundary = f0Timeline.nearestFrameBoundary(requestedSourceSeconds);
    if (frameBoundary <= 0 || frameBoundary >= f0FrameCount) {
        AppLogger::log("Split rejected: nearest F0 frame boundary has no frames on both sides");
        return std::nullopt;
    }

    // F0-local frame time 是唯一分割边界；音频侧只把它投影成离散 sample 访问坐标。
    const double splitSourceSeconds = f0Timeline.timeAtFrame(frameBoundary);
    const int64_t splitSample = TimeCoordinate::secondsToSamplesNearest(splitSourceSeconds, audioSampleRate);
    // timeAtFrame 是 double 乘法，理论对齐的帧也可能有 ulp 级舍入；容差只吸收浮点噪声，
    // 任何真实样本非对齐的帧都会被拒绝，绝不把 sample 量化回写到 F0/notes/timeline。
    constexpr double sampleGridToleranceSeconds = 1.0e-9;
    if (std::abs(TimeCoordinate::samplesToSeconds(splitSample, audioSampleRate) - splitSourceSeconds)
        > sampleGridToleranceSeconds) {
        AppLogger::log("Split rejected: F0 frame boundary does not land on the audio sample grid");
        return std::nullopt;
    }

    const int64_t totalSamples = originalSnapshot->audioBuffer->getNumSamples();
    if (splitSample <= 0 || splitSample >= totalSamples) {
        return std::nullopt;
    }

    const double splitOffsetSeconds = splitSourceSeconds - originalPlacement.clipInSeconds;
    if (splitOffsetSeconds <= minDurationSeconds
        || splitOffsetSeconds >= originalPlacement.durationSeconds - minDurationSeconds) {
        AppLogger::log("Split rejected: snapped split point out of valid placement range");
        return std::nullopt;
    }
    const double snappedSplitSeconds = originalPlacement.timelineStartSeconds + splitOffsetSeconds;

    // 两个子窗口共享同一 source-absolute boundary：由父窗口起始 sample + splitSample 合成；
    // 父窗口两端保持原对齐值不动。前提：父窗口本身已在该 sample 网格上。
    const auto alignedParentWindow = alignSourceWindowToSampleGrid(
        originalSnapshot->sourceWindow, audioSampleRate);
    if (!alignedParentWindow.has_value()
        || alignedParentWindow->sourceStartSeconds != originalSnapshot->sourceWindow.sourceStartSeconds
        || alignedParentWindow->sourceEndSeconds != originalSnapshot->sourceWindow.sourceEndSeconds) {
        AppLogger::log("Split rejected: parent source window is not sample-aligned");
        return std::nullopt;
    }

    const int64_t parentStartSample = TimeCoordinate::secondsToSamplesNearest(
        alignedParentWindow->sourceStartSeconds, audioSampleRate);
    const double boundarySourceSeconds = TimeCoordinate::samplesToSeconds(
        parentStartSample + splitSample, audioSampleRate);
    const double parentDurationSeconds = originalSnapshot->sourceWindow.durationSeconds();

    ContentState leadingPayload = contentStateFromSnapshot(*originalSnapshot);
    leadingPayload.sourceWindow = originalSnapshot->sourceWindow;
    leadingPayload.sourceWindow.sourceEndSeconds = boundarySourceSeconds;
    leadingPayload.audioBuffer = sliceAudioBuffer(originalSnapshot->audioBuffer, 0, splitSample);
    leadingPayload.analysis.pitchCurve = slicePitchCurveToFrameRange(originalSnapshot->pitchCurve, 0, frameBoundary);
    leadingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes, 0.0, splitSourceSeconds);
    leadingPayload.volumeEnvelope = sliceVolumeEnvelopeToLocalRange(
        originalSnapshot->volumeEnvelope, 0.0, splitSourceSeconds);
    leadingPayload.analysis.silentGaps = sliceSilentGaps(originalSnapshot->silentGaps, 0, splitSample);
    // 派生特征绑定父 content/父绝对窗口，无可靠 slice：清空使其失效并可重新分析。
    leadingPayload.analysis.referenceFeatures.reset();
    // timeGrid 沿用原 identity grid：时长与 slice 不匹配，owner bootstrap 会覆盖为 slice identity。

    ContentState trailingPayload = contentStateFromSnapshot(*originalSnapshot);
    trailingPayload.sourceWindow = originalSnapshot->sourceWindow;
    trailingPayload.sourceWindow.sourceStartSeconds = boundarySourceSeconds;
    trailingPayload.audioBuffer = sliceAudioBuffer(originalSnapshot->audioBuffer, splitSample, totalSamples);
    trailingPayload.analysis.pitchCurve = slicePitchCurveToFrameRange(
        originalSnapshot->pitchCurve, frameBoundary, f0FrameCount);
    trailingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes,
                                                   splitSourceSeconds,
                                                   parentDurationSeconds);
    trailingPayload.volumeEnvelope = sliceVolumeEnvelopeToLocalRange(
        originalSnapshot->volumeEnvelope, splitSourceSeconds, parentDurationSeconds);
    trailingPayload.analysis.silentGaps = sliceSilentGaps(originalSnapshot->silentGaps, splitSample, totalSamples);
    trailingPayload.analysis.referenceFeatures.reset();
    // timeGrid 沿用原 identity grid：时长与 slice 不匹配，owner bootstrap 会覆盖为 slice identity。

    const ContentKey leadingKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                            *contentRenderService_,
                                                            std::move(leadingPayload));
    const ContentKey trailingKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                             *contentRenderService_,
                                                             std::move(trailingPayload));
    if (!leadingKey.isValid() || !trailingKey.isValid()) {
        if (leadingKey.isValid()) standaloneContentRepository_->releaseClip(leadingKey);
        if (trailingKey.isValid()) standaloneContentRepository_->releaseClip(trailingKey);
        return std::nullopt;
    }

    StandaloneArrangement::Placement leadingPlacement = originalPlacement;
    leadingPlacement.placementId = 0;
    leadingPlacement.contentKey = leadingKey;
    leadingPlacement.durationSeconds = splitOffsetSeconds;
    leadingPlacement.fadeOutDuration = 0.0;

    StandaloneArrangement::Placement trailingPlacement = originalPlacement;
    trailingPlacement.placementId = 0;
    trailingPlacement.contentKey = trailingKey;
    trailingPlacement.timelineStartSeconds = snappedSplitSeconds;
    trailingPlacement.durationSeconds = originalPlacement.durationSeconds - splitOffsetSeconds;
    trailingPlacement.fadeInDuration = 0.0;
    trailingPlacement.clipInSeconds = 0.0;

    if (!standaloneArrangement_->insertPlacement(trackId, placementIndex, leadingPlacement)) {
        standaloneContentRepository_->releaseClip(leadingKey);
        standaloneContentRepository_->releaseClip(trailingKey);
        return std::nullopt;
    }

    if (!standaloneArrangement_->insertPlacement(trackId, placementIndex + 1, trailingPlacement)) {
        standaloneArrangement_->deletePlacementById(trackId, leadingPlacement.placementId, nullptr, nullptr);
        standaloneContentRepository_->releaseClip(leadingKey);
        standaloneContentRepository_->releaseClip(trailingKey);
        return std::nullopt;
    }

    standaloneArrangement_->retirePlacement(trackId, originalPlacement.placementId);
    standaloneContentRepository_->retireClip(originalPlacement.contentKey);

    standaloneArrangement_->selectPlacement(trackId, trailingPlacement.placementId);
    scheduleReclaimSweep();

    SplitOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = originalSnapshot->sourceWindow.sourceId;
    outcome.originalPlacementId = originalPlacement.placementId;
    outcome.originalContentKey = originalPlacement.contentKey;
    outcome.leadingPlacementId = leadingPlacement.placementId;
    outcome.trailingPlacementId = trailingPlacement.placementId;
    outcome.leadingContentKey = leadingKey;
    outcome.trailingContentKey = trailingKey;
    return outcome;
}

std::optional<MergeOutcome> OpenTuneAudioProcessor::mergePlacements(int trackId,
                                                                    uint64_t leadingPlacementId,
                                                                    uint64_t trailingPlacementId,
                                                                    int targetPlacementIndex)
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("Merge rejected: invalid trackId=" + juce::String(trackId));
        return std::nullopt;
    }

    StandaloneArrangement::Placement leadingPlacement;
    StandaloneArrangement::Placement trailingPlacement;
    if (!standaloneArrangement_->getPlacementById(trackId, leadingPlacementId, leadingPlacement)
        || !standaloneArrangement_->getPlacementById(trackId, trailingPlacementId, trailingPlacement)) {
        AppLogger::log("Merge rejected: placement not found");
        return std::nullopt;
    }

    constexpr double epsilonSeconds = 1.0 / TimeCoordinate::kRenderSampleRate;
    if (std::abs(leadingPlacement.timelineEndSeconds() - trailingPlacement.timelineStartSeconds) > epsilonSeconds) {
        AppLogger::log("Merge rejected: placements do not form one continuous placement span");
        return std::nullopt;
    }

    if (std::abs(leadingPlacement.gain - trailingPlacement.gain) > 1.0e-6f
        || leadingPlacement.name != trailingPlacement.name) {
        AppLogger::log("Merge rejected: placement metadata diverged");
        return std::nullopt;
    }

    const auto leadingSnapshot = getContentSnapshot(leadingPlacement.contentKey);
    const auto trailingSnapshot = getContentSnapshot(trailingPlacement.contentKey);
    if (!leadingSnapshot || !trailingSnapshot
        || leadingSnapshot->audioBuffer == nullptr
        || trailingSnapshot->audioBuffer == nullptr
        || leadingSnapshot->sourceWindow.sourceId == 0
        || leadingSnapshot->sourceWindow.sourceId != trailingSnapshot->sourceWindow.sourceId) {
        AppLogger::log("Merge rejected: placements do not resolve to the same source lineage");
        return std::nullopt;
    }

    const double leadingAudioSampleRate = leadingSnapshot->audioSampleRate;
    const double trailingAudioSampleRate = trailingSnapshot->audioSampleRate;
    if (!std::isfinite(leadingAudioSampleRate) || leadingAudioSampleRate <= 0.0
        || leadingAudioSampleRate != trailingAudioSampleRate) {
        AppLogger::log("Merge rejected: content audio sample rates are unavailable or diverged");
        return std::nullopt;
    }

    // 两个窗口已 sample-aligned；连续性用各自 sample rate 投影到 nearest sample
    // index 后精确比较，不用整样本秒容差接受真实 gap/overlap。
    if (TimeCoordinate::secondsToSamplesNearest(leadingSnapshot->sourceWindow.sourceEndSeconds,
                                                leadingAudioSampleRate)
        != TimeCoordinate::secondsToSamplesNearest(trailingSnapshot->sourceWindow.sourceStartSeconds,
                                                    trailingAudioSampleRate)) {
        AppLogger::log("Merge rejected: clips do not describe one contiguous source provenance window");
        return std::nullopt;
    }

    if (!detectedKeysMatch(leadingSnapshot->detectedKey, trailingSnapshot->detectedKey)) {
        AppLogger::log("Merge rejected: content metadata diverged");
        return std::nullopt;
    }

    if (leadingSnapshot->originalF0State == OriginalF0State::Extracting
        || trailingSnapshot->originalF0State == OriginalF0State::Extracting) {
        AppLogger::log("Merge rejected: content payload is still being refreshed");
        return std::nullopt;
    }

    const auto mergedPitchCurve = mergePitchCurves(leadingSnapshot->pitchCurve,
                                                   trailingSnapshot->pitchCurve,
                                                   leadingSnapshot->originalF0State,
                                                   trailingSnapshot->originalF0State);
    const bool mergedCurveRejected = (leadingSnapshot->pitchCurve != nullptr || trailingSnapshot->pitchCurve != nullptr)
        && mergedPitchCurve == nullptr;
    if (mergedCurveRejected) {
        AppLogger::log("Merge rejected: pitch-curve payload cannot be merged without data loss");
        return std::nullopt;
    }

    const int leadingSamples = leadingSnapshot->audioBuffer->getNumSamples();
    const int trailingSamples = trailingSnapshot->audioBuffer->getNumSamples();
    const double leadingDurationSeconds = TimeCoordinate::samplesToSeconds(leadingSamples, TimeCoordinate::kRenderSampleRate);
    auto mergedBuffer = std::make_shared<juce::AudioBuffer<float>>(juce::jmax(leadingSnapshot->audioBuffer->getNumChannels(),
                                                                              trailingSnapshot->audioBuffer->getNumChannels()),
                                                                   leadingSamples + trailingSamples);
    mergedBuffer->clear();
    for (int channel = 0; channel < mergedBuffer->getNumChannels(); ++channel) {
        if (channel < leadingSnapshot->audioBuffer->getNumChannels()) {
            mergedBuffer->copyFrom(channel, 0, *leadingSnapshot->audioBuffer, channel, 0, leadingSamples);
        }
        if (channel < trailingSnapshot->audioBuffer->getNumChannels()) {
            mergedBuffer->copyFrom(channel, leadingSamples, *trailingSnapshot->audioBuffer, channel, 0, trailingSamples);
        }
    }

    std::vector<Note> mergedNotes = leadingSnapshot->notes;
    for (auto note : trailingSnapshot->notes) {
        note.startTime += leadingDurationSeconds;
        note.endTime += leadingDurationSeconds;
        mergedNotes.push_back(note);
    }
    mergedNotes = normalizeStoredNotes(mergedNotes);

    ContentState mergedPayload;
    // 合并窗口端点直接取 leading start / trailing end（已 sample-aligned），
    // 并保留 leading 的 source identity（sourceId/sourcePersistentId）。
    mergedPayload.sourceWindow = leadingSnapshot->sourceWindow;
    mergedPayload.sourceWindow.sourceEndSeconds = trailingSnapshot->sourceWindow.sourceEndSeconds;
    mergedPayload.audioBuffer = mergedBuffer;
    mergedPayload.sampleRate = leadingSnapshot->audioSampleRate > 0.0 ? leadingSnapshot->audioSampleRate : TimeCoordinate::kRenderSampleRate;
    mergedPayload.analysis.pitchCurve = mergedPitchCurve;
    mergedPayload.analysis.setOriginalF0State(leadingSnapshot->originalF0State);
    mergedPayload.analysis.detectedKey = leadingSnapshot->detectedKey;
    mergedPayload.notes = std::move(mergedNotes);
    mergedPayload.noteTopologyInitialized = leadingSnapshot->noteTopologyInitialized
        || trailingSnapshot->noteTopologyInitialized;
    mergedPayload.analysis.silentGaps = mergeSilentGaps(leadingSnapshot->silentGaps, trailingSnapshot->silentGaps, leadingSamples);
    mergedPayload.pitchShiftSettings = leadingSnapshot->pitchShiftSettings;

    const ContentKey mergedKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                           *contentRenderService_,
                                                           std::move(mergedPayload));
    if (!mergedKey.isValid()) {
        return std::nullopt;
    }

    StandaloneArrangement::Placement mergedPlacement = leadingPlacement;
    mergedPlacement.placementId = 0;
    mergedPlacement.contentKey = mergedKey;
    mergedPlacement.durationSeconds = leadingPlacement.durationSeconds + trailingPlacement.durationSeconds;
    mergedPlacement.fadeOutDuration = trailingPlacement.fadeOutDuration;
    mergedPlacement.clipInSeconds = leadingPlacement.clipInSeconds;

    const int mergedInsertIndex = targetPlacementIndex >= 0 ? targetPlacementIndex : 0;
    if (!standaloneArrangement_->insertPlacement(trackId, mergedInsertIndex, mergedPlacement)) {
        standaloneContentRepository_->releaseClip(mergedKey);
        return std::nullopt;
    }

    standaloneArrangement_->retirePlacement(trackId, trailingPlacementId);
    standaloneArrangement_->retirePlacement(trackId, leadingPlacementId);
    standaloneContentRepository_->retireClip(leadingPlacement.contentKey);
    standaloneContentRepository_->retireClip(trailingPlacement.contentKey);

    if (targetPlacementIndex >= 0) {
        standaloneArrangement_->setSelectedPlacementIndex(trackId, targetPlacementIndex);
    } else {
        standaloneArrangement_->selectPlacement(trackId, mergedPlacement.placementId);
    }

    scheduleReclaimSweep();

    MergeOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = leadingSnapshot->sourceWindow.sourceId;
    outcome.leadingPlacementId = leadingPlacementId;
    outcome.trailingPlacementId = trailingPlacementId;
    outcome.leadingContentKey = leadingPlacement.contentKey;
    outcome.trailingContentKey = trailingPlacement.contentKey;
    outcome.mergedPlacementId = mergedPlacement.placementId;
    outcome.mergedContentKey = mergedKey;
    return outcome;
}

std::optional<DeleteOutcome> OpenTuneAudioProcessor::deletePlacement(int trackId, int placementIndex)
{
    StandaloneArrangement::Placement placement;
    if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
        return std::nullopt;
    }

    uint64_t sourceId = 0;
    if (const auto snap = getContentSnapshot(placement.contentKey)) {
        sourceId = snap->sourceWindow.sourceId;
    }

    standaloneArrangement_->retirePlacement(trackId, placement.placementId);
    standaloneContentRepository_->retireClip(placement.contentKey);
    scheduleReclaimSweep();

    DeleteOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = sourceId;
    outcome.placementId = placement.placementId;
    outcome.contentKey = placement.contentKey;
    return outcome;
}

void OpenTuneAudioProcessor::scheduleReclaimSweep()
{
    jassert(juce::MessageManager::getInstanceWithoutCreating() != nullptr);
    triggerAsyncUpdate();
}

void OpenTuneAudioProcessor::handleAsyncUpdate()
{
    runReclaimSweepOnMessageThread();
}
#endif // JucePlugin_Build_Standalone

#if JucePlugin_Build_VST3
void OpenTuneAudioProcessor::timerCallback()
{
    if (auto* session = getCaptureSession())
        session->tick();
}
#endif // JucePlugin_Build_VST3

#if JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::runReclaimSweepOnMessageThread()
{
    cancelPendingUpdate();

    const auto retiredPlacements = standaloneArrangement_->getRetiredPlacements();
    for (const auto& entry : retiredPlacements) {
        standaloneArrangement_->deletePlacementById(entry.trackId, entry.placementId, nullptr, nullptr);
    }

    const auto retiredClips = standaloneContentRepository_->getRetiredClips();
    for (const auto& key : retiredClips) {
        if (standaloneArrangement_->referencesContentAnyState(key)) {
            continue;
        }

        uint64_t sourceId = 0;
        if (auto* clip = standaloneContentRepository_->findClip(key)) {
            sourceId = clip->content().sourceWindow.sourceId;
        }

        if (contentRenderService_ != nullptr) {
            contentRenderService_->removePlaybackSource(key);
            contentRenderService_->removeRenderCache(key);
            contentRenderService_->removeStretcher(key);
            contentRenderService_->getTimeStretchCache().invalidate(key);
        }

        standaloneContentRepository_->releaseClip(key);

        if (sourceId != 0
            && sourceStore_ != nullptr
            && sourceStore_->containsSource(sourceId)
            && !standaloneRepositoryReferencesSource(*standaloneContentRepository_, sourceId)) {
            sourceStore_->retireSource(sourceId);
        }
    }

    if (sourceStore_ != nullptr) {
        const auto retiredSourceIds = sourceStore_->getRetiredSourceIds();
        for (const uint64_t sourceId : retiredSourceIds) {
            if (standaloneRepositoryReferencesSource(*standaloneContentRepository_, sourceId)) {
                continue;
            }
            sourceStore_->physicallyDeleteIfReclaimable(sourceId);
        }
    }
}
#endif // JucePlugin_Build_Standalone

// ============================================================================
// Content 读写代理接口
// ============================================================================

std::shared_ptr<const EditableContentSnapshot> OpenTuneAudioProcessor::getContentSnapshot(ContentKey key) const
{
    if (!key.isValid()) {
        return nullptr;
    }

    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            return clip ? clip->snapshotContent() : nullptr;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            const auto* dc = getDocumentController();
            return dc ? dc->readContentSnapshot(key) : nullptr;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            const auto* session = getCaptureSession();
            const auto* segment = session ? session->findSegmentByContentKey(key) : nullptr;
            return segment && segment->content ? segment->content->snapshotContent() : nullptr;
        }
#endif
        default:
            break;
    }

    return nullptr;
}

const ContentRenderService* OpenTuneAudioProcessor::resolveReadableContentRenderService(ContentKey key) const noexcept
{
    switch (key.domainKind) {
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            return dc != nullptr ? dc->getContentRenderService() : nullptr;
        }
#endif
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip:
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture:
#endif
            return contentRenderService_.get();
        default:
            break;
    }
    return nullptr;
}

RenderCache::ChunkStats OpenTuneAudioProcessor::getReadableContentChunkStats(ContentKey key) const noexcept
{
    const auto* readableCrs = resolveReadableContentRenderService(key);
    if (readableCrs == nullptr)
        return {};

    const auto renderCache = readableCrs->getRenderCache(key);
    return renderCache != nullptr ? renderCache->getChunkStats() : RenderCache::ChunkStats{};
}

ContentRenderService* OpenTuneAudioProcessor::resolveMutableLocalContentRenderService(ContentKey key) const noexcept
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip:
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture:
#endif
            return contentRenderService_.get();
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification:
            return nullptr;  // ARA CRS is owned by DC, processor must not mutate it
#endif
        default:
            break;
    }
    return nullptr;
}

OpenTuneAudioProcessor::AnalysisAudioProvider
OpenTuneAudioProcessor::resolveAnalysisAudioProvider(ContentKey key)
{
    AnalysisAudioProvider result;

    // 优先从 CRS PlaybackReadSource 获取（适用于所有域，包括 ARA）
    const ContentRenderService* readableCrs = resolveReadableContentRenderService(key);
    PlaybackReadSource readSource;
    if (readableCrs != nullptr
        && readableCrs->getPlaybackReadSource(key, readSource)
        && readSource.audioBuffer != nullptr
        && readSource.audioBuffer->getNumSamples() > 0
        && readSource.audioSampleRate > 0.0)
    {
        result.audioBuffer = readSource.audioBuffer;
        result.samples = readSource.audioBuffer->getReadPointer(0);
        result.numSamples = readSource.audioBuffer->getNumSamples();
        result.sampleRate = readSource.audioSampleRate;
        result.valid = true;
        return result;
    }

    if (auto snap = getContentSnapshot(key))
    {
        if (snap->audioBuffer
            && snap->audioBuffer->getNumSamples() > 0
            && snap->audioSampleRate > 0.0)
        {
            result.audioBuffer = snap->audioBuffer;
            result.samples = snap->audioBuffer->getReadPointer(0);
            result.numSamples = snap->audioBuffer->getNumSamples();
            result.sampleRate = snap->audioSampleRate;
            result.valid = true;
        }
    }

    return result;
}

void OpenTuneAudioProcessor::onContentLocalMutationCompleted(ContentKey key,
                                                              ContentEditRangeFrames affectedRange)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return;
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    onContentLocalMutationCompletedSeconds(
        key,
        static_cast<double>(affectedRange.startFrame) * secondsPerFrame,
        static_cast<double>(affectedRange.endFrameExclusive) * secondsPerFrame,
        std::move(snap));
}

void OpenTuneAudioProcessor::onContentLocalMutationCompletedSeconds(ContentKey key,
                                                                     double startSeconds,
                                                                     double endSeconds,
                                                                     std::shared_ptr<const EditableContentSnapshot> snapshot)
{
#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        auto* dc = getDocumentController();
        if (dc != nullptr)
        {
            dc->requestModificationRender(
                key, startSeconds, endSeconds, std::move(snapshot));
        }
        return;
    }
#endif

    // Non-ARA path: processor-local CRS
    requestRenderForLocalMutationRange(key,
                                       startSeconds,
                                       endSeconds,
                                       std::move(snapshot));
}

void OpenTuneAudioProcessor::onContentFullMutationCompleted(ContentKey key)
{
    auto snapshot = getContentSnapshot(key);
    if (!snapshot)
        return;

#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        auto* dc = getDocumentController();
        if (dc != nullptr)
        {
            dc->requestFullModificationRender(key, std::move(snapshot));
        }
        return;
    }
#endif

    // Non-ARA path
    const double durationSeconds = contentDurationSeconds(*snapshot);
    if (durationSeconds > 0.0)
        requestRenderForLocalMutationRange(key, 0.0, durationSeconds, std::move(snapshot));
}

void OpenTuneAudioProcessor::requestFullContentRender(ContentKey key)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;
    const double durationSeconds = contentDurationSeconds(*snap);
    if (durationSeconds <= 0.0) return;
    requestRenderForLocalMutationRange(key, 0.0, durationSeconds, std::move(snap));
}

void OpenTuneAudioProcessor::requestRenderForLocalMutationRange(ContentKey key,
                                                                 double startSeconds,
                                                                 double endSeconds,
                                                                 std::shared_ptr<const EditableContentSnapshot> snap)
{
    if (!snap) return;

    auto* crs = resolveMutableLocalContentRenderService(key);
    if (crs == nullptr) return;

    PlaybackReadSource readSource;
    if (!crs->getPlaybackReadSource(key, readSource)) return;

    const double crsSampleRate = readSource.audioSampleRate;
    auto audioBuffer = readSource.audioBuffer;
    if (crsSampleRate <= 0.0 || audioBuffer == nullptr || audioBuffer->getNumSamples() <= 0) return;

    if (!crs->republishPlaybackSource(key, snap))
        return;

    if (!snap->hasUsableOriginalF0()) return;

    const int64_t totalSamples = audioBuffer->getNumSamples();
    const int64_t startSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesFloor(startSeconds, crsSampleRate));
    const int64_t endSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesCeil(endSeconds, crsSampleRate));

    if (endSample <= startSample) return;

    RenderJob job;
    job.contentKey = key;
    job.renderCache = crs->getOrCreateRenderCache(key);
    job.audioBuffer = audioBuffer;
    job.audioSampleRate = crsSampleRate;
    job.startSample = startSample;
    job.endSampleExclusive = endSample;
    job.contentSnapshot = snap;

    crs->enqueueRender(std::move(job));
}

void OpenTuneAudioProcessor::enqueueStage2WhenCanonicalSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> snapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (key.domainKind == DomainKind::ARAAudioModification)
        return;

    if (snapshot == nullptr || audioBuffer == nullptr || snapshot->timeGrid->isIdentity())
        return;

    auto* crs = resolveMutableLocalContentRenderService(key);
    if (crs == nullptr)
        return;

    crs->enqueueStage2RebuildWhenCanonicalSettled(
        key, std::move(snapshot), std::move(audioBuffer), audioSampleRate);
}

void OpenTuneAudioProcessor::handleStage1ChunkSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> snapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
#if JucePlugin_Build_VST3
    // Only transition to Edited when ALL chunks are canonical settled.
    // Partial completion must not promote the segment prematurely.
    if (auto* session = getCaptureSession()) {
        auto cache = contentRenderService_->getRenderCache(key);
        if (cache && cache->isCanonicalSettled())
            session->onRenderComplete(key);
    }
#endif

    if (contentRenderService_ != nullptr
        && key.domainKind != DomainKind::ARAAudioModification)
    {
        enqueueStage2WhenCanonicalSettled(
            key,
            std::move(snapshot),
            std::move(audioBuffer),
            audioSampleRate);
    }
}

#if JucePlugin_Build_Standalone
// WAV文件写入辅助函数
static bool writeAudioBufferToWavFile(const juce::AudioBuffer<float>& buffer,
                                       const juce::File& file,
                                       juce::String* errorOut = nullptr)
{
    auto outFile = file;
    if (!outFile.hasFileExtension(".wav")) {
        outFile = outFile.withFileExtension(".wav");
    }
    outFile.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(outFile.createOutputStream());
    if (!stream) {
        if (errorOut) *errorOut = "无法创建输出文件";
        return false;
    }

    std::unique_ptr<juce::OutputStream> outStream(stream.release());

    auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(kExportSampleRateHz)
        .withNumChannels(buffer.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);

    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) {
        if (errorOut) *errorOut = "Unable to create WAV writer";
        return false;
    }

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

// 导出单个 placement 的音频
// ============================================================================
// 音频导出
// ============================================================================

bool OpenTuneAudioProcessor::exportPlacementAudio(int trackId, int placementIndex, const juce::File& file) {
    lastExportError_.clear();
    
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        lastExportError_ = "无效的轨道ID: " + juce::String(trackId);
        return false;
    }

    jassert(standaloneArrangement_ != nullptr);

    StandaloneArrangement::Placement placement;
    if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
        lastExportError_ = "无效的片段索引 " + juce::String(placementIndex);
        return false;
    }

    // 等待 RenderWorker 完成当前渲染，确保导出最新数据
    contentRenderService_->drainRenderWorker();

    PlaybackReadSource source;
    if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.hasAudio()) {
        lastExportError_ = "Placement audio is unavailable";
        return false;
    }

    const int64_t placementLen = TimeCoordinate::secondsToSamplesCeil(
        placement.durationSeconds, kExportSampleRateHz);
    if (placementLen <= 0) {
        lastExportError_ = "片段音频长度为零";
        return false;
    }
    
    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(placementLen));
    out.clear();
    
    renderPlacementForExport(*this,
                             StandaloneArrangement::PlaybackPlacement{
                                 placement.contentKey,
                                 placement.timelineStartSeconds,
                                 placement.durationSeconds,
                                 placement.clipInSeconds,
                                 placement.gain,
                                 placement.fadeInDuration,
                                 placement.fadeOutDuration
                             },
                             standaloneArrangement_->getTrackVolume(trackId),
                             0,
                             placementLen,
                             placement.timelineStartSeconds,
                             out,
                             placementLen,
                             source);
    
    return writeAudioBufferToWavFile(out, file, &lastExportError_);
}

bool OpenTuneAudioProcessor::exportTrackAudio(int trackId, const juce::File& file) {
    lastExportError_.clear();
    
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        lastExportError_ = "无效的轨道ID: " + juce::String(trackId);
        return false;
    }

    jassert(standaloneArrangement_ != nullptr);

    const int placementCount = standaloneArrangement_->getNumPlacements(trackId);
    if (placementCount <= 0) {
        lastExportError_ = "轨道 " + juce::String(trackId + 1) + " 没有音频片段";
        return false;
    }

    // 等待 RenderWorker 完成当前渲染，确保导出最新数据
    contentRenderService_->drainRenderWorker();

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
        StandaloneArrangement::Placement placement;
        if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
            continue;
        }
        PlaybackReadSource checkSource;
        if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, checkSource) || !checkSource.hasAudio()) {
            continue;
        }
        const int64_t placementEnd = TimeCoordinate::secondsToSamplesCeil(
            placement.timelineStartSeconds + placement.durationSeconds, kExportSr);
        totalLen = std::max(totalLen, placementEnd);
    }
    if (totalLen <= 0) {
        lastExportError_ = "音频总长度为零或无效";
        return false;
    }

    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(totalLen));
    out.clear();

    const float trackVolume = standaloneArrangement_->getTrackVolume(trackId);
    for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
        StandaloneArrangement::Placement placement;
        if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
            continue;
        }
        PlaybackReadSource source;
        if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.hasAudio()) {
            continue;
        }
        const int64_t placementStart = TimeCoordinate::secondsToSamplesFloor(
            placement.timelineStartSeconds, kExportSr);
        const int64_t placementEnd = TimeCoordinate::secondsToSamplesCeil(
            placement.timelineStartSeconds + placement.durationSeconds, kExportSr);
        renderPlacementForExport(*this,
                                 StandaloneArrangement::PlaybackPlacement{
                                     placement.contentKey,
                                     placement.timelineStartSeconds,
                                     placement.durationSeconds,
                                     placement.clipInSeconds,
                                     placement.gain,
                                     placement.fadeInDuration,
                                     placement.fadeOutDuration
                                 },
                                 trackVolume, placementStart, placementEnd - placementStart,
                                 0.0, out, totalLen, source);
    }

    return writeAudioBufferToWavFile(out, file, &lastExportError_);
}

bool OpenTuneAudioProcessor::exportMasterMixAudio(const juce::File& file) {
    jassert(standaloneArrangement_ != nullptr);

    const auto playbackSnapshot = standaloneArrangement_->loadPlaybackSnapshot();
    if (playbackSnapshot == nullptr) {
        return false;
    }

    // 等待 RenderWorker 完成当前渲染，确保导出最新数据
    contentRenderService_->drainRenderWorker();

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const auto& track = playbackSnapshot->tracks[static_cast<size_t>(trackId)];
        for (const auto& placement : track.placements) {
            PlaybackReadSource checkSource;
            if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, checkSource) || !checkSource.hasAudio()) {
                continue;
            }
            const int64_t placementEnd = TimeCoordinate::secondsToSamplesCeil(
                placement.timelineStartSeconds + placement.durationSeconds, kExportSr);
            totalLen = std::max(totalLen, placementEnd);
        }
    }
    if (totalLen <= 0) return false;

    juce::AudioBuffer<float> mix(kExportMasterNumChannels, static_cast<int>(totalLen));
    mix.clear();

    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const auto& track = playbackSnapshot->tracks[static_cast<size_t>(trackId)];
        
        if (playbackSnapshot->anySoloed) {
            if (!track.isSolo) continue;
        } else {
            if (track.isMuted) continue;
        }

        if (track.placements.empty()) continue;

        for (const auto& placement : track.placements) {
            PlaybackReadSource source;
            if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.hasAudio()) {
                continue;
            }
            const int64_t placementStart = TimeCoordinate::secondsToSamplesFloor(
                placement.timelineStartSeconds, kExportSr);
            const int64_t placementEnd = TimeCoordinate::secondsToSamplesCeil(
                placement.timelineStartSeconds + placement.durationSeconds, kExportSr);
            renderPlacementForExport(*this, placement, track.volume, placementStart,
                                     placementEnd - placementStart,
                                     0.0,
                                     mix, totalLen, source);
        }
    }

    return writeAudioBufferToWavFile(mix, file);
}

// ============================================================================
// 播放控制 (Standalone writes only) — control thread posts atomic commands;
// audio thread executes the state machine in processBlock.
// ============================================================================

void OpenTuneAudioProcessor::play() {
    const double posSec = playHeadState_.getPresentedPositionSeconds();

    playStartPosition_.store(posSec);

    controlSequence_.fetch_add(1, std::memory_order_acq_rel);
    playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
    playHeadState_.isPlaying.store(true, std::memory_order_release);
    playHeadState_.presentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    pendingPresentationTime_.store(posSec, std::memory_order_relaxed);
    pendingCompletionTime_.store(posSec, std::memory_order_relaxed);
    pendingTerminalPhase_.store(RuntimePhase::Playing, std::memory_order_relaxed);
    pendingCommand_.store(TransportCommand::Play, std::memory_order_relaxed);
    controlSequence_.fetch_add(1, std::memory_order_release);

    AppLogger::log("Playback: play posSec=" + juce::String(posSec, 3));
}

void OpenTuneAudioProcessor::pause() {
    const double posSec = playHeadState_.getPresentedPositionSeconds();

    controlSequence_.fetch_add(1, std::memory_order_acq_rel);
    playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
    playHeadState_.isPlaying.store(false, std::memory_order_release);
    playHeadState_.presentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    pendingPresentationTime_.store(posSec, std::memory_order_relaxed);
    pendingCompletionTime_.store(posSec, std::memory_order_relaxed);
    pendingTerminalPhase_.store(RuntimePhase::Paused, std::memory_order_relaxed);
    pendingCommand_.store(TransportCommand::Pause, std::memory_order_relaxed);
    controlSequence_.fetch_add(1, std::memory_order_release);

    AppLogger::log("Playback: pause posSec=" + juce::String(posSec, 3));
}

void OpenTuneAudioProcessor::stop() {
    const double posSec = playHeadState_.getPresentedPositionSeconds();

    controlSequence_.fetch_add(1, std::memory_order_acq_rel);
    playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
    playHeadState_.isPlaying.store(false, std::memory_order_release);
    playHeadState_.presentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    pendingPresentationTime_.store(posSec, std::memory_order_relaxed);
    pendingCompletionTime_.store(0.0, std::memory_order_relaxed);
    pendingTerminalPhase_.store(RuntimePhase::Stopped, std::memory_order_relaxed);
    pendingCommand_.store(TransportCommand::Stop, std::memory_order_relaxed);
    controlSequence_.fetch_add(1, std::memory_order_release);

    AppLogger::log("Playback: stop posSec=" + juce::String(posSec, 3));
}

void OpenTuneAudioProcessor::pauseAtPosition(double targetSeconds) {
    const double posSec = playHeadState_.getPresentedPositionSeconds();

    controlSequence_.fetch_add(1, std::memory_order_acq_rel);
    playHeadState_.timeInSeconds.store(posSec, std::memory_order_relaxed);
    playHeadState_.isPlaying.store(false, std::memory_order_release);
    playHeadState_.presentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    pendingPresentationTime_.store(posSec, std::memory_order_relaxed);
    pendingCompletionTime_.store(targetSeconds, std::memory_order_relaxed);
    pendingTerminalPhase_.store(RuntimePhase::Paused, std::memory_order_relaxed);
    pendingCommand_.store(TransportCommand::PauseAtPosition, std::memory_order_relaxed);
    controlSequence_.fetch_add(1, std::memory_order_release);

    AppLogger::log("Playback: pauseAtPosition posSec=" + juce::String(posSec, 3)
                   + " target=" + juce::String(targetSeconds, 3));
}

void OpenTuneAudioProcessor::setLoopEnabled(bool enabled) {
    playHeadState_.isLooping.store(enabled, std::memory_order_relaxed);
}
#endif // JucePlugin_Build_Standalone

void OpenTuneAudioProcessor::copyOutputSpectrum(SpectrumArray& spectrum,
                                                SpectrumArray& peaks) const noexcept
{
    outputSpectrumAnalyzer_.copySnapshot(spectrum, peaks);
}

#if JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::setPosition(double seconds) {
    const bool wasPlaying = playHeadState_.isPlaying.load(std::memory_order_acquire);

    playStartPosition_.store(seconds);

    controlSequence_.fetch_add(1, std::memory_order_acq_rel);
    playHeadState_.timeInSeconds.store(seconds, std::memory_order_relaxed);
    playHeadState_.presentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    pendingPresentationTime_.store(seconds, std::memory_order_relaxed);
    pendingCompletionTime_.store(seconds, std::memory_order_relaxed);
    pendingTerminalPhase_.store(wasPlaying ? RuntimePhase::Playing : RuntimePhase::Paused, std::memory_order_relaxed);
    pendingCommand_.store(TransportCommand::Seek, std::memory_order_relaxed);
    controlSequence_.fetch_add(1, std::memory_order_release);

    AppLogger::log("Playback: seek to " + juce::String(seconds, 3) + "s wasPlaying=" + (wasPlaying ? "true" : "false"));
}

// ============================================================================
// Canonical Standalone BPM / Time Signature setters
// These write ONLY the processor-owned canonical state. They never touch host
// transport atomics (hostTransportBpm_, hostTransportTimeSignatureNumerator_,
// hostTransportTimeSignatureDenominator_) — those are written exclusively by
// updateHostTransportSnapshot() from the host PlayHead PositionInfo.
// ============================================================================

void OpenTuneAudioProcessor::setBpm(double bpm) {
    bpm_ = juce::jlimit(1.0, 999.0, bpm);
}

void OpenTuneAudioProcessor::setTimeSignature(int numerator, int denominator) {
    // Denominator must be one of the valid powers-of-two from whole-note to 64th.
    // Validate denominator FIRST; write numerator+denominator atomically
    // to avoid numerator-only change on invalid denominator.
    switch (denominator) {
        case 1: case 2: case 4: case 8: case 16: case 32: case 64:
            timeSigNumerator_ = juce::jlimit(1, 64, numerator);
            timeSigDenominator_ = denominator;
            break;
        default: break; // reject invalid denominator, keep previous canonical pair
    }
}
#endif // JucePlugin_Build_Standalone

void OpenTuneAudioProcessor::setUiZoomPercent(int percent) noexcept {
    switch (percent) {
        case 75: case 90: case 100: case 110: case 125: case 150:
            uiZoomPercent_.store(percent, std::memory_order_relaxed);
            break;
        default:
            break; // reject invalid zoom percent, keep previous value
    }
}

int OpenTuneAudioProcessor::getUiZoomPercent() const noexcept {
    return uiZoomPercent_.load(std::memory_order_relaxed);
}

SnapSettings OpenTuneAudioProcessor::getSnapSettings() const {
    if (appPreferences_ != nullptr)
        return appPreferences_->getSnapSettings();
    return SnapSettings{};
}

void OpenTuneAudioProcessor::setSnapSettings(const SnapSettings& snap)
{
    if (appPreferences_ != nullptr) {
        appPreferences_->setSnapSettings(snap);
    }
}

#if JucePlugin_Build_Standalone
// ============================================================================
// Two-phase Import Implementation (Standalone-only)
// ============================================================================

bool OpenTuneAudioProcessor::prepareImport(juce::AudioBuffer<float>&& inBuffer,
                                           double inSampleRate,
                                           const juce::String& displayName,
                                           const juce::String& sourceFilePath,
                                           OpenTuneAudioProcessor::PreparedImport& out,
                                           const char* entrySourceTag)
{
    const int declaredChannels = inBuffer.getNumChannels();

    if (inBuffer.getNumSamples() <= 0) {
        AppLogger::log("Import rejected: empty audio buffer (zero samples) for '" + displayName + "'");
        ChannelLayoutLog::logReject(entrySourceTag, declaredChannels,
                                     "empty-buffer", displayName);
        return false;
    }
    if (declaredChannels <= 0) {
        AppLogger::log("Import rejected: invalid channel count=" + juce::String(declaredChannels) + " for '" + displayName + "'");
        ChannelLayoutLog::logReject(entrySourceTag, declaredChannels,
                                     "invalid-channel-count", displayName);
        return false;
    }
    if (declaredChannels > 2) {
        // Multichannel (>2) imports are explicitly unsupported per channel-layout-policy.
        AppLogger::log("Import rejected: multichannel (>2) audio not supported, channels=" + juce::String(declaredChannels) + " for '" + displayName + "'");
        ChannelLayoutLog::logReject(entrySourceTag, declaredChannels,
                                     "multichannel-not-supported", displayName);
        return false;
    }
    if (inSampleRate <= 0.0) {
        AppLogger::log("Import rejected: invalid sample rate=" + juce::String(inSampleRate, 2) + " for '" + displayName + "'");
        ChannelLayoutLog::logReject(entrySourceTag, declaredChannels,
                                     "invalid-sample-rate", displayName);
        return false;
    }

    // Storage layout exactly matches the declaration (1 = mono, 2 = stereo).
    ChannelLayoutLog::logEntry(entrySourceTag, declaredChannels, declaredChannels, displayName);

    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    const bool needsResample = std::abs(inSampleRate - targetSampleRate) > 1.0;
    // 存储统一为 canonical 44.1kHz：窗口 span 直接取最终 stored buffer 的
    // canonical sample 数，保证 window duration 与 stored audio 一致。
    const int64_t storedSampleCount = needsResample
        ? juce::jmax<int64_t>(1, TimeCoordinate::sampleRateProject(
              inBuffer.getNumSamples(), inSampleRate, targetSampleRate))
        : static_cast<int64_t>(inBuffer.getNumSamples());

    out.displayName = displayName;
    out.sourceFilePath = sourceFilePath;
    // 端点由最终 stored buffer 的 canonical sample 数直接生成，本身就在
    // source sample 网格上，无需再经对齐 round-trip。
    out.sourceWindow = SourceWindow{
        0,
        juce::String(),
        0.0,
        TimeCoordinate::samplesToSeconds(storedSampleCount, targetSampleRate)
    };

    if (needsResample) {
        const int numChannels = inBuffer.getNumChannels();
        const int originalLen = inBuffer.getNumSamples();
        const int newLen = static_cast<int>(storedSampleCount);

        out.storedAudioBuffer.setSize(numChannels, newLen);
        
        for (int ch = 0; ch < numChannels; ++ch) {
            auto resampledData = resamplingManager_->upsampleForHost(
                inBuffer.getReadPointer(ch),
                originalLen,
                static_cast<int>(inSampleRate),
                static_cast<int>(targetSampleRate)
            );
            const int toCopy = juce::jmin(newLen, static_cast<int>(resampledData.size()));
            out.storedAudioBuffer.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
    } else {
        out.storedAudioBuffer = std::move(inBuffer);
    }
    
    // 计算静默段间隙（同步执行，避免异步竞态）
    out.silentGaps = SilentGapDetector::detectAllGapsAdaptive(out.storedAudioBuffer);
    
    return true;
}

ContentKey OpenTuneAudioProcessor::ensureSourceAndCreateStandaloneClip(PreparedImport&& prepared, uint64_t& sourceId, bool& createdSource)
{
    jassert(sourceStore_ != nullptr);

    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(prepared.storedAudioBuffer));
    createdSource = false;
    if (sourceId == 0) {
        SourceStore::CreateSourceRequest sourceRequest;
        sourceRequest.displayName = prepared.displayName;
        sourceRequest.sourceFilePath = prepared.sourceFilePath;
        sourceRequest.audioBuffer = storedAudioBuffer;
        sourceRequest.sampleRate = TimeCoordinate::kRenderSampleRate;
        sourceId = sourceStore_->createSource(std::move(sourceRequest));
        if (sourceId == 0) return {};
        createdSource = true;
    } else if (!sourceStore_->containsSource(sourceId)) {
        return {};
    }

    SourceWindow sw = prepared.sourceWindow;
    sw.sourceId = sourceId;
    sw.sourcePersistentId.clear();

    ContentState payload;
    payload.sourceWindow = sw;
    payload.audioBuffer = storedAudioBuffer;
    payload.sampleRate = TimeCoordinate::kRenderSampleRate;
    payload.analysis.setOriginalF0State(OriginalF0State::NotRequested);
    payload.analysis.silentGaps = std::move(prepared.silentGaps);

    const ContentKey key = createStandaloneClipOwner(*standaloneContentRepository_,
                                                     *contentRenderService_,
                                                     std::move(payload));
    if (!key.isValid() && createdSource) {
        sourceStore_->deleteSource(sourceId);
    }
    return key;
}

OpenTuneAudioProcessor::CommittedPlacement OpenTuneAudioProcessor::commitPreparedImportAsPlacement(
    PreparedImport&& prepared, const ImportPlacement& placement, uint64_t sourceId)
{
    if (!placement.isValid()) return {};
    jassert(standaloneArrangement_ != nullptr);

    const juce::String displayName = prepared.displayName;
    bool createdSource = false;
    const ContentKey clipKey = ensureSourceAndCreateStandaloneClip(std::move(prepared), sourceId, createdSource);
    if (!clipKey.isValid()) return {};

    auto durationSnap = getContentSnapshot(clipKey);
    const double contentDuration = durationSnap
        ? contentDurationSeconds(*durationSnap)
        : 0.0;

    StandaloneArrangement::Placement importedPlacement;
    importedPlacement.contentKey = clipKey;
    importedPlacement.timelineStartSeconds = placement.timelineStartSeconds;
    importedPlacement.durationSeconds = contentDuration;
    importedPlacement.gain = 1.0f;
    importedPlacement.name = displayName;

    if (!standaloneArrangement_->insertPlacement(placement.trackId, importedPlacement)) {
        standaloneContentRepository_->releaseClip(clipKey);
        if (createdSource) sourceStore_->deleteSource(sourceId);
        return {};
    }
    standaloneArrangement_->selectPlacement(placement.trackId, importedPlacement.placementId);

    return { sourceId, clipKey, importedPlacement.placementId };
}
#endif // JucePlugin_Build_Standalone

// ============================================================================
// requestContentRefresh -- Standalone / regular VST3 F0 refresh.
// Normally refreshes F0 only -- does NOT run GAME note generation.
// An OpenDyne import request may additionally carry a one-shot whole-content
// note generation intent (generateNotesWholeContentOnReady), executed here
// once F0 is Ready.
// ============================================================================

bool OpenTuneAudioProcessor::requestContentRefresh(const OpenTuneAudioProcessor::ContentRefreshRequest& request)
{
    if (!request.contentKey.isValid()) {
        return false;
    }

    auto snap = getContentSnapshot(request.contentKey);
    if (snap == nullptr || snap->audioBuffer == nullptr) {
        return false;
    }

    const bool hasChangedRange = request.preserveCorrectionsOutsideChangedRange
        && request.changedEndSeconds > request.changedStartSeconds;

    if (hasChangedRange) {
        if (!snap->notes.empty()) {
            NoteSequence sequence;
            sequence.setNotesSorted(snap->notes);
            sequence.eraseRange(request.changedStartSeconds, request.changedEndSeconds);
            replaceContentNotesForFullMutation(request.contentKey, sequence.getNotes());
        }

        if (snap->pitchCurve != nullptr) {
            const auto pSnapshot = snap->pitchCurve;
            if (const auto frameRange = f0FrameRangeForSeconds(
                    *pSnapshot, request.changedStartSeconds, request.changedEndSeconds)) {
                auto clearedCurve = PitchCurve::fromSnapshot(snap->pitchCurve);
                clearedCurve->clearCorrectionRange(frameRange->startFrame,
                                                    frameRange->endFrameExclusive);
                if (!writePitchCurveToOwner(request.contentKey, std::move(clearedCurve))) {
                    return false;
                }
                onContentFullMutationCompleted(request.contentKey);
            }
        }
    }

    switch (request.contentKey.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(request.contentKey);
            if (clip) clip->applyOriginalF0State(OriginalF0State::Extracting);
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification:
            // Handled by DC path in setContentOriginalF0State
            break;
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture:
            setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting);
            break;
#endif
        default:
            break;
    }

    if (f0ExtractionService_->isActive(F0RequestKey{request.contentKey})) {
        f0ExtractionService_->cancel(F0RequestKey{request.contentKey});
    }

    auto gate = completionGate_;
    OpenTuneAudioProcessor* const processor = this;
    const auto capturedRequest = request;
    // 提交时捕获不可变内容快照：worker 只执行纯数据 F0 计算，owner 销毁后
    // 绝不访问裸 processor（快照 shared_ptr 与进程级 F0 服务随 worker 存活）。
    const auto capturedSnap = snap;

    const auto submitResult = f0ExtractionService_->submit(
        F0RequestKey{request.contentKey},
        [capturedSnap, capturedRequest](const std::shared_ptr<F0RunOwnerState>& runOwnerState) -> F0ExtractionService::Result {
            F0ExtractionService::Result result;
            result.contentKey = capturedRequest.contentKey;

            if (!capturedSnap || capturedSnap->audioBuffer == nullptr) {
                result.errorMessage = "content_snapshot_failed";
                return result;
            }

            result.sourceAudioBuffer = capturedSnap->audioBuffer;

            // 进程级 F0 服务（进程寿命，owner 销毁后仍安全）；惰性初始化保留原语义。
            auto& f0Runtime = ProcessF0Runtime::getInstance();
            if (!f0Runtime.isReady())
                f0Runtime.initialize(ModelPathResolver::getModelsDirectory());
            auto f0Service = f0Runtime.getF0Service();
            if (!f0Service) {
                result.errorMessage = "f0_service_unavailable";
                return result;
            }

            std::string errorMessage;
            if (!extractOriginalF0ForImportedClip(*f0Service, runOwnerState, *capturedSnap, result, errorMessage)) {
                result.errorMessage = errorMessage;
                return result;
            }

            result.success = true;
            return result;
        },
        [processor, gate, capturedRequest](F0ExtractionService::Result&& result) {
            // 持锁访问 processor：与析构置 closed 互斥。closed 后不再访问 owner。
            std::lock_guard<std::mutex> lk(gate->mutex);
            if (gate->closed)
                return;

            auto currentSnap = processor->getContentSnapshot(capturedRequest.contentKey);
            if (!currentSnap
                || currentSnap->audioBuffer == nullptr
                || result.sourceAudioBuffer == nullptr
                || currentSnap->audioBuffer != result.sourceAudioBuffer) {
                AppLogger::log("ContentRefresh: stale result dropped contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                return;
            }

            if (!result.success) {
                AppLogger::log("ContentRefresh: extraction failed contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId))
                    + " reason=" + juce::String(result.errorMessage));
                processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Failed);
                return;
            }

            auto pitchCurve = std::make_shared<PitchCurve>();
            pitchCurve->setHopSize(result.hopSize);
            pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
            pitchCurve->setOriginalF0(result.f0);
            if (!result.energy.empty()) {
                pitchCurve->setOriginalEnergy(result.energy);
            }

            if (capturedRequest.preserveCorrectionsOutsideChangedRange) {
                auto previousSnap = processor->getContentSnapshot(capturedRequest.contentKey);
                if (previousSnap && previousSnap->pitchCurve != nullptr) {
                    auto previousSnapshot = previousSnap->pitchCurve;
                    auto segments = previousSnapshot->getCorrectionSegments();
                    if (!segments.empty()) {
                        const int maxFrame = static_cast<int>(result.f0.size());

                        if (const auto changedRange = f0FrameRangeForSeconds(
                                result.hopSize,
                                static_cast<double>(result.f0SampleRate),
                                maxFrame,
                                capturedRequest.changedStartSeconds,
                                capturedRequest.changedEndSeconds)) {
                            const int changedStartFrame = changedRange->startFrame;
                            const int changedEndFrame = changedRange->endFrameExclusive;

                            segments.erase(std::remove_if(segments.begin(),
                                                          segments.end(),
                                                          [changedStartFrame, changedEndFrame, maxFrame](const PitchCorrectionSegment& segment) {
                                                              if (segment.startFrame >= maxFrame) return true;
                                                              const int clampedEnd = juce::jmin(segment.endFrame, maxFrame);
                                                              if (clampedEnd <= segment.startFrame) return true;
                                                              return clampedEnd > changedStartFrame
                                                                  && segment.startFrame < changedEndFrame;
                                                          }),
                                           segments.end());
                        } else {
                            segments.erase(std::remove_if(segments.begin(),
                                                          segments.end(),
                                                          [maxFrame](const PitchCorrectionSegment& segment) {
                                                              if (segment.startFrame >= maxFrame) return true;
                                                              return juce::jmin(segment.endFrame, maxFrame) <= segment.startFrame;
                                                          }),
                                           segments.end());
                        }

                        if (!segments.empty()) {
                            pitchCurve->replaceCorrectionSegments(segments);
                        }
                    }
                }
            }

            if (!processor->writeOriginalF0ToOwner(capturedRequest.contentKey, pitchCurve)) {
                AppLogger::log("ContentRefresh: OriginalF0 commit failed contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Failed);
                return;
            }
            // OriginalF0 只更新分析数据，不触发音频渲染
            // 音频渲染只在 Correction/Final F0 写入时触发

            {
                AppLogger::log("F0Alignment: contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId))
                    + " audioDuration=" + juce::String(result.audioDurationSeconds, 6)
                    + " firstAudibleTime=" + juce::String(result.firstAudibleTimeSeconds, 6)
                    + " firstVoicedFrame=" + juce::String(result.firstVoicedFrame)
                    + " firstVoicedTime=" + juce::String(result.firstVoicedTimeSeconds, 6)
                    + " f0FrameCount=" + juce::String(static_cast<int>(result.f0.size()))
                    + " expectedInferenceFrameCount=" + juce::String(result.expectedInferenceFrameCount));
            }

            processor->updateContentKeyFromOriginalF0(capturedRequest.contentKey);

            if (capturedRequest.generateNotesWholeContentOnReady) {
                // OpenDyne 导入：仅生成音符，不写修正曲线（还原 Melodyne 初始状态）。
                // 不吸附、不请求 render；成功回调（onNotesGenerated）推进调用方 dirty。
                if (processor->generateNotesOnlyByContentKey(
                        capturedRequest.contentKey,
                        capturedRequest.noteGenerationParams)) {
                    if (capturedRequest.onNotesGenerated) {
                        capturedRequest.onNotesGenerated();
                    }
                } else {
                    AppLogger::log("ContentRefresh: note generation failed contentKey objectId="
                        + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                }
            }

            processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Ready);
            if (processor->pendingTimeToolSeedKeys_.count(capturedRequest.contentKey) != 0)
                processor->ensureTimeToolAnchorSeed(capturedRequest.contentKey);

            // F0 就绪后请求完整渲染：此前因无 F0 而 Blank 的 chunk 需重新渲染
            processor->requestFullContentRender(capturedRequest.contentKey);
        });

    if (submitResult != F0ExtractionService::SubmitResult::Accepted) {
        AppLogger::log("ContentRefresh: submit rejected contentKey objectId="
            + juce::String(static_cast<juce::int64>(request.contentKey.objectId)));
        setContentOriginalF0State(request.contentKey, OriginalF0State::Failed);
        return false;
    }

    return true;
}

// ============================================================================
// Placement Movement (Standalone-only)
// ============================================================================

#if JucePlugin_Build_Standalone
bool OpenTuneAudioProcessor::movePlacementToTrack(int sourceTrackId,
                                                  int targetTrackId,
                                                  uint64_t placementId,
                                                  double newTimelineStartSeconds)
{
    jassert(standaloneArrangement_ != nullptr);
    return standaloneArrangement_->movePlacementToTrack(sourceTrackId, targetTrackId, placementId, newTimelineStartSeconds);
}
#endif // JucePlugin_Build_Standalone

void OpenTuneAudioProcessor::updateContentKeyFromOriginalF0(ContentKey key)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return;

    if (snap->detectedKey.origin == Origin::Manual) return;

    const auto curveSnapshot = snap->pitchCurve;
    const auto& originalF0 = curveSnapshot->getOriginalF0();
    if (originalF0.empty()) return;

    const auto& energies = curveSnapshot->getOriginalEnergy();
    F0KeyDetector detector;
    const auto detectedKey = detector.detect(originalF0, energies);
    if (detectedKey.origin == Origin::Unset) return;   // 无有效帧
    setContentDetectedKey(key, detectedKey);
}

PitchShiftSettings OpenTuneAudioProcessor::getPitchShiftSettings(ContentKey key) const
{
    auto snap = getContentSnapshot(key);
    return snap ? snap->pitchShiftSettings : PitchShiftSettings{};
}

ReferenceFeatureSet OpenTuneAudioProcessor::getReferenceFeatures(ContentKey key) const
{
    auto snap = getContentSnapshot(key);
    if (!snap) return {};
    return snap->referenceFeatures;
}

// ============================================================================
// vocal-time-stretch §3.6 -- TimeGrid processor accessors
// ============================================================================

bool OpenTuneAudioProcessor::ensureTimeToolAnchorSeed(ContentKey key)
{
    if (!key.isValid()) {
        return false;
    }

    auto snap = getContentSnapshot(key);
    if (!snap) return false;

    const auto existingGrid = snap->timeGrid;
    if (existingGrid != nullptr) {
        const auto& existingHandles = existingGrid->handles();
        const bool hasInternalOnset = std::any_of(existingHandles.begin(), existingHandles.end(),
                                                  [](const TimeHandle& handle) {
                                                      return handle.kind == HandleKind::InternalOnset;
                                                  });
        if (hasInternalOnset || !existingGrid->isIdentity()) {
            return true;
        }
    }

    const auto desiredProducer = resolveReferenceFeatureProducer();
    auto& features = snap->referenceFeatures;
    const bool featuresReady = features.isReady()
        && features.producer == desiredProducer
        && features.inputFingerprint == static_cast<int64_t>(snap->audioRevision)
        && features.hasTimingAnchors();

    if (!featuresReady) {
        pendingTimeToolSeedKeys_.insert(key);
        const auto preheatStatus = preheatReferenceAlignmentFeatures(key);
        if (preheatStatus == ReferenceAnalysisPreheatStatus::InvalidContent
            || preheatStatus == ReferenceAnalysisPreheatStatus::AnalysisFailed) {
            pendingTimeToolSeedKeys_.erase(key);
            return false;
        }
        return false;
    }

    pendingTimeToolSeedKeys_.erase(key);

    const double durationSeconds = snap->timeGrid->totalDurationSeconds();
    if (!(durationSeconds > 0.0)) {
        return false;
    }

    std::vector<TimeHandle> handles;
    handles.reserve(features.timing.anchors.size() + 2);

    uint64_t nextHandleId = 1;
    auto pushHandle = [&](double sourceSeconds,
                          double outputSeconds,
                          HandleKind kind,
                          Confidence confidence = Confidence::Default) {
        TimeHandle handle;
        handle.id = nextHandleId++;
        handle.source_seconds = sourceSeconds;
        handle.output_seconds = outputSeconds;
        handle.kind = kind;
        handle.confidence = confidence;
        handles.push_back(handle);
    };

    pushHandle(0.0, 0.0, HandleKind::ClipStart);

    std::vector<double> eventTimes;
    eventTimes.reserve(features.timing.anchors.size());
    for (const auto& event : features.timing.anchors) {
        const double eventTime = juce::jlimit(0.0, durationSeconds, event.sourceSeconds);
        if (eventTime <= 0.0 || eventTime >= durationSeconds) {
            continue;
        }

        eventTimes.push_back(eventTime);
    }

    std::sort(eventTimes.begin(), eventTimes.end());
    eventTimes.erase(std::unique(eventTimes.begin(), eventTimes.end(),
                                  [](double a, double b) { return std::abs(a - b) < 0.005; }),
                      eventTimes.end());

    double lastAcceptedSource = 0.0;
    for (const double eventTime : eventTimes) {
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(lastAcceptedSource, eventTime)) {
            continue;
        }
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(eventTime, durationSeconds)) {
            continue;
        }

        pushHandle(eventTime,
                   eventTime,
                   HandleKind::InternalOnset);
        lastAcceptedSource = eventTime;
    }

    pushHandle(durationSeconds,
               durationSeconds,
               HandleKind::ClipEnd);

    auto seededGrid = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (seededGrid == nullptr) {
        return false;
    }

    return setContentTimeGrid(key, std::move(seededGrid));
}

OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus
OpenTuneAudioProcessor::preheatReferenceAlignmentFeatures(ContentKey key)
{
    if (!key.isValid() || key.domainKind != DomainKind::StandaloneClip) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    auto snap = getContentSnapshot(key);
    if (!snap) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    const auto producer = resolveReferenceFeatureProducer();
    const auto inputFingerprint = static_cast<int64_t>(snap->audioRevision);
    auto& features = snap->referenceFeatures;
    if (producer == ReferenceFeatureProducer::StandardAuto) {
        switch (snap->originalF0State) {
            case OriginalF0State::NotRequested: {
                ContentRefreshRequest refreshRequest;
                refreshRequest.contentKey = key;
                refreshRequest.preserveCorrectionsOutsideChangedRange = true;
                return requestContentRefresh(refreshRequest)
                    ? ReferenceAnalysisPreheatStatus::WaitingForSource
                    : ReferenceAnalysisPreheatStatus::AnalysisFailed;
            }
            case OriginalF0State::Extracting:
                return ReferenceAnalysisPreheatStatus::WaitingForSource;
            case OriginalF0State::Failed:
                return ReferenceAnalysisPreheatStatus::AnalysisFailed;
            case OriginalF0State::Ready:
                break;
        }

        if (snap->pitchCurve == nullptr
            || snap->pitchCurve->getOriginalF0().empty()) {
            return ReferenceAnalysisPreheatStatus::AnalysisFailed;
        }
    }

    if (features.isReady()
        && features.producer == producer
        && features.inputFingerprint == inputFingerprint) {
        return ReferenceAnalysisPreheatStatus::AlreadyReady;
    }

    if (features.status == ReferenceFeatureStatus::Failed
        && features.producer == producer
        && features.inputFingerprint == inputFingerprint) {
        return ReferenceAnalysisPreheatStatus::AnalysisFailed;
    }

    if (features.status == ReferenceFeatureStatus::Extracting
        && features.producer == producer
        && features.inputFingerprint == inputFingerprint) {
        return ReferenceAnalysisPreheatStatus::Queued;
    }

    ReferenceFeatureSet extracting;
    extracting.analysisRevision = features.analysisRevision + 1;
    extracting.producer = producer;
    extracting.status = ReferenceFeatureStatus::Extracting;
    extracting.inputFingerprint = inputFingerprint;
    extracting.sourceDurationSeconds = snap->audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(snap->audioBuffer->getNumSamples(),
                                            snap->audioSampleRate)
        : 0.0;
    setContentReferenceFeatures(key, extracting);

    // 提交时捕获不可变快照/音频与 analysisRevision：worker 只执行纯数据分析，
    // 不访问 processor（owner 销毁后仍安全）。completion 经消息线程执行，
    // 以 completion gate 决定是否访问 owner。
    const int analysisRevision = extracting.analysisRevision;
    const std::shared_ptr<const EditableContentSnapshot> analysisSnap = snap;
    AnalysisAudioProvider audio;
    double sourceDurationSeconds = 0.0;
    if (producer == ReferenceFeatureProducer::Game) {
        audio = resolveAnalysisAudioProvider(key);
        sourceDurationSeconds = audio.valid
            ? TimeCoordinate::samplesToSeconds(audio.numSamples, audio.sampleRate)
            : 0.0;
    }

    auto gate = completionGate_;
    auto* processor = this;
    referenceAnalysisService_->submitAnalysis(
        key, inputFingerprint, producer,
        [analysisSnap, audio, sourceDurationSeconds, analysisRevision](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
            ReferenceFeatureSet failed;
            failed.producer = jobKey.producer;
            failed.inputFingerprint = jobKey.inputFingerprint;
            failed.analysisRevision = analysisRevision;

            if (jobKey.producer == ReferenceFeatureProducer::Game) {
                // 进程级共享 GAME generator：ProcessF0Runtime::generateNotes。
                if (!audio.valid || audio.numSamples <= 0) {
                    failed.status = ReferenceFeatureStatus::Failed;
                    failed.errorMessage = "AUTO Ref GAME analysis requires content audio";
                    return failed;
                }
                NoteGeneratorInput input;
                input.sampleRate = audio.sampleRate;
                input.audio.assign(audio.samples, audio.samples + audio.numSamples);
                const auto gameNotes = ProcessF0Runtime::getInstance().generateNotes(input);
                return makeReferenceFeatureSetFromNotes(
                    ReferenceFeatureProducer::Game,
                    jobKey.inputFingerprint,
                    analysisRevision,
                    sourceDurationSeconds,
                    gameNotes,
                    "AUTO Ref GAME analysis found no notes or timing anchors");
            }

            // StandardAuto：提交时捕获的纯数据快照。
            if (!analysisSnap) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis could not read content snapshot";
                return failed;
            }
            if (static_cast<int64_t>(analysisSnap->audioRevision) != jobKey.inputFingerprint) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis job is stale";
                return failed;
            }
            return buildStandardAutoReferenceFeatureSet(*analysisSnap, analysisRevision);
        },
        [processor, gate](const ReferenceAnalysisService::AnalysisJobKey& jobKey,
                          const ReferenceFeatureSet& result) {
            // 消息线程执行；与析构置 closed 互斥。closed 后不再访问 owner。
            std::lock_guard<std::mutex> lk(gate->mutex);
            if (gate->closed)
                return;
            processor->analysisFinished(jobKey.contentKey, result);
        });
    return ReferenceAnalysisPreheatStatus::Queued;
}

#if JucePlugin_Build_Standalone
OpenTuneAudioProcessor::ReferenceAlignmentResult
OpenTuneAudioProcessor::executeReferenceAlignmentForPlacement(uint64_t targetPlacementId)
{
    ReferenceAlignmentResult result;

    if (targetPlacementId == 0 || standaloneArrangement_ == nullptr) {
        result.status = ReferenceAlignmentResult::Status::TargetPlacementNotFound;
        result.message = "AUTO Ref target placement is not available";
        return result;
    }

    int targetTrackId = -1;
    StandaloneArrangement::Placement targetPlacement;
    if (!findPlacementByIdGlobal(*standaloneArrangement_, targetPlacementId, targetTrackId, targetPlacement)) {
        result.status = ReferenceAlignmentResult::Status::TargetPlacementNotFound;
        result.message = "AUTO Ref target placement is not available";
        return result;
    }

    const uint64_t referencePlacementId =
        standaloneArrangement_->getPlacementReferencePlacement(targetTrackId, targetPlacementId);
    if (referencePlacementId == 0) {
        result.status = ReferenceAlignmentResult::Status::NoReferenceBinding;
        result.message = "AUTO Ref target has no reference clip binding";
        return result;
    }

    if (referencePlacementId == targetPlacementId) {
        result.status = ReferenceAlignmentResult::Status::SelfReference;
        result.message = "AUTO Ref cannot align a clip to itself";
        return result;
    }

    int referenceTrackId = -1;
    StandaloneArrangement::Placement referencePlacement;
    if (!findPlacementByIdGlobal(*standaloneArrangement_, referencePlacementId, referenceTrackId, referencePlacement)) {
        result.status = ReferenceAlignmentResult::Status::ReferencePlacementNotFound;
        result.message = "AUTO Ref reference placement is not available";
        return result;
    }
    juce::ignoreUnused(referenceTrackId);

    const double overlapStart = std::max(targetPlacement.timelineStartSeconds,
                                         referencePlacement.timelineStartSeconds);
    const double overlapEnd = std::min(targetPlacement.timelineEndSeconds(),
                                       referencePlacement.timelineEndSeconds());
    if (overlapEnd <= overlapStart) {
        result.status = ReferenceAlignmentResult::Status::NoOverlap;
        result.message = "AUTO Ref target and reference clips do not overlap";
        return result;
    }

    auto targetSnap = getContentSnapshot(targetPlacement.contentKey);
    auto referenceSnap = getContentSnapshot(referencePlacement.contentKey);
    if (!targetSnap) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target analysis could not read content snapshot";
        return result;
    }
    if (!referenceSnap) {
        result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
        result.message = "AUTO Ref reference analysis could not read content snapshot";
        return result;
    }

    const auto desiredProducer = resolveReferenceFeatureProducer();
    const auto featureIsCurrent = [desiredProducer](const ReferenceFeatureSet& features,
                                                     const EditableContentSnapshot& snapshot) {
        return features.isReady()
            && features.producer == desiredProducer
            && features.inputFingerprint == static_cast<int64_t>(snapshot.audioRevision);
    };

    const ReferenceFeatureSet targetFeatures = targetSnap->referenceFeatures;
    if (!featureIsCurrent(targetFeatures, *targetSnap)) {
        const auto preheatStatus = preheatReferenceAlignmentFeatures(targetPlacement.contentKey);
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = targetFeatures.errorMessage.isNotEmpty()
            ? targetFeatures.errorMessage
            : preheatStatus == ReferenceAnalysisPreheatStatus::AnalysisFailed
                ? juce::String("AUTO Ref target source analysis failed")
                : juce::String("AUTO Ref target analysis is pending");
        return result;
    }

    const ReferenceFeatureSet referenceFeatures = referenceSnap->referenceFeatures;
    if (!featureIsCurrent(referenceFeatures, *referenceSnap)) {
        const auto preheatStatus = preheatReferenceAlignmentFeatures(referencePlacement.contentKey);
        result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
        result.message = referenceFeatures.errorMessage.isNotEmpty()
            ? referenceFeatures.errorMessage
            : preheatStatus == ReferenceAnalysisPreheatStatus::AnalysisFailed
                ? juce::String("AUTO Ref reference source analysis failed")
                : juce::String("AUTO Ref reference analysis is pending");
        return result;
    }

    const auto oldNotes = targetSnap->notes;
    const auto oldCurve = targetSnap->pitchCurve;
    if (oldCurve == nullptr) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target pitch curve is not available";
        return result;
    }

    const auto oldSegments = oldCurve->getCorrectionSegments();
    const double targetDurationSeconds = targetSnap->sourceWindow.isValid()
        ? targetSnap->sourceWindow.durationSeconds()
        : targetFeatures.sourceDurationSeconds;
    const double referenceDurationSeconds = referenceSnap->sourceWindow.isValid()
        ? referenceSnap->sourceWindow.durationSeconds()
        : referenceFeatures.sourceDurationSeconds;
    if (!(targetDurationSeconds > 0.0) || !(referenceDurationSeconds > 0.0)) {
        result.status = ReferenceAlignmentResult::Status::InsufficientFeatures;
        result.message = "AUTO Ref requires positive target and reference durations";
        return result;
    }

    ReferenceAlignmentRequest request;
    request.target.placementId = targetPlacement.placementId;
    request.target.contentKey = targetPlacement.contentKey;
    request.target.timelineStartSeconds = targetPlacement.timelineStartSeconds;
    request.target.timelineEndSeconds = targetPlacement.timelineEndSeconds();
    request.target.sourceStartSeconds = targetPlacement.clipInSeconds;
    request.target.sourceEndSeconds = targetPlacement.clipInSeconds + targetPlacement.durationSeconds;
    request.reference.placementId = referencePlacement.placementId;
    request.reference.contentKey = referencePlacement.contentKey;
    request.reference.timelineStartSeconds = referencePlacement.timelineStartSeconds;
    request.reference.timelineEndSeconds = referencePlacement.timelineEndSeconds();
    request.reference.sourceStartSeconds = referencePlacement.clipInSeconds;
    request.reference.sourceEndSeconds = referencePlacement.clipInSeconds + referencePlacement.durationSeconds;
    request.targetTimeMap = EffectiveTimeMap::fromTimeGrid(targetSnap->timeGrid, targetDurationSeconds);
    request.referenceTimeMap = EffectiveTimeMap::fromTimeGrid(referenceSnap->timeGrid, referenceDurationSeconds);
    request.targetFeatures = targetFeatures;
    request.referenceFeatures = referenceFeatures;
    request.targetNotesBefore = oldNotes;
    request.overlapStartTimelineSeconds = overlapStart;
    request.overlapEndTimelineSeconds = overlapEnd;

    auto patch = ReferenceAutoAlign::align(request);
    if (!patch.success) {
        switch (patch.error) {
            case AlignmentPatch::ErrorCode::NoOverlap:
                result.status = ReferenceAlignmentResult::Status::NoOverlap;
                break;
            case AlignmentPatch::ErrorCode::TargetAnalysisNotReady:
                result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
                break;
            case AlignmentPatch::ErrorCode::ReferenceAnalysisNotReady:
                result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
                break;
            case AlignmentPatch::ErrorCode::InsufficientFeatures:
                result.status = ReferenceAlignmentResult::Status::InsufficientFeatures;
                break;
            case AlignmentPatch::ErrorCode::NoMutation:
                result.status = ReferenceAlignmentResult::Status::NoMutation;
                break;
            case AlignmentPatch::ErrorCode::InvalidRequest:
            case AlignmentPatch::ErrorCode::None:
                result.status = ReferenceAlignmentResult::Status::CommitFailed;
                break;
        }
        result.message = patch.diagnostics;
        result.targetContentKey = targetPlacement.contentKey;
        return result;
    }

    const auto oldPitchSnapshot = oldCurve;
    const int hopSize = oldPitchSnapshot->getHopSize();
    const double pitchSampleRate = oldPitchSnapshot->getSampleRate();
    const int frameCount = static_cast<int>(oldPitchSnapshot->getOriginalF0().size());
    if (hopSize <= 0 || pitchSampleRate <= 0.0 || frameCount <= 0) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target F0 coordinates are not available";
        return result;
    }

    const auto affectedFrameRange = f0FrameRangeForSeconds(
        hopSize,
        pitchSampleRate,
        frameCount,
        patch.affectedSourceStartSeconds,
        patch.affectedSourceEndSeconds);
    if (!affectedFrameRange.has_value() || affectedFrameRange->isEmpty()) {
        result.status = ReferenceAlignmentResult::Status::NoOverlap;
        result.message = "AUTO Ref overlap maps to an empty F0 range";
        return result;
    }
    const int affectedStartFrame = affectedFrameRange->startFrame;
    const int affectedEndFrame = affectedFrameRange->endFrameExclusive;

    const auto& normalizedNotes = patch.notesAfter;
    const auto correctionRange = PitchCurve::expandNoteBasedCorrectionRange(
        affectedStartFrame, affectedEndFrame, frameCount);
    const ContentEditRangeFrames commitRange{
        correctionRange.startFrame,
        correctionRange.endFrameExclusive
    };

    const double secondsPerFrame = static_cast<double>(hopSize) / pitchSampleRate;
    const double rangeStartSeconds = static_cast<double>(commitRange.startFrame) * secondsPerFrame;
    const double rangeEndSeconds = static_cast<double>(commitRange.endFrameExclusive) * secondsPerFrame;

    auto derivedCurve = PitchCurve::fromSnapshot(oldCurve);
    derivedCurve->applyCorrectionToRange(
        normalizedNotes,
        affectedStartFrame,
        affectedEndFrame,
        static_cast<float>(targetSnap->pitchShiftSettings.getPitchRatio()),
        PitchControlConfig::kDefaultRetuneSpeedNormalized,
        PitchControlConfig::kDefaultVibratoDepth,
        PitchControlConfig::kDefaultVibratoRateHz);

    const auto segmentsInRange = clipSegmentsToFrameRange(
        derivedCurve->copyCorrectionSegments(),
        commitRange.startFrame,
        commitRange.endFrameExclusive);
    auto beforeNotesScoped = filterNotesToRange(oldNotes, rangeStartSeconds, rangeEndSeconds);
    auto beforeSegmentsScoped = clipSegmentsToFrameRange(
        oldSegments,
        commitRange.startFrame,
        commitRange.endFrameExclusive);

    const auto commitSnap = commitContentNotesAndSegments(
        targetPlacement.contentKey,
        normalizedNotes,
        segmentsInRange,
        commitRange);
    if (commitSnap == nullptr) {
        result.status = ReferenceAlignmentResult::Status::CommitFailed;
        result.message = "AUTO Ref could not commit the pitch patch";
        result.targetContentKey = targetPlacement.contentKey;
        result.affectedStartFrame = commitRange.startFrame;
        result.affectedEndFrame = commitRange.endFrameExclusive;
        return result;
    }

    undoManager_.addAction(std::make_unique<PianoRollEditAction>(
        contentCommands_,
        targetPlacement.contentKey,
        "AUTO (Ref)",
        std::move(beforeNotesScoped),
        filterNotesToRange(commitSnap->notes, rangeStartSeconds, rangeEndSeconds),
        std::move(beforeSegmentsScoped),
        clipSegmentsToFrameRange(commitSnap->pitchCurve->getCorrectionSegments(),
                                 commitRange.startFrame,
                                 commitRange.endFrameExclusive),
        commitRange));

    result.status = ReferenceAlignmentResult::Status::Succeeded;
    result.message = "AUTO Ref pitch correction applied";
    result.targetContentKey = targetPlacement.contentKey;
    result.affectedStartFrame = commitRange.startFrame;
    result.affectedEndFrame = commitRange.endFrameExclusive;
    return result;
}
#endif // JucePlugin_Build_Standalone

bool OpenTuneAudioProcessor::replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(normalizeStoredNotes(std::move(notes)));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyNotesToModification(key, normalizeStoredNotes(std::move(notes)));
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyNotes(key, normalizeStoredNotes(std::move(notes))))
                return false;
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (!ok)
        return false;

    // 发布由调用方统一负责：Scissors 提交后调用 republishPlaybackSource；
    // requestContentRefresh 经 onContentFullMutationCompleted → render 请求内
    // requestRenderForLocalMutationRange 的 republishPlaybackSource 重建包络。
    // 此处不再发布，避免同一变更两次 publish（不含包络的中间态双发布）。
    return true;
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotesAndSegments(ContentKey key,
                                                            std::vector<Note> notesInRange,
                                                            std::vector<PitchCorrectionSegment> segments,
                                                            ContentEditRangeFrames affectedRange)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return {};

    // Range-scoped notes merge (mergeNotesRange semantics, same as
    // commitContentNoteTopologyPatch; the helper filters incoming notes by range)
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    const double rangeStartSec = static_cast<double>(affectedRange.startFrame) * secondsPerFrame;
    const double rangeEndSec   = static_cast<double>(affectedRange.endFrameExclusive) * secondsPerFrame;

    auto normalizedNotes = mergeNotesRange(
        snap->notes,
        NoteRangeSeconds{ rangeStartSec, rangeEndSec },
        filterNotesToRange(notesInRange, rangeStartSec, rangeEndSec));

    // Range-scoped segments merge with split-preserve for boundary-crossing segments.
    // When a segment crosses the affected range boundary, the outside parts are kept.
    // Example: old segment [0,100], edit range [40,60] → keep [0,40] and [60,100], replace [40,60].
    auto mergedSegments = replaceSegmentsInFrameRange(
        snap->pitchCurve->getCorrectionSegments(),
        clipSegmentsToFrameRange(segments, affectedRange.startFrame, affectedRange.endFrameExclusive),
        affectedRange.startFrame,
        affectedRange.endFrameExclusive);

    auto newCurve = PitchCurve::fromSnapshot(snap->pitchCurve);
    newCurve->replaceCorrectionSegments(mergedSegments);
    if (!newCurve) return {};

    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return {};
            clip->applyNotes(std::move(normalizedNotes));
            clip->applyPitchCurve(std::move(newCurve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return {};
            if (!dc->applyNotesToModification(key, std::move(normalizedNotes))) return {};
            if (newCurve) dc->applyPitchCurveToModification(key, std::move(newCurve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr
                || !session->applyNotesAndPitchCurve(key, std::move(normalizedNotes), std::move(newCurve))) {
                return {};
            }
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok) {
        onContentLocalMutationCompleted(key, affectedRange);
        auto committedSnap = getContentSnapshot(key);
        jassert(committedSnap != nullptr);
        return committedSnap;
    }
    return {};
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNoteTopologyPatch(ContentKey key, ContentNoteRangePatch patch)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return {};

    auto normalizedNotes = mergeNotesRange(snap->notes, patch.affectedRange, patch.afterNotesInRange);

    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return {};
            clip->applyNotes(std::move(normalizedNotes));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return {};
            ok = dc->applyNotesToModification(key, std::move(normalizedNotes));
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr) return {};
            ok = session->applyNotes(key, std::move(normalizedNotes));
            break;
        }
#endif
        default:
            break;
    }

    if (ok) {
        // 根因修复：拓扑 patch 提交后按 patch.affectedRange（秒域）调度局部重渲染，
        // Undo/Redo 经同一 command（PianoRollNotePatchAction）自动重渲染。
        // onContentLocalMutationCompletedSeconds 内部刷新/发布 playback source 并调度 Stage1。
        auto committedSnap = getContentSnapshot(key);
        onContentLocalMutationCompletedSeconds(
            key,
            patch.affectedRange.startSeconds,
            patch.affectedRange.endSeconds,
            committedSnap);
        jassert(committedSnap != nullptr);
        return committedSnap;
    }
    return {};
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitVolumeEnvelope(ContentKey key, AutomationLane envelope)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return {};
            clip->applyVolumeEnvelope(std::move(envelope));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return {};
            ok = dc->applyVolumeEnvelopeToModification(key, std::move(envelope));
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr) return {};
            ok = session->applyVolumeEnvelope(key, std::move(envelope));
            break;
        }
#endif
        default:
            break;
    }

    if (!ok) return {};

    republishPlaybackSource(key);
    return getContentSnapshot(key);
}

void OpenTuneAudioProcessor::republishPlaybackSource(ContentKey key)
{
#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        auto* dc = getDocumentController();
        if (dc != nullptr)
            dc->republishPlaybackSourceForModification(key);
        return;
    }
#endif

    auto snap = getContentSnapshot(key);
    if (!snap) return;
    auto* crs = resolveMutableLocalContentRenderService(key);
    if (crs == nullptr) return;
    crs->republishPlaybackSource(key, std::move(snap));
}

bool OpenTuneAudioProcessor::writePitchCurveToOwner(ContentKey key,
                                                      std::shared_ptr<PitchCurve> curve)
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyPitchCurve(std::move(curve));
            return true;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyPitchCurveToModification(key, std::move(curve));
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyPitchCurve(key, std::move(curve)))
                return false;
            return true;
        }
#endif
        default:
            break;
    }
    return false;
}

bool OpenTuneAudioProcessor::writeOriginalF0ToOwner(ContentKey key,
                                                      std::shared_ptr<PitchCurve> curve)
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyOriginalF0(std::move(curve));
            return true;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyOriginalF0ToModification(key, std::move(curve));
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr) return false;
            auto* seg = session->findSegmentByContentKey(key);
            if (!seg || !seg->content) return false;
            seg->content->applyOriginalF0(std::move(curve));
            return true;
        }
#endif
        default:
            break;
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentTimeGrid(ContentKey key,
                                                  std::shared_ptr<const TimeGridSnapshot> grid)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyTimeGrid(grid);
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyTimeGridToModification(key, std::move(grid));
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyTimeGrid(key, std::move(grid)))
                return false;
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok) {
        auto snapshot = getContentSnapshot(key);
        auto* crs = resolveMutableLocalContentRenderService(key);
        if (key.domainKind != DomainKind::ARAAudioModification
            && crs != nullptr
            && snapshot != nullptr)
        {
            // TimeGrid affects only Stage2. Requeue a full Stage1 plan so any
            // pending jobs carry the new snapshot, then let the canonical-settled
            // callback enqueue Stage2 if the plan is still running.
            if (!crs->republishPlaybackSource(key, snapshot))
                return ok;
            crs->getTimeStretchCache().invalidate(key);
            requestFullContentRender(key);
        }
        else if (key.domainKind == DomainKind::ARAAudioModification
                 && crs != nullptr
                 && snapshot != nullptr)
            crs->republishPlaybackSource(key, std::move(snapshot));
    }
    return ok;
}

bool OpenTuneAudioProcessor::setContentDetectedKey(ContentKey key, const DetectedKey& detectedKey)
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyDetectedKey(detectedKey);
            return true;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyDetectedKeyToModification(key, detectedKey);
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            return session != nullptr && session->applyDetectedKey(key, detectedKey);
        }
#endif
        default:
            break;
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentReferenceFeatures(ContentKey key,
                                                          const ReferenceFeatureSet& features)
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyReferenceFeatures(features);
            return true;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyReferenceFeaturesToModification(key, features);
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            seg->content->applyReferenceFeatures(features);
            return true;
        }
#endif
        default:
            break;
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentOriginalF0State(ContentKey key, OriginalF0State state)
{
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyOriginalF0State(state);
            return true;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyOriginalF0StateToModification(key, state);
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            seg->content->applyOriginalF0State(state);
            return true;
        }
#endif
        default:
            break;
    }
    return false;
}

bool OpenTuneAudioProcessor::applyContentPitchShiftState(ContentKey key,
                                                          const PitchShiftEditState& state)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            ok = clip->applyPitchShiftState(state);
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyPitchShiftStateToModification(key, state);
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyPitchShiftState(key, state))
                return false;
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok) {
        onContentFullMutationCompleted(key);
    }
    return ok;
}

std::unique_ptr<PitchShiftEditAction> OpenTuneAudioProcessor::commitPitchShiftEdit(
    ContentKey key,
    const PitchShiftSettings& newSettings)
{
    const auto snap = getContentSnapshot(key);
    if (snap == nullptr || snap->pitchCurve == nullptr)
        return {};

    const auto& oldSettings = snap->pitchShiftSettings;
    if (oldSettings.getTotalCents() == newSettings.getTotalCents())
        return {};

    PitchShiftEditState before;
    before.settings = oldSettings;
    before.notes = snap->notes;
    before.segments = snap->pitchCurve->getCorrectionSegments();

    PitchShiftEditState after = before;
    after.settings = newSettings;
    const float delta = static_cast<float>(newSettings.getPitchRatio() / oldSettings.getPitchRatio());

    for (auto& note : after.notes) {
        if (note.pitch > 0.0f)
            note.pitch *= delta;
        if (note.originalPitch > 0.0f)
            note.originalPitch *= delta;
    }

    for (auto& segment : after.segments) {
        for (auto& f0 : segment.f0Data) {
            if (f0 > 0.0f)
                f0 *= delta;
        }
    }

    if (!applyContentPitchShiftState(key, after))
        return {};

    return std::make_unique<PitchShiftEditAction>(
        contentCommands_, key, std::move(before), std::move(after));
}

std::optional<std::vector<Note>> OpenTuneAudioProcessor::generateNotesFromOriginalF0(
    const std::shared_ptr<const PitchCurveSnapshot>& curveSnapshot,
    int startFrame, int endFrameExclusive, const NoteGeneratorParams& params)
{
    if (curveSnapshot == nullptr) return std::nullopt;
    const auto& originalF0 = curveSnapshot->getOriginalF0();
    const int f0Count = static_cast<int>(originalF0.size());
    if (f0Count == 0) return std::nullopt;
    if (startFrame < 0 || endFrameExclusive <= startFrame || endFrameExclusive > f0Count) return std::nullopt;
    const int hopSize = curveSnapshot->getHopSize();
    const double sampleRate = curveSnapshot->getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0) return std::nullopt;
    auto generatedNotes = LegacyNoteGenerator::generate(
        originalF0.data(), f0Count, nullptr, startFrame, endFrameExclusive,
        hopSize, sampleRate, params);
    if (!LegacyNoteGenerator::validate(generatedNotes)) return std::nullopt;
    return generatedNotes;   // 空 vector 通过验证，作为合法结果返回
}

bool OpenTuneAudioProcessor::generateNotesOnlyByContentKey(
    ContentKey key, const NoteGeneratorParams& params)
{
    // 单一快照：读取一次，curveSnapshot 贯穿生成、秒域范围、拓扑提交
    auto snap = getContentSnapshot(key);
    if (snap == nullptr || snap->pitchCurve == nullptr) return false;
    const auto curveSnapshot = snap->pitchCurve;

    auto generatedNotes = generateNotesFromOriginalF0(curveSnapshot, 0,
        curveSnapshot ? static_cast<int>(curveSnapshot->getOriginalF0().size()) : 0, params);
    if (!generatedNotes.has_value()) return false;

    // 初始状态不量化：保持 originalPitch，用户点击 SNAP 时再吸附到音阶。
    // Melodyne 工作流：导入后 only note creation, no pitch shift.
    for (auto& note : *generatedNotes) {
        if (note.originalPitch > 0.0f)
            note.pitch = note.originalPitch;
    }

    const double secondsPerFrame = static_cast<double>(curveSnapshot->getHopSize())
                                 / curveSnapshot->getSampleRate();
    // 生成音符可能因 tailExtendMs 超出 f0Count 秒域范围：
    // affected range 必须覆盖生成音符实际范围，mergeNotesRange 不做范围过滤
    const int f0Count = static_cast<int>(curveSnapshot->getOriginalF0().size());
    double rangeStartSec = 0.0;
    double rangeEndSec = static_cast<double>(f0Count) * secondsPerFrame;
    for (const auto& n : *generatedNotes) {
        rangeStartSec = std::min(rangeStartSec, n.startTime);
        rangeEndSec = std::max(rangeEndSec, n.endTime);
    }

    ContentNoteRangePatch patch;
    patch.affectedRange.startSeconds = rangeStartSec;
    patch.affectedRange.endSeconds = rangeEndSec;
    patch.afterNotesInRange = std::move(*generatedNotes);   // 空 vector 也提交（清除该 range 音符）

    // 拓扑提交：merge notes + 推进 notes/content revision + republish，
    // 不推进 pitchRevision、不请求 render（ContentEditCommands.h 契约）
    return commitContentNoteTopologyPatch(key, std::move(patch)) != nullptr;
}

bool OpenTuneAudioProcessor::autoTuneContentRangeByContentKey(
    ContentKey key,
    int startFrame,
    int endFrameExclusive,
    const NoteGeneratorParams& params,
    const std::optional<ScaleSnapConfig>& scaleSnap)
{
    auto snap = getContentSnapshot(key);
    if (snap == nullptr || snap->pitchCurve == nullptr) return false;

    const auto curveSnapshot = snap->pitchCurve;

    auto generatedNotes = generateNotesFromOriginalF0(
        curveSnapshot, startFrame, endFrameExclusive, params);
    if (!generatedNotes.has_value()) return false;

    if (scaleSnap.has_value()) {
        scaleSnap->applyToNotes(*generatedNotes);
    }

    return commitAutoTuneGeneratedNotesByContentKey(
        key,
        std::move(*generatedNotes),
        startFrame,
        endFrameExclusive,
        params.retuneSpeed,
        params.vibratoDepth,
        params.vibratoRate);
}

bool OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByContentKey(ContentKey key,
                                                                        std::vector<Note> generatedNotes,
                                                                        int startFrame,
                                                                        int endFrameExclusive,
                                                                        float retuneSpeed,
                                                                        float vibratoDepth,
                                                                        float vibratoRate)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return false;

    if (endFrameExclusive <= startFrame) return false;
    auto normalizedNotes = normalizeStoredNotes(std::move(generatedNotes));
    if (normalizedNotes.empty()) return false;

    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    const double rangeStartTime = static_cast<double>(startFrame) * secondsPerFrame;
    const double rangeEndTime = static_cast<double>(endFrameExclusive) * secondsPerFrame;

    auto existingNotes = snap->notes;
    std::vector<Note> mergedNotes;
    mergedNotes.reserve(existingNotes.size() + normalizedNotes.size());
    for (const auto& note : existingNotes)
        if (note.endTime <= rangeStartTime || note.startTime >= rangeEndTime)
            mergedNotes.push_back(note);
    for (auto& note : normalizedNotes) {
        mergedNotes.push_back(note);
    }
    std::sort(mergedNotes.begin(), mergedNotes.end(),
        [](const Note& a, const Note& b) { return a.startTime < b.startTime; });

    auto derivedCurve = PitchCurve::fromSnapshot(snap->pitchCurve);
    derivedCurve->applyCorrectionToRange(mergedNotes,
                                         startFrame,
                                         endFrameExclusive,
                                         static_cast<float>(snap->pitchShiftSettings.getPitchRatio()),
                                         retuneSpeed,
                                         vibratoDepth,
                                         vibratoRate);

    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(std::move(mergedNotes));
            clip->applyPitchCurve(std::move(derivedCurve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            if (!dc->applyNotesToModification(key, std::move(mergedNotes))) return false;
            if (derivedCurve) dc->applyPitchCurveToModification(key, std::move(derivedCurve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Build_VST3
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr
                || !session->applyAutoTuneGeneratedNotes(key, std::move(mergedNotes), std::move(derivedCurve))) {
                return false;
            }
            ok = true;
            break;
        }
#endif
        default:
            break;
    }

    if (!ok) return false;

    // render range 必须覆盖 applyCorrectionToRange 的 calculationRange（两侧各
    // expand getCorrectedF0BoundaryContextFrames 帧），否则边界处 chunk 保留旧
    // 音频，与新 correction segment 产生不连续 → click。
    const int maxFrame = static_cast<int>(snap->pitchCurve->getOriginalF0().size());
    const auto expandedRange = PitchCurve::expandNoteBasedCorrectionRange(
        startFrame, endFrameExclusive, maxFrame);
    onContentLocalMutationCompleted(key, ContentEditRangeFrames{ expandedRange.startFrame, expandedRange.endFrameExclusive });
    return true;
}

// ============================================================================
// Clipboard -- content range copy for paste/duplicate (Standalone-only)
// ============================================================================

#if JucePlugin_Build_Standalone
ContentKey OpenTuneAudioProcessor::copyContentRange(ContentKey sourceContentKey,
                                                           double offsetSeconds,
                                                           double durationSeconds,
                                                           const juce::String& newName)
{
    juce::ignoreUnused(newName);
    if (!sourceContentKey.isValid() || durationSeconds <= 0.0) {
        return ContentKey{};
    }

    const auto sourceSnap = getContentSnapshot(sourceContentKey);
    if (!sourceSnap || sourceSnap->audioBuffer == nullptr || sourceSnap->audioBuffer->getNumSamples() == 0) {
        return ContentKey{};
    }

    const int64_t totalSamples = sourceSnap->audioBuffer->getNumSamples();
    const double sampleRate = sourceSnap->audioSampleRate > 0.0 ? sourceSnap->audioSampleRate : TimeCoordinate::kRenderSampleRate;
    // offsetSeconds/durationSeconds 是父 content-local 时间；映射到 source-absolute
    // 坐标只依赖父窗口起点：start = parent.sourceStart + offset，
    // end = start + duration。父 sourceEndSeconds 不参与派生。
    SourceWindow requestedWindow = sourceSnap->sourceWindow;
    requestedWindow.sourceStartSeconds += offsetSeconds;
    requestedWindow.sourceEndSeconds = requestedWindow.sourceStartSeconds + durationSeconds;
    const auto alignedWindow = alignSourceWindowToSampleGrid(requestedWindow, sampleRate);
    if (!alignedWindow.has_value())
        return ContentKey{};

    // slice offset 与 local 区间出自同一个 aligned absolute window；local 0 是父
    // buffer sample 0（父窗口已采样对齐）。
    const int64_t parentStartSample = TimeCoordinate::secondsToSamplesNearest(
        sourceSnap->sourceWindow.sourceStartSeconds, sampleRate);
    const int64_t offsetSamples = TimeCoordinate::secondsToSamplesNearest(
        alignedWindow->sourceStartSeconds, sampleRate) - parentStartSample;
    const int64_t endOffsetSamples = TimeCoordinate::secondsToSamplesNearest(
        alignedWindow->sourceEndSeconds, sampleRate) - parentStartSample;
    if (offsetSamples < 0 || endOffsetSamples <= offsetSamples || endOffsetSamples > totalSamples) {
        return ContentKey{};
    }

    // local seconds 是绝对时间合同：由 aligned absolute window 与父窗口起点之差派生。
    // offsetSamples/endOffsetSamples 只用于 audio/silentGaps 的离散访问，
    // 不从样本反算 local seconds。
    const double localStartSeconds =
        alignedWindow->sourceStartSeconds - sourceSnap->sourceWindow.sourceStartSeconds;
    const double localEndSeconds =
        alignedWindow->sourceEndSeconds - sourceSnap->sourceWindow.sourceStartSeconds;
    ContentState payload;
    payload.sourceWindow = *alignedWindow;
    payload.audioBuffer = sliceAudioBuffer(sourceSnap->audioBuffer,
                                           offsetSamples,
                                           endOffsetSamples);
    payload.sampleRate = sampleRate;
    payload.analysis.pitchCurve = slicePitchCurveToLocalRange(sourceSnap->pitchCurve,
                                                     localStartSeconds,
                                                     localEndSeconds);
    payload.analysis.setOriginalF0State(sourceSnap->originalF0State);
    payload.analysis.detectedKey = sourceSnap->detectedKey;
    payload.notes = sliceNotesToLocalRange(sourceSnap->notes, localStartSeconds, localEndSeconds);
    payload.noteTopologyInitialized = sourceSnap->noteTopologyInitialized;
    payload.analysis.silentGaps = sliceSilentGaps(sourceSnap->silentGaps, offsetSamples, endOffsetSamples);
    payload.pitchShiftSettings = sourceSnap->pitchShiftSettings;

    // 截断失败时保留 bootstrap identity，由 createStandaloneClipOwner
    // 用真实 slice duration 覆盖。
    if (auto rangeGrid = buildCopiedRangeTimeGrid(*sourceSnap->timeGrid,
                                                  localStartSeconds,
                                                  localEndSeconds - localStartSeconds)) {
        payload.timeGrid = std::move(rangeGrid);
    }

    const ContentKey newKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                        *contentRenderService_,
                                                        std::move(payload));
    return newKey;
}

ContentKey OpenTuneAudioProcessor::cloneContent(ContentKey sourceContentKey,
                                                      const juce::String& newName)
{
    juce::ignoreUnused(newName);
    if (!sourceContentKey.isValid()) {
        return ContentKey{};
    }

    const auto sourceSnap = getContentSnapshot(sourceContentKey);
    if (!sourceSnap || sourceSnap->audioBuffer == nullptr || sourceSnap->audioBuffer->getNumSamples() == 0) {
        return ContentKey{};
    }

    ContentState payload = contentStateFromSnapshot(*sourceSnap);
    payload.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(*sourceSnap->audioBuffer);

    const ContentKey newKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                        *contentRenderService_,
                                                        std::move(payload));
    return newKey;
}
#endif // JucePlugin_Build_Standalone

// ============================================================================
// Plugin piano roll session memory (message thread only, no locks)
// ============================================================================

void OpenTuneAudioProcessor::rememberPianoRollViewport(
    PianoRollPlacementIdentity placement, PianoRollViewportPrimitive viewport)
{
    auto& remembered = pianoRollSession_.remembered;
    const auto it = std::find_if(remembered.begin(), remembered.end(),
                                 [&placement](const auto& entry) { return entry.first == placement; });
    if (it != remembered.end())
        it->second = viewport;
    else
        remembered.emplace_back(placement, viewport);
}

std::optional<PianoRollViewportPrimitive> OpenTuneAudioProcessor::readPianoRollViewport(
    const PianoRollPlacementIdentity& placement) const
{
    const auto& remembered = pianoRollSession_.remembered;
    const auto it = std::find_if(remembered.begin(), remembered.end(),
                                 [&placement](const auto& entry) { return entry.first == placement; });
    if (it == remembered.end())
        return std::nullopt;
    return it->second;
}

} // namespace OpenTune

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenTune::OpenTuneAudioProcessor();
}
