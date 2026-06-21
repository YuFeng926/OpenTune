#include "PluginProcessor.h"
#include "SourceStore.h"
#include "StandaloneArrangement.h"
#include "Editor/EditorFactory.h"
#include "DSP/ResamplingManager.h"
#include "DSP/MelSpectrogram.h"
#include "Services/F0ExtractionService.h"
#include "Services/ImportedClipF0Extraction.h"
#include "Utils/ModelPathResolver.h"
#include "Utils/AppLogger.h"
#include "Utils/ChannelLayoutLogger.h"
#include "Plugin/Capture/CaptureSession.h"
#include "DSP/ReferenceAutoAlign.h"
#include "DSP/TimeGridPatchBuilder.h"
#include <onnxruntime_cxx_api.h>
#include "Utils/AccelerationDetector.h"
#include "Utils/TimeCoordinate.h"
#include "Inference/GameNoteGenerator.h"      // GAME backend (Standalone / regular VST3)
#include "Utils/LegacyNoteGenerator.h"        // Legacy fallback
#include "Utils/CompositeUndoAction.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/TimeGridEditAction.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif
#include "Content/ContentEditCommands.h"

namespace OpenTune {

void fillF0GapsForVocoder(std::vector<float>& f0,
                          const std::shared_ptr<const PitchCurveSnapshot>& snap,
                          double frameStartTimeSec,
                          double frameEndTimeSec,
                          double hopDuration,
                          double f0FrameRate,
                          bool allowTrailingExtension);

// ============================================================================
// Export Helper Functions (Anonymous Namespace)
// ============================================================================
namespace {

constexpr double kExportSampleRateHz = 44100.0;
constexpr int kExportNumChannels = 1;
constexpr int kExportMasterNumChannels = 2;
constexpr int kExportBitsPerSample = static_cast<int>(sizeof(float) * 8);

struct AutoRefGameBackendProbe {
    bool forceLegacy = false;
    bool gameBundlePresent = false;
    bool currentBackendIsGame = false;
    bool currentBackendIsLegacy = false;
};

AutoRefGameBackendProbe probeAutoRefGameBackendLocked(INoteGenerator* generator, const juce::String& modelsDir)
{
    AutoRefGameBackendProbe probe;
    const auto envBackend = juce::SystemStats::getEnvironmentVariable("OPENTUNE_NOTE_BACKEND", {})
                                .trim()
                                .toLowerCase();
    probe.forceLegacy = (envBackend == "legacy");

    const auto gameDir = juce::File(modelsDir).getChildFile("GAME");
    probe.gameBundlePresent = gameDir.getChildFile("encoder.onnx").existsAsFile();
    probe.currentBackendIsGame = dynamic_cast<GameNoteGenerator*>(generator) != nullptr;
    probe.currentBackendIsLegacy = generator != nullptr && !probe.currentBackendIsGame;
    return probe;
}

juce::String makeAutoRefGameUnavailableMessage(const AutoRefGameBackendProbe& probe)
{
    if (probe.forceLegacy) {
        return juce::String("Reference clip is bound, but Legacy backend is forced; using regular AUTO.");
    }

    if (probe.currentBackendIsLegacy && !probe.gameBundlePresent) {
        return juce::String("Reference clip is bound, but GAME backend/models are missing; using regular AUTO.");
    }

    if (!probe.gameBundlePresent) {
        return juce::String("Reference clip is bound, but GAME backend/models are missing; using regular AUTO.");
    }

    return juce::String("Reference clip is bound, but GAME backend is not ready; using regular AUTO.");
}

std::vector<CorrectedSegment> copyCorrectedSegments(const std::shared_ptr<PitchCurve>& curve)
{
    std::vector<CorrectedSegment> copiedSegments;
    if (curve == nullptr) {
        return copiedSegments;
    }

    const auto snapshot = curve->getSnapshot();
    copiedSegments.reserve(snapshot->getCorrectedSegments().size());
    for (const auto& segment : snapshot->getCorrectedSegments()) {
        copiedSegments.push_back(segment);
    }
    return copiedSegments;
}

std::vector<CorrectedSegment> correctedSegmentsFromCurve(const std::shared_ptr<PitchCurve>& curve)
{
    return copyCorrectedSegments(curve);
}

ContentPayloadState payloadFromSnapshot(const EditableContentSnapshot& snap)
{
    ContentPayloadState payload;
    payload.sourceWindow = snap.sourceWindow;
    payload.audioBuffer = snap.audioBuffer;
    payload.sampleRate = snap.audioSampleRate > 0.0 ? snap.audioSampleRate : TimeCoordinate::kRenderSampleRate;
    payload.pitchCurve = snap.pitchCurve;
    payload.originalF0State = snap.originalF0State;
    payload.detectedKey = snap.detectedKey;
    payload.silentGaps = snap.silentGaps;
    payload.referenceFeatures = snap.referenceFeatures;
    payload.notes = snap.notes;
    payload.correctedSegments = snap.correctedSegments;
    payload.timeGrid = snap.timeGrid;
    payload.pitchShiftSettings = snap.pitchShiftSettings;
    payload.notesRevision = snap.notesRevision;
    payload.pitchRevision = snap.pitchRevision;
    payload.timeGridRevision = snap.timeGridRevision;
    payload.pitchShiftRevision = snap.pitchShiftRevision;
    payload.contentRevision = snap.contentRevision;
    payload.audioRevision = snap.audioRevision;
    payload.lifecycle = ContentLifecycle::Ready;
    return payload;
}

void publishStandalonePlaybackSource(ContentRenderService& crs,
                                     ContentKey key,
                                     const ContentPayloadState& payload)
{
    PlaybackReadSource readSource;
    readSource.contentKey = key;
    readSource.audioBuffer = payload.audioBuffer;
    readSource.audioSampleRate = payload.sampleRate;
    readSource.renderCache = crs.getOrCreateRenderCache(key);
    readSource.timeStretchCache = &crs.getTimeStretchCache();
    readSource.pitchRevision = payload.pitchRevision;
    readSource.timeGridRevision = payload.timeGridRevision;
    readSource.pitchShiftRevision = payload.pitchShiftRevision;
    readSource.pitchShiftSettings = payload.pitchShiftSettings;
    readSource.timeGridIsIdentity = payload.timeGrid == nullptr || payload.timeGrid->isIdentity();
    crs.publishPlaybackSource(key, std::move(readSource));
}

ContentKey createStandaloneClipOwner(StandaloneContentRepository& repository,
                                     ContentRenderService& crs,
                                     ContentPayloadState payload,
                                     uint64_t forcedId = 0)
{
    const ContentKey key = repository.createClip(forcedId);
    auto* clip = key.isValid() ? repository.findClip(key) : nullptr;
    if (clip == nullptr) {
        return {};
    }

    payload.lifecycle = ContentLifecycle::Ready;
    clip->payload() = std::move(payload);
    publishStandalonePlaybackSource(crs, key, clip->payload());
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
        if (clip != nullptr && clip->payload().sourceWindow.sourceId == sourceId) {
            return true;
        }
    }

    return false;
}

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

inline float computePlacementFadeGain(int64_t sampleInPlacement,
                                      int64_t placementLengthSamples,
                                      int64_t fadeInSamples,
                                      int64_t fadeOutSamples) {
    float fade = 1.0f;
    if (fadeInSamples > 1 && sampleInPlacement < fadeInSamples) {
        fade *= static_cast<float>(sampleInPlacement) / static_cast<float>(fadeInSamples - 1);
    }
    if (fadeOutSamples > 1 && (placementLengthSamples - 1 - sampleInPlacement) < fadeOutSamples) {
        fade *= static_cast<float>(placementLengthSamples - 1 - sampleInPlacement) / static_cast<float>(fadeOutSamples - 1);
    }
    return fade;
}

bool hasRemainingPlacementForContent(const StandaloneArrangement& arrangement, ContentKey contentKey)
{
    if (!contentKey.isValid()) {
        return false;
    }

    for (int trackId = 0; trackId < arrangement.getNumTracks(); ++trackId) {
        const int placementCount = arrangement.getNumPlacements(trackId);
        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            if (arrangement.getPlacementByIndex(trackId, placementIndex, placement)
                && placement.contentKey == contentKey) {
                return true;
            }
        }
    }

    return false;
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

std::shared_ptr<PitchCurve> slicePitchCurveToLocalRange(const std::shared_ptr<PitchCurve>& pitchCurve,
                                                        double startSeconds,
                                                        double endSeconds)
{
    if (pitchCurve == nullptr || endSeconds <= startSeconds) {
        return nullptr;
    }

    const auto snapshot = pitchCurve->getSnapshot();
    const int hopSize = snapshot->getHopSize();
    const double sampleRate = snapshot->getSampleRate();
    if (hopSize <= 0 || sampleRate <= 0.0) {
        return nullptr;
    }

    const double frameRate = sampleRate / static_cast<double>(hopSize);
    if (frameRate <= 0.0) {
        return nullptr;
    }

    const int startFrame = juce::jlimit(0,
                                        static_cast<int>(snapshot->getOriginalF0().size()),
                                        static_cast<int>(std::floor(startSeconds * frameRate)));
    const int endFrame = juce::jlimit(startFrame,
                                      static_cast<int>(snapshot->getOriginalF0().size()),
                                      static_cast<int>(std::ceil(endSeconds * frameRate)));
    if (endFrame <= startFrame) {
        return nullptr;
    }

    auto slicedCurve = std::make_shared<PitchCurve>();
    slicedCurve->setHopSize(hopSize);
    slicedCurve->setSampleRate(sampleRate);

    const auto& originalF0 = snapshot->getOriginalF0();
    slicedCurve->setOriginalF0(std::vector<float>(originalF0.begin() + startFrame, originalF0.begin() + endFrame));

    const auto& originalEnergy = snapshot->getOriginalEnergy();
    if (originalEnergy.size() >= static_cast<size_t>(endFrame)) {
        slicedCurve->setOriginalEnergy(std::vector<float>(originalEnergy.begin() + startFrame, originalEnergy.begin() + endFrame));
    }

    std::vector<CorrectedSegment> slicedSegments;
    for (const auto& segment : snapshot->getCorrectedSegments()) {
        const int overlapStart = std::max(segment.startFrame, startFrame);
        const int overlapEnd = std::min(segment.endFrame, endFrame);
        if (overlapEnd <= overlapStart) {
            continue;
        }

        CorrectedSegment slicedSegment = segment;
        const int originalOffsetStart = overlapStart - segment.startFrame;
        const int originalOffsetEnd = overlapEnd - segment.startFrame;
        slicedSegment.startFrame = overlapStart - startFrame;
        slicedSegment.endFrame = overlapEnd - startFrame;
        slicedSegment.f0Data.assign(segment.f0Data.begin() + originalOffsetStart,
                                    segment.f0Data.begin() + originalOffsetEnd);
        slicedSegments.push_back(std::move(slicedSegment));
    }
    slicedCurve->replaceCorrectedSegments(slicedSegments);
    return slicedCurve;
}

bool detectedKeysMatch(const DetectedKey& lhs, const DetectedKey& rhs)
{
    return lhs.root == rhs.root
        && lhs.scale == rhs.scale
        && std::abs(lhs.confidence - rhs.confidence) <= 1.0e-6f;
}

bool nearlyEqualSeconds(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= (1.0 / TimeCoordinate::kRenderSampleRate);
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

std::shared_ptr<PitchCurve> mergePitchCurves(const std::shared_ptr<PitchCurve>& leadingCurve,
                                             const std::shared_ptr<PitchCurve>& trailingCurve,
                                             OriginalF0State leadingState,
                                             OriginalF0State trailingState)
{
    if (leadingState != trailingState) {
        return nullptr;
    }

    if (leadingCurve == nullptr || trailingCurve == nullptr) {
        return (leadingCurve == nullptr && trailingCurve == nullptr) ? std::shared_ptr<PitchCurve>{} : nullptr;
    }

    const auto leadingSnapshot = leadingCurve->getSnapshot();
    const auto trailingSnapshot = trailingCurve->getSnapshot();
    if (leadingSnapshot == nullptr
        || trailingSnapshot == nullptr
        || leadingSnapshot->getHopSize() <= 0
        || leadingSnapshot->getHopSize() != trailingSnapshot->getHopSize()
        || std::abs(leadingSnapshot->getSampleRate() - trailingSnapshot->getSampleRate()) > 1.0e-6) {
        return nullptr;
    }

    std::vector<float> mergedOriginalF0 = leadingSnapshot->getOriginalF0();
    const auto& trailingOriginalF0 = trailingSnapshot->getOriginalF0();
    mergedOriginalF0.insert(mergedOriginalF0.end(), trailingOriginalF0.begin(), trailingOriginalF0.end());

    std::vector<float> mergedOriginalEnergy = leadingSnapshot->getOriginalEnergy();
    if (mergedOriginalEnergy.size() < leadingSnapshot->getOriginalF0().size()) {
        mergedOriginalEnergy.resize(leadingSnapshot->getOriginalF0().size(), 0.0f);
    }

    const auto& trailingOriginalEnergy = trailingSnapshot->getOriginalEnergy();
    if (trailingOriginalEnergy.size() < trailingOriginalF0.size()) {
        mergedOriginalEnergy.insert(mergedOriginalEnergy.end(),
                                    trailingOriginalF0.size() - trailingOriginalEnergy.size(),
                                    0.0f);
    } else {
        mergedOriginalEnergy.insert(mergedOriginalEnergy.end(),
                                    trailingOriginalEnergy.begin(),
                                    trailingOriginalEnergy.begin() + static_cast<std::ptrdiff_t>(trailingOriginalF0.size()));
    }

    std::vector<CorrectedSegment> mergedSegments = copyCorrectedSegments(leadingCurve);
    const int leadingFrameCount = static_cast<int>(leadingSnapshot->getOriginalF0().size());
    for (auto segment : trailingSnapshot->getCorrectedSegments()) {
        segment.startFrame += leadingFrameCount;
        segment.endFrame += leadingFrameCount;
        mergedSegments.push_back(std::move(segment));
    }

    auto mergedCurve = std::make_shared<PitchCurve>();
    mergedCurve->setHopSize(leadingSnapshot->getHopSize());
    mergedCurve->setSampleRate(leadingSnapshot->getSampleRate());
    mergedCurve->setOriginalF0(mergedOriginalF0);
    mergedCurve->setOriginalEnergy(mergedOriginalEnergy);
    mergedCurve->replaceCorrectedSegments(mergedSegments);
    return mergedCurve;
}

} // anonymous namespace


namespace {

void renderPlacementForExport(OpenTuneAudioProcessor& processor,
                              const StandaloneArrangement::PlaybackPlacement& placement,
                              float trackGain,
                              int64_t placementStartInOutput,
                              juce::AudioBuffer<float>& out,
                              int64_t totalLen,
                              const PlaybackReadSource& source)
{
    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;

    if (!placement.contentKey.isValid() || placement.durationSeconds <= 0.0 || placementStartInOutput >= totalLen
        || !source.canRead()) {
        return;
    }

    const int64_t requestedPlacementSamples = juce::jmax<int64_t>(1,
        TimeCoordinate::secondsToSamples(placement.durationSeconds, kExportSr));
    const int64_t remainingOutputSamples = totalLen - placementStartInOutput;
    const int samplesToRender = static_cast<int>(juce::jmin<int64_t>(requestedPlacementSamples, remainingOutputSamples));
    if (samplesToRender <= 0) {
        return;
    }

    juce::AudioBuffer<float> placementBuffer(out.getNumChannels(), samplesToRender);
    placementBuffer.clear();

    OpenTuneAudioProcessor::PlaybackReadRequest readRequest;
    readRequest.source = source;
    readRequest.readStartSeconds = placement.clipInSeconds; // was 0.0 �?respect trim offset
    readRequest.targetSampleRate = kExportSr;
    readRequest.numSamples = samplesToRender;

    const int renderedSamples = processor.readPlaybackAudio(readRequest, placementBuffer, 0);
    if (renderedSamples <= 0) {
        return;
    }

    const float baseGain = trackGain * placement.gain;
    const int64_t fadeInSamples = placement.fadeInDuration > 0.0
        ? TimeCoordinate::secondsToSamples(placement.fadeInDuration, kExportSr)
        : 0;
    const int64_t fadeOutSamples = placement.fadeOutDuration > 0.0
        ? TimeCoordinate::secondsToSamples(placement.fadeOutDuration, kExportSr)
        : 0;

    for (int ch = 0; ch < out.getNumChannels(); ++ch) {
        const float* src = placementBuffer.getReadPointer(ch);
        float* dst = out.getWritePointer(ch);
        for (int sampleIndex = 0; sampleIndex < renderedSamples; ++sampleIndex) {
            const int64_t dstIndex = placementStartInOutput + sampleIndex;
            if (dstIndex < 0 || dstIndex >= totalLen) {
                continue;
            }

            const float fade = computePlacementFadeGain(sampleIndex,
                                                        requestedPlacementSamples,
                                                        fadeInSamples,
                                                        fadeOutSamples);
            dst[static_cast<size_t>(dstIndex)] += src[sampleIndex] * baseGain * fade;
        }
    }
}

} // anonymous namespace

constexpr uint32_t kProcessorStateMagic = 0x4F545354; // OTST
constexpr int kProcessorStateVersion = 9; // v9 removes persisted Note selection
// vocal-time-stretch §3.8: bumped 5 �?6 to add per-content TimeGrid section.
// v5 projects load with auto-seeded identity TimeGrid (output==source).
// Processor state v7 adds per-handle confidence. v6 reads default confidence=Default.
constexpr uint32_t kStandaloneSettingsMagic = 0x4F545353; // OTSS (OpenTune Standalone Settings)
constexpr int kStandaloneSettingsVersion = 1;

// --- Serialization helpers (full state) ---
// Compiled unconditionally into the shared OpenTune lib; getStateInformation /
// setStateInformation dispatch by runtime wrapperType, so both Standalone and
// VST3 binaries link the same compiled object and need these symbols available.

void OpenTuneAudioProcessor::configureReferenceAnalysisService()
{
    referenceAnalysisService_.setAnalysisFunc(
        [this](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
            ReferenceFeatureSet failed;
            failed.producer = ReferenceFeatureProducer::Game;
            failed.inputFingerprint = jobKey.contentRevision;

            auto snap = getContentSnapshot(jobKey.contentKey);
            if (!snap) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis could not read content snapshot";
                return failed;
            }
            if (static_cast<int64_t>(snap->contentRevision) != jobKey.contentRevision) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis job is stale";
                return failed;
            }
            return buildReferenceFeatureSet(jobKey.contentKey, *snap);
        });
    referenceAnalysisService_.addListener(this);
}

ReferenceFeatureSet OpenTuneAudioProcessor::buildReferenceFeatureSet(
    ContentKey key, const EditableContentSnapshot& snapshot)
{
    juce::ignoreUnused(experimentalReferenceAlignMode_);
    return buildGameReferenceFeatureSet(key, snapshot);
}

ReferenceFeatureSet OpenTuneAudioProcessor::buildGameReferenceFeatureSet(
    ContentKey key, const EditableContentSnapshot& snapshot)
{
    ReferenceFeatureSet result;
    result.producer = ReferenceFeatureProducer::Game;
    result.inputFingerprint = static_cast<int64_t>(snapshot.contentRevision);

    auto audio = resolveAnalysisAudioProvider(key);
    result.sourceDurationSeconds = audio.valid
        ? TimeCoordinate::samplesToSeconds(audio.numSamples, TimeCoordinate::kRenderSampleRate)
        : 0.0;

    auto failGame = [&](const juce::String& reason) {
        result.status = ReferenceFeatureStatus::Failed;
        result.pitch.clear();
        result.timing.clear();
        result.errorMessage = reason;
        return result;
    };

    if (!audio.valid || audio.numSamples <= 0) {
        return failGame("AUTO Ref GAME analysis requires content audio");
    }

    if (!ensureNoteGeneratorReady() || noteGenerator_ == nullptr) {
        return failGame("AUTO Ref GAME analysis requires GAME note generator");
    }

    auto* const gameGenerator = dynamic_cast<GameNoteGenerator*>(noteGenerator_.get());
    if (gameGenerator == nullptr) {
        return failGame("AUTO Ref GAME analysis requires GAME backend");
    }

    NoteGeneratorInput input;
    input.sampleRate = TimeCoordinate::kRenderSampleRate;
    input.audio.assign(audio.samples, audio.samples + audio.numSamples);

    std::vector<Note> gameNotes;
    try {
        std::lock_guard<std::mutex> lk(noteGeneratorInferenceMutex_);
        gameNotes = gameGenerator->generate(input);
    } catch (const std::exception& e) {
        return failGame("AUTO Ref GAME note generation failed: " + juce::String(e.what()));
    } catch (...) {
        return failGame("AUTO Ref GAME note generation failed");
    }

    result.pitch.notes = std::move(gameNotes);

    uint64_t nextAnchorId = 1;
    double lastAcceptedSourceSeconds = 0.0;
    for (const auto& note : result.pitch.notes) {
        const double sourceSeconds = juce::jlimit(0.0, result.sourceDurationSeconds, note.startTime);
        if (sourceSeconds <= 0.0 || sourceSeconds >= result.sourceDurationSeconds) {
            continue;
        }
        if (!result.timing.anchors.empty()
            && !TimeGridSnapshot::hasMinimumSourceSpacing(lastAcceptedSourceSeconds, sourceSeconds)) {
            continue;
        }

        ReferenceTimingAnchor anchor;
        anchor.anchorId = nextAnchorId++;
        anchor.sourceSeconds = sourceSeconds;
        anchor.strength = 1.0f;
        anchor.kind = ReferenceTimingAnchorKind::Onset;
        anchor.confidence = 1.0f;
        result.timing.anchors.push_back(anchor);
        lastAcceptedSourceSeconds = sourceSeconds;
    }

    std::sort(result.timing.anchors.begin(), result.timing.anchors.end(),
              [](const auto& a, const auto& b) { return a.sourceSeconds < b.sourceSeconds; });
    result.timing.anchors.erase(
        std::unique(result.timing.anchors.begin(), result.timing.anchors.end(),
                    [](const auto& a, const auto& b) {
                        return std::abs(a.sourceSeconds - b.sourceSeconds) < 0.005;
                    }),
        result.timing.anchors.end());

    if (result.pitch.notes.empty() && result.timing.anchors.empty()) {
        return failGame("AUTO Ref GAME analysis found no notes or timing anchors");
    }

    result.status = ReferenceFeatureStatus::Ready;
    result.analysisRevision = 1;
    return result;
}

void OpenTuneAudioProcessor::analysisCompleted(
    ContentKey key,
    const ReferenceFeatureSet& result)
{
    if (!result.isReady() || result.producer != ReferenceFeatureProducer::Game) {
        return;
    }

    auto snap = getContentSnapshot(key);
    if (!snap) return;

    if (result.inputFingerprint != static_cast<int64_t>(snap->contentRevision)) {
        return;
    }

    setContentReferenceFeatures(key, result);

    ensureTimeToolAnchorSeed(key);
}

void OpenTuneAudioProcessor::analysisFailed(ContentKey key, const juce::String& reason)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;

    ReferenceFeatureSet failed;
    failed.producer = ReferenceFeatureProducer::Game;
    failed.status = ReferenceFeatureStatus::Failed;
    failed.inputFingerprint = static_cast<int64_t>(snap->contentRevision);
    failed.sourceDurationSeconds = snap->audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(snap->audioBuffer->getNumSamples(),
                                            TimeCoordinate::kRenderSampleRate)
        : 0.0;
    failed.errorMessage = reason;
    setContentReferenceFeatures(key, failed);
}

static std::shared_ptr<PitchCurve> clonePitchCurveWithCorrectedSegments(
    const std::shared_ptr<PitchCurve>& sourceCurve,
    const std::vector<CorrectedSegment>& segments)
{
    if (sourceCurve == nullptr) {
        return nullptr;
    }

    auto committedCurve = sourceCurve->clone();
    committedCurve->replaceCorrectedSegments(segments);
    return committedCurve;
}

OpenTuneAudioProcessor::OpenTuneAudioProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    AppLogger::initialize();
    AppLogger::log("OpenTuneAudioProcessor: ctor");

    editVersionParam_ = new juce::AudioParameterInt("editVersion", "EditVersion", 0, 100000, 0);
    addParameter(editVersionParam_);

    sourceStore_ = std::make_shared<SourceStore>();
    contentRenderService_ = std::make_shared<ContentRenderService>();
    standaloneContentRepository_ = std::make_unique<StandaloneContentRepository>();
    // Bind processor's render callback to CRS via ExecutionLease (ARA 重构路由改制)
    {
        ContentRenderService::ExecutionLease lease;
        lease.owner = this;
        lease.renderJobCallback = [this](RenderJob& job) {
            if (job.renderCache == nullptr) return;
            auto contentSnap = getContentSnapshot(job.contentKey);
            if (!contentSnap) {
                job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
                return;
            }
            ProcessRenderRuntime::CompletionContext completion;
            completion.alive = contentRefreshAliveFlag_;
            completion.chunkPublished = [this](ContentKey key, uint64_t revision) {
                handleStage1ChunkPublished(key, revision);
            };
            const bool lightPitchEnabled = appPreferences_ != nullptr
                && appPreferences_->getState().shared.lightPitchCorrectionEnabled;
            ProcessRenderRuntime::getInstance().processChunkRenderJob(
                contentRenderService_, job, std::move(contentSnap),
                lightPitchEnabled, std::move(completion));
        };
        contentRenderService_->attachExecutionLease(std::move(lease));
    }
    // [ARA 重构] 内联 ProcessorContentEditCommands 替代工厂函数 �?直接分发到域所有�?
    class ProcessorContentCommandsInline final : public ContentEditCommands
    {
    public:
        explicit ProcessorContentCommandsInline(OpenTuneAudioProcessor* proc) noexcept : proc_(proc) {}

        bool setDetectedKey(ContentKey key, const DetectedKey& detectedKey) override
        {
            if (!proc_) return false;
            return proc_->setContentDetectedKey(key, detectedKey);
        }

        bool setPitchShiftSettings(ContentKey key, const PitchShiftSettings& settings) override
        {
            if (!proc_) return false;
            return proc_->setContentPitchShiftSettings(key, settings);
        }

        bool commitAutoTuneGeneratedNotes(ContentKey key,
                                           std::vector<Note> generatedNotes,
                                           int startFrame, int endFrameExclusive,
                                           float retuneSpeed, float vibratoDepth,
                                           float vibratoRate) override
        {
            if (!proc_) return false;
            return proc_->commitAutoTuneGeneratedNotesByContentKey(
                key, std::move(generatedNotes), startFrame, endFrameExclusive,
                retuneSpeed, vibratoDepth, vibratoRate);
        }

        bool replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes) override
        {
            if (!proc_) return false;
            return proc_->replaceContentNotesForFullMutation(key, std::move(notes));
        }

        bool commitNotePatch(ContentKey key, ContentNoteRangePatch patch) override
        {
            if (!proc_) return false;
            return proc_->commitContentNotePatch(key, std::move(patch));
        }

        bool commitNotesAndSegments(ContentKey key,
                                     std::vector<Note> notes,
                                     std::vector<CorrectedSegment> segments,
                                     ContentEditRangeFrames affectedRange) override
        {
            if (!proc_) return false;
            return proc_->commitContentNotesAndSegments(key, std::move(notes), std::move(segments), affectedRange);
        }

        bool setCorrectedSegments(ContentKey key,
                                   std::vector<CorrectedSegment> segments) override
        {
            if (!proc_) return false;
            return proc_->setContentCorrectedSegments(key, std::move(segments));
        }

        bool setPitchCurve(ContentKey key, std::shared_ptr<PitchCurve> curve,
                           ContentEditRangeFrames affectedRange) override
        {
            if (!proc_) return false;
            return proc_->setContentPitchCurve(key, std::move(curve), affectedRange);
        }

        bool setTimeGrid(ContentKey key,
                          std::shared_ptr<const TimeGridSnapshot> grid) override
        {
            if (!proc_) return false;
            return proc_->setContentTimeGrid(key, std::move(grid));
        }

    private:
        OpenTuneAudioProcessor* proc_;
    };
    contentCommands_ = std::make_shared<ProcessorContentCommandsInline>(this);
    standaloneArrangement_ = std::make_unique<StandaloneArrangement>();
    configureReferenceAnalysisService();

    resamplingManager_ = std::make_shared<ResamplingManager>();

    // Capture session: regular VST3 runtime mode. ARA-capable VST3 builds also
    // need this when the host loads the binary as an unbound insert instance.
    if (wrapperType == juce::AudioProcessor::wrapperType_VST3) {
        Capture::ProcessorBindings bindings;

        bindings.replaceWithRendered = [this](juce::AudioBuffer<float>& buffer,
                                               int destStart, int numSamples,
                                               ContentKey segmentContentKey,
                                               double readStartSeconds,
                                               double targetSampleRate) {
            // Capture segment contentKey is the CRS content key.
            jassert(contentRenderService_ != nullptr);

            PlaybackReadSource readSource;
            if (!contentRenderService_->getPlaybackReadSource(segmentContentKey, readSource)
                || !readSource.hasAudio()) {
                buffer.clear(destStart, numSamples);
                return;
            }
            PlaybackReadRequest req;
            req.source = readSource;
            req.readStartSeconds = readStartSeconds;
            req.targetSampleRate = targetSampleRate;
            req.numSamples = numSamples;
            buffer.clear(destStart, numSamples);
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
                std::shared_ptr<std::atomic<bool>> lifetimeFlag = contentRefreshAliveFlag_;
                auto* f0Svc = getF0Service();

                f0ExtractionService_.submit(
                    F0RequestKey{segContentKey},
                    [lifetimeFlag, audio, sr, segContentKey, f0Svc]() -> F0ExtractionService::Result {
                        F0ExtractionService::Result result;
                        result.contentKey = segContentKey;
                        if (!lifetimeFlag->load(std::memory_order_acquire)) {
                            result.errorMessage = "processor_destroyed";
                            return result;
                        }
                        if (!audio || audio->getNumSamples() == 0) {
                            result.errorMessage = "no_audio_data";
                            return result;
                        }
                        if (!f0Svc) {
                            result.errorMessage = "no_f0_service";
                            return result;
                        }
                        const float* src = audio->getReadPointer(0);
                        const int numSamples = audio->getNumSamples();
                        auto extraction = f0Svc->extractF0(
                            src, static_cast<size_t>(numSamples),
                            static_cast<int>(sr));
                        if (!extraction.ok() || extraction.value().empty()) {
                            result.errorMessage = "f0_empty_or_unvoiced";
                            return result;
                        }
                        result.f0 = extraction.value();
                        result.hopSize = f0Svc->getF0HopSize();
                        result.f0SampleRate = f0Svc->getF0SampleRate();
                        result.modelName = "RMVPE";
                        result.success = true;
                        return result;
                    },
                    [this, segContentKey](F0ExtractionService::Result&& result) {
                        if (auto* session = getCaptureSession()) {
                            if (!result.success) {
                                session->commitSegmentF0Result(
                                    segContentKey, nullptr,
                                    OriginalF0State::Failed, DetectedKey{});
                                return;
                            }
                            auto pitchCurve = std::make_shared<PitchCurve>();
                            pitchCurve->setHopSize(result.hopSize);
                            pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
                            pitchCurve->setOriginalF0(result.f0);
                            if (!result.energy.empty())
                                pitchCurve->setOriginalEnergy(result.energy);
                            session->commitSegmentF0Result(
                                segContentKey, std::move(pitchCurve),
                                OriginalF0State::Ready, DetectedKey{});
                        }
                    }
                );
            }
        };

        bindings.publishPlaybackSource = [this](const ContentKey& key,
                                                 std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                 double sampleRate) {
            jassert(contentRenderService_ != nullptr);
            if (audio == nullptr || !key.isValid())
                return;

            auto renderCache = contentRenderService_->getOrCreateRenderCache(key);
            PlaybackReadSource readSource;
            readSource.contentKey = key;
            readSource.renderCache = renderCache;
            readSource.audioBuffer = std::move(audio);
            readSource.audioSampleRate = sampleRate;
            readSource.timeStretchCache = &contentRenderService_->getTimeStretchCache();
            readSource.pitchRevision = 0;
            readSource.pitchShiftRevision = 0;
            readSource.timeGridRevision = 0;
            readSource.timeGridIsIdentity = true;
            contentRenderService_->publishPlaybackSource(key, readSource);
        };

        bindings.requestFullRender = [this](ContentKey key) {
            requestFullContentRender(key, FullRenderReason::CaptureRestore);
        };

        bindings.onRenderComplete = [this](ContentKey key) {
            if (auto* session = getCaptureSession())
                session->onRenderComplete(key);
        };

        captureSession_ = std::make_unique<Capture::CaptureSession>(std::move(bindings));
        AppLogger::log("OpenTuneAudioProcessor: regular VST3 capture session created processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)));
    }
}

OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
    AppLogger::log("OpenTuneAudioProcessor: dtor processor="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
#if JucePlugin_Enable_ARA
        + " araBound=" + juce::String(isBoundToARA() ? "true" : "false")
#else
        + " araBound=false"
#endif
    );

    // Phase 1: 停止内部刷新标志（阻止新 work 提交�?
    contentRefreshAliveFlag_->store(false, std::memory_order_release);

    // Phase 2: 解除 CRS execution lease �?取消 pending render jobs
    if (contentRenderService_) {
        contentRenderService_->detachExecutionLease(this);
    }

    // Phase 4: 内部清理
    cancelPendingUpdate();
    referenceAnalysisService_.removeListener(this);
    referenceAnalysisService_.cancelAll();
    isPlaying_.store(false);

    // Vocoder is process-level (ProcessRenderRuntime singleton); do not
    // shutdown here — that would break every other processor in the process.
    // F0 inference service is process-level (ProcessF0Runtime singleton); do not
    // shutdown here — that would break every other processor in the process.

    AppLogger::shutdown();
}

// ============================================================================
// 推理引擎初始化与生命周期
// ============================================================================

bool OpenTuneAudioProcessor::ensureServiceReady(
    std::atomic<bool>& readyFlag,
    std::atomic<bool>& attemptedFlag,
    std::mutex& initMutex,
    const char* serviceName,
    std::function<bool(const std::string&)> initFunc)
{
    if (readyFlag.load()) return true;

    std::lock_guard<std::mutex> lock(initMutex);

    if (readyFlag.load()) return true;

    if (attemptedFlag.load()) return readyFlag.load();

    attemptedFlag.store(true);

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    AppLogger::log(juce::String(serviceName) + " models dir: " + juce::String(modelsDir));

    if (!ModelPathResolver::ensureOnnxRuntimeLoaded()) {
        AppLogger::log("ensureOnnxRuntimeLoaded failed");
        readyFlag.store(false);
        return false;
    }

    AccelerationDetector::getInstance().detect();

    bool ok = false;
    try {
        ok = initFunc(modelsDir);
        if (!ok) {
            AppLogger::log(juce::String(serviceName) + " initialize failed");
            readyFlag.store(false);
            return false;
        }
        AppLogger::log(juce::String(serviceName) + " inference service initialized successfully");
    } catch (const std::exception& e) {
        AppLogger::log(juce::String(serviceName) + " initialize exception: " + juce::String(e.what()));
        ok = false;
    } catch (...) {
        AppLogger::log(juce::String(serviceName) + " initialize unknown exception");
        ok = false;
    }

    readyFlag.store(ok);
    return ok;
}

bool OpenTuneAudioProcessor::ensureF0Ready()
{
    // F0 is process-level (ProcessF0Runtime singleton).
    if (ProcessF0Runtime::getInstance().isReady())
        return true;
    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    return ProcessF0Runtime::getInstance().initialize(modelsDir);
}

bool OpenTuneAudioProcessor::isNoteGenInFlightForContent(ContentKey contentKey) const
{
    if (!contentKey.isValid()) return false;
    std::lock_guard<std::mutex> lk(noteGenInFlightMutex_);
    return noteGenInFlightContentKeys_.count(contentKey) > 0;
}

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

    const auto modelsDir = juce::String(ModelPathResolver::getModelsDirectory());
    std::lock_guard<std::mutex> lock(noteGenInitMutex_);
    const auto backendProbe = probeAutoRefGameBackendLocked(noteGenerator_.get(), modelsDir);

    if (backendProbe.currentBackendIsGame) {
        availability.status = AutoRefAvailability::Status::Ready;
        availability.message = juce::String("AUTO(Ref) will analyze the reference clip with GAME.");
        return availability;
    }

    if (backendProbe.forceLegacy || backendProbe.currentBackendIsLegacy || !backendProbe.gameBundlePresent) {
        availability.status = AutoRefAvailability::Status::GameUnavailable;
        availability.message = makeAutoRefGameUnavailableMessage(backendProbe);
        return availability;
    }

    availability.status = AutoRefAvailability::Status::Ready;
    availability.message = juce::String("AUTO(Ref) will analyze the reference clip with GAME.");
    return availability;
}

bool OpenTuneAudioProcessor::ensureNoteGeneratorReady()
{
    // Backend selection (per design D7):
    //   1. env OPENTUNE_NOTE_BACKEND=legacy  �?LegacyNoteGenerator
    //   2. else if GAME-small ONNX bundle present �?GameNoteGenerator
    //   3. else fallback to LegacyNoteGenerator
    // Logged once at first init so support can identify which path ran.
    return ensureServiceReady(noteGenReady_, noteGenInitAttempted_, noteGenInitMutex_, "NoteGen",
        [this](const std::string& modelsDir) {
            if (noteGenerator_) return true;

            const auto envBackendRaw = juce::SystemStats::getEnvironmentVariable(
                "OPENTUNE_NOTE_BACKEND", {});
            const auto envBackend = envBackendRaw.trim().toLowerCase();
            const bool forceLegacy = (envBackend == "legacy");
            AppLogger::info(juce::String("[NoteGen] env OPENTUNE_NOTE_BACKEND=")
                            + (envBackendRaw.isEmpty() ? "<unset>" : envBackendRaw)
                            + " �?forceLegacy=" + (forceLegacy ? "true" : "false"));

            if (!forceLegacy) {
                auto ortEnv = ProcessF0Runtime::getInstance().getOrtEnv();
                if (!ortEnv) {
                    Ort::InitApi();
                    ProcessF0Runtime::getInstance().initialize(ModelPathResolver::getModelsDirectory());
                    ortEnv = ProcessF0Runtime::getInstance().getOrtEnv();
                }
                const auto gameDir = juce::File(juce::String(modelsDir))
                                         .getChildFile("GAME").getFullPathName().toStdString();
                if (juce::File(juce::String(gameDir)).getChildFile("encoder.onnx").existsAsFile()) {
                    try {
                        noteGenerator_ = std::make_unique<GameNoteGenerator>(gameDir, *ortEnv);
                        AppLogger::info("[NoteGen] backend=GAME-small (modelsDir=" + juce::String(gameDir) + ")");
                        return true;
                    } catch (const std::exception& e) {
                        AppLogger::warn(juce::String("[NoteGen] GAME init failed, falling back to Legacy: ") + e.what());
                    }
                } else {
                    AppLogger::info("[NoteGen] GAME bundle missing at " + juce::String(gameDir)
                                    + " �?falling back to Legacy");
                }
            }

            noteGenerator_ = std::make_unique<LegacyNoteGenerator>();
            AppLogger::info(juce::String("[NoteGen] backend=Legacy (forceLegacy=")
                            + (forceLegacy ? "true" : "false") + ")");
            return true;
        });
}

void OpenTuneAudioProcessor::resetInferenceBackend(bool forceCpu)
{
    AppLogger::info("[Processor] Resetting inference backend, forceCpu=" 
        + juce::String(forceCpu ? "true" : "false"));
    
    // 1. Pause render worker before releasing services
    if (contentRenderService_)
        contentRenderService_->pauseRenderWorker();
    
    // 2. �?worker 已暂停，安全释放推理服�?
    // 2. Vocoder and F0 are process-level singletons (ProcessRenderRuntime / ProcessF0Runtime).
    //    Reset vocoder via process runtime; do NOT reset F0 (would break other processors).
    ProcessRenderRuntime::getInstance().resetVocoder();
    
    // 3. 重置加速检测器并重新检�?
    auto& detector = AccelerationDetector::getInstance();
    detector.reset();
    detector.detect(forceCpu);
    
    // 4. Resume render worker (services will be lazily re-initialized by ensureVocoderReady)
    if (contentRenderService_)
        contentRenderService_->resumeRenderWorker();
    
    AppLogger::info("[Processor] Inference backend reset to: " 
        + juce::String(detector.getBackendName()));
}

void OpenTuneAudioProcessor::setVocoderModelWeight(VocoderModelWeight weight)
{
    // 2. Delegate vocoder weight change to process-level runtime
    ProcessRenderRuntime::getInstance().setVocoderModelWeight(weight);

    // 4. 清所�?content �?RenderCache + TimeStretchCache
    if (contentRenderService_ && standaloneContentRepository_) {
        const auto keys = standaloneContentRepository_->getAllClips();
        for (const auto key : keys) {
            if (auto cache = contentRenderService_->getRenderCache(key))
                cache->clear();

            requestFullContentRender(key, FullRenderReason::ModelOrSettingsWholeContentRerender);
        }
        // �?TimeStretchCache
        contentRenderService_->getTimeStretchCache().clear();
    }

    // 5. Resume render worker
    if (contentRenderService_)
        contentRenderService_->resumeRenderWorker();

    // 不重�?F0 / AccelerationDetector / GAME
}

bool OpenTuneAudioProcessor::extractImportedClipOriginalF0(const EditableContentSnapshot& snap,
                                                           F0ExtractionService::Result& out,
                                                           std::string& errorMessage)
{
    if (!ensureF0Ready()) {
        errorMessage = "inference_not_ready";
        AppLogger::log("RecordTrace: F0 initialization unavailable"
            " processor=" + juce::String::toHexString(reinterpret_cast<juce::int64>(this)));
        return false;
    }

    auto f0Service = ProcessF0Runtime::getInstance().getF0Service();

    if (f0Service == nullptr) {
        errorMessage = "f0_service_unavailable";
        AppLogger::log("RecordTrace: F0 service missing after init"
            " processor=" + juce::String::toHexString(reinterpret_cast<juce::int64>(this)));
        return false;
    }

    struct ReleaseGuard {
        std::shared_ptr<F0InferenceService> service;
        ~ReleaseGuard()
        {
            if (service)
                service->releaseImmediately();
        }
    } releaseGuard{f0Service};

    return extractOriginalF0ForImportedClip(*f0Service, snap, out, errorMessage);
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
    const bool wasInitialized = (currentSampleRate_ > 0.0);
    const double oldSampleRate = currentSampleRate_;
    const bool sampleRateChanged = wasInitialized && (std::abs(oldSampleRate - sampleRate) > 1.0);
    
    AppLogger::log("prepareToPlay: sampleRate=" + juce::String(sampleRate, 2) +
                   " blockSize=" + juce::String(samplesPerBlock) +
                   " oldRate=" + juce::String(oldSampleRate, 0) +
                   " changed=" + (sampleRateChanged ? "true" : "false"));

    currentSampleRate_ = sampleRate;
    currentBlockSize_ = samplesPerBlock;
    
    // Calculate fade-out duration: 200ms = 0.2 seconds
    fadeOutTotalSamples_ = static_cast<int>(sampleRate * 0.2);
    
    if (sampleRateChanged && oldSampleRate > 0.0) {
        AppLogger::log("Sample rate changed: " + juce::String(oldSampleRate, 0) + 
                        " -> " + juce::String(sampleRate, 0) + 
                       ", rebuilding playback assets");


    }


    doublePrecisionScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);
    trackMixScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);
    clipReadScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);

#if JucePlugin_Enable_ARA
    prepareToPlayForARA(sampleRate,
                        samplesPerBlock,
                        getMainBusNumOutputChannels(),
                        getProcessingPrecision());
#endif
    pianoKeyAudition_.loadSamples();

    if (auto* captureSession = getCaptureSession())
        captureSession->prepareToPlay(sampleRate, samplesPerBlock, getMainBusNumInputChannels());
}

void OpenTuneAudioProcessor::releaseResources() {
    isPlaying_.store(false);

#if JucePlugin_Enable_ARA
    releaseResourcesForARA();
#endif

    if (auto* captureSession = getCaptureSession())
        captureSession->releaseResources();
}

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

void OpenTuneAudioProcessor::didBindToARA() noexcept
{
    juce::AudioProcessorARAExtension::didBindToARA();

    if (auto* dc = getDocumentController())
    {
        dc->setAsyncWorkThreadPool(&noteGeneratorPool_);
        ensureF0Ready();

        // The DC installs its own lease on its CRS in its constructor
        // (installDocumentRenderExecution -> processDocumentRenderJob -> ProcessRenderRuntime).
        // The processor does NOT attach an additional lease: doing so would
        // override the DC's lease and break the ARA2 render path.

        AppLogger::log("ARA: didBindToARA - DC owns its CRS lease; processor=" 
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc)));

        // Replay any pre-bind state that was cached by setStateInformation.
        if (pendingAraState_.getSize() > 0) {
            setStateInformation(pendingAraState_.getData(),
                                static_cast<int>(pendingAraState_.getSize()));
            pendingAraState_.reset();
        }
    }
}
#endif

bool OpenTuneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo()) {
        return false;
    }

    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) {
        return false;
    }

    return true;
}

bool OpenTuneAudioProcessor::supportsDoublePrecisionProcessing() const {
    return true;
}

// ============================================================================
// 音频处理（processBlock�?
// ============================================================================

void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer,
                                          juce::MidiBuffer& midiMessages) {
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels <= 0 || numSamples <= 0) {
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

    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

#if JucePlugin_Enable_ARA
    if (isBoundToARA()) {
        if (auto* hostPlayHead = getPlayHead()) {
            const auto pos = hostPlayHead->getPosition().orFallback(juce::AudioPlayHead::PositionInfo{});
            const auto hostSnapshot = updateHostTransportSnapshot(pos);
            positionAtomic_->store(hostSnapshot.timeSeconds, std::memory_order_relaxed);
            isPlaying_.store(hostSnapshot.isPlaying, std::memory_order_relaxed);
        }
    }

    if (isBoundToARA()) {
        if (processBlockForARA(buffer, isRealtime(), getPlayHead())) {
            pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, static_cast<double>(getSampleRate()));
            return;
        }
    }
#endif

    // Regular VST3 capture path: dry pass-through unless an Edited segment claims
    // this host_t, recording the dry input into the active Capturing segment if any.
    // ARA-capable builds reach this path only while the instance is not ARA-bound.
    if (auto* captureSession = getCaptureSession()) {
        double host_t = 0.0;
        bool isPlayingNow = false;
        if (auto* hostPlayHead = getPlayHead()) {
            const auto pos = hostPlayHead->getPosition().orFallback(juce::AudioPlayHead::PositionInfo{});
            host_t = pos.getTimeInSeconds().orFallback(0.0);
            isPlayingNow = pos.getIsPlaying();
        }

        // Host transport mirror: in non-ARA VST3 mode the plugin is a passive observer.
        // Forcing positionAtomic_ and isPlaying_ to host values every block ensures any
        // local transport change (spacebar / setPlaying) is overridden by host within
        // one audio block �?there is no plugin-side play/pause illusion to maintain.
        positionAtomic_->store(host_t, std::memory_order_relaxed);
        isPlaying_.store(isPlayingNow, std::memory_order_relaxed);

        // Diagnostic: once per ~1 second, post capture buffer stats via atomic event
        // for message-thread consumption (PluginEditor::timerCallback �?consumeAudioThreadLogs).
        static std::atomic<int> diagBlockCounter { 0 };
        const int blockIdx = diagBlockCounter.fetch_add(1, std::memory_order_relaxed);
        const int blocksPerSecond = static_cast<int>(juce::jmax(1.0, getSampleRate())) / juce::jmax(1, numSamples);
        if (totalNumOutputChannels > 0 && numSamples > 0 && blockIdx % juce::jmax(1, blocksPerSecond) == 0) {
            const float* ch0 = buffer.getReadPointer(0);
            AudioThreadLogEvent evt;
            evt.type = AudioThreadLogEvent::Type::CaptureDiag;
            evt.diagNumChannels = totalNumOutputChannels;
            evt.diagNumSamples = numSamples;
            evt.diagMag = buffer.getMagnitude(0, juce::jmin(numSamples, 256));
            evt.diagS0 = ch0[0];
            evt.diagS1 = ch0[juce::jmin(1, numSamples - 1)];
            evt.diagS2 = ch0[juce::jmin(2, numSamples - 1)];
            evt.diagS3 = ch0[juce::jmin(3, numSamples - 1)];
            evt.diagS64 = ch0[juce::jmin(64, numSamples - 1)];
            logEventData_ = evt;
            logEventGeneration_.fetch_add(1, std::memory_order_release);
        }

        captureSession->processBlock(buffer, host_t, getSampleRate(), isPlayingNow);
        pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, static_cast<double>(getSampleRate()));
        return;
    }

    // Clear output buffer
    for (int i = 0; i < totalNumOutputChannels; ++i) {
        buffer.clear(i, 0, numSamples);
    }

    // Handle fade-out state
    bool isFading = isFadingOut_.load();
    bool isPlaying = isPlaying_.load();
    
    if (!isPlaying && !isFading) {
        // Fully stopped �?still mix piano key audition so preview works without transport
        pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, currentSampleRate_.load());
        jassert(standaloneArrangement_ != nullptr);
        for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
            standaloneArrangement_->setTrackRmsDb(trackId, -100.0f);
        }
        return;
    }

    jassert(trackMixScratch_.getNumChannels() >= totalNumOutputChannels);
    jassert(trackMixScratch_.getNumSamples() >= numSamples);
    jassert(clipReadScratch_.getNumChannels() >= totalNumOutputChannels);
    jassert(clipReadScratch_.getNumSamples() >= numSamples);

    const double deviceSampleRate = currentSampleRate_.load();
    const double blockDurationSeconds = static_cast<double>(numSamples) / deviceSampleRate;
    const double currentPosSeconds = positionAtomic_->load(std::memory_order_relaxed);
    const double blockEndSeconds = currentPosSeconds + blockDurationSeconds;
    const int64_t blockStartSample = TimeCoordinate::secondsToSamples(currentPosSeconds, deviceSampleRate);
    const int64_t blockEndSample = blockStartSample + static_cast<int64_t>(numSamples);

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

            const int64_t placementDurationSamples = TimeCoordinate::secondsToSamples(placement.durationSeconds,
                                                                                      deviceSampleRate);
            if (placementDurationSamples <= 0) {
                continue;
            }

            const int64_t placementStartSample = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds,
                                                                                  deviceSampleRate);
            const int64_t placementEndSample = placementStartSample + placementDurationSamples;

            if (placementEndSample <= blockStartSample || placementStartSample >= blockEndSample) {
                continue;
            }

            const int64_t overlapStartSample = std::max(blockStartSample, placementStartSample);
            const int64_t overlapEndSample = std::min(blockEndSample, placementEndSample);
            const int64_t samplesToCopy64 = overlapEndSample - overlapStartSample;
            if (samplesToCopy64 <= 0) {
                continue;
            }

            const int64_t clipInSampleOffset = TimeCoordinate::secondsToSamples(placement.clipInSeconds, deviceSampleRate);
            const int64_t readStartSample = overlapStartSample - placementStartSample + clipInSampleOffset;
            const double readStartSeconds = TimeCoordinate::samplesToSeconds(readStartSample, deviceSampleRate);
            const int offsetInBlock = static_cast<int>(overlapStartSample - blockStartSample);
            const int samplesToCopy = static_cast<int>(samplesToCopy64);

            const float placementGain = placement.gain * trackVolume;
            const double fadeInSeconds = placement.fadeInDuration;
            const double fadeOutSeconds = placement.fadeOutDuration;

            // ====================================================================
            // Unified Playback Read API call
            // ====================================================================
            PlaybackReadRequest readRequest;
            readRequest.source = readSource;
            readRequest.readStartSeconds = readStartSeconds;
            readRequest.targetSampleRate = deviceSampleRate;
            readRequest.numSamples = samplesToCopy;

            clipReadScratch_.clear();
            const int availableReadSamples = readPlaybackAudio(readRequest, clipReadScratch_, 0);

            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const float* src = clipReadScratch_.getReadPointer(ch);
                float* dst = trackMixScratch_.getWritePointer(ch, offsetInBlock);

                // Use placement-local time (offset by clipInSeconds so fade works
                // relative to visible clip, not from raw content start)
                double timeInPlacement = TimeCoordinate::samplesToSeconds(readStartSample, deviceSampleRate) - placement.clipInSeconds;
                const double dt = 1.0 / deviceSampleRate;
                for (int s = 0; s < availableReadSamples; ++s) {
                    float gain = placementGain;

                    if (fadeInSeconds > 0.0 && timeInPlacement < fadeInSeconds) {
                        gain *= static_cast<float>(timeInPlacement / fadeInSeconds);
                    }
                    if (fadeOutSeconds > 0.0 && timeInPlacement >= placement.durationSeconds - fadeOutSeconds) {
                        gain *= static_cast<float>((placement.durationSeconds - timeInPlacement) / fadeOutSeconds);
                    }

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

    if (isFadingOut_.load()) {
        int fadeCount = fadeOutSampleCount_.load();
        int fadeTotal = fadeOutTotalSamples_;
        
        for (int sample = 0; sample < numSamples; ++sample) {
            int currentSample = fadeCount + sample;
            float fadeGain = 1.0f;
            
            if (currentSample < fadeTotal) {
                fadeGain = 1.0f - static_cast<float>(currentSample) / static_cast<float>(fadeTotal);
            } else {
                fadeGain = 0.0f;
            }
            
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * fadeGain);
            }
        }
        
        fadeCount += numSamples;
        fadeOutSampleCount_.store(fadeCount);
        
        if (fadeCount >= fadeTotal) {
            isFadingOut_.store(false);
            isPlaying_.store(false);
            AudioThreadLogEvent evt;
            evt.type = AudioThreadLogEvent::Type::FadeOutComplete;
            logEventData_ = evt;
            logEventGeneration_.fetch_add(1, std::memory_order_release);
            
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                for (int s = 0; s < numSamples; ++s) {
                    buffer.setSample(ch, s, 0.0f);
                }
            }
        }
    }

    pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, deviceSampleRate);

    positionAtomic_->store(blockEndSeconds, std::memory_order_relaxed);
}

OpenTuneAudioProcessor::HostTransportSnapshot OpenTuneAudioProcessor::getHostTransportSnapshot() const
{
    HostTransportSnapshot snapshot;
    snapshot.isPlaying = hostTransportIsPlaying_.load(std::memory_order_relaxed);
    snapshot.timeSeconds = hostTransportTimeSeconds_.load(std::memory_order_relaxed);
    snapshot.bpm = hostTransportBpm_.load(std::memory_order_relaxed);
    snapshot.ppqPosition = hostTransportPpqPosition_.load(std::memory_order_relaxed);
    snapshot.loopEnabled = hostTransportLoopEnabled_.load(std::memory_order_relaxed);
    snapshot.loopPpqStart = hostTransportLoopPpqStart_.load(std::memory_order_relaxed);
    snapshot.loopPpqEnd = hostTransportLoopPpqEnd_.load(std::memory_order_relaxed);
    snapshot.isRecording = hostTransportIsRecording_.load(std::memory_order_relaxed);
    snapshot.timeSignatureNumerator = hostTransportTimeSignatureNumerator_.load(std::memory_order_relaxed);
    snapshot.timeSignatureDenominator = hostTransportTimeSignatureDenominator_.load(std::memory_order_relaxed);
    return snapshot;
}

OpenTuneAudioProcessor::HostTransportSnapshot OpenTuneAudioProcessor::updateHostTransportSnapshot(
    const juce::AudioPlayHead::PositionInfo& positionInfo)
{
    HostTransportSnapshot snapshot = getHostTransportSnapshot();

    snapshot.isPlaying = positionInfo.getIsPlaying();
    snapshot.isRecording = positionInfo.getIsRecording();
    snapshot.loopEnabled = positionInfo.getIsLooping();

    if (const auto timeSeconds = positionInfo.getTimeInSeconds()) {
        snapshot.timeSeconds = *timeSeconds;
    }

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

    if (const auto loopPoints = positionInfo.getLoopPoints()) {
        snapshot.loopPpqStart = loopPoints->ppqStart;
        snapshot.loopPpqEnd = loopPoints->ppqEnd;
    }

    hostTransportIsPlaying_.store(snapshot.isPlaying, std::memory_order_relaxed);
    hostTransportTimeSeconds_.store(snapshot.timeSeconds, std::memory_order_relaxed);
    hostTransportBpm_.store(snapshot.bpm, std::memory_order_relaxed);
    hostTransportPpqPosition_.store(snapshot.ppqPosition, std::memory_order_relaxed);
    hostTransportLoopEnabled_.store(snapshot.loopEnabled, std::memory_order_relaxed);
    hostTransportLoopPpqStart_.store(snapshot.loopPpqStart, std::memory_order_relaxed);
    hostTransportLoopPpqEnd_.store(snapshot.loopPpqEnd, std::memory_order_relaxed);
    hostTransportIsRecording_.store(snapshot.isRecording, std::memory_order_relaxed);
    hostTransportTimeSignatureNumerator_.store(snapshot.timeSignatureNumerator, std::memory_order_relaxed);
    hostTransportTimeSignatureDenominator_.store(snapshot.timeSignatureDenominator, std::memory_order_relaxed);
    return snapshot;
}

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

juce::AudioProcessorEditor* OpenTuneAudioProcessor::createEditor() {
    return createOpenTuneEditor(*this);
}

bool OpenTuneAudioProcessor::hasEditor() const {
    return true;
}

// ============================================================================
// 项目状态序列化/反序列化
// ============================================================================

void OpenTuneAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    // Dispatch by runtime wrapperType �?same compiled object serves both
    // Standalone and VST3 binaries (shared `OpenTune` lib), so a build-time
    // guard cannot differentiate. Standalone owns its own project save format
    // and only needs settings here; VST3 needs full state for host round-trip.
    if (wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        destData.reset();
        juce::MemoryOutputStream output(destData, false);
        output.writeInt(static_cast<int>(kStandaloneSettingsMagic));
        output.writeInt(kStandaloneSettingsVersion);
        output.writeDouble(getBpm());
        output.writeDouble(zoomLevel_);
        output.writeInt(trackHeight_);
        return;
    }

    // Per ARA2 spec: ARA AudioModification objects are persisted via
    // doStoreObjectsToStream/doRestoreObjectsFromStream, NOT via VST3 processor state.
    // ARA host owns document archive lifecycle.
    // VST3 state only stores standalone arrangement when ARA is not active.

    destData.reset();
    juce::MemoryOutputStream output(destData, false);
    output.writeInt(static_cast<int>(kProcessorStateMagic));
    output.writeInt(kProcessorStateVersion);
    output.writeDouble(zoomLevel_);
    output.writeInt(trackHeight_);

    output.writeInt(standaloneArrangement_->getActiveTrackId());
    output.writeInt(MAX_TRACKS);
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const uint64_t selectedPlacementId = standaloneArrangement_->getSelectedPlacementId(trackId);
        output.writeInt64(static_cast<juce::int64>(selectedPlacementId));
        output.writeBool(standaloneArrangement_->isTrackMuted(trackId));
        output.writeBool(standaloneArrangement_->isTrackSolo(trackId));
        output.writeFloat(standaloneArrangement_->getTrackVolume(trackId));

        const int placementCount = standaloneArrangement_->getNumPlacements(trackId);
        output.writeInt(placementCount);
        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
                jassertfalse; // Placement index/track mismatch �?data integrity error
                AppLogger::error("SerializationCorruption: getPlacementByIndex failed for track="
                                 + juce::String(trackId) + " index=" + juce::String(placementIndex));
                return; // Don't serialize known-corrupt data
            }

            output.writeInt64(static_cast<juce::int64>(placement.placementId));
            output.writeInt(static_cast<int>(placement.contentKey.domainKind));
            output.writeInt64(static_cast<juce::int64>(placement.contentKey.objectId));
            output.writeInt64(static_cast<juce::int64>(placement.contentKey.sourceWindowDiscriminator));
            output.writeInt64(static_cast<juce::int64>(placement.mappingRevision));
            output.writeDouble(placement.timelineStartSeconds);
            output.writeDouble(placement.durationSeconds);
            output.writeFloat(placement.gain);
            output.writeDouble(placement.fadeInDuration);
            output.writeDouble(placement.fadeOutDuration);
            output.writeDouble(placement.clipInSeconds);
            output.writeString(placement.name);
        }
    }

    // Append regular VST3 capture data at end of stream. ARA-bound instances keep
    // their edit state in the ARA document/session archive instead.
    if (const auto* captureSession = getCaptureSession()) {
        const auto captureBlock = captureSession->serialize();
        if (captureBlock.getSize() > 0)
            output.write(captureBlock.getData(), captureBlock.getSize());
    }
}


void OpenTuneAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) {
        return;
    }

    // Per ARA2 spec: ARA AudioModification objects are restored via
    // doRestoreObjectsFromStream, NOT via VST3 processor state.
    // ARA host owns document archive lifecycle.
    // VST3 state only restores standalone arrangement when ARA is not active.

    juce::MemoryInputStream input(data, static_cast<size_t>(sizeInBytes), false);
    const int magic = input.readInt();
    const int version = input.readInt();

    // Standalone settings-only payload
    if (magic == static_cast<int>(kStandaloneSettingsMagic)) {
        if (version != kStandaloneSettingsVersion) {
            AppLogger::warn("StateRestore: unsupported standalone settings version");
            return;
        }
        setBpm(input.readDouble());
        zoomLevel_ = input.readDouble();
        trackHeight_ = input.readInt();
        return;
    }

    // Full state payload (VST3)
    // vocal-time-stretch §3.8: state v6 adds TimeGrid section per content.
    // Accept v5 (no TimeGrid), v6 (TimeGrid w/o confidence), v7 (TimeGrid w/ confidence).
    if (magic != static_cast<int>(kProcessorStateMagic) || version != kProcessorStateVersion) {
        AppLogger::warn("StateRestore: unsupported processor state payload (version=" + juce::String(version) + ")");
        return;
    }

    AppLogger::log("StateRestore: VST3 full-state begin sizeBytes=" + juce::String(sizeInBytes));

    // Standalone instance receiving a full-state payload: settings are not in
    // this format anymore (BPM/zoom/trackHeight live in OTSS), so there is
    // nothing meaningful to restore �?return without touching stores.
    if (wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        return;
    }

    // Note: BPM is not in OTST v5 �?host owns transport tempo in plugin mode.
    zoomLevel_ = input.readDouble();
    trackHeight_ = input.readInt();


    jassert(sourceStore_ != nullptr);
    sourceStore_->clear();

    standaloneContentRepository_->clear();
    contentRenderService_->clearAll();

    jassert(standaloneArrangement_ != nullptr);
    standaloneArrangement_->clear();

    const int restoredActiveTrackId = input.readInt();
    const int trackCount = input.readInt();
    jassert(standaloneArrangement_ != nullptr);
    if (trackCount != MAX_TRACKS) {
        return;
    }

    for (int trackId = 0; trackId < trackCount; ++trackId) {
        const uint64_t selectedPlacementId = static_cast<uint64_t>(input.readInt64());
        standaloneArrangement_->setTrackMuted(trackId, input.readBool());
        standaloneArrangement_->setTrackSolo(trackId, input.readBool());
        standaloneArrangement_->setTrackVolume(trackId, input.readFloat());

        const int placementCount = input.readInt();
        if (placementCount < 0) {
            return;
        }

        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            placement.placementId = static_cast<uint64_t>(input.readInt64());
            placement.contentKey.domainKind = static_cast<DomainKind>(input.readInt());
            placement.contentKey.objectId = static_cast<uint64_t>(input.readInt64());
            placement.contentKey.sourceWindowDiscriminator = static_cast<uint64_t>(input.readInt64());
            placement.mappingRevision = static_cast<uint64_t>(input.readInt64());
            placement.timelineStartSeconds = input.readDouble();
            placement.durationSeconds = input.readDouble();
            placement.gain = input.readFloat();
            placement.fadeInDuration = input.readDouble();
            placement.fadeOutDuration = input.readDouble();
            placement.clipInSeconds = input.readDouble();
            placement.name = input.readString();

            if (!placement.contentKey.isValid()) {
                continue;
            }

            standaloneArrangement_->insertPlacement(trackId, placement);
        }

        standaloneArrangement_->selectPlacement(trackId, selectedPlacementId);
    }

    standaloneArrangement_->setActiveTrack(restoredActiveTrackId);

    // After project state is restored, attempt to load regular VST3 capture payload
    // from remaining bytes. ARA-bound instances keep their state in the ARA archive.
    if (auto* captureSession = getCaptureSession()) {
        const auto remaining = static_cast<int>(input.getNumBytesRemaining());
        if (remaining > 0) {
            juce::MemoryBlock captureBlock;
            captureBlock.setSize(static_cast<size_t>(remaining));
            input.read(captureBlock.getData(), remaining);
            const bool ok = captureSession->deserialize(captureBlock);
            if (ok)
                AppLogger::log("CaptureSession: state restored ("
                               + juce::String(captureSession->listSegments().size()) + " segments)");
        }
    }
}

// ============================================================================
// Standalone Arrangement 代理接口
// ============================================================================

void OpenTuneAudioProcessor::setTrackHeight(int height) {
    trackHeight_ = height;
}

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

    const double splitOffsetSeconds = splitSeconds - originalPlacement.timelineStartSeconds;
    constexpr double minDurationSeconds = 0.1;
    if (splitOffsetSeconds <= minDurationSeconds
        || splitOffsetSeconds >= originalPlacement.durationSeconds - minDurationSeconds) {
        AppLogger::log("Split rejected: split point out of valid placement range");
        return std::nullopt;
    }

    const auto originalSnapshot = getContentSnapshot(originalPlacement.contentKey);
    if (!originalSnapshot || originalSnapshot->audioBuffer == nullptr) {
        return std::nullopt;
    }

    const int64_t splitSample = TimeCoordinate::secondsToSamples(splitOffsetSeconds, TimeCoordinate::kRenderSampleRate);
    const int64_t totalSamples = originalSnapshot->audioBuffer->getNumSamples();
    if (splitSample <= 0 || splitSample >= totalSamples) {
        return std::nullopt;
    }

    ContentPayloadState leadingPayload = payloadFromSnapshot(*originalSnapshot);
    leadingPayload.sourceWindow = SourceWindow{
        originalSnapshot->sourceWindow.sourceId,
        juce::String(),
        originalSnapshot->sourceWindow.sourceStartSeconds,
        originalSnapshot->sourceWindow.sourceStartSeconds + splitOffsetSeconds
    };
    leadingPayload.audioBuffer = sliceAudioBuffer(originalSnapshot->audioBuffer, 0, splitSample);
    leadingPayload.pitchCurve = slicePitchCurveToLocalRange(originalSnapshot->pitchCurve, 0.0, splitOffsetSeconds);
    leadingPayload.correctedSegments = correctedSegmentsFromCurve(leadingPayload.pitchCurve);
    leadingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes, 0.0, splitOffsetSeconds);
    leadingPayload.silentGaps = sliceSilentGaps(originalSnapshot->silentGaps, 0, splitSample);
    leadingPayload.timeGrid = nullptr;

    ContentPayloadState trailingPayload = payloadFromSnapshot(*originalSnapshot);
    trailingPayload.sourceWindow = SourceWindow{
        originalSnapshot->sourceWindow.sourceId,
        juce::String(),
        originalSnapshot->sourceWindow.sourceStartSeconds + splitOffsetSeconds,
        originalSnapshot->sourceWindow.sourceEndSeconds
    };
    trailingPayload.audioBuffer = sliceAudioBuffer(originalSnapshot->audioBuffer, splitSample, totalSamples);
    trailingPayload.pitchCurve = slicePitchCurveToLocalRange(originalSnapshot->pitchCurve,
                                                             splitOffsetSeconds,
                                                             originalPlacement.durationSeconds);
    trailingPayload.correctedSegments = correctedSegmentsFromCurve(trailingPayload.pitchCurve);
    trailingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes,
                                                   splitOffsetSeconds,
                                                   originalPlacement.durationSeconds);
    trailingPayload.silentGaps = sliceSilentGaps(originalSnapshot->silentGaps, splitSample, totalSamples);
    trailingPayload.timeGrid = nullptr;

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
    ++leadingPlacement.mappingRevision;

    StandaloneArrangement::Placement trailingPlacement = originalPlacement;
    trailingPlacement.placementId = 0;
    trailingPlacement.contentKey = trailingKey;
    trailingPlacement.timelineStartSeconds = splitSeconds;
    trailingPlacement.durationSeconds = originalPlacement.durationSeconds - splitOffsetSeconds;
    trailingPlacement.fadeInDuration = 0.0;
    trailingPlacement.clipInSeconds = originalPlacement.clipInSeconds + splitOffsetSeconds;
    ++trailingPlacement.mappingRevision;

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

    if (!nearlyEqualSeconds(leadingSnapshot->sourceWindow.sourceEndSeconds,
                            trailingSnapshot->sourceWindow.sourceStartSeconds)) {
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

    ContentPayloadState mergedPayload;
    mergedPayload.sourceWindow = SourceWindow{
        leadingSnapshot->sourceWindow.sourceId,
        juce::String(),
        leadingSnapshot->sourceWindow.sourceStartSeconds,
        trailingSnapshot->sourceWindow.sourceEndSeconds
    };
    mergedPayload.audioBuffer = mergedBuffer;
    mergedPayload.sampleRate = leadingSnapshot->audioSampleRate > 0.0 ? leadingSnapshot->audioSampleRate : TimeCoordinate::kRenderSampleRate;
    mergedPayload.pitchCurve = mergedPitchCurve;
    mergedPayload.originalF0State = leadingSnapshot->originalF0State;
    mergedPayload.detectedKey = leadingSnapshot->detectedKey;
    mergedPayload.notes = std::move(mergedNotes);
    mergedPayload.correctedSegments = correctedSegmentsFromCurve(mergedPayload.pitchCurve);
    mergedPayload.silentGaps = mergeSilentGaps(leadingSnapshot->silentGaps, trailingSnapshot->silentGaps, leadingSamples);
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
    ++mergedPlacement.mappingRevision;

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
            sourceId = clip->payload().sourceWindow.sourceId;
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

// ============================================================================
// Content 读写代理接口
// ============================================================================

bool OpenTuneAudioProcessor::getSourceSnapshotById(uint64_t sourceId, SourceStore::SourceSnapshot& out) const
{
    out = SourceStore::SourceSnapshot{};
    if (sourceId == 0)
        return false;

    // ARA mode: source metadata is in AudioSource objects, not a separate store
    // Non-ARA mode: use processor sourceStore_
    return sourceStore_ != nullptr && sourceStore_->getSnapshot(sourceId, out);
}

std::shared_ptr<const EditableContentSnapshot> OpenTuneAudioProcessor::getContentSnapshot(ContentKey key) const
{
    if (!key.isValid()) {
        return nullptr;
    }

    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            return clip ? clip->snapshotContent() : nullptr;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            const auto* dc = getDocumentController();
            return dc ? dc->readContentSnapshot(key) : nullptr;
        }
#else
        case DomainKind::ARAAudioModification:
            return nullptr;
#endif
        case DomainKind::RegularVST3Capture: {
            const auto* session = getCaptureSession();
            const auto* segment = session ? session->findSegmentByContentKey(key) : nullptr;
            return segment && segment->content ? segment->content->snapshotContent() : nullptr;
        }
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
#else
        case DomainKind::ARAAudioModification:
            return nullptr;
#endif
        case DomainKind::StandaloneClip:
        case DomainKind::RegularVST3Capture:
            return contentRenderService_.get();
    }
    return nullptr;
}

ContentRenderService* OpenTuneAudioProcessor::resolveMutableLocalContentRenderService(ContentKey key) const noexcept
{
    switch (key.domainKind) {
        case DomainKind::StandaloneClip:
        case DomainKind::RegularVST3Capture:
            return contentRenderService_.get();
        case DomainKind::ARAAudioModification:
            return nullptr;  // ARA CRS is owned by DC, processor must not mutate it
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
            result.samples = snap->audioBuffer->getReadPointer(0);
            result.numSamples = snap->audioBuffer->getNumSamples();
            result.sampleRate = snap->audioSampleRate;
            result.valid = true;
        }
    }

    return result;
}

void OpenTuneAudioProcessor::onContentLocalMutationCompleted(ContentKey key,
                                                              MutationScope scope,
                                                              ContentEditRangeFrames affectedRange)
{
    juce::ignoreUnused(scope);

#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        auto* dc = getDocumentController();
        if (dc != nullptr)
        {
            dc->refreshModificationCRSMetadata(key);
            auto snap = getContentSnapshot(key);
            if (snap && snap->pitchCurve)
            {
                const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                             / snap->pitchCurve->getSampleRate();
                const double startSec = static_cast<double>(affectedRange.startFrame) * secondsPerFrame;
                const double endSec   = static_cast<double>(affectedRange.endFrameExclusive) * secondsPerFrame;
                dc->requestModificationRender(key, startSec, endSec);
            }
        }
        return;
    }
#endif

    // Non-ARA path: processor-local CRS
    refreshCRSMetadata(key);
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return;
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    const double startSec = static_cast<double>(affectedRange.startFrame) * secondsPerFrame;
    const double endSec   = static_cast<double>(affectedRange.endFrameExclusive) * secondsPerFrame;
    requestRenderForLocalMutationRange(key, startSec, endSec);
}

void OpenTuneAudioProcessor::onContentFullMutationCompleted(ContentKey key,
                                                             MutationScope scope,
                                                             FullRenderReason reason)
{
    juce::ignoreUnused(scope);

#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        auto* dc = getDocumentController();
        if (dc != nullptr)
        {
            dc->refreshModificationCRSMetadata(key);
            dc->requestFullModificationRender(key);
        }
        return;
    }
#endif

    // Non-ARA path
    refreshCRSMetadata(key);
    requestFullContentRender(key, reason);
}

void OpenTuneAudioProcessor::requestFullContentRender(ContentKey key, FullRenderReason reason)
{
    juce::ignoreUnused(reason);
    auto snap = getContentSnapshot(key);
    if (!snap) return;
    const double durationSeconds = contentDurationSeconds(*snap);
    if (durationSeconds <= 0.0) return;
    requestRenderForLocalMutationRange(key, 0.0, durationSeconds);
}

void OpenTuneAudioProcessor::requestRenderForLocalMutationRange(ContentKey key,
                                                                double startSeconds,
                                                                double endSeconds)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;
    auto* crs = resolveMutableLocalContentRenderService(key);
    if (crs == nullptr) return;

    PlaybackReadSource readSource;
    if (!crs->getPlaybackReadSource(key, readSource)) return;

    const double crsSampleRate = readSource.audioSampleRate;
    auto audioBuffer = readSource.audioBuffer;
    if (crsSampleRate <= 0.0 || audioBuffer == nullptr || audioBuffer->getNumSamples() <= 0) return;

    auto renderCache = crs->getOrCreateRenderCache(key);
    readSource.renderCache = renderCache;
    crs->publishPlaybackSource(key, readSource);

    const int64_t totalSamples = audioBuffer->getNumSamples();
    const int64_t startSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesFloor(startSeconds, crsSampleRate));
    const int64_t endSample = juce::jlimit<int64_t>(
        0, totalSamples, TimeCoordinate::secondsToSamplesCeil(endSeconds, crsSampleRate));

    if (endSample <= startSample) return;

    RenderJob job;
    job.contentKey = key;
    job.renderCache = renderCache;
    job.audioBuffer = audioBuffer;
    job.startSeconds = static_cast<double>(startSample) / crsSampleRate;
    job.endSeconds = static_cast<double>(endSample) / crsSampleRate;
    job.targetRevision = snap->contentRevision;
    job.renderRevision = snap->contentRevision;
    job.pitchRevision = snap->pitchRevision;
    job.pitchShiftRevision = snap->pitchShiftRevision;
    job.timeGridRevision = snap->timeGridRevision;
    job.contentRevision = snap->contentRevision;
    job.silentGaps = snap->silentGaps;
    job.audioSampleRate = crsSampleRate;
    job.startSample = startSample;
    job.endSampleExclusive = endSample;

    crs->enqueueRender(std::move(job));
}

void OpenTuneAudioProcessor::handleStage1ChunkPublished(ContentKey key, uint64_t publishedRevision)
{
    auto snap = getContentSnapshot(key);
    if (!snap || snap->contentRevision != publishedRevision)
        return;

    refreshCRSMetadata(key);
    
    // Notify capture session that render is complete for this segment
    if (auto* session = getCaptureSession())
        session->onRenderComplete(key);
    
    if (snap->timeGrid != nullptr && !snap->timeGrid->isIdentity())
    {
        // Use CRS-owned Stage2 rebuild API
        ContentRenderService::Stage2Request stage2Req;
        stage2Req.contentKey = key;
        stage2Req.pitchRevision = snap->pitchRevision;
        stage2Req.pitchShiftRevision = snap->pitchShiftRevision;
        stage2Req.timeGridRevision = snap->timeGridRevision;

        auto* crs = resolveMutableLocalContentRenderService(key);
        if (crs != nullptr)
        {
            crs->requestStage2Rebuild(stage2Req, snap);
        }
    }
}

void OpenTuneAudioProcessor::refreshCRSMetadata(ContentKey key)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;
    auto* resolvedCrs = resolveMutableLocalContentRenderService(key);
    if (resolvedCrs == nullptr) return;

    PlaybackReadSource src;
    auto& crs = *resolvedCrs;
    if (!crs.getPlaybackReadSource(key, src))
        return;

    src.renderCache        = crs.getOrCreateRenderCache(key);
    src.renderRevision     = snap->contentRevision;
    src.pitchRevision      = snap->pitchRevision;
    src.timeGridRevision   = snap->timeGridRevision;
    src.pitchShiftRevision = snap->pitchShiftRevision;
    src.pitchShiftSettings = snap->pitchShiftSettings;
    src.timeGridIsIdentity = snap->timeGrid == nullptr || snap->timeGrid->isIdentity();

    crs.publishPlaybackSource(key, src);
}

bool OpenTuneAudioProcessor::ensureSourceById(uint64_t sourceId,
                                              const juce::String& displayName,
                                              std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                              double sampleRate)
{
    if (sourceId == 0 || audioBuffer == nullptr) {
        return false;
    }
    jassert(sourceStore_ != nullptr);

    if (audioBuffer->getNumChannels() <= 0 || audioBuffer->getNumSamples() <= 0) {
        return false;
    }

    if (sourceStore_->containsSource(sourceId)) {
        return true;
    }

    SourceStore::CreateSourceRequest request;
    request.displayName = displayName;
    request.audioBuffer = std::move(audioBuffer);
    request.sampleRate = sampleRate > 0.0 ? sampleRate : TimeCoordinate::kRenderSampleRate;
    return sourceStore_->createSource(std::move(request), sourceId) == sourceId;
}




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

// 导出单个 placement 的音�?
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
        lastExportError_ = "无效的片段索�? " + juce::String(placementIndex);
        return false;
    }

    // 等待 RenderWorker 完成当前渲染，确保导出最新数据
    contentRenderService_->drainRenderWorker();

    PlaybackReadSource source;
    if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.canRead()) {
        lastExportError_ = "Placement audio is unavailable";
        return false;
    }

    const int64_t placementLen = TimeCoordinate::secondsToSamples(placement.durationSeconds, kExportSampleRateHz);
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
        if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, checkSource) || !checkSource.canRead()) {
            continue;
        }
        const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
        const int64_t placementEnd = placementStart + TimeCoordinate::secondsToSamples(placement.durationSeconds, kExportSr);
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
        if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.canRead()) {
            continue;
        }
        const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
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
                                 trackVolume, placementStart, out, totalLen,
                                 source);
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
            if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, checkSource) || !checkSource.canRead()) {
                continue;
            }
            const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
            const int64_t placementEnd = placementStart + TimeCoordinate::secondsToSamples(placement.durationSeconds, kExportSr);
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
            if (!contentRenderService_->getPlaybackReadSource(placement.contentKey, source) || !source.canRead()) {
                continue;
            }
            const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
            renderPlacementForExport(*this, placement, track.volume, placementStart, mix, totalLen, source);
        }
    }

    return writeAudioBufferToWavFile(mix, file);
}

// ============================================================================
// 播放控制
// ============================================================================

void OpenTuneAudioProcessor::setPlaying(bool playing) {
    if (playing) {
        playStartPosition_.store(positionAtomic_->load(std::memory_order_relaxed));
        isFadingOut_.store(false);
        isPlaying_.store(true);
        AppLogger::log("Playback: start");
    } else {
        if (isPlaying_.load()) {
            isFadingOut_.store(true);
            fadeOutSampleCount_.store(0);
            AppLogger::log("Playback: fade-out started");
        }
    }
}

void OpenTuneAudioProcessor::setLoopEnabled(bool enabled) {
    loopEnabled_.store(enabled);
}

void OpenTuneAudioProcessor::setPosition(double seconds) {
    positionAtomic_->store(seconds, std::memory_order_relaxed);
}

double OpenTuneAudioProcessor::getPosition() const {
    return positionAtomic_->load(std::memory_order_relaxed);
}

void OpenTuneAudioProcessor::setBpm(double bpm) {
    bpm_ = bpm;
    hostTransportBpm_.store(bpm, std::memory_order_relaxed);
}

void OpenTuneAudioProcessor::setZoomLevel(double zoom) {
    zoomLevel_ = zoom;
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

// ============================================================================
// Two-phase Import Implementation
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

    // Storage layout exactly matches the declaration (1 �?mono, 2 �?stereo).
    ChannelLayoutLog::logEntry(entrySourceTag, declaredChannels, declaredChannels, displayName);

    out.displayName = displayName;
    out.sourceFilePath = sourceFilePath;

    // 导入后的 content �?shared runtime 内统一落到固定 44.1kHz �?content-local 存储采样率�?
    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    if (std::abs(inSampleRate - targetSampleRate) > 1.0) {
        const int numChannels = inBuffer.getNumChannels();
        const int originalLen = inBuffer.getNumSamples();
        const double sourceDurationSeconds = TimeCoordinate::samplesToSeconds(originalLen, inSampleRate);
        const int newLen = juce::jmax(
            1,
            static_cast<int>(TimeCoordinate::secondsToSamples(sourceDurationSeconds, targetSampleRate)));
        
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

    ContentKey key = standaloneContentRepository_->createClip();
    if (!key.isValid()) {
        if (createdSource) sourceStore_->deleteSource(sourceId);
        return {};
    }

    auto* clip = standaloneContentRepository_->findClip(key);
    if (!clip) {
        if (createdSource) sourceStore_->deleteSource(sourceId);
        return {};
    }

    clip->applyAudioBuffer(storedAudioBuffer, TimeCoordinate::kRenderSampleRate);
    clip->applyOriginalF0State(OriginalF0State::NotRequested);

    SourceWindow sw = prepared.sourceWindow;
    if (sw.sourceId == 0) {
        const double durationSeconds = TimeCoordinate::samplesToSeconds(
            storedAudioBuffer->getNumSamples(), TimeCoordinate::kRenderSampleRate);
        sw = SourceWindow{sourceId, juce::String(), 0.0, durationSeconds};
    }
    clip->payload().sourceWindow = sw;
    clip->payload().silentGaps = std::move(prepared.silentGaps);

    publishStandalonePlaybackSource(*contentRenderService_, key, clip->payload());

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
    importedPlacement.mappingRevision = 0;
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

uint64_t OpenTuneAudioProcessor::commitPreparedImportAsContent(PreparedImport&& prepared, uint64_t sourceId)
{
    bool createdSource = false;
    const auto key = ensureSourceAndCreateStandaloneClip(std::move(prepared), sourceId, createdSource);
    return key.objectId;
}



// ============================================================================
// requestContentRefresh �?Standalone / regular VST3 F0 refresh.
// F0 only �?does NOT run GAME note generation.
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
            const auto pSnapshot = snap->pitchCurve->getSnapshot();
            const double frameRate = pSnapshot->getSampleRate()
                / static_cast<double>(juce::jmax(1, pSnapshot->getHopSize()));
            if (frameRate > 0.0) {
                const int startFrame = juce::jmax(0,
                    static_cast<int>(std::floor(request.changedStartSeconds * frameRate)));
                const int endFrame = juce::jmax(startFrame,
                    static_cast<int>(std::ceil(request.changedEndSeconds * frameRate)));
                auto clearedCurve = snap->pitchCurve->clone();
                clearedCurve->clearCorrectionRange(startFrame, endFrame);
                if (!writePitchCurveToOwner(request.contentKey, std::move(clearedCurve))) {
                    return false;
                }
                onContentFullMutationCompleted(request.contentKey, MutationScope::PitchCurveChanged, FullRenderReason::GlobalPitchShift);
            }
        }

        setContentDetectedKey(request.contentKey, DetectedKey{});
    }

    switch (request.contentKey.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(request.contentKey);
            if (clip) clip->applyOriginalF0State(OriginalF0State::Extracting);
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification:
            // Handled by DC path in setContentOriginalF0State
            break;
#endif
        case DomainKind::RegularVST3Capture:
            setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting);
            break;
    }

    if (f0ExtractionService_.isActive(F0RequestKey{request.contentKey})) {
        f0ExtractionService_.cancel(F0RequestKey{request.contentKey});
    }

    const auto lifetimeFlag = contentRefreshAliveFlag_;
    OpenTuneAudioProcessor* const processor = this;
    const auto capturedRequest = request;

    const auto submitResult = f0ExtractionService_.submit(
        F0RequestKey{request.contentKey},
        [lifetimeFlag, processor, capturedRequest]() -> F0ExtractionService::Result {
            F0ExtractionService::Result result;
            result.contentKey = capturedRequest.contentKey;

            if (!lifetimeFlag->load(std::memory_order_acquire)) {
                result.errorMessage = "processor_destroyed";
                return result;
            }

            auto snap = processor->getContentSnapshot(capturedRequest.contentKey);
            if (!snap || snap->audioBuffer == nullptr) {
                result.errorMessage = "content_snapshot_failed";
                return result;
            }

            result.sourceAudioBuffer = snap->audioBuffer;

            std::string errorMessage;
            if (!processor->extractImportedClipOriginalF0(*snap, result, errorMessage)) {
                result.errorMessage = errorMessage;
                return result;
            }

            result.success = true;
            return result;
        },
        [lifetimeFlag, processor, capturedRequest](F0ExtractionService::Result&& result) {
            if (!lifetimeFlag->load(std::memory_order_acquire)) {
                return;
            }

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
                    auto previousSnapshot = previousSnap->pitchCurve->getSnapshot();
                    auto segments = previousSnapshot->getCorrectedSegments();
                    if (!segments.empty()) {
                        const double frameRate = static_cast<double>(result.f0SampleRate)
                            / static_cast<double>(juce::jmax(1, result.hopSize));
                        const int maxFrame = static_cast<int>(result.f0.size());

                        if (frameRate > 0.0 && capturedRequest.changedEndSeconds > capturedRequest.changedStartSeconds) {
                            const int changedStartFrame = juce::jmax(0,
                                static_cast<int>(std::floor(capturedRequest.changedStartSeconds * frameRate)));
                            const int changedEndFrame = juce::jmax(changedStartFrame,
                                static_cast<int>(std::ceil(capturedRequest.changedEndSeconds * frameRate)));

                            segments.erase(std::remove_if(segments.begin(),
                                                          segments.end(),
                                                          [changedStartFrame, changedEndFrame, maxFrame](const CorrectedSegment& segment) {
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
                                                          [maxFrame](const CorrectedSegment& segment) {
                                                              if (segment.startFrame >= maxFrame) return true;
                                                              return juce::jmin(segment.endFrame, maxFrame) <= segment.startFrame;
                                                          }),
                                           segments.end());
                        }

                        if (!segments.empty()) {
                            pitchCurve->replaceCorrectedSegments(segments);
                        }
                    }
                }
            }

            if (!processor->writePitchCurveToOwner(capturedRequest.contentKey, pitchCurve)) {
                AppLogger::log("ContentRefresh: pitch curve commit failed contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Failed);
                return;
            }
            processor->onContentFullMutationCompleted(capturedRequest.contentKey, MutationScope::PitchCurveChanged, FullRenderReason::GlobalPitchShift);

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

            processor->detectContentKeyIfUnset(capturedRequest.contentKey);

            processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Ready);
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
// Placement Movement
// ============================================================================

bool OpenTuneAudioProcessor::movePlacementToTrack(int sourceTrackId,
                                                  int targetTrackId,
                                                  uint64_t placementId,
                                                  double newTimelineStartSeconds)
{
    jassert(standaloneArrangement_ != nullptr);
    return standaloneArrangement_->movePlacementToTrack(sourceTrackId, targetTrackId, placementId, newTimelineStartSeconds);
}

void OpenTuneAudioProcessor::detectContentKeyIfUnset(ContentKey key)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;

    if (snap->detectedKey.confidence > 0.0f) {
        return;
    }

    auto audio = resolveAnalysisAudioProvider(key);
    if (!audio.valid || audio.numSamples <= 0) {
        return;
    }

    ChromaKeyDetector detector;
    const auto detectedKey = detector.detect(audio.samples,
                                             audio.numSamples,
                                             static_cast<int>(audio.sampleRate));
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
// vocal-time-stretch §3.6 �?TimeGrid processor accessors
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

    auto& features = snap->referenceFeatures;
    const bool featuresReady = features.isReady()
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snap->contentRevision)
        && features.hasTimingAnchors();

    if (!featuresReady) {
        const auto preheatStatus = preheatReferenceAlignmentFeatures(key);
        if (preheatStatus == ReferenceAnalysisPreheatStatus::InvalidContent
            || preheatStatus == ReferenceAnalysisPreheatStatus::AnalysisFailed) {
            return false;
        }
        return false;
    }

    if (!features.isReady()
        || features.producer != ReferenceFeatureProducer::Game
        || !features.hasTimingAnchors()) {
        return false;
    }

    const double durationSeconds = snap->timeGrid != nullptr
        ? snap->timeGrid->totalDurationSeconds()
        : TimeCoordinate::samplesToSeconds(snap->audioBuffer != nullptr ? snap->audioBuffer->getNumSamples() : 0,
                                           TimeCoordinate::kRenderSampleRate);
    if (!(durationSeconds > 0.0)) {
        return false;
    }

    std::vector<TimeHandle> handles;
    handles.reserve(features.timing.anchors.size() + 2);

    uint64_t nextHandleId = 1;
    auto pushHandle = [&](double sourceSeconds,
                          double outputSeconds,
                          HandleKind kind,
                          bool locked,
                          Confidence confidence = Confidence::Default) {
        TimeHandle handle;
        handle.id = nextHandleId++;
        handle.source_seconds = sourceSeconds;
        handle.output_seconds = outputSeconds;
        handle.kind = kind;
        handle.locked = locked;
        handle.confidence = confidence;
        handles.push_back(handle);
    };

    pushHandle(0.0, 0.0, HandleKind::ClipStart, true);

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
                   HandleKind::InternalOnset,
                   false);
        lastAcceptedSource = eventTime;
    }

    pushHandle(durationSeconds,
               durationSeconds,
               HandleKind::ClipEnd,
               true);

    auto seededGrid = TimeGridSnapshot::makeFromHandles(std::move(handles),
                                                        existingGrid != nullptr ? existingGrid->revision() + 1 : 1);
    if (seededGrid == nullptr) {
        return false;
    }

    return setContentTimeGrid(key, std::move(seededGrid));
}

OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus
OpenTuneAudioProcessor::preheatReferenceAlignmentFeatures(ContentKey key)
{
    if (!key.isValid()) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    auto snap = getContentSnapshot(key);
    if (!snap) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    auto& features = snap->referenceFeatures;
    if (features.isReady()
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snap->contentRevision)) {
        return ReferenceAnalysisPreheatStatus::AlreadyReady;
    }

    if (features.status == ReferenceFeatureStatus::Failed
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snap->contentRevision)) {
        return ReferenceAnalysisPreheatStatus::AnalysisFailed;
    }

    if (features.status != ReferenceFeatureStatus::Extracting
        || features.producer != ReferenceFeatureProducer::Game
        || features.inputFingerprint != static_cast<int64_t>(snap->contentRevision)) {
        ReferenceFeatureSet extracting;
        extracting.producer = ReferenceFeatureProducer::Game;
        extracting.status = ReferenceFeatureStatus::Extracting;
        extracting.inputFingerprint = static_cast<int64_t>(snap->contentRevision);
        extracting.sourceDurationSeconds = snap->audioBuffer != nullptr
            ? TimeCoordinate::samplesToSeconds(snap->audioBuffer->getNumSamples(),
                                                TimeCoordinate::kRenderSampleRate)
            : 0.0;
        setContentReferenceFeatures(key, extracting);
    }

    referenceAnalysisService_.submitAnalysis(key,
                                             static_cast<int64_t>(snap->contentRevision));
    return ReferenceAnalysisPreheatStatus::Queued;
}

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

    ReferenceFeatureSet targetFeatures = targetSnap->referenceFeatures;
    if (!targetFeatures.isReady()
        || targetFeatures.producer != ReferenceFeatureProducer::Game
        || targetFeatures.inputFingerprint != static_cast<int64_t>(targetSnap->contentRevision)) {
        targetFeatures = buildReferenceFeatureSet(targetPlacement.contentKey, *targetSnap);
        setContentReferenceFeatures(targetPlacement.contentKey, targetFeatures);
    }
    if (!targetFeatures.isReady()) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = targetFeatures.errorMessage.isNotEmpty()
            ? targetFeatures.errorMessage
            : "AUTO Ref target alignment features are not ready";
        return result;
    }

    ReferenceFeatureSet referenceFeatures = referenceSnap->referenceFeatures;
    if (!referenceFeatures.isReady()
        || referenceFeatures.producer != ReferenceFeatureProducer::Game
        || referenceFeatures.inputFingerprint != static_cast<int64_t>(referenceSnap->contentRevision)) {
        referenceFeatures = buildReferenceFeatureSet(referencePlacement.contentKey, *referenceSnap);
        setContentReferenceFeatures(referencePlacement.contentKey, referenceFeatures);
    }
    if (!referenceFeatures.isReady()) {
        result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
        result.message = referenceFeatures.errorMessage.isNotEmpty()
            ? referenceFeatures.errorMessage
            : "AUTO Ref reference alignment features are not ready";
        return result;
    }

    const auto oldNotes = targetSnap->notes;
    auto oldCurve = targetSnap->pitchCurve;
    if (oldCurve == nullptr) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target pitch curve is not available";
        return result;
    }

    const auto oldSegments = copyCorrectedSegments(oldCurve);
    // 优先�?sourceWindow 或已�?features �?duration，不加载 PCM
    const double targetDurationSeconds = targetSnap->sourceWindow.isValid()
        ? targetSnap->sourceWindow.durationSeconds()
        : targetFeatures.sourceDurationSeconds;
    const double referenceDurationSeconds = referenceSnap->sourceWindow.isValid()
        ? referenceSnap->sourceWindow.durationSeconds()
        : referenceFeatures.sourceDurationSeconds;
    if (!(targetDurationSeconds > 0.0) || !(referenceDurationSeconds > 0.0)) {
        result.status = ReferenceAlignmentResult::Status::InvalidTimeGrid;
        result.message = "AUTO Ref requires positive target and reference durations";
        return result;
    }

    auto oldTimeGrid = targetSnap->timeGrid;
    if (oldTimeGrid == nullptr) {
        oldTimeGrid = TimeGridSnapshot::makeIdentity(targetDurationSeconds);
    }
    if (oldTimeGrid == nullptr) {
        result.status = ReferenceAlignmentResult::Status::InvalidTimeGrid;
        result.message = "AUTO Ref could not bootstrap target TimeGrid";
        return result;
    }

    ReferenceAlignmentRequest request;
    request.target.placementId = targetPlacement.placementId;
    request.target.contentKey = targetPlacement.contentKey;
    request.target.timelineStartSeconds = targetPlacement.timelineStartSeconds;
    request.target.timelineEndSeconds = targetPlacement.timelineEndSeconds();
    request.reference.placementId = referencePlacement.placementId;
    request.reference.contentKey = referencePlacement.contentKey;
    request.reference.timelineStartSeconds = referencePlacement.timelineStartSeconds;
    request.reference.timelineEndSeconds = referencePlacement.timelineEndSeconds();
    request.targetTimeMap = EffectiveTimeMap::fromTimeGrid(targetSnap->timeGrid, targetDurationSeconds);
    request.referenceTimeMap = EffectiveTimeMap::fromTimeGrid(referenceSnap->timeGrid, referenceDurationSeconds);
    request.targetFeatures = targetFeatures;
    request.referenceFeatures = referenceFeatures;
    request.targetNotesBefore = oldNotes;
    request.targetSegmentsBefore = oldSegments;
    request.targetTimeGridBefore = oldTimeGrid;
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
            case AlignmentPatch::ErrorCode::InsufficientNotes:
                result.status = ReferenceAlignmentResult::Status::InsufficientFeatures;
                break;
            case AlignmentPatch::ErrorCode::TimeGridInvalid:
                result.status = ReferenceAlignmentResult::Status::InvalidTimeGrid;
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
        result.affectedStartFrame = patch.affectedStartFrame;
        result.affectedEndFrame = patch.affectedEndFrame;
        return result;
    }

    auto timeGridAfter = oldTimeGrid;
    if (patch.timingChanged) {
        TimeGridPatchRequest timeRequest;
        timeRequest.before = oldTimeGrid;
        timeRequest.affectedSourceStartSeconds =
            static_cast<double>(patch.affectedStartFrame) / TimeGridSnapshot::kSourceSpacingFrameRate;
        timeRequest.affectedSourceEndSeconds =
            static_cast<double>(patch.affectedEndFrame) / TimeGridSnapshot::kSourceSpacingFrameRate;
        timeRequest.intents = patch.timingIntents;

        const auto timeResult = TimeGridPatchBuilder::build(timeRequest);
        if (!timeResult.success || timeResult.after == nullptr) {
            result.status = ReferenceAlignmentResult::Status::InvalidTimeGrid;
            result.message = timeResult.diagnostic.isEmpty()
                ? juce::String("AUTO Ref produced an invalid TimeGrid patch")
                : timeResult.diagnostic;
            return result;
        }
        timeGridAfter = timeResult.after;
    }

    auto newCurve = clonePitchCurveWithCorrectedSegments(oldCurve, patch.correctedSegmentsAfter);
    if (newCurve == nullptr || timeGridAfter == nullptr) {
        result.status = ReferenceAlignmentResult::Status::CommitFailed;
        result.message = "AUTO Ref produced an incomplete patch";
        return result;
    }

    const auto normalizedNotes = normalizeStoredNotes(patch.notesAfter);
    const bool commitOk = commitContentNotesAndSegments(
        targetPlacement.contentKey, normalizedNotes, patch.correctedSegmentsAfter,
        ContentEditRangeFrames{patch.affectedStartFrame, patch.affectedEndFrame});
    if (commitOk && patch.timingChanged && timeGridAfter)
        setContentTimeGrid(targetPlacement.contentKey, timeGridAfter);
    if (!commitOk) {
        result.status = ReferenceAlignmentResult::Status::CommitFailed;
        result.message = patch.diagnostics;
        result.targetContentKey = targetPlacement.contentKey;
        result.affectedStartFrame = patch.affectedStartFrame;
        result.affectedEndFrame = patch.affectedEndFrame;
        return result;
    }

    auto composite = std::make_unique<CompositeUndoAction>("AUTO (Ref)");
    if (patch.pitchChanged) {
        composite->addAction(std::make_unique<PianoRollEditAction>(
            contentCommands_,
            targetPlacement.contentKey,
            "AUTO (Ref) Pitch",
            oldNotes,
            normalizedNotes,
            oldSegments,
            patch.correctedSegmentsAfter,
            patch.affectedStartFrame,
            patch.affectedEndFrame));
    }
    if (patch.timingChanged) {
        composite->addAction(std::make_unique<TimeGridEditAction>(
            contentCommands_,
            targetPlacement.contentKey,
            "AUTO (Ref) Time",
            oldTimeGrid,
            timeGridAfter));
    }

    if (composite->getNumActions() > 0) {
        undoManager_.addAction(std::move(composite));
    }

    if (patch.timingChanged) {
        if (const auto latestSnap = getContentSnapshot(targetPlacement.contentKey)) {
            ContentRenderService::Stage2Request stage2Req;
            stage2Req.contentKey = targetPlacement.contentKey;
            stage2Req.pitchRevision = latestSnap->pitchRevision;
            stage2Req.pitchShiftRevision = latestSnap->pitchShiftRevision;
            stage2Req.timeGridRevision = latestSnap->timeGridRevision;

#if JucePlugin_Enable_ARA
            if (targetPlacement.contentKey.domainKind == DomainKind::ARAAudioModification) {
                auto* dc = getDocumentController();
                if (dc != nullptr)
                    dc->requestModificationStage2Rebuild(targetPlacement.contentKey);
            } else
#endif
            {
                auto* crs = resolveMutableLocalContentRenderService(targetPlacement.contentKey);
                if (crs != nullptr)
                    crs->requestStage2Rebuild(stage2Req, latestSnap);
            }
        }
    }

    result.status = ReferenceAlignmentResult::Status::Succeeded;
    result.message = "AUTO Ref alignment applied";
    result.targetContentKey = targetPlacement.contentKey;
    result.affectedStartFrame = patch.affectedStartFrame;
    result.affectedEndFrame = patch.affectedEndFrame;
    return result;
}

bool OpenTuneAudioProcessor::replaceContentNotesForFullMutation(ContentKey key, std::vector<Note> notes)
{
    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(normalizeStoredNotes(std::move(notes)));
            ok = true;
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyNotesToModification(key, normalizeStoredNotes(std::move(notes)));
            break;
        }
#else
        case DomainKind::ARAAudioModification:
            break;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyNotes(key, normalizeStoredNotes(std::move(notes))))
                return false;
            ok = true;
            break;
        }
    }
    if (ok) {
        onContentFullMutationCompleted(key, MutationScope::NotesChanged, FullRenderReason::Import);
    }
    return ok;
}

bool OpenTuneAudioProcessor::setContentCorrectedSegments(ContentKey key,
                                                           std::vector<CorrectedSegment> segments)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return false;

    auto newCurve = clonePitchCurveWithCorrectedSegments(snap->pitchCurve, std::move(segments));
    if (!newCurve) return false;

    if (!writePitchCurveToOwner(key, std::move(newCurve)))
        return false;
    onContentFullMutationCompleted(key, MutationScope::PitchCurveChanged, FullRenderReason::GlobalPitchShift);
    return true;
}

bool OpenTuneAudioProcessor::commitContentNotesAndSegments(ContentKey key,
                                                            std::vector<Note> notesInRange,
                                                            std::vector<CorrectedSegment> segments,
                                                            ContentEditRangeFrames affectedRange)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return false;

    // Range-scoped notes merge (same logic as commitContentNotePatch)
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    const double rangeStartSec = static_cast<double>(affectedRange.startFrame) * secondsPerFrame;
    const double rangeEndSec   = static_cast<double>(affectedRange.endFrameExclusive) * secondsPerFrame;

    std::vector<Note> mergedNotes;
    mergedNotes.reserve(snap->notes.size() + notesInRange.size());

    // keptBefore: notes entirely before the range
    for (const auto& note : snap->notes) {
        if (note.endTime <= rangeStartSec)
            mergedNotes.push_back(note);
    }

    // afterNotesInRange: notes overlapping the range
    mergedNotes.insert(mergedNotes.end(),
                       notesInRange.begin(),
                       notesInRange.end());

    // keptAfter: notes entirely after the range
    for (const auto& note : snap->notes) {
        if (note.startTime >= rangeEndSec)
            mergedNotes.push_back(note);
    }

    auto normalizedNotes = normalizeStoredNotes(std::move(mergedNotes));

    // Segments are sparse structure — full replacement is correct
    auto newCurve = clonePitchCurveWithCorrectedSegments(snap->pitchCurve, std::move(segments));
    if (!newCurve) return false;

    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(std::move(normalizedNotes));
            clip->applyPitchCurve(std::move(newCurve));
            ok = true;
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            if (!dc->applyNotesToModification(key, std::move(normalizedNotes))) return false;
            if (newCurve) dc->applyPitchCurveToModification(key, std::move(newCurve));
            ok = true;
            break;
        }
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr
                || !session->applyNotesAndPitchCurve(key, std::move(normalizedNotes), std::move(newCurve))) {
                return false;
            }
            ok = true;
            break;
        }
        default:
            break;
    }
    if (ok) {
        onContentLocalMutationCompleted(key, MutationScope::NotesChanged, affectedRange);
    }
    return ok;
}

bool OpenTuneAudioProcessor::commitContentNotePatch(ContentKey key, ContentNoteRangePatch patch)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return false;

    const double rangeStartSec = patch.affectedRange.startSeconds;
    const double rangeEndSec   = patch.affectedRange.endSeconds;

    // Merge: keptBefore + afterNotesInRange + keptAfter
    // Notes are time-ordered; split at range boundaries.
    std::vector<Note> mergedNotes;
    mergedNotes.reserve(snap->notes.size() + patch.afterNotesInRange.size());

    for (const auto& note : snap->notes) {
        if (note.endTime <= rangeStartSec)
            mergedNotes.push_back(note);
    }

    mergedNotes.insert(mergedNotes.end(),
                       patch.afterNotesInRange.begin(),
                       patch.afterNotesInRange.end());

    for (const auto& note : snap->notes) {
        if (note.startTime >= rangeEndSec)
            mergedNotes.push_back(note);
    }

    auto normalizedNotes = normalizeStoredNotes(std::move(mergedNotes));

    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(std::move(normalizedNotes));
            ok = true;
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyNotesToModification(key, std::move(normalizedNotes));
            break;
        }
#else
        case DomainKind::ARAAudioModification:
            break;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr) return false;
            ok = session->applyNotes(key, std::move(normalizedNotes));
            break;
        }
        default:
            break;
    }

    if (ok) {
        // Convert seconds-based range to frames for render invalidation
        ContentEditRangeFrames frameRange;
        if (snap->pitchCurve && patch.affectedRange.endSeconds > patch.affectedRange.startSeconds) {
            const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                         / snap->pitchCurve->getSampleRate();
            frameRange.startFrame = static_cast<int>(patch.affectedRange.startSeconds / secondsPerFrame);
            frameRange.endFrameExclusive = static_cast<int>(std::ceil(patch.affectedRange.endSeconds / secondsPerFrame));
        }
        onContentLocalMutationCompleted(key, MutationScope::NotesChanged, frameRange);
    }
    return ok;
}

bool OpenTuneAudioProcessor::writePitchCurveToOwner(ContentKey key,
                                                      std::shared_ptr<PitchCurve> curve)
{
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyPitchCurve(std::move(curve));
            return true;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyPitchCurveToModification(key, std::move(curve));
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyPitchCurve(key, std::move(curve)))
                return false;
            return true;
        }
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentPitchCurve(ContentKey key,
                                                   std::shared_ptr<PitchCurve> curve,
                                                   ContentEditRangeFrames affectedRange)
{
    if (!writePitchCurveToOwner(key, std::move(curve)))
        return false;
    onContentLocalMutationCompleted(key, MutationScope::PitchCurveChanged, affectedRange);
    return true;
}

bool OpenTuneAudioProcessor::setContentTimeGrid(ContentKey key,
                                                  std::shared_ptr<const TimeGridSnapshot> grid)
{
    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyTimeGrid(grid);
            ok = true;
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyTimeGridToModification(key, std::move(grid));
            break;
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyTimeGrid(key, std::move(grid)))
                return false;
            ok = true;
            break;
        }
    }
    if (ok) {
        onContentFullMutationCompleted(key, MutationScope::TimeGridChanged, FullRenderReason::GlobalTimeGrid);
    }
    return ok;
}

bool OpenTuneAudioProcessor::setContentDetectedKey(ContentKey key, const DetectedKey& detectedKey)
{
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyDetectedKey(detectedKey);
            return true;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyDetectedKeyToModification(key, detectedKey);
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            return session != nullptr && session->applyDetectedKey(key, detectedKey);
        }
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentReferenceFeatures(ContentKey key,
                                                          const ReferenceFeatureSet& features)
{
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyReferenceFeatures(features);
            return true;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyReferenceFeaturesToModification(key, features);
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            seg->content->applyReferenceFeatures(features);
            return true;
        }
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentOriginalF0State(ContentKey key, OriginalF0State state)
{
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyOriginalF0State(state);
            return true;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            return dc->applyOriginalF0StateToModification(key, state);
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            seg->content->applyOriginalF0State(state);
            return true;
        }
    }
    return false;
}

bool OpenTuneAudioProcessor::setContentPitchShiftSettings(ContentKey key,
                                                           const PitchShiftSettings& settings)
{
    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyPitchShiftSettings(settings);
            ok = true;
            break;
        }
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyPitchShiftToModification(key, settings);
            break;
        }
#else
        case DomainKind::ARAAudioModification:
            return false;
#endif
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyPitchShiftSettings(key, settings))
                return false;
            ok = true;
            break;
        }
    }
    if (ok) {
        onContentFullMutationCompleted(key, MutationScope::PitchShiftChanged, FullRenderReason::GlobalPitchShift);
    }
    return ok;
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
    for (const auto& note : normalizedNotes)
        mergedNotes.push_back(note);
    std::sort(mergedNotes.begin(), mergedNotes.end(),
        [](const Note& a, const Note& b) { return a.startTime < b.startTime; });

    auto derivedCurve = snap->pitchCurve->clone();
    derivedCurve->applyCorrectionToRange(mergedNotes,
                                         startFrame,
                                         endFrameExclusive,
                                         retuneSpeed,
                                         vibratoDepth,
                                         vibratoRate);

    bool ok = false;
    switch (key.domainKind) {
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyNotes(std::move(mergedNotes));
            clip->applyPitchCurve(std::move(derivedCurve));
            ok = true;
            break;
        }
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
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr
                || !session->applyAutoTuneGeneratedNotes(key, std::move(mergedNotes), std::move(derivedCurve))) {
                return false;
            }
            ok = true;
            break;
        }
        default:
            break;
    }

    if (!ok) return false;
    onContentLocalMutationCompleted(key, MutationScope::NotesChanged,
                                    ContentEditRangeFrames{startFrame, endFrameExclusive});
    return true;
}

// ============================================================================
// Unified Playback Read API
// ============================================================================

int OpenTuneAudioProcessor::readPlaybackAudio(const PlaybackReadRequest& request,
                                              juce::AudioBuffer<float>& destination,
                                              int destinationStartSample) const
{
    if (request.numSamples <= 0
        || request.targetSampleRate <= 0.0
        || !request.source.canRead()
        || request.source.audioBuffer == nullptr) {
        return 0;
    }

    const int destinationChannels = destination.getNumChannels();
    const int destinationSamples = destination.getNumSamples();
    if (destinationChannels <= 0
        || destinationSamples <= 0
        || destinationStartSample < 0
        || destinationStartSample >= destinationSamples) {
        return 0;
    }

    const int writableSamples = juce::jmin(request.numSamples, destinationSamples - destinationStartSample);
    if (writableSamples <= 0) {
        return 0;
    }

    // ============================================================
    // vocal-time-stretch §7 (Phase D MVP) �?TimeStretchCache fast-path
    //
    // When a non-identity TimeGrid is published and Stage 2 has populated the
    // TimeStretchCache for this ContentKey/revision tuple, serve the stretched
    // audio directly from cache.
    //
    // Otherwise continue with the owner-published base audio plus RenderCache
    // overlay below.
    // ============================================================
    const uint64_t objectId = request.source.contentKey.objectId;
    if (!request.source.timeGridIsIdentity
        && request.source.timeStretchCache != nullptr
        && objectId != 0) {
        const int wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.contentKey,
            request.source.pitchRevision,
            request.source.pitchShiftRevision,
            request.source.timeGridRevision,
            request.readStartSeconds,
            destination,
            destinationStartSample,
            writableSamples,
            static_cast<int>(request.targetSampleRate));
        if (wrote > 0) {
            return wrote;
        }
        // Cache is not ready for this revision yet; continue with the base
        // audio plus render overlay for this block.
    }

    const auto& srcBuffer = *request.source.audioBuffer;
    const int srcChannels = srcBuffer.getNumChannels();
    const int64_t srcLengthSamples = srcBuffer.getNumSamples();
    const double srcSampleRate = request.source.audioSampleRate;
    if (srcChannels <= 0 || srcLengthSamples <= 0 || srcSampleRate <= 0.0) {
        return 0;
    }

    const double ratio = srcSampleRate / request.targetSampleRate;
    const double readStartInSrcSamples = request.readStartSeconds * srcSampleRate;
    if (readStartInSrcSamples < 0.0 || readStartInSrcSamples >= static_cast<double>(srcLengthSamples)) {
        return 0;
    }

    // Compute available output samples
    const int maxSrcSample = static_cast<int>(srcLengthSamples) - 1;
    int availableSamples = writableSamples;
    {
        const double lastSrcPos = readStartInSrcSamples + (writableSamples - 1) * ratio;
        if (lastSrcPos >= static_cast<double>(srcLengthSamples)) {
            availableSamples = static_cast<int>((static_cast<double>(srcLengthSamples) - readStartInSrcSamples) / ratio);
            if (availableSamples <= 0) return 0;
        }
    }

    // Write dry signal with linear interpolation (single pass, pointer-based).
    //
    // Per channel-layout-policy spec: srcChannels is guaranteed �?{1, 2} (enforced
    // at `content creation`). The `srcCh = ch % srcChannels`
    // mapping below covers both layouts naturally:
    //   - srcChannels=1 (mono storage): every dest ch maps to src 0 �?broadcast.
    //   - srcChannels=2 (stereo storage): dest ch 0 �?src 0, ch 1 �?src 1 �?1:1 map.
    // No extra channel-count guards or general-N-channel handling needed.
    for (int channel = 0; channel < destinationChannels; ++channel) {
        const int srcCh = channel % srcChannels;
        const float* srcPtr = srcBuffer.getReadPointer(srcCh);
        float* dstPtr = destination.getWritePointer(channel, destinationStartSample);

        double srcPos = readStartInSrcSamples;
        for (int s = 0; s < availableSamples; ++s) {
            const int idx0 = static_cast<int>(srcPos);
            const int idx1 = juce::jmin(idx0 + 1, maxSrcSample);
            const float fraction = static_cast<float>(srcPos - idx0);
            dstPtr[s] = srcPtr[idx0] + (srcPtr[idx1] - srcPtr[idx0]) * fraction;
            srcPos += ratio;
        }
    }

    if (request.source.renderCache != nullptr) {
        request.source.renderCache->overlayPublishedAudioForRate(destination,
                                                                 destinationStartSample,
                                                                 availableSamples,
                                                                 request.readStartSeconds,
                                                                 static_cast<int>(request.targetSampleRate));
    }

    return availableSamples;
}

// ============================================================================
// Clipboard �?content range copy for paste/duplicate
// ============================================================================

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
    const int64_t offsetSamples = TimeCoordinate::secondsToSamples(offsetSeconds, sampleRate);
    const int64_t durSamples = TimeCoordinate::secondsToSamples(durationSeconds, sampleRate);
    if (offsetSamples < 0 || durSamples <= 0 || offsetSamples + durSamples > totalSamples) {
        return ContentKey{};
    }

    ContentPayloadState payload;
    payload.sourceWindow = SourceWindow{
        sourceSnap->sourceWindow.sourceId,
        juce::String(),
        sourceSnap->sourceWindow.sourceStartSeconds + offsetSeconds,
        sourceSnap->sourceWindow.sourceStartSeconds + offsetSeconds + durationSeconds
    };
    payload.audioBuffer = sliceAudioBuffer(sourceSnap->audioBuffer,
                                           offsetSamples,
                                           offsetSamples + durSamples);
    payload.sampleRate = sampleRate;
    payload.pitchCurve = slicePitchCurveToLocalRange(sourceSnap->pitchCurve,
                                                     offsetSeconds,
                                                     offsetSeconds + durationSeconds);
    payload.originalF0State = sourceSnap->originalF0State;
    payload.detectedKey = sourceSnap->detectedKey;
    payload.notes = sliceNotesToLocalRange(sourceSnap->notes,
                                           offsetSeconds,
                                           offsetSeconds + durationSeconds);
    payload.correctedSegments = correctedSegmentsFromCurve(payload.pitchCurve);
    payload.silentGaps = sliceSilentGaps(sourceSnap->silentGaps,
                                         offsetSamples,
                                         offsetSamples + durSamples);
    payload.pitchShiftSettings = sourceSnap->pitchShiftSettings;

    if (sourceSnap->timeGrid && !sourceSnap->timeGrid->empty()) {
        const auto& srcHandles = sourceSnap->timeGrid->handles();
        std::vector<TimeHandle> newHandles;
        newHandles.reserve(srcHandles.size());
        for (const auto& handle : srcHandles) {
            TimeHandle shifted = handle;
            shifted.source_seconds -= offsetSeconds;
            shifted.output_seconds -= offsetSeconds;
            if (shifted.source_seconds >= 0.0 && shifted.source_seconds <= durationSeconds) {
                newHandles.push_back(std::move(shifted));
            }
        }
        if (!newHandles.empty()) {
            payload.timeGrid = TimeGridSnapshot::makeFromHandles(std::move(newHandles),
                                                                 sourceSnap->timeGrid->revision());
        }
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

    ContentPayloadState payload = payloadFromSnapshot(*sourceSnap);
    payload.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(*sourceSnap->audioBuffer);
    payload.pitchCurve = sourceSnap->pitchCurve != nullptr ? sourceSnap->pitchCurve->clone() : nullptr;
    payload.correctedSegments = correctedSegmentsFromCurve(payload.pitchCurve);

    const ContentKey newKey = createStandaloneClipOwner(*standaloneContentRepository_,
                                                        *contentRenderService_,
                                                        std::move(payload));
    return newKey;
}

void OpenTuneAudioProcessor::consumeAudioThreadLogs()
{
    const uint64_t gen = logEventGeneration_.load(std::memory_order_acquire);
    if (gen == logEventReadGeneration_)
        return; // No new events

    logEventReadGeneration_ = gen;
    const AudioThreadLogEvent evt = logEventData_; // Plain read �?single consumer, no tearing risk

    switch (evt.type) {
    case AudioThreadLogEvent::Type::FadeOutComplete:
        AppLogger::log("Playback: fade-out complete, stopped");
        break;
    case AudioThreadLogEvent::Type::CaptureDiag:
        juce::Logger::writeToLog("CaptureDiag: numChannels=" + juce::String(evt.diagNumChannels)
            + " numSamples=" + juce::String(evt.diagNumSamples)
            + " mag=" + juce::String(evt.diagMag, 6)
            + " s[0,1,2,3,64]=" + juce::String(evt.diagS0, 4)
            + "," + juce::String(evt.diagS1, 4)
            + "," + juce::String(evt.diagS2, 4)
            + "," + juce::String(evt.diagS3, 4)
            + "," + juce::String(evt.diagS64, 4));
        break;
    case AudioThreadLogEvent::Type::None:
    default:
        break;
    }
}

} // namespace OpenTune

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenTune::OpenTuneAudioProcessor();
}