#include "PluginProcessor.h"
#include "MaterializationStore.h"
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
#include "Inference/SoundTouchStretcher.h"   // §7 Phase D �?Stage 2 worker (WSOLA, replaces RB)
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

namespace OpenTune {

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

std::vector<Note> normalizeStoredNotes(const std::vector<Note>& notes)
{
    NoteSequence sequence;
    sequence.setNotesSorted(notes);
    return sequence.getNotes();
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

bool hasRemainingPlacementForMaterialization(const StandaloneArrangement& arrangement, uint64_t materializationId)
{
    if (materializationId == 0) {
        return false;
    }

    for (int trackId = 0; trackId < arrangement.getNumTracks(); ++trackId) {
        const int placementCount = arrangement.getNumPlacements(trackId);
        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            if (arrangement.getPlacementByIndex(trackId, placementIndex, placement)
                && placement.materializationId == materializationId) {
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

// ==============================================================================
// F0 Gap Filling for Vocoder (Mel Frame Space)
// ==============================================================================
// 在渲染提交前填补 correctedF0 中的零值间隙：
//   1. 内部间隙：≤10帧用 log-domain 线性插值填�?
//   2. 边界延伸：起�?终点若为零，向边界外查询并延伸填�?
//      - 检测延伸方向是否有 voiced 段，有则延伸到该段起点为�?
//
// 目的：消�?PC-NSF-HiFiGAN �?F0 不连续处的相位震荡（低频砰砰声）
void fillF0GapsForVocoder(
    std::vector<float>& f0,
    const std::shared_ptr<const PitchCurveSnapshot>& snap,
    double frameStartTimeSec,
    double frameEndTimeSec,
    double hopDuration,
    double f0FrameRate,
    bool allowTrailingExtension)
{
    if (f0.empty() || !snap) return;

    constexpr int maxGapFrames = 50;  // ~580ms at 86fps
    const int n = static_cast<int>(f0.size());

    // ---- Step 1: Fill internal gaps with log-domain interpolation ----
    {
        int i = 0;
        while (i < n) {
            // Find next voiced frame
            while (i < n && f0[static_cast<size_t>(i)] <= 0.0f) ++i;
            if (i >= n) break;

            // Find voiced segment end
            int segEnd = i;
            while (segEnd < n && f0[static_cast<size_t>(segEnd)] > 0.0f) ++segEnd;

            // Find next voiced segment after gap
            int gapStart = segEnd;
            while (gapStart < n && f0[static_cast<size_t>(gapStart)] <= 0.0f) ++gapStart;

            if (gapStart >= n) break;  // No more voiced segments

            int gapLen = gapStart - segEnd;
            if (gapLen > 0 && gapLen <= maxGapFrames) {
                // Fill gap with log-domain interpolation
                const float fStart = f0[static_cast<size_t>(segEnd - 1)];
                const float fEnd = f0[static_cast<size_t>(gapStart)];
                const float logStart = std::log2(std::max(fStart, 1e-6f));
                const float logEnd = std::log2(std::max(fEnd, 1e-6f));
                for (int j = 0; j < gapLen; ++j) {
                    float t = static_cast<float>(j + 1) / static_cast<float>(gapLen + 1);
                    f0[static_cast<size_t>(segEnd + j)] = std::pow(2.0f, logStart + (logEnd - logStart) * t);
                }
            }

            i = gapStart;
        }
    }

    // ---- Step 2: Extend leading zeros (f0[0] == 0) ----
    if (n > 0 && f0[0] <= 0.0f) {
        // Find first voiced frame in current chunk
        int firstVoicedIdx = 0;
        while (firstVoicedIdx < n && f0[static_cast<size_t>(firstVoicedIdx)] <= 0.0f) ++firstVoicedIdx;

        if (firstVoicedIdx < n) {
            const float firstVoicedF0 = f0[static_cast<size_t>(firstVoicedIdx)];

            // Query PitchCurve for F0 before this chunk's start
            // We need to look backward from frameStartTimeSec
            const int lookbackF0Frames = 100;  // Look back up to 1 second (100 frames at 100fps)
            const int queryStartFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate)) - lookbackF0Frames;
            const int queryEndFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate));

            std::vector<float> prevF0(static_cast<size_t>(queryEndFrame - queryStartFrame), 0.0f);
            snap->renderF0Range(queryStartFrame, queryEndFrame,
                [&prevF0, queryStartFrame](int frameIndex, const float* data, int length) {
                    if (!data || length <= 0) return;
                    const int offset = frameIndex - queryStartFrame;
                    if (offset < 0) return;
                    const int copyLen = std::min(length, static_cast<int>(prevF0.size()) - offset);
                    if (copyLen > 0) {
                        std::copy(data, data + copyLen, prevF0.begin() + offset);
                    }
                });

            // Find the nearest voiced F0 going backward
            float extendF0 = 0.0f;
            for (int j = static_cast<int>(prevF0.size()) - 1; j >= 0; --j) {
                if (prevF0[static_cast<size_t>(j)] > 0.0f) {
                    extendF0 = prevF0[static_cast<size_t>(j)];
                    break;
                }
            }

            // If we found a voiced F0 before, fill the leading zeros
            // But check if there's a voiced segment between extend point and firstVoicedIdx
            if (extendF0 > 0.0f) {
                // Use the closer F0 value (extendF0 or firstVoicedF0) for smoother transition
                const float fillF0 = (extendF0 > 0.0f && firstVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * firstVoicedF0)  // Geometric mean
                    : (firstVoicedF0 > 0.0f ? firstVoicedF0 : extendF0);

                // Fill leading zeros with gradual transition
                for (int j = 0; j < firstVoicedIdx; ++j) {
                    float t = static_cast<float>(j) / static_cast<float>(firstVoicedIdx + 1);
                    // Linear interpolation in log domain
                    float logFill = std::log2(std::max(fillF0, 1e-6f));
                    float logFirst = std::log2(std::max(firstVoicedF0, 1e-6f));
                    f0[static_cast<size_t>(j)] = std::pow(2.0f, logFill + (logFirst - logFill) * t);
                }
            }
        }
    }

    // ---- Step 3: Extend trailing zeros (f0[n-1] == 0) ----
    if (allowTrailingExtension && n > 0 && f0[static_cast<size_t>(n - 1)] <= 0.0f) {
        // Find last voiced frame in current chunk
        int lastVoicedIdx = n - 1;
        while (lastVoicedIdx >= 0 && f0[static_cast<size_t>(lastVoicedIdx)] <= 0.0f) --lastVoicedIdx;

        if (lastVoicedIdx >= 0) {
            const float lastVoicedF0 = f0[static_cast<size_t>(lastVoicedIdx)];

            // Query PitchCurve for F0 after this chunk's end
            const int lookaheadF0Frames = 100;  // Look ahead up to 1 second
            const int queryStartFrame = static_cast<int>(std::ceil(frameEndTimeSec * f0FrameRate));
            const int queryEndFrame = queryStartFrame + lookaheadF0Frames;

            std::vector<float> nextF0(static_cast<size_t>(queryEndFrame - queryStartFrame), 0.0f);
            snap->renderF0Range(queryStartFrame, queryEndFrame,
                [&nextF0, queryStartFrame](int frameIndex, const float* data, int length) {
                    if (!data || length <= 0) return;
                    const int offset = frameIndex - queryStartFrame;
                    if (offset < 0) return;
                    const int copyLen = std::min(length, static_cast<int>(nextF0.size()) - offset);
                    if (copyLen > 0) {
                        std::copy(data, data + copyLen, nextF0.begin() + offset);
                    }
                });

            // Find the nearest voiced F0 going forward
            float extendF0 = 0.0f;
            for (size_t j = 0; j < nextF0.size(); ++j) {
                if (nextF0[j] > 0.0f) {
                    extendF0 = nextF0[j];
                    break;
                }
            }

            // If we found a voiced F0 after, fill the trailing zeros
            if (extendF0 > 0.0f || lastVoicedF0 > 0.0f) {
                const float fillF0 = (extendF0 > 0.0f && lastVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * lastVoicedF0)
                    : (lastVoicedF0 > 0.0f ? lastVoicedF0 : extendF0);

                // Fill trailing zeros with gradual transition
                const int trailingLen = n - lastVoicedIdx - 1;
                for (int j = 0; j < trailingLen; ++j) {
                    float t = static_cast<float>(j + 1) / static_cast<float>(trailingLen + 1);
                    float logLast = std::log2(std::max(lastVoicedF0, 1e-6f));
                    float logFill = std::log2(std::max(fillF0, 1e-6f));
                    f0[static_cast<size_t>(lastVoicedIdx + 1 + j)] = std::pow(2.0f, logLast + (logFill - logLast) * t);
                }
            }
        }
    }
}

namespace {

void renderPlacementForExport(OpenTuneAudioProcessor& processor,
                              const StandaloneArrangement::Placement& placement,
                              float trackGain,
                              int64_t placementStartInOutput,
                              juce::AudioBuffer<float>& out,
                              int64_t totalLen)
{
    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;

    if (placement.materializationId == 0 || placement.durationSeconds <= 0.0 || placementStartInOutput >= totalLen) {
        return;
    }

    OpenTuneAudioProcessor::PlaybackReadSource source;
    if (!processor.getPlaybackReadSourceByMaterializationId(placement.materializationId, source) || !source.canRead()) {
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
constexpr int kProcessorStateVersion = 8; // v8 adds Placement::clipInSeconds
// vocal-time-stretch §3.8: bumped 5 �?6 to add per-materialization TimeGrid section.
// v5 projects load with auto-seeded identity TimeGrid (output==source).
// Processor state v7 adds per-handle confidence. v6 reads default confidence=Default.
constexpr uint32_t kStandaloneSettingsMagic = 0x4F545353; // OTSS (OpenTune Standalone Settings)
constexpr int kStandaloneSettingsVersion = 1;

// --- Serialization helpers (full state) ---
// Compiled unconditionally into the shared OpenTune lib; getStateInformation /
// setStateInformation dispatch by runtime wrapperType, so both Standalone and
// VST3 binaries link the same compiled object and need these symbols available.

static bool readBytesExact(juce::InputStream& input, void* destination, size_t numBytes)
{
    return numBytes == 0 || input.read(destination, static_cast<int>(numBytes)) == static_cast<int>(numBytes);
}

static void writeFloatVector(juce::OutputStream& output, const std::vector<float>& values)
{
    output.writeInt(static_cast<int>(values.size()));
    if (!values.empty()) {
        output.write(values.data(), values.size() * sizeof(float));
    }
}

static bool readFloatVector(juce::InputStream& input, std::vector<float>& out)
{
    out.clear();
    const int valueCount = input.readInt();
    if (valueCount < 0) {
        return false;
    }

    out.resize(static_cast<size_t>(valueCount));
    return readBytesExact(input, out.data(), out.size() * sizeof(float));
}

static void appendUniqueMaterializationId(std::vector<uint64_t>& materializationIds, uint64_t materializationId)
{
    if (materializationId == 0) {
        return;
    }

    if (std::find(materializationIds.begin(), materializationIds.end(), materializationId) == materializationIds.end()) {
        materializationIds.push_back(materializationId);
    }
}

static void writePitchCurve(juce::OutputStream& output,
                            const std::shared_ptr<PitchCurve>& pitchCurve)
{
    output.writeBool(pitchCurve != nullptr);
    if (pitchCurve == nullptr) {
        return;
    }

    const auto snapshot = pitchCurve->getSnapshot();
    output.writeInt(snapshot->getHopSize());
    output.writeDouble(snapshot->getSampleRate());
    writeFloatVector(output, snapshot->getOriginalF0());
    writeFloatVector(output, snapshot->getOriginalEnergy());

    const auto& segments = snapshot->getCorrectedSegments();
    output.writeInt(static_cast<int>(segments.size()));
    for (const auto& segment : segments) {
        output.writeInt(segment.startFrame);
        output.writeInt(segment.endFrame);
        output.writeInt(static_cast<int>(segment.source));
        output.writeFloat(segment.retuneSpeed);
        output.writeFloat(segment.vibratoDepth);
        output.writeFloat(segment.vibratoRate);
        writeFloatVector(output, segment.f0Data);
    }
}

static std::shared_ptr<PitchCurve> readPitchCurve(juce::InputStream& input)
{
    if (!input.readBool()) {
        return nullptr;
    }

    const int hopSize = input.readInt();
    const double sampleRate = input.readDouble();
    if (hopSize <= 0 || sampleRate <= 0.0) {
        return nullptr;
    }

    std::vector<float> originalF0;
    std::vector<float> originalEnergy;
    if (!readFloatVector(input, originalF0) || !readFloatVector(input, originalEnergy)) {
        return nullptr;
    }

    auto pitchCurve = std::make_shared<PitchCurve>();
    pitchCurve->setHopSize(hopSize);
    pitchCurve->setSampleRate(sampleRate);
    pitchCurve->setOriginalF0(originalF0);
    pitchCurve->setOriginalEnergy(originalEnergy);
    pitchCurve->clearAllCorrections();

    const int segmentCount = input.readInt();
    if (segmentCount < 0) {
        return nullptr;
    }

    for (int segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
        CorrectedSegment segment;
        segment.startFrame = input.readInt();
        segment.endFrame = input.readInt();
        segment.source = static_cast<CorrectedSegment::Source>(input.readInt());
        segment.retuneSpeed = input.readFloat();
        segment.vibratoDepth = input.readFloat();
        segment.vibratoRate = input.readFloat();
        if (!readFloatVector(input, segment.f0Data)) {
            return nullptr;
        }

        if (segment.startFrame < segment.endFrame && !segment.f0Data.empty()) {
            pitchCurve->restoreCorrectedSegment(segment);
        }
    }

    return pitchCurve;
}

static void writeDetectedKey(juce::OutputStream& output, const DetectedKey& detectedKey)
{
    output.writeInt(static_cast<int>(detectedKey.root));
    output.writeInt(static_cast<int>(detectedKey.scale));
    output.writeFloat(detectedKey.confidence);
}

static DetectedKey readDetectedKey(juce::InputStream& input)
{
    DetectedKey detectedKey;
    detectedKey.root = static_cast<Key>(input.readInt());
    detectedKey.scale = static_cast<Scale>(input.readInt());
    detectedKey.confidence = input.readFloat();
    return detectedKey;
}

static void writeNotes(juce::OutputStream& output, const std::vector<Note>& notes)
{
    output.writeInt(static_cast<int>(notes.size()));
    for (const auto& note : notes) {
        output.writeDouble(note.startTime);
        output.writeDouble(note.endTime);
        output.writeFloat(note.pitch);
        output.writeFloat(note.originalPitch);
        output.writeFloat(note.pitchOffset);
        output.writeFloat(note.retuneSpeed);
        output.writeFloat(note.vibratoDepth);
        output.writeFloat(note.vibratoRate);
        output.writeFloat(note.velocity);
        output.writeBool(note.isVoiced);
        output.writeBool(note.selected);
        output.writeBool(note.dirty);
    }
}

static bool readNotes(juce::InputStream& input, std::vector<Note>& out)
{
    out.clear();
    const int noteCount = input.readInt();
    if (noteCount < 0) {
        return false;
    }

    out.reserve(static_cast<size_t>(noteCount));
    for (int noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
        Note note;
        note.startTime = input.readDouble();
        note.endTime = input.readDouble();
        note.pitch = input.readFloat();
        note.originalPitch = input.readFloat();
        note.pitchOffset = input.readFloat();
        note.retuneSpeed = input.readFloat();
        note.vibratoDepth = input.readFloat();
        note.vibratoRate = input.readFloat();
        note.velocity = input.readFloat();
        note.isVoiced = input.readBool();
        note.selected = input.readBool();
        note.dirty = input.readBool();
        out.push_back(note);
    }

    return true;
}

static void writeAudioBuffer(juce::OutputStream& output,
                             const std::shared_ptr<const juce::AudioBuffer<float>>& audioBuffer)
{
    output.writeBool(audioBuffer != nullptr);
    if (audioBuffer == nullptr) {
        return;
    }

    const int numChannels = audioBuffer->getNumChannels();
    const int numSamples = audioBuffer->getNumSamples();
    output.writeInt(numChannels);
    output.writeInt(numSamples);
    for (int channel = 0; channel < numChannels; ++channel) {
        const auto* channelData = audioBuffer->getReadPointer(channel);
        output.write(channelData, static_cast<size_t>(numSamples) * sizeof(float));
    }
}

static std::shared_ptr<const juce::AudioBuffer<float>> readAudioBuffer(juce::InputStream& input)
{
    if (!input.readBool()) {
        return nullptr;
    }

    const int numChannels = input.readInt();
    const int numSamples = input.readInt();
    if (numChannels <= 0 || numSamples <= 0) {
        return nullptr;
    }

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(numChannels, numSamples);
    buffer->clear();
    for (int channel = 0; channel < numChannels; ++channel) {
        if (!readBytesExact(input,
                            buffer->getWritePointer(channel),
                            static_cast<size_t>(numSamples) * sizeof(float))) {
            return nullptr;
        }
    }

    return buffer;
}

#if JucePlugin_Enable_ARA
static bool scanAudioBufferHeader(juce::InputStream& input, bool& hasAudioBuffer)
{
    if (input.getNumBytesRemaining() < 1) {
        return false;
    }

    hasAudioBuffer = input.readBool();
    if (!hasAudioBuffer) {
        return true;
    }

    if (input.getNumBytesRemaining() < 8) {
        return false;
    }

    const int numChannels = input.readInt();
    const int numSamples = input.readInt();
    if (numChannels <= 0 || numSamples <= 0) {
        return false;
    }

    const auto bytesToSkip = static_cast<int64_t>(numChannels)
        * static_cast<int64_t>(numSamples)
        * static_cast<int64_t>(sizeof(float));
    if (bytesToSkip < 0 || bytesToSkip > input.getNumBytesRemaining()) {
        return false;
    }

    input.skipNextBytes(bytesToSkip);
    return true;
}

static bool processorStateContainsMetadataOnlySource(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) {
        return false;
    }

    juce::MemoryInputStream scan(data, static_cast<size_t>(sizeInBytes), false);
    if (scan.getNumBytesRemaining() < 8) {
        return false;
    }

    const int magic = scan.readInt();
    const int version = scan.readInt();
    if (magic != static_cast<int>(kProcessorStateMagic)
        || (version != kProcessorStateVersion && version != 7 && version != 6 && version != 5)) {
        return false;
    }

    if (scan.getNumBytesRemaining() < 16) {
        return false;
    }

    scan.readDouble(); // zoomLevel
    scan.readInt();    // trackHeight

    const int sourceCount = scan.readInt();
    if (sourceCount < 0) {
        return false;
    }

    for (int sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
        if (scan.getNumBytesRemaining() < 8) {
            return false;
        }

        scan.readInt64();  // sourceId
        scan.readString(); // displayName

        if (scan.getNumBytesRemaining() < 8) {
            return false;
        }

        scan.readDouble(); // sampleRate

        bool hasAudioBuffer = false;
        if (!scanAudioBufferHeader(scan, hasAudioBuffer)) {
            return false;
        }

        if (!hasAudioBuffer) {
            return true;
        }
    }

    return false;
}
#endif

static void writeSilentGaps(juce::OutputStream& output,
                            const std::vector<SilentGap>& silentGaps)
{
    output.writeInt(static_cast<int>(silentGaps.size()));
    for (const auto& gap : silentGaps) {
        output.writeInt64(gap.startSample);
        output.writeInt64(gap.endSampleExclusive);
        output.writeFloat(gap.minLevel_dB);
    }
}

static bool readSilentGaps(juce::InputStream& input, std::vector<SilentGap>& out)
{
    out.clear();
    const int gapCount = input.readInt();
    if (gapCount < 0) {
        return false;
    }

    out.reserve(static_cast<size_t>(gapCount));
    for (int gapIndex = 0; gapIndex < gapCount; ++gapIndex) {
        SilentGap gap;
        gap.startSample = input.readInt64();
        gap.endSampleExclusive = input.readInt64();
        gap.minLevel_dB = input.readFloat();
        if (gap.isValid()) {
            out.push_back(gap);
        }
    }

    return true;
}

void OpenTuneAudioProcessor::configureReferenceAnalysisService()
{
    referenceAnalysisService_.setAnalysisFunc(
        [this](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
            ReferenceFeatureSet failed;
            failed.producer = ReferenceFeatureProducer::Game;
            failed.inputFingerprint = jobKey.renderRevision;

            MaterializationStore::MaterializationSnapshot snapshot;
            if (materializationStore_ == nullptr
                || !materializationStore_->getSnapshot(jobKey.materializationId, snapshot)) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis could not read materialization";
                return failed;
            }
            if (static_cast<int64_t>(snapshot.renderRevision) != jobKey.renderRevision) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "AUTO Ref analysis job is stale";
                return failed;
            }
            return buildReferenceFeatureSet(snapshot);
        });
    referenceAnalysisService_.addListener(this);
}

ReferenceFeatureSet OpenTuneAudioProcessor::buildReferenceFeatureSet(
    const MaterializationStore::MaterializationSnapshot& snapshot)
{
    juce::ignoreUnused(experimentalReferenceAlignMode_);
    return buildGameReferenceFeatureSet(snapshot);
}

ReferenceFeatureSet OpenTuneAudioProcessor::buildGameReferenceFeatureSet(
    const MaterializationStore::MaterializationSnapshot& snapshot)
{
    ReferenceFeatureSet result;
    result.producer = ReferenceFeatureProducer::Game;
    result.inputFingerprint = static_cast<int64_t>(snapshot.renderRevision);
    result.sourceDurationSeconds = snapshot.audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(snapshot.audioBuffer->getNumSamples(),
                                           TimeCoordinate::kRenderSampleRate)
        : 0.0;

    auto failGame = [&](const juce::String& reason) {
        result.status = ReferenceFeatureStatus::Failed;
        result.pitch.clear();
        result.timing.clear();
        result.errorMessage = reason;
        return result;
    };

    if (snapshot.audioBuffer == nullptr || snapshot.audioBuffer->getNumSamples() <= 0) {
        return failGame("AUTO Ref GAME analysis requires materialization audio");
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
    const auto* readPtr = snapshot.audioBuffer->getReadPointer(0);
    input.audio.assign(readPtr, readPtr + snapshot.audioBuffer->getNumSamples());
    input.hostSampleRate = TimeCoordinate::kRenderSampleRate;

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
    uint64_t materializationId,
    const ReferenceFeatureSet& result)
{
    if (materializationStore_ == nullptr
        || !result.isReady()
        || result.producer != ReferenceFeatureProducer::Game) {
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!materializationStore_->getSnapshot(materializationId, snapshot)) {
        return;
    }
    if (result.inputFingerprint != static_cast<int64_t>(snapshot.renderRevision)) {
        return;
    }

    materializationStore_->setReferenceFeatures(materializationId, result);

    // Async GAME analysis completed �?seed TimeGrid handles from the extracted timing anchors.
    // ensureTimeToolAnchorSeed is idempotent: if handles already exist or features aren't Game-produced,
    // it returns early. Called here (not in the caller) because this is the single completion point
    // that guarantees ReferenceFeatureStatus::Ready is visible before handle seeding.
    ensureTimeToolAnchorSeed(materializationId);
}

void OpenTuneAudioProcessor::analysisFailed(uint64_t materializationId, const juce::String& reason)
{
    if (materializationStore_ == nullptr) {
        return;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!materializationStore_->getSnapshot(materializationId, snapshot)) {
        return;
    }

    ReferenceFeatureSet failed;
    failed.producer = ReferenceFeatureProducer::Game;
    failed.status = ReferenceFeatureStatus::Failed;
    failed.inputFingerprint = static_cast<int64_t>(snapshot.renderRevision);
    failed.sourceDurationSeconds = snapshot.audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(snapshot.audioBuffer->getNumSamples(),
                                           TimeCoordinate::kRenderSampleRate)
        : 0.0;
    failed.errorMessage = reason;
    materializationStore_->setReferenceFeatures(materializationId, failed);
}

// ============================================================================
// vocal-time-stretch §3.8 �?TimeGrid serialization (state version �?6)
// State version 7 adds the per-handle confidence field.
//
// Layout per materialization (v6+ writes; v7+ writes confidence):
//   int32  handleCount
//   for each handle:
//     int64   id
//     double  source_seconds
//     double  output_seconds
//     int32   kind (HandleKind enum value, range 0-6 in v6, 0-7 in v7)
//     bool    locked
//     uint8   confidence  // v7+ only; v6 reads default to Default(0)
//
// If a v5 (or earlier) project is loaded, readTimeGridHandles writes nothing
// and the createMaterialization auto-seed path produces an identity TimeGrid
// downstream.
// ============================================================================

static void writeTimeGridHandles(juce::OutputStream& output,
                                  const std::shared_ptr<const TimeGridSnapshot>& snapshot)
{
    if (snapshot == nullptr) {
        output.writeInt(0);
        return;
    }
    const auto& handles = snapshot->handles();
    output.writeInt(static_cast<int>(handles.size()));
    for (const auto& h : handles) {
        output.writeInt64(static_cast<juce::int64>(h.id));
        output.writeDouble(h.source_seconds);
        output.writeDouble(h.output_seconds);
        output.writeInt(static_cast<int>(h.kind));
        output.writeBool(h.locked);
        output.writeByte(static_cast<char>(h.confidence)); // v7+
    }
}

static bool readTimeGridHandles(juce::InputStream& input,
                                 std::vector<TimeHandle>& out,
                                 bool readConfidenceField /* v7+ */)
{
    out.clear();
    const int count = input.readInt();
    if (count < 0) return false;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        TimeHandle h;
        h.id             = static_cast<uint64_t>(input.readInt64());
        h.source_seconds = input.readDouble();
        h.output_seconds = input.readDouble();
        const int kindRaw = input.readInt();
        h.kind = static_cast<HandleKind>(kindRaw);
        h.locked = input.readBool();
        if (readConfidenceField) {
            const int confRaw = static_cast<unsigned char>(input.readByte());
            h.confidence = static_cast<Confidence>(confRaw);
        } else {
            h.confidence = Confidence::Default; // v6 backward compat
        }
        out.push_back(h);
    }
    return true;
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
    materializationStore_ = std::make_shared<MaterializationStore>();
    standaloneArrangement_ = std::make_unique<StandaloneArrangement>();
    configureReferenceAnalysisService();

    resamplingManager_ = std::make_shared<ResamplingManager>();

    // Capture session: regular VST3 runtime mode. ARA-capable VST3 builds also
    // need this when the host loads the binary as an unbound insert instance.
    if (wrapperType == juce::AudioProcessor::wrapperType_VST3) {
        Capture::ProcessorBindings bindings;

        bindings.submitForRender = [this](std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                           double sampleRate,
                                           juce::String displayName) -> uint64_t {
            if (!pcm) return 0;
            PreparedImport prepared;
            // prepareImport takes the buffer by rvalue; copy out of shared_ptr
            // into a local owning buffer.
            juce::AudioBuffer<float> owningCopy;
            owningCopy.makeCopyOf(*pcm);
            if (!prepareImport(std::move(owningCopy), sampleRate, displayName, {}, prepared,
                               "vst3-capture-submit"))
                return 0;
            const auto matId = commitPreparedImportAsMaterialization(std::move(prepared));
            if (matId == 0) return 0;
            MaterializationRefreshRequest req;
            req.materializationId = matId;
            requestMaterializationRefresh(req);
            return matId;
        };

        bindings.replaceWithRendered = [this](juce::AudioBuffer<float>& buffer,
                                               int destStart, int numSamples,
                                               uint64_t materializationId,
                                               double readStartSeconds,
                                               double targetSampleRate) {
            if (materializationStore_ == nullptr) return;
            MaterializationStore::PlaybackReadSource readSource;
            if (!materializationStore_->getPlaybackReadSource(materializationId, readSource)
                || !readSource.hasAudio()) {
                buffer.clear(destStart, numSamples);
                return;
            }
            PlaybackReadRequest req;
            req.source.renderCache = readSource.renderCache;
            req.source.audioBuffer = readSource.audioBuffer;
            // §7 �?propagate Stage 2 fast-path fields (vocal-time-stretch).
            req.source.timeStretchCache    = readSource.timeStretchCache;
            req.source.materializationId   = readSource.materializationId;
            req.source.pitchRevision       = readSource.pitchRevision;
            req.source.timeGridRevision    = readSource.timeGridRevision;
            req.source.timeGridIsIdentity  = readSource.timeGridIsIdentity;
            req.readStartSeconds = readStartSeconds;
            req.targetSampleRate = targetSampleRate;
            req.numSamples = numSamples;
            buffer.clear(destStart, numSamples);
            readPlaybackAudio(req, buffer, destStart);
        };

        bindings.isRenderReady = [this](uint64_t materializationId) -> bool {
            return getMaterializationOriginalF0StateById(materializationId) == OriginalF0State::Ready;
        };

        bindings.isRenderFailed = [this](uint64_t materializationId) -> bool {
            return getMaterializationOriginalF0StateById(materializationId) == OriginalF0State::Failed;
        };

        bindings.retireMaterialization = [this](uint64_t materializationId) {
            if (materializationStore_ != nullptr)
                materializationStore_->retireMaterialization(materializationId);
        };

        bindings.containsMaterialization = [this](uint64_t materializationId) -> bool {
            return materializationStore_ != nullptr
                   && materializationStore_->containsMaterialization(materializationId);
        };

        bindings.refreshMaterialization = [this](uint64_t materializationId) {
            // Trigger vocoder-only re-render against the existing materialization. Used
            // by CapturePersistence::deserialize to repopulate RenderCache after restore.
            //
            // CRITICAL: must NOT call requestMaterializationRefresh �?that path re-runs
            // F0 extraction and overwrites the user's restored PitchCurve / Notes. We
            // need the same vocoder-chunk-render path PianoRoll edits already use:
            // it skips F0, reads the existing pitchCurve, runs vocoder, publishes to
            // RenderCache. F0State stays Ready throughout.
            if (materializationStore_ == nullptr)
                return;
            const auto duration = getMaterializationAudioDurationById(materializationId);
            if (duration <= 0.0)
                return;
            enqueueMaterializationPartialRenderById(materializationId, 0.0, duration);
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

    // Detach ARA back-pointer so DocumentController no longer calls into this processor.
#if JucePlugin_Enable_ARA
    if (auto* dc = getDocumentController()) {
        if (dc->getProcessor() == this)
            dc->setProcessor(nullptr);
    }
#endif

    // Cancel any pending async reclaim sweep to avoid JUCE jassert in AsyncUpdater destructor
    cancelPendingUpdate();
    referenceAnalysisService_.removeListener(this);
    referenceAnalysisService_.cancelAll();

    isPlaying_.store(false);
    materializationRefreshAliveFlag_->store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        chunkRenderWorkerRunning_.store(false, std::memory_order_release);
    }
    schedulerCv_.notify_all();
    if (chunkRenderWorkerThread_.joinable()) {
        chunkRenderWorkerThread_.join();
    }

    // §7 (Phase D) �?stop Stage 2 worker before destroying stores
    {
        std::lock_guard<std::mutex> lock(stage2Mutex_);
        stage2WorkerRunning_.store(false, std::memory_order_release);
    }
    stage2Cv_.notify_all();
    if (stage2WorkerThread_.joinable()) {
        stage2WorkerThread_.join();
    }

    if (vocoderDomain_) {
        vocoderDomain_->shutdown();
    }
    if (f0Service_) {
        f0Service_->shutdown();
    }

    AppLogger::shutdown();
}

// ============================================================================
// 推理引擎初始化与生命周期
// ============================================================================

void OpenTuneAudioProcessor::ensureChunkRenderWorkerStarted()
{
    bool expected = false;
    if (!chunkRenderWorkerRunning_.compare_exchange_strong(expected,
                                                           true,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
        return;
    }

    chunkRenderWorkerThread_ = std::thread([this]() { chunkRenderWorkerLoop(); });
}

// ============================================================================
// vocal-time-stretch §7 (Phase D MVP) �?Stage 2 worker lifecycle + processing
// ============================================================================

void OpenTuneAudioProcessor::ensureStage2WorkerStarted()
{
    bool expected = false;
    if (!stage2WorkerRunning_.compare_exchange_strong(expected,
                                                       true,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        return;
    }
    stage2WorkerThread_ = std::thread([this]() { stage2WorkerLoop(); });
}

void OpenTuneAudioProcessor::requestStage2Rebuild(uint64_t materializationId)
{
    if (materializationId == 0) return;
    ensureStage2WorkerStarted();
    {
        std::lock_guard<std::mutex> lock(stage2Mutex_);
        // Coalesce: don't enqueue duplicate matIds; the worker always pulls the
        // most recent revision from the store anyway.
        for (uint64_t pending : stage2RebuildQueue_) {
            if (pending == materializationId) return;
        }
        stage2RebuildQueue_.push_back(materializationId);
        stage2QueueDepth_.store(static_cast<int>(stage2RebuildQueue_.size()),
                                 std::memory_order_release);
    }
    stage2Cv_.notify_one();
}

void OpenTuneAudioProcessor::stage2WorkerLoop()
{
    AppLogger::log("Stage2Worker: started");
    while (true) {
        uint64_t materializationId = 0;
        {
            std::unique_lock<std::mutex> lock(stage2Mutex_);
            stage2Cv_.wait_for(lock, std::chrono::seconds(10), [this]() {
                return !stage2WorkerRunning_.load(std::memory_order_acquire)
                    || !stage2RebuildQueue_.empty();
            });
            if (!stage2WorkerRunning_.load(std::memory_order_acquire)) {
                AppLogger::log("Stage2Worker: stopping");
                return;
            }

            if (stage2RebuildQueue_.empty()) continue;
            materializationId = stage2RebuildQueue_.front();
            stage2RebuildQueue_.pop_front();
            stage2QueueDepth_.store(static_cast<int>(stage2RebuildQueue_.size()),
                                     std::memory_order_release);
        }

        // §7 (Journey-1 fix) �?publish "in-flight" status for UI badge.
        stage2InFlight_.store(true, std::memory_order_release);
        stage2InFlightMatId_.store(materializationId, std::memory_order_release);

        const bool ok = runStage2RebuildForMaterialization(materializationId);

        stage2InFlight_.store(false, std::memory_order_release);
        stage2InFlightMatId_.store(0, std::memory_order_release);

        if (!ok) {
            AppLogger::warn("Stage2Worker: rebuild failed for matId="
                            + juce::String(static_cast<juce::int64>(materializationId)));
        }
    }
}

bool OpenTuneAudioProcessor::runStage2RebuildForMaterialization(uint64_t materializationId)
{
    if (materializationId == 0 || materializationStore_ == nullptr) return false;

    // Pull the freshest snapshot the message thread has published.
    MaterializationStore::MaterializationSnapshot snap;
    if (!materializationStore_->getSnapshot(materializationId, snap)) return false;
    if (snap.audioBuffer == nullptr) return false;
    if (snap.audioBuffer->getNumChannels() <= 0 || snap.audioBuffer->getNumSamples() <= 0) return false;

    // If TimeGrid is identity, nothing to do �?invalidate any stale entry.
    if (snap.timeGrid == nullptr || snap.timeGrid->isIdentity()) {
        materializationStore_->getTimeStretchCache().invalidate(materializationId);
        return true;
    }

    // Lazy-construct (or fetch) the per-materialization stretcher.
    constexpr double sampleRate = TimeCoordinate::kRenderSampleRate;
    auto* stretcher = materializationStore_->getOpenTuneStretcher(materializationId, sampleRate, /*channels=*/1);
    if (stretcher == nullptr) return false;

    // SoundTouch (WSOLA) drives time-stretch via a TempoSchedule derived from the
    // TimeGrid.  Endpoint-locked invariant ensures totalOutputSeconds ==
    // totalSourceSeconds, so the output length matches the input length within
    // a few WSOLA hops (corrected by truncate/zero-pad below).
    auto schedule = stretcher->buildTempoScheduleFromTimeGrid(*snap.timeGrid);
    stretcher->beginRebuild(schedule);

    // ============================================================
    // §7 (Phase E) �?Stage 2 input = Stage 1 (NSF vocoder) output
    //
    // We stream the materialization's playback signal *as if TimeGrid were
    // identity* (i.e., pre-stretch) by calling readPlaybackAudio in chunks
    // with TimeStretchCache fast-path explicitly disabled.  readPlaybackAudio
    // here writes dry source then has RenderCache overlay REPLACE
    // (setSample, not additive) every sample inside published chunk ranges
    // with NSF vocoder output.  When all chunks are published this yields
    // pure vocoder; gaps fall back to dry source.
    //
    // Edge cases:
    //   - PitchCurve untouched / RenderCache empty �?buffer == dry source
    //     (graceful degradation; result equals Phase D MVP behavior).
    //   - RenderCache partially populated (mid-edit) �?vocoder for rendered
    //     chunks, dry for the rest; Phase F may add a "wait for Stage 1
    //     complete" gate.
    // ============================================================
    const int totalSamples = snap.audioBuffer->getNumSamples();
    constexpr int kBlock = 4096;

    // Build a PlaybackReadSource with TimeStretchCache fast-path DISABLED to
    // avoid recursion (Stage 2 reading its own output).
    PlaybackReadSource stage1Source;
    stage1Source.renderCache         = snap.renderCache;
    stage1Source.audioBuffer         = snap.audioBuffer;
    stage1Source.timeStretchCache    = nullptr;   // ⚠️ explicit disable
    stage1Source.materializationId   = 0;          // ⚠️ explicit disable
    stage1Source.timeGridIsIdentity  = true;       // forces dry-path branch

    // Mono read buffer; we ignore stereo because storage is mono-channel-0
    // canonical (per channel-layout-policy).
    juce::AudioBuffer<float> readBuf(/*channels=*/1, kBlock);

    // SoundTouch single-pass push + drain (replaces RB's study + process double pass).
    std::vector<float> output;
    output.reserve(static_cast<size_t>(totalSamples + kBlock));
    std::vector<float> retrieveBuf(static_cast<size_t>(kBlock));

    auto drainAvailable = [&]() {
        while (true) {
            const size_t avail = stretcher->available();
            if (avail == 0) break;
            const size_t want = std::min<size_t>(avail, retrieveBuf.size());
            const size_t got = stretcher->pull(retrieveBuf.data(), want);
            if (got == 0) break;
            output.insert(output.end(),
                          retrieveBuf.begin(),
                          retrieveBuf.begin() + static_cast<std::ptrdiff_t>(got));
        }
    };

    for (int offset = 0; offset < totalSamples; offset += kBlock) {
        const int n = std::min(kBlock, totalSamples - offset);
        readBuf.clear(0, 0, n);

        PlaybackReadRequest req(stage1Source,
                                /*readStartSeconds=*/static_cast<double>(offset) / sampleRate,
                                /*targetSampleRate=*/sampleRate,
                                /*numSamples=*/n);

        const int wrote = readPlaybackAudio(req, readBuf, /*destStart=*/0);
        const int actuallyWrote = juce::jlimit(0, n, wrote);

        // If readPlaybackAudio short-wrote (e.g., end-of-source), pad with
        // dry tail from snap.audioBuffer to keep input length == totalSamples.
        for (int i = actuallyWrote; i < n; ++i) {
            const int srcIdx = offset + i;
            readBuf.setSample(0, i,
                              (srcIdx < totalSamples)
                                  ? snap.audioBuffer->getSample(0, srcIdx)
                                  : 0.0f);
        }
        const bool isLast = (offset + n) >= totalSamples;
        stretcher->push(readBuf.getReadPointer(0), static_cast<size_t>(n), isLast);
        drainAvailable();
    }
    // After push(isLast=true), SoundTouch::flush() has been called �?drain any
    // remaining buffered output (typically a few WSOLA hops worth).
    drainAvailable();

    // Endpoint enforcement (WSOLA discrete hops can drift ±N samples around the
    // mathematical expected output length).  Truncate-or-zero-pad to the value
    // the schedule predicts so TimeStretchCache length contract holds.
    const size_t expectedSamples = stretcher->expectedOutputSamples();
    if (output.size() > expectedSamples) {
        output.resize(expectedSamples);
    } else if (output.size() < expectedSamples) {
        output.resize(expectedSamples, 0.0f);
    }

    // Re-fetch revisions just before publish (they may have advanced again).
    const uint64_t timeGridRev = materializationStore_->getTimeGridRevision(materializationId);

    materializationStore_->getTimeStretchCache().store(materializationId,
                                                        std::move(output),
                                                        /*pitchRev=*/0,
                                                        static_cast<uint32_t>(timeGridRev),
                                                        sampleRate);

    AppLogger::log("Stage2Worker: rebuilt matId="
                   + juce::String(static_cast<juce::int64>(materializationId))
                   + " timeGridRev=" + juce::String(static_cast<juce::int64>(timeGridRev))
                   + " stage1InputSamples=" + juce::String(totalSamples)
                   + " stage2OutputSamples=" + juce::String(static_cast<int>(stretcher->expectedOutputSamples()))
                   + " (SoundTouch WSOLA, Stage 1 via readPlaybackAudio dry+vocoder-overlay)");
    return true;
}

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
    return ensureServiceReady(f0Ready_, f0InitAttempted_, f0InitMutex_, "F0",
        [this](const std::string& modelsDir) {
            if (!f0Service_) {
                if (!ortEnv_) {
                    Ort::InitApi();
                    ortEnv_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
                }
                f0Service_ = std::make_unique<F0InferenceService>(ortEnv_);
            }
            return f0Service_->initialize(modelsDir);
        });
}

std::string OpenTuneAudioProcessor::modelPathForWeight(const std::string& modelDir, VocoderModelWeight weight)
{
    switch (weight) {
        case VocoderModelWeight::Community: return modelDir + "/hifigan.onnx";
        case VocoderModelWeight::Coulin9V4: return modelDir + "/hifigan_coulin9.onnx";
    }
    return modelDir + "/hifigan.onnx";
}

bool OpenTuneAudioProcessor::ensureVocoderReady()
{
    return ensureServiceReady(vocoderReady_, vocoderInitAttempted_, vocoderInitMutex_, "Vocoder",
        [this](const std::string& modelsDir) {
            if (!vocoderDomain_) {
                if (!ortEnv_) {
                    Ort::InitApi();
                    ortEnv_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
                }
                vocoderDomain_ = std::make_unique<VocoderDomain>(ortEnv_);
            }
            const auto modelPath = modelPathForWeight(modelsDir, currentVocoderModelWeight_);
            return vocoderDomain_->initialize(modelPath);
        });
}

bool OpenTuneAudioProcessor::isNoteGenInFlightForMaterialization(uint64_t materializationId) const
{
    if (materializationId == 0) return false;
    std::lock_guard<std::mutex> lk(noteGenInFlightMutex_);
    return noteGenInFlightMatIds_.count(materializationId) > 0;
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
                if (!ortEnv_) {
                    Ort::InitApi();
                    ortEnv_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTune");
                }
                const auto gameDir = juce::File(juce::String(modelsDir))
                                         .getChildFile("GAME").getFullPathName().toStdString();
                if (juce::File(juce::String(gameDir)).getChildFile("encoder.onnx").existsAsFile()) {
                    try {
                        noteGenerator_ = std::make_unique<GameNoteGenerator>(gameDir, *ortEnv_);
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
    
    // 1. 停止 chunk render worker（确保无线程在使�?f0Service_/vocoderDomain_�?
    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        chunkRenderWorkerRunning_.store(false, std::memory_order_release);
    }
    schedulerCv_.notify_all();
    if (chunkRenderWorkerThread_.joinable()) {
        chunkRenderWorkerThread_.join();
    }
    
    // 2. worker 已停，安全释放推理服�?
    if (vocoderDomain_) {
        vocoderDomain_->shutdown();
        vocoderDomain_.reset();
    }
    vocoderReady_.store(false);
    vocoderInitAttempted_.store(false);
    
    if (f0Service_) {
        f0Service_->shutdown();
        f0Service_.reset();
    }
    f0Ready_.store(false);
    f0InitAttempted_.store(false);
    
    // 3. 重置加速检测器并重新检�?
    auto& detector = AccelerationDetector::getInstance();
    detector.reset();
    detector.detect(forceCpu);
    
    AppLogger::info("[Processor] Inference backend reset to: " 
        + juce::String(detector.getBackendName()));
}

void OpenTuneAudioProcessor::setVocoderModelWeight(VocoderModelWeight weight)
{
    if (currentVocoderModelWeight_ == weight) return;  // 幂等

    // 1. �?chunk render worker（对�?resetInferenceBackend 模式�?
    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        chunkRenderWorkerRunning_.store(false, std::memory_order_release);
    }
    schedulerCv_.notify_all();
    if (chunkRenderWorkerThread_.joinable()) {
        chunkRenderWorkerThread_.join();
    }

    // 2. Shutdown + 销�?vocoder domain（释放旧 ONNX session�?
    if (vocoderDomain_) {
        vocoderDomain_->shutdown();
        vocoderDomain_.reset();
    }
    vocoderReady_.store(false);
    vocoderInitAttempted_.store(false);

    // 3. 更新 runtime 权重（worker 已停，线程安全）
    currentVocoderModelWeight_ = weight;

    // 4. 清所�?materialization �?RenderCache + TimeStretchCache
    if (materializationStore_) {
        const auto ids = materializationStore_->getAllActiveMaterializationIds();
        bool hasJobs = false;
        for (const auto matId : ids) {
            std::shared_ptr<RenderCache> cache;
            if (materializationStore_->getRenderCache(matId, cache) && cache) {
                cache->clear();
            }
            MaterializationSnapshot snap;
            if (getMaterializationSnapshotById(matId, snap) && snap.audioBuffer) {
                const int numSamples = snap.audioBuffer->getNumSamples();
                const double durationSec = static_cast<double>(numSamples) / 44100.0;
                if (durationSec > 0.0) {
                    materializationStore_->enqueuePartialRender(matId, 0.0, durationSec, 512);
                    hasJobs = true;
                }
            }
        }
        // �?TimeStretchCache
        materializationStore_->getTimeStretchCache().clear();

        // 5. 重启 chunk worker（最后执行，避免 worker �?vocoderDomain_ 重建竞态）
        //    vocoderDomain_ 保持 nullptr，由 worker �?ensureVocoderReady() 懒创建（vocoderInitMutex_ 保护�?
        if (hasJobs) {
            ensureChunkRenderWorkerStarted();
            schedulerCv_.notify_one();
        }
    }
    // 不重�?F0 / AccelerationDetector / GAME
}

bool OpenTuneAudioProcessor::extractImportedClipOriginalF0(const MaterializationSnapshot& snap,
                                                           F0ExtractionService::Result& out,
                                                           std::string& errorMessage)
{
    if (!ensureF0Ready()) {
        errorMessage = "inference_not_ready";
        AppLogger::log("RecordTrace: F0 initialization unavailable"
            " processor=" + juce::String::toHexString(reinterpret_cast<juce::int64>(this))
            + " f0Ready=" + juce::String(f0Ready_.load() ? 1 : 0));
        return false;
    }

    F0InferenceService* f0Service = nullptr;
    {
        std::lock_guard<std::mutex> lock(f0InitMutex_);
        f0Service = f0Service_.get();
    }

    if (f0Service == nullptr) {
        errorMessage = "f0_service_unavailable";
        AppLogger::log("RecordTrace: F0 service missing after init"
            " processor=" + juce::String::toHexString(reinterpret_cast<juce::int64>(this))
            + " f0Ready=" + juce::String(f0Ready_.load() ? 1 : 0)
            + " f0InitAttempted=" + juce::String(f0InitAttempted_.load() ? 1 : 0));
        return false;
    }

    struct ReleaseGuard {
        F0InferenceService* service = nullptr;
        ~ReleaseGuard()
        {
            if (service != nullptr)
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

        jassert(materializationStore_ != nullptr);

    }

    jassert(materializationStore_ != nullptr);

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
        dc->setProcessor(this);
        AppLogger::log("ARA: didBindToARA - attached processor stores"
            " processor=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(this))
            + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc))
            + " sourceStore=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(sourceStore_.get()))
            + " materializationStore=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(materializationStore_.get())));

        // Replay any pre-bind state that was cached by setStateInformation.
        // After processor attach, isBoundToARA() returns true, so the replay
        // restores directly into the processor-owned stores.
        if (pendingAraState_.getSize() > 0) {
            AppLogger::log("ARA: didBindToARA - replaying cached state ("
                           + juce::String(static_cast<int>(pendingAraState_.getSize()))
                           + " bytes) into processor stores");
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
            updateHostTransportSnapshot(pos);

            {
                const auto hostSnapshot = getHostTransportSnapshot();
                positionAtomic_->store(hostSnapshot.timeSeconds, std::memory_order_relaxed);
                isPlaying_.store(hostSnapshot.isPlaying, std::memory_order_relaxed);
            }
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

        // Diagnostic: once per ~1 second, post a sample snapshot of the input buffer to
        // help diagnose host routing issues (silent buffer / int24-as-float / etc.).
        // Audio thread cannot call AppLogger directly; we stash data into atomics that a
        // periodic message-thread inspection (PluginEditor timer) could later read. For
        // now we just log inline using JUCE Logger which is thread-safe enough for
        // diagnostic-only paths.
        static std::atomic<int> diagBlockCounter { 0 };
        const int blockIdx = diagBlockCounter.fetch_add(1, std::memory_order_relaxed);
        const int blocksPerSecond = static_cast<int>(juce::jmax(1.0, getSampleRate())) / juce::jmax(1, numSamples);
        if (totalNumOutputChannels > 0 && numSamples > 0 && blockIdx % juce::jmax(1, blocksPerSecond) == 0) {
            const float* ch0 = buffer.getReadPointer(0);
            const float mag = buffer.getMagnitude(0, juce::jmin(numSamples, 256));
            juce::Logger::writeToLog("CaptureDiag: numChannels=" + juce::String(totalNumOutputChannels)
                + " numSamples=" + juce::String(numSamples)
                + " mag=" + juce::String(mag, 6)
                + " s[0,1,2,3,64]=" + juce::String(ch0[0], 4)
                + "," + juce::String(ch0[juce::jmin(1, numSamples - 1)], 4)
                + "," + juce::String(ch0[juce::jmin(2, numSamples - 1)], 4)
                + "," + juce::String(ch0[juce::jmin(3, numSamples - 1)], 4)
                + "," + juce::String(ch0[juce::jmin(64, numSamples - 1)], 4));
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
            jassert(materializationStore_ != nullptr);

            MaterializationStore::PlaybackReadSource materializationReadSource;
            if (!materializationStore_->getPlaybackReadSource(placement.materializationId, materializationReadSource)
                || !materializationReadSource.hasAudio()) {
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
            readRequest.source.renderCache = materializationReadSource.renderCache;
            readRequest.source.audioBuffer = materializationReadSource.audioBuffer;
            // ⚡️ vocal-time-stretch §7 �?propagate Stage 2 fast-path fields so
            // readPlaybackAudio can consume the TimeStretchCache when the user
            // has placed non-identity TimeGrid handles.  Forgetting these fields
            // makes the audio path silently degrade to dry passthrough (the bug
            // observed in Journey 1 manual smoke test 2026-05-12).
            readRequest.source.timeStretchCache    = materializationReadSource.timeStretchCache;
            readRequest.source.materializationId   = materializationReadSource.materializationId;
            readRequest.source.pitchRevision       = materializationReadSource.pitchRevision;
            readRequest.source.timeGridRevision    = materializationReadSource.timeGridRevision;
            readRequest.source.timeGridIsIdentity  = materializationReadSource.timeGridIsIdentity;
            readRequest.readStartSeconds = readStartSeconds;
            readRequest.targetSampleRate = deviceSampleRate;
            readRequest.numSamples = samplesToCopy;

            clipReadScratch_.clear();
            const int availableReadSamples = readPlaybackAudio(readRequest, clipReadScratch_, 0);

            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const float* src = clipReadScratch_.getReadPointer(ch);
                float* dst = trackMixScratch_.getWritePointer(ch, offsetInBlock);

                // Use placement-local time (offset by clipInSeconds so fade works
                // relative to visible clip, not from raw materialization start)
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
            AppLogger::log("Playback: fade-out complete, stopped");
            
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
    const juce::SpinLock::ScopedLockType lock(hostTransportSnapshotLock_);
    return hostTransportSnapshot_;
}

void OpenTuneAudioProcessor::updateHostTransportSnapshot(const juce::AudioPlayHead::PositionInfo& positionInfo)
{
    HostTransportSnapshot previousSnapshot;
    {
        const juce::SpinLock::ScopedLockType lock(hostTransportSnapshotLock_);
        previousSnapshot = hostTransportSnapshot_;
    }

    HostTransportSnapshot snapshot = previousSnapshot;

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

    const bool playbackStateChanged = snapshot.isPlaying != previousSnapshot.isPlaying;
    const bool loopStateChanged = snapshot.loopEnabled != previousSnapshot.loopEnabled;
    const bool timelineJumped = std::abs(snapshot.timeSeconds - previousSnapshot.timeSeconds) > 0.050;
    const bool bpmChanged = std::abs(snapshot.bpm - previousSnapshot.bpm) > 0.001;

    if (playbackStateChanged || loopStateChanged || timelineJumped || bpmChanged) {
        AppLogger::log("HostTransportSnapshot: playing=" + juce::String(snapshot.isPlaying ? "true" : "false")
            + " time=" + juce::String(snapshot.timeSeconds, 6)
            + " ppq=" + juce::String(snapshot.ppqPosition, 6)
            + " bpm=" + juce::String(snapshot.bpm, 3)
            + " loop=" + juce::String(snapshot.loopEnabled ? "true" : "false")
            + " loopRange=[" + juce::String(snapshot.loopPpqStart, 6)
            + "," + juce::String(snapshot.loopPpqEnd, 6) + "]");
    }

    const juce::SpinLock::ScopedLockType lock(hostTransportSnapshotLock_);
    hostTransportSnapshot_ = snapshot;
}

RenderCache::ChunkStats OpenTuneAudioProcessor::getMaterializationChunkStatsById(uint64_t materializationId) const
{
    if (materializationId == 0) {
        return {};
    }
    jassert(materializationStore_ != nullptr);

    std::shared_ptr<RenderCache> renderCache;
    if (!materializationStore_->getRenderCache(materializationId, renderCache) || renderCache == nullptr) {
        return {};
    }

    return renderCache->getChunkStats();
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
    jassert(materializationStore_ != nullptr);

    info.materializationId = targetPlacement.materializationId;
    info.placementId = targetPlacement.placementId;
    std::shared_ptr<RenderCache> renderCache;
    if (materializationStore_->getRenderCache(targetPlacement.materializationId, renderCache) && renderCache != nullptr) {
        const auto renderState = renderCache->getStateSnapshot();
        info.chunkStats = renderState.chunkStats;
        info.publishedRevision = 0;
        info.desiredRevision = 0;
    }

    return info;
}

bool OpenTuneAudioProcessor::getPlaybackReadSourceByMaterializationId(uint64_t materializationId, PlaybackReadSource& out) const
{
    out = PlaybackReadSource{};

    if (materializationId == 0) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    MaterializationStore::PlaybackReadSource coreSource;
    if (!materializationStore_->getPlaybackReadSource(materializationId, coreSource)) {
        return false;
    }

    out.renderCache = coreSource.renderCache;
    out.audioBuffer = coreSource.audioBuffer;
    // §7 (Phase D): forward TimeStretchCache + revision snapshot to audio thread
    out.timeStretchCache    = coreSource.timeStretchCache;
    out.materializationId   = coreSource.materializationId;
    out.pitchRevision       = coreSource.pitchRevision;
    out.timeGridRevision    = coreSource.timeGridRevision;
    out.timeGridIsIdentity  = coreSource.timeGridIsIdentity;
    return out.canRead();
}

bool OpenTuneAudioProcessor::freezeRenderBoundaries(const MaterializationSampleRange& materializationRange,
                                                     int64_t startSample,
                                                     int64_t endSampleExclusive,
                                                     int hopSize,
                                                     FrozenRenderBoundaries& out)
{
    out = FrozenRenderBoundaries{};

    if (!materializationRange.isValid() || hopSize <= 0) {
        return false;
    }

    auto& trueStartSample = out.trueStartSample;
    auto& trueEndSample = out.trueEndSample;
    auto& publishSampleCount = out.publishSampleCount;
    auto& frameCount = out.frameCount;
    auto& synthSampleCount = out.synthSampleCount;
    auto& synthEndSample = out.synthEndSample;

    trueStartSample = juce::jlimit(materializationRange.startSample, materializationRange.endSampleExclusive, startSample);
    trueEndSample = juce::jlimit(trueStartSample, materializationRange.endSampleExclusive, endSampleExclusive);
    publishSampleCount = trueEndSample - trueStartSample;
    if (publishSampleCount <= 0) {
        out = FrozenRenderBoundaries{};
        return false;
    }

    const bool isLastChunk = (trueEndSample == materializationRange.endSampleExclusive);
    if (!isLastChunk && (publishSampleCount % hopSize) != 0) {
        out = FrozenRenderBoundaries{};
        return false;
    }

    frameCount = juce::jmax(1, static_cast<int>((publishSampleCount + hopSize - 1) / hopSize));
    synthSampleCount = isLastChunk ? frameCount * hopSize : publishSampleCount;
    synthEndSample = trueStartSample + synthSampleCount;
    out.hopSize = hopSize;
    return true;
}

bool OpenTuneAudioProcessor::preparePublishedAudioFromSynthesis(const FrozenRenderBoundaries& boundaries,
                                                                const std::vector<float>& synthesizedAudio,
                                                                std::vector<float>& publishedAudio)
{
    publishedAudio.clear();

    if (boundaries.publishSampleCount <= 0 || boundaries.synthSampleCount <= 0) {
        return false;
    }

    if (synthesizedAudio.size() != static_cast<size_t>(boundaries.synthSampleCount)) {
        return false;
    }

    publishedAudio.assign(synthesizedAudio.begin(),
                          synthesizedAudio.begin() + static_cast<size_t>(boundaries.publishSampleCount));
    return true;
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

    std::vector<uint64_t> serializedMaterializationIds;
    jassert(standaloneArrangement_ != nullptr);
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const int placementCount = standaloneArrangement_->getNumPlacements(trackId);
        for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
            StandaloneArrangement::Placement placement;
            if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
                continue;
            }

            appendUniqueMaterializationId(serializedMaterializationIds, placement.materializationId);
        }
    }

    // Capture-flow materializations live only inside captureSession_ (no placement),
    // but their full snapshot �?audio + pitchCurve + notes + detectedKey �?must
    // round-trip. Union them into the standard state set.
    if (const auto* captureSession = getCaptureSession()) {
        for (const auto& info : captureSession->listSegments()) {
            if (info.materializationId != 0) {
                appendUniqueMaterializationId(serializedMaterializationIds, info.materializationId);
            }
        }
    }

#if JucePlugin_Enable_ARA
    if (const auto* dc = getDocumentController()) {
        for (const auto& region : dc->getPlaybackRegionProjections()) {
            appendUniqueMaterializationId(serializedMaterializationIds, region.materializationId);
        }
    }
#endif

    jassert(materializationStore_ != nullptr);
    std::vector<MaterializationStore::MaterializationSnapshot> materializationSnapshots;
    for (uint64_t materializationId : serializedMaterializationIds) {
        MaterializationStore::MaterializationSnapshot materializationSnapshot;
        if (!materializationStore_->getSnapshot(materializationId, materializationSnapshot)) {
            continue;
        }

        materializationSnapshots.push_back(std::move(materializationSnapshot));
    }

    std::vector<SourceStore::SourceSnapshot> sourceSnapshots;
    jassert(sourceStore_ != nullptr);
    for (const auto& materializationSnapshot : materializationSnapshots) {
        SourceStore::SourceSnapshot sourceSnapshot;
        if (!sourceStore_->getSnapshot(materializationSnapshot.sourceId, sourceSnapshot)) {
            continue;
        }

        const auto alreadySerialized = std::find_if(sourceSnapshots.begin(),
                                                    sourceSnapshots.end(),
                                                    [&sourceSnapshot](const SourceStore::SourceSnapshot& existing)
                                                    {
                                                        return existing.sourceId == sourceSnapshot.sourceId;
                                                    });
        if (alreadySerialized == sourceSnapshots.end()) {
            sourceSnapshots.push_back(std::move(sourceSnapshot));
        }
    }

    destData.reset();
    juce::MemoryOutputStream output(destData, false);
    output.writeInt(static_cast<int>(kProcessorStateMagic));
    output.writeInt(kProcessorStateVersion);
    // BPM intentionally omitted in non-Standalone path: host owns transport
    // tempo via PlayHead PositionInfo, plugin reads it live.
    output.writeDouble(zoomLevel_);
    output.writeInt(trackHeight_);
    output.writeInt(static_cast<int>(sourceSnapshots.size()));
    for (const auto& sourceSnapshot : sourceSnapshots) {
        output.writeInt64(static_cast<juce::int64>(sourceSnapshot.sourceId));
        output.writeString(sourceSnapshot.displayName);
        output.writeDouble(sourceSnapshot.sampleRate);
        writeAudioBuffer(output, sourceSnapshot.audioBuffer);
    }

    output.writeInt(static_cast<int>(materializationSnapshots.size()));
    for (const auto& contentSnapshot : materializationSnapshots) {
        output.writeInt64(static_cast<juce::int64>(contentSnapshot.materializationId));
        output.writeInt64(static_cast<juce::int64>(contentSnapshot.sourceId));
        output.writeInt64(static_cast<juce::int64>(contentSnapshot.lineageParentMaterializationId));
        output.writeInt64(static_cast<juce::int64>(contentSnapshot.sourceWindow.sourceId));
        output.writeDouble(contentSnapshot.sourceWindow.sourceStartSeconds);
        output.writeDouble(contentSnapshot.sourceWindow.sourceEndSeconds);
        output.writeInt64(static_cast<juce::int64>(contentSnapshot.renderRevision));
        output.writeInt(static_cast<int>(contentSnapshot.originalF0State));
        writeDetectedKey(output, contentSnapshot.detectedKey);
        writeAudioBuffer(output, contentSnapshot.audioBuffer);
        writePitchCurve(output, contentSnapshot.pitchCurve);
        writeNotes(output, contentSnapshot.notes);
        writeSilentGaps(output, contentSnapshot.silentGaps);
        // §3.8 (state v6+): TimeGrid handles
        writeTimeGridHandles(output, contentSnapshot.timeGrid);
    }

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
            output.writeInt64(static_cast<juce::int64>(placement.materializationId));
            output.writeInt64(static_cast<juce::int64>(placement.mappingRevision));
            output.writeDouble(placement.timelineStartSeconds);
            output.writeDouble(placement.durationSeconds);
            output.writeFloat(placement.gain);
            output.writeDouble(placement.fadeInDuration);
            output.writeDouble(placement.fadeOutDuration);
            output.writeDouble(placement.clipInSeconds);
            output.writeString(placement.name);
            output.writeInt(0);  // was placement.colour; kept for binary stream compat
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
    // vocal-time-stretch §3.8: state v6 adds TimeGrid section per materialization.
    // Accept v5 (no TimeGrid), v6 (TimeGrid w/o confidence), v7 (TimeGrid w/ confidence).
    if (magic != static_cast<int>(kProcessorStateMagic)
        || (version != kProcessorStateVersion && version != 7 && version != 6 && version != 5)) {
        AppLogger::warn("StateRestore: unsupported processor state payload (version=" + juce::String(version) + ")");
        return;
    }
    const bool stateHasTimeGrid = (version >= 6);
    const bool stateHasConfidence = (version >= 7);
    const bool stateHasClipInSeconds = (version >= 8);

#if JucePlugin_Enable_ARA
    // ARA-capable VST3 can receive ARA document state before didBindToARA. Only
    // metadata-only source payloads are treated as that ARA pre-bind path;
    // regular unbound VST3 state still restores immediately into local stores.
    if (wrapperType == juce::AudioProcessor::wrapperType_VST3
        && !isBoundToARA()
        && processorStateContainsMetadataOnlySource(data, sizeInBytes)) {
        pendingAraState_.reset();
        pendingAraState_.append(data, static_cast<size_t>(sizeInBytes));
        AppLogger::log("StateRestore: ARA pre-bind metadata-only state - caching "
                       + juce::String(sizeInBytes)
                       + " bytes for replay after didBindToARA without restoring local stores");
        return;
    }
#endif

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

    jassert(materializationStore_ != nullptr);
    materializationStore_->clear();

    jassert(standaloneArrangement_ != nullptr);
    standaloneArrangement_->clear();

    const int sourceCount = input.readInt();
    if (sourceCount < 0) {
        return;
    }

    for (int sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
        const auto sourceId = static_cast<uint64_t>(input.readInt64());
        const auto displayName = input.readString();
        const auto sourceSampleRate = input.readDouble();
        const auto sourceAudioBuffer = readAudioBuffer(input);
        if (sourceId == 0) {
            return;
        }
        jassert(sourceStore_ != nullptr);

        SourceStore::CreateSourceRequest request;
        request.displayName = displayName;
        request.sampleRate = sourceSampleRate > 0.0 ? sourceSampleRate : TimeCoordinate::kRenderSampleRate;
        if (sourceAudioBuffer != nullptr) {
            request.audioBuffer = sourceAudioBuffer;
        } else {
            // Metadata-only source (e.g. ARA): register without PCM buffer.
            // The source may already exist from ARA document controller restore;
            // if not, createSource will register it with identity only.
        }
        if (sourceStore_->createSource(std::move(request), sourceId) != sourceId) {
            return;
        }
    }

    const int materializationCount = input.readInt();
    if (materializationCount < 0) {
        return;
    }

    for (int materializationIndex = 0; materializationIndex < materializationCount; ++materializationIndex) {
        const auto materializationId = static_cast<uint64_t>(input.readInt64());
        const auto sourceId = static_cast<uint64_t>(input.readInt64());
        const auto lineageParentMaterializationId = static_cast<uint64_t>(input.readInt64());
        SourceWindow sourceWindow;
        sourceWindow.sourceId = static_cast<uint64_t>(input.readInt64());
        sourceWindow.sourceStartSeconds = input.readDouble();
        sourceWindow.sourceEndSeconds = input.readDouble();
        const auto renderRevision = static_cast<uint64_t>(input.readInt64());
        const auto originalF0State = static_cast<OriginalF0State>(input.readInt());
        const auto detectedKey = readDetectedKey(input);
        const auto audioBuffer = readAudioBuffer(input);
        auto pitchCurve = readPitchCurve(input);
        std::vector<Note> notes;
        std::vector<SilentGap> silentGaps;
        if (materializationId == 0
            || audioBuffer == nullptr
            || !readNotes(input, notes)
            || !readSilentGaps(input, silentGaps)
            || sourceId == 0) {
            return;
        }

        // §3.8 (state v6+): read TimeGrid handles. v7+ also reads per-handle confidence.
        std::shared_ptr<const TimeGridSnapshot> timeGridSnapshot;
        if (stateHasTimeGrid) {
            std::vector<TimeHandle> handles;
            if (!readTimeGridHandles(input, handles, stateHasConfidence)) {
                return;
            }
            if (!handles.empty()) {
                timeGridSnapshot = TimeGridSnapshot::makeFromHandles(std::move(handles), /*revision=*/1);
                if (timeGridSnapshot == nullptr) {
                    AppLogger::warn("StateRestore: TimeGrid validation failed for matId="
                                    + juce::String(static_cast<juce::int64>(materializationId))
                                    + ", falling back to identity");
                }
            }
        }

        jassert(materializationStore_ != nullptr);

        MaterializationStore::CreateMaterializationRequest request;
        request.sourceId = sourceId;
        request.lineageParentMaterializationId = lineageParentMaterializationId;
        request.sourceWindow = sourceWindow;
        request.audioBuffer = audioBuffer;
        request.pitchCurve = std::move(pitchCurve);
        request.originalF0State = originalF0State;
        request.detectedKey = detectedKey;
        request.renderCache = std::make_shared<RenderCache>();
        request.notes = normalizeStoredNotes(notes);
        request.silentGaps = std::move(silentGaps);
        request.renderRevision = renderRevision;
        request.timeGrid = std::move(timeGridSnapshot);   // null �?createMaterialization auto-seeds identity

        if (materializationStore_->createMaterialization(std::move(request),
                                                         materializationId) != materializationId) {
            return;
        }

        std::shared_ptr<RenderCache> renderCache;
        if (materializationStore_->getRenderCache(materializationId, renderCache) && renderCache != nullptr) {
            renderCache->clear();
        }
    }

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
            placement.materializationId = static_cast<uint64_t>(input.readInt64());
            placement.mappingRevision = static_cast<uint64_t>(input.readInt64());
            placement.timelineStartSeconds = input.readDouble();
            placement.durationSeconds = input.readDouble();
            placement.gain = input.readFloat();
            placement.fadeInDuration = input.readDouble();
            placement.fadeOutDuration = input.readDouble();
            if (stateHasClipInSeconds) {
                placement.clipInSeconds = input.readDouble();
            }
            placement.name = input.readString();
            input.readInt();  // skip legacy placement colour field

            if (placement.materializationId == 0 || !materializationStore_->containsMaterialization(placement.materializationId)) {
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

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneAudioProcessor::getMaterializationAudioBufferById(uint64_t materializationId) const
{
    if (materializationId == 0) {
        return nullptr;
    }
    jassert(materializationStore_ != nullptr);

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    return materializationStore_->getAudioBuffer(materializationId, audioBuffer) ? audioBuffer : nullptr;
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
    jassert(standaloneArrangement_ != nullptr);

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

    MaterializationSnapshot originalSnapshot;
    if (!getMaterializationSnapshotById(originalPlacement.materializationId, originalSnapshot)
        || originalSnapshot.audioBuffer == nullptr) {
        return std::nullopt;
    }

    const int64_t splitSample = TimeCoordinate::secondsToSamples(splitOffsetSeconds, TimeCoordinate::kRenderSampleRate);
    const int64_t totalSamples = originalSnapshot.audioBuffer->getNumSamples();
    if (splitSample <= 0 || splitSample >= totalSamples) {
        return std::nullopt;
    }

    MaterializationStore::CreateMaterializationRequest leadingRequest;
    leadingRequest.sourceId = originalSnapshot.sourceId;
    leadingRequest.lineageParentMaterializationId = originalPlacement.materializationId;
    leadingRequest.sourceWindow = SourceWindow{
        originalSnapshot.sourceWindow.sourceId,
        originalSnapshot.sourceWindow.sourceStartSeconds,
        originalSnapshot.sourceWindow.sourceStartSeconds + splitOffsetSeconds
    };
    leadingRequest.audioBuffer = sliceAudioBuffer(originalSnapshot.audioBuffer, 0, splitSample);
    leadingRequest.pitchCurve = slicePitchCurveToLocalRange(originalSnapshot.pitchCurve, 0.0, splitOffsetSeconds);
    leadingRequest.originalF0State = originalSnapshot.originalF0State;
    leadingRequest.detectedKey = originalSnapshot.detectedKey;
    leadingRequest.renderCache = std::make_shared<RenderCache>();
    leadingRequest.notes = sliceNotesToLocalRange(originalSnapshot.notes, 0.0, splitOffsetSeconds);
    leadingRequest.silentGaps = sliceSilentGaps(originalSnapshot.silentGaps, 0, splitSample);

    MaterializationStore::CreateMaterializationRequest trailingRequest;
    trailingRequest.sourceId = originalSnapshot.sourceId;
    trailingRequest.lineageParentMaterializationId = originalPlacement.materializationId;
    trailingRequest.sourceWindow = SourceWindow{
        originalSnapshot.sourceWindow.sourceId,
        originalSnapshot.sourceWindow.sourceStartSeconds + splitOffsetSeconds,
        originalSnapshot.sourceWindow.sourceEndSeconds
    };
    trailingRequest.audioBuffer = sliceAudioBuffer(originalSnapshot.audioBuffer, splitSample, totalSamples);
    trailingRequest.pitchCurve = slicePitchCurveToLocalRange(originalSnapshot.pitchCurve,
                                                            splitOffsetSeconds,
                                                            originalPlacement.durationSeconds);
    trailingRequest.originalF0State = originalSnapshot.originalF0State;
    trailingRequest.detectedKey = originalSnapshot.detectedKey;
    trailingRequest.renderCache = std::make_shared<RenderCache>();
    trailingRequest.notes = sliceNotesToLocalRange(originalSnapshot.notes,
                                                  splitOffsetSeconds,
                                                  originalPlacement.durationSeconds);
    trailingRequest.silentGaps = sliceSilentGaps(originalSnapshot.silentGaps, splitSample, totalSamples);

    const uint64_t leadingMaterializationId = materializationStore_->createMaterialization(std::move(leadingRequest));
    const uint64_t trailingMaterializationId = materializationStore_->createMaterialization(std::move(trailingRequest));
    if (leadingMaterializationId == 0 || trailingMaterializationId == 0) {
        // Error rollback: new ids not yet in any owner graph, physical delete is correct
        if (leadingMaterializationId != 0) {
            materializationStore_->deleteMaterialization(leadingMaterializationId);
        }
        if (trailingMaterializationId != 0) {
            materializationStore_->deleteMaterialization(trailingMaterializationId);
        }
        return std::nullopt;
    }

    StandaloneArrangement::Placement leadingPlacement = originalPlacement;
    leadingPlacement.placementId = 0;
    leadingPlacement.materializationId = leadingMaterializationId;
    leadingPlacement.durationSeconds = splitOffsetSeconds;
    leadingPlacement.fadeOutDuration = 0.0;
    // leading clipInSeconds stays the same as original (trim offset from original)
    // leadingPlacement.clipInSeconds = originalPlacement.clipInSeconds; (already correct from copy)
    ++leadingPlacement.mappingRevision;

    StandaloneArrangement::Placement trailingPlacement = originalPlacement;
    trailingPlacement.placementId = 0;
    trailingPlacement.materializationId = trailingMaterializationId;
    trailingPlacement.timelineStartSeconds = splitSeconds;
    trailingPlacement.durationSeconds = originalPlacement.durationSeconds - splitOffsetSeconds;
    trailingPlacement.fadeInDuration = 0.0;
    trailingPlacement.clipInSeconds = originalPlacement.clipInSeconds + splitOffsetSeconds;
    ++trailingPlacement.mappingRevision;

    if (!standaloneArrangement_->insertPlacement(trackId, placementIndex, leadingPlacement)) {
        // Error rollback
        materializationStore_->deleteMaterialization(leadingMaterializationId);
        materializationStore_->deleteMaterialization(trailingMaterializationId);
        return std::nullopt;
    }

    if (!standaloneArrangement_->insertPlacement(trackId, placementIndex + 1, trailingPlacement)) {
        // Error rollback
        standaloneArrangement_->deletePlacementById(trackId, leadingPlacement.placementId, nullptr, nullptr);
        materializationStore_->deleteMaterialization(leadingMaterializationId);
        materializationStore_->deleteMaterialization(trailingMaterializationId);
        return std::nullopt;
    }

    // Retire original placement and materialization (do not physically delete; sweep will reclaim when safe)
    standaloneArrangement_->retirePlacement(trackId, originalPlacement.placementId);
    materializationStore_->retireMaterialization(originalPlacement.materializationId);

    standaloneArrangement_->selectPlacement(trackId, trailingPlacement.placementId);
    scheduleReclaimSweep();

    SplitOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = originalSnapshot.sourceId;
    outcome.originalPlacementId = originalPlacement.placementId;
    outcome.originalMaterializationId = originalPlacement.materializationId;
    outcome.leadingPlacementId = leadingPlacement.placementId;
    outcome.trailingPlacementId = trailingPlacement.placementId;
    outcome.leadingMaterializationId = leadingMaterializationId;
    outcome.trailingMaterializationId = trailingMaterializationId;
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
    jassert(standaloneArrangement_ != nullptr);

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

    MaterializationSnapshot leadingSnapshot;
    MaterializationSnapshot trailingSnapshot;
    if (!getMaterializationSnapshotById(leadingPlacement.materializationId, leadingSnapshot)
        || !getMaterializationSnapshotById(trailingPlacement.materializationId, trailingSnapshot)
        || leadingSnapshot.audioBuffer == nullptr
        || trailingSnapshot.audioBuffer == nullptr
        || leadingSnapshot.sourceId == 0
        || leadingSnapshot.sourceId != trailingSnapshot.sourceId) {
        AppLogger::log("Merge rejected: placements do not resolve to the same source lineage");
        return std::nullopt;
    }

    if (!nearlyEqualSeconds(leadingSnapshot.sourceWindow.sourceEndSeconds, trailingSnapshot.sourceWindow.sourceStartSeconds)) {
        AppLogger::log("Merge rejected: materializations do not describe one contiguous source provenance window");
        return std::nullopt;
    }

    if (!detectedKeysMatch(leadingSnapshot.detectedKey, trailingSnapshot.detectedKey)) {
        AppLogger::log("Merge rejected: materialization metadata diverged");
        return std::nullopt;
    }

    if (leadingSnapshot.originalF0State == OriginalF0State::Extracting
        || trailingSnapshot.originalF0State == OriginalF0State::Extracting) {
        AppLogger::log("Merge rejected: materialization payload is still being refreshed");
        return std::nullopt;
    }

    const auto mergedPitchCurve = mergePitchCurves(leadingSnapshot.pitchCurve,
                                                   trailingSnapshot.pitchCurve,
                                                   leadingSnapshot.originalF0State,
                                                   trailingSnapshot.originalF0State);
    const bool mergedCurveRejected = (leadingSnapshot.pitchCurve != nullptr || trailingSnapshot.pitchCurve != nullptr)
        && mergedPitchCurve == nullptr;
    if (mergedCurveRejected) {
        AppLogger::log("Merge rejected: pitch-curve payload cannot be merged without data loss");
        return std::nullopt;
    }

    const int leadingSamples = leadingSnapshot.audioBuffer->getNumSamples();
    const int trailingSamples = trailingSnapshot.audioBuffer->getNumSamples();
    const double leadingDurationSeconds = TimeCoordinate::samplesToSeconds(leadingSamples, TimeCoordinate::kRenderSampleRate);
    auto mergedBuffer = std::make_shared<juce::AudioBuffer<float>>(juce::jmax(leadingSnapshot.audioBuffer->getNumChannels(),
                                                                              trailingSnapshot.audioBuffer->getNumChannels()),
                                                                   leadingSamples + trailingSamples);
    mergedBuffer->clear();
    for (int channel = 0; channel < mergedBuffer->getNumChannels(); ++channel) {
        if (channel < leadingSnapshot.audioBuffer->getNumChannels()) {
            mergedBuffer->copyFrom(channel, 0, *leadingSnapshot.audioBuffer, channel, 0, leadingSamples);
        }
        if (channel < trailingSnapshot.audioBuffer->getNumChannels()) {
            mergedBuffer->copyFrom(channel, leadingSamples, *trailingSnapshot.audioBuffer, channel, 0, trailingSamples);
        }
    }

    std::vector<Note> mergedNotes = leadingSnapshot.notes;
    for (auto note : trailingSnapshot.notes) {
        note.startTime += leadingDurationSeconds;
        note.endTime += leadingDurationSeconds;
        mergedNotes.push_back(note);
    }
    mergedNotes = normalizeStoredNotes(mergedNotes);

    MaterializationStore::CreateMaterializationRequest mergedRequest;
    mergedRequest.sourceId = leadingSnapshot.sourceId;
    mergedRequest.audioBuffer = mergedBuffer;
    mergedRequest.pitchCurve = mergedPitchCurve;
    mergedRequest.originalF0State = leadingSnapshot.originalF0State;
    mergedRequest.detectedKey = leadingSnapshot.detectedKey;
    mergedRequest.renderCache = std::make_shared<RenderCache>();
    mergedRequest.notes = std::move(mergedNotes);
    mergedRequest.silentGaps = mergeSilentGaps(leadingSnapshot.silentGaps, trailingSnapshot.silentGaps, leadingSamples);
    mergedRequest.sourceWindow = SourceWindow{
        leadingSnapshot.sourceWindow.sourceId,
        leadingSnapshot.sourceWindow.sourceStartSeconds,
        trailingSnapshot.sourceWindow.sourceEndSeconds
    };

    const uint64_t mergedMaterializationId = materializationStore_->createMaterialization(std::move(mergedRequest));
    if (mergedMaterializationId == 0) {
        return std::nullopt;
    }

    StandaloneArrangement::Placement mergedPlacement = leadingPlacement;
    mergedPlacement.placementId = 0;
    mergedPlacement.materializationId = mergedMaterializationId;
    mergedPlacement.durationSeconds = leadingPlacement.durationSeconds + trailingPlacement.durationSeconds;
    mergedPlacement.fadeOutDuration = trailingPlacement.fadeOutDuration;
    mergedPlacement.clipInSeconds = leadingPlacement.clipInSeconds; // merge uses leading trim offset
    ++mergedPlacement.mappingRevision;

    const int mergedInsertIndex = targetPlacementIndex >= 0 ? targetPlacementIndex : 0;
    if (!standaloneArrangement_->insertPlacement(trackId, mergedInsertIndex, mergedPlacement)) {
        materializationStore_->deleteMaterialization(mergedMaterializationId);
        return std::nullopt;
    }

    // Retire leading and trailing placements and materializations (sweep will reclaim when safe)
    standaloneArrangement_->retirePlacement(trackId, trailingPlacementId);
    standaloneArrangement_->retirePlacement(trackId, leadingPlacementId);
    materializationStore_->retireMaterialization(leadingPlacement.materializationId);
    materializationStore_->retireMaterialization(trailingPlacement.materializationId);

    if (targetPlacementIndex >= 0) {
        standaloneArrangement_->setSelectedPlacementIndex(trackId, targetPlacementIndex);
    } else {
        standaloneArrangement_->selectPlacement(trackId, mergedPlacement.placementId);
    }

    scheduleReclaimSweep();

    MergeOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = leadingSnapshot.sourceId;
    outcome.leadingPlacementId = leadingPlacementId;
    outcome.trailingPlacementId = trailingPlacementId;
    outcome.leadingMaterializationId = leadingPlacement.materializationId;
    outcome.trailingMaterializationId = trailingPlacement.materializationId;
    outcome.mergedPlacementId = mergedPlacement.placementId;
    outcome.mergedMaterializationId = mergedMaterializationId;
    return outcome;
}

std::optional<DeleteOutcome> OpenTuneAudioProcessor::deletePlacement(int trackId, int placementIndex)
{
    jassert(standaloneArrangement_ != nullptr);
    jassert(materializationStore_ != nullptr);

    StandaloneArrangement::Placement placement;
    if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
        return std::nullopt;
    }

    MaterializationSnapshot snapshot;
    uint64_t sourceId = 0;
    if (getMaterializationSnapshotById(placement.materializationId, snapshot)) {
        sourceId = snapshot.sourceId;
    }

    standaloneArrangement_->retirePlacement(trackId, placement.placementId);
    materializationStore_->retireMaterialization(placement.materializationId);
    scheduleReclaimSweep();

    DeleteOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = sourceId;
    outcome.placementId = placement.placementId;
    outcome.materializationId = placement.materializationId;
    return outcome;
}



// ============================================================================
// 垃圾回收（Reclaim Sweep�?
// ============================================================================

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
    // Cancel any pending async reclaim sweep to avoid double-run on sync sweep.
    cancelPendingUpdate();

    jassert(materializationStore_ != nullptr);
    jassert(standaloneArrangement_ != nullptr);

    // Phase 1: physically erase retired placements.
    {
        const auto retiredPlacements = standaloneArrangement_->getRetiredPlacements();
        for (const auto& entry : retiredPlacements) {
            standaloneArrangement_->deletePlacementById(entry.trackId, entry.placementId, nullptr, nullptr);
        }
    }

    // Phase 2: reclaim retired materializations whose reference counters are all zero.
    // Counters: (a) any-state placement refs in StandaloneArrangement, (b) ARA published-region refs.
    // A retired materialization with both at zero is unreachable from any owner and is safe to
    // physically delete. The cascade below switches its source to retired (not hard-delete) so
    // that Phase 3 can apply the same protection to sources.
    const auto retiredIds = materializationStore_->getRetiredIds();
    for (const uint64_t id : retiredIds) {
        // (a) standalone arrangement reference (active OR retired placement)
        if (standaloneArrangement_->referencesMaterializationAnyState(id)) {
            continue;
        }

#if JucePlugin_Enable_ARA
        // (b) ARA playback-region reference (only meaningful when ARA is compiled in)
        if (const auto* dc = getDocumentController()) {
            if (dc->referencesMaterialization(id)) continue;
        }
#endif

        // All references zero �?physically reclaim the materialization, then cascade-retire the
        // source if it has lost its last active materialization.
        const uint64_t sourceId = materializationStore_->getSourceIdAnyState(id);
        if (materializationStore_->physicallyDeleteIfReclaimable(id)) {
            if (sourceId != 0 && sourceStore_ != nullptr
                && !materializationStore_->hasMaterializationForSource(sourceId)
                && sourceStore_->containsSource(sourceId)) {
                sourceStore_->retireSource(sourceId);
            }
        }
    }

    // Phase 3: reclaim retired sources whose reference counter is zero.
    // Counters: (a) any-state materialization refs in MaterializationStore.
    if (sourceStore_ != nullptr) {
        const auto retiredSourceIds = sourceStore_->getRetiredSourceIds();
        for (const uint64_t sourceId : retiredSourceIds) {
            if (materializationStore_->hasMaterializationForSourceAnyState(sourceId)) {
                continue;
            }
            sourceStore_->physicallyDeleteIfReclaimable(sourceId);
        }
    }
}

// ============================================================================
// Materialization 读写代理接口
// ============================================================================

bool OpenTuneAudioProcessor::getMaterializationSnapshotById(uint64_t materializationId, MaterializationSnapshot& out) const
{
    out = MaterializationSnapshot{};
    if (materializationId == 0) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    MaterializationStore::MaterializationSnapshot coreSnapshot;
    if (!materializationStore_->getSnapshot(materializationId, coreSnapshot)) {
        return false;
    }

    out.materializationId = coreSnapshot.materializationId;
    out.sourceId = coreSnapshot.sourceId;
    out.lineageParentMaterializationId = coreSnapshot.lineageParentMaterializationId;
    out.sourceWindow = coreSnapshot.sourceWindow;
    out.audioBuffer = coreSnapshot.audioBuffer;
    out.renderRevision = coreSnapshot.renderRevision;
    out.pitchCurve = coreSnapshot.pitchCurve;
    out.originalF0State = coreSnapshot.originalF0State;
    out.detectedKey = coreSnapshot.detectedKey;
    out.renderCache = coreSnapshot.renderCache;
    out.notes = coreSnapshot.notes;
    out.notesRevision = coreSnapshot.notesRevision;
    out.silentGaps = coreSnapshot.silentGaps;
    return out.audioBuffer != nullptr;
}

double OpenTuneAudioProcessor::getMaterializationAudioDurationById(uint64_t materializationId) const noexcept
{
    if (materializationId == 0)
        return 0.0;
    jassert(materializationStore_ != nullptr);
    return materializationStore_->getMaterializationAudioDurationById(materializationId);
}

bool OpenTuneAudioProcessor::getSourceSnapshotById(uint64_t sourceId, SourceStore::SourceSnapshot& out) const
{
    out = SourceStore::SourceSnapshot{};
    return sourceId != 0
        && sourceStore_ != nullptr
        && sourceStore_->getSnapshot(sourceId, out);
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

bool OpenTuneAudioProcessor::getMaterializationChunkBoundariesById(uint64_t materializationId, std::vector<double>& outSeconds) const
{
    outSeconds.clear();
    if (materializationId == 0) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    MaterializationStore::MaterializationSnapshot coreSnapshot;
    if (!materializationStore_->getSnapshot(materializationId, coreSnapshot) || coreSnapshot.audioBuffer == nullptr) {
        return false;
    }

    int hopSize = 512;
    if (vocoderDomain_) {
        const int currentHopSize = vocoderDomain_->getVocoderHopSize();
        if (currentHopSize > 0) {
            hopSize = currentHopSize;
        }
    }

    MaterializationSampleRange materializationRange;
    materializationRange.startSample = 0;
    materializationRange.endSampleExclusive = static_cast<int64_t>(coreSnapshot.audioBuffer->getNumSamples());

    const auto boundaries = MaterializationStore::buildChunkBoundariesFromSilentGaps(materializationRange.endSampleExclusive, coreSnapshot.silentGaps, hopSize);
    outSeconds.reserve(boundaries.size());
    for (const auto sample : boundaries) {
        outSeconds.push_back(TimeCoordinate::samplesToSeconds(sample - materializationRange.startSample,
                                                              TimeCoordinate::kRenderSampleRate));
    }

    return true;
}

bool OpenTuneAudioProcessor::replaceMaterializationAudioById(uint64_t materializationId,
                                                      std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                                      std::vector<SilentGap> silentGaps)
{
    if (materializationId == 0 || audioBuffer == nullptr) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    if (audioBuffer->getNumChannels() <= 0 || audioBuffer->getNumSamples() <= 0) {
        return false;
    }

    return materializationStore_->replaceAudio(materializationId,
                                               std::move(audioBuffer),
                                               std::move(silentGaps));
}

uint64_t OpenTuneAudioProcessor::replaceMaterializationWithNewLineage(uint64_t oldId,
                                                       MaterializationStore::CreateMaterializationRequest request)
{
    if (oldId == 0) {
        return 0;
    }
    jassert(materializationStore_ != nullptr);

    return materializationStore_->replaceMaterializationWithNewLineage(oldId,
                                                                       std::move(request));
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

    if (getMaterializationAudioBufferById(placement.materializationId) == nullptr) {
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
                             placement,
                             standaloneArrangement_->getTrackVolume(trackId),
                             0,
                             out,
                             placementLen);
    
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

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex) {
        StandaloneArrangement::Placement placement;
        if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
            continue;
        }
        if (getMaterializationAudioBufferById(placement.materializationId) == nullptr) {
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
        if (getMaterializationAudioBufferById(placement.materializationId) == nullptr) {
            continue;
        }
        const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
        renderPlacementForExport(*this, placement, trackVolume, placementStart, out, totalLen);
    }

    return writeAudioBufferToWavFile(out, file, &lastExportError_);
}

bool OpenTuneAudioProcessor::exportMasterMixAudio(const juce::File& file) {
    jassert(standaloneArrangement_ != nullptr);

    const auto playbackSnapshot = standaloneArrangement_->loadPlaybackSnapshot();
    if (playbackSnapshot == nullptr) {
        return false;
    }

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const auto& track = playbackSnapshot->tracks[static_cast<size_t>(trackId)];
        for (const auto& placement : track.placements) {
            if (getMaterializationAudioBufferById(placement.materializationId) == nullptr) {
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
            if (getMaterializationAudioBufferById(placement.materializationId) == nullptr) {
                continue;
            }
            const int64_t placementStart = TimeCoordinate::secondsToSamples(placement.timelineStartSeconds, kExportSr);
            renderPlacementForExport(*this, placement, track.volume, placementStart, mix, totalLen);
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

    const juce::SpinLock::ScopedLockType lock(hostTransportSnapshotLock_);
    hostTransportSnapshot_.bpm = bpm;
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

    // 导入后的 materialization �?shared runtime 内统一落到固定 44.1kHz �?materialization-local 存储采样率�?
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

uint64_t OpenTuneAudioProcessor::ensureSourceAndCreateMaterialization(PreparedImport&& prepared, uint64_t& sourceId, bool& createdSource)
{
    jassert(sourceStore_ != nullptr && materializationStore_ != nullptr);

    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(prepared.storedAudioBuffer));
    createdSource = false;
    if (sourceId == 0) {
        SourceStore::CreateSourceRequest sourceRequest;
        sourceRequest.displayName = prepared.displayName;
        sourceRequest.sourceFilePath = prepared.sourceFilePath;
        sourceRequest.audioBuffer = storedAudioBuffer;
        sourceRequest.sampleRate = TimeCoordinate::kRenderSampleRate;
        sourceId = sourceStore_->createSource(std::move(sourceRequest));
        if (sourceId == 0) return 0;
        createdSource = true;
    } else if (!sourceStore_->containsSource(sourceId)) {
        return 0;
    }

    MaterializationStore::CreateMaterializationRequest request;
    request.sourceId = sourceId;
    request.audioBuffer = storedAudioBuffer;
    request.sourceWindow = prepared.sourceWindow;
    if (request.sourceWindow.sourceId == 0) {
        const double durationSeconds = TimeCoordinate::samplesToSeconds(
            request.audioBuffer->getNumSamples(), TimeCoordinate::kRenderSampleRate);
        request.sourceWindow = SourceWindow{sourceId, 0.0, durationSeconds};
    }
    request.originalF0State = OriginalF0State::NotRequested;
    request.silentGaps = std::move(prepared.silentGaps);
    request.renderCache = std::make_shared<RenderCache>();

    const uint64_t materializationId = materializationStore_->createMaterialization(std::move(request));
    if (materializationId == 0 && createdSource) {
        sourceStore_->deleteSource(sourceId);
    }
    return materializationId;
}

OpenTuneAudioProcessor::CommittedPlacement OpenTuneAudioProcessor::commitPreparedImportAsPlacement(
    PreparedImport&& prepared, const ImportPlacement& placement, uint64_t sourceId)
{
    if (!placement.isValid()) return {};
    jassert(standaloneArrangement_ != nullptr);

    const juce::String displayName = prepared.displayName;
    bool createdSource = false;
    const uint64_t materializationId = ensureSourceAndCreateMaterialization(std::move(prepared), sourceId, createdSource);
    if (materializationId == 0) return {};

    const double materializationDurationSeconds = getMaterializationAudioDurationById(materializationId);

    StandaloneArrangement::Placement importedPlacement;
    importedPlacement.materializationId = materializationId;
    importedPlacement.mappingRevision = 0;
    importedPlacement.timelineStartSeconds = placement.timelineStartSeconds;
    importedPlacement.durationSeconds = materializationDurationSeconds;
    importedPlacement.gain = 1.0f;
    importedPlacement.name = displayName;
    // importedPlacement.colour removed �?no longer a field on Placement

    if (!standaloneArrangement_->insertPlacement(placement.trackId, importedPlacement)) {
        materializationStore_->deleteMaterialization(materializationId);
        if (createdSource) sourceStore_->deleteSource(sourceId);
        return {};
    }
    standaloneArrangement_->selectPlacement(placement.trackId, importedPlacement.placementId);

    return { sourceId, materializationId, importedPlacement.placementId };
}

uint64_t OpenTuneAudioProcessor::commitPreparedImportAsMaterialization(PreparedImport&& prepared, uint64_t sourceId)
{
    bool createdSource = false;
    return ensureSourceAndCreateMaterialization(std::move(prepared), sourceId, createdSource);
}

#if JucePlugin_Enable_ARA




std::optional<OpenTuneAudioProcessor::AraRegionMaterializationBirthResult>
OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0(AraOriginalF0BirthRequest request)
{
    // ================================================================
    // 1. Validation �?immediately take ownership of the reader lease
    // ================================================================
    auto readerLease = std::move(request.readerLease);
    if (readerLease == nullptr || request.audioSource == nullptr || request.sourceId == 0
        || request.numChannels <= 0 || request.numSamples <= 0 || request.sourceSampleRate <= 0.0)
        return std::nullopt;

    const juce::String sourceName = request.audioSource->getName() != nullptr
        ? juce::String::fromUTF8(request.audioSource->getName())
        : juce::String("ARA Source");

    AppLogger::info("ARA OriginalF0 pipeline: start sourceId="
        + juce::String(static_cast<juce::int64>(request.sourceId))
        + " window=[" + juce::String(request.sourceWindow.sourceStartSeconds, 3)
        + ", " + juce::String(request.sourceWindow.sourceEndSeconds, 3) + "]");

    // ================================================================
    // 2. Source window bounds
    // ================================================================
    const int64_t sourceStartSample = static_cast<int64_t>(
        std::round(request.sourceWindow.sourceStartSeconds * request.sourceSampleRate));
    const int64_t sourceEndSample = static_cast<int64_t>(
        std::round(request.sourceWindow.sourceEndSeconds * request.sourceSampleRate));
    const int64_t windowSamples = std::max<int64_t>(0,
        std::min<int64_t>(sourceEndSample, request.numSamples)
        - std::max<int64_t>(0, sourceStartSample));

    if (windowSamples <= 0)
        return std::nullopt;

    // ================================================================
    // 3. Allocate playable accum buffer (multi-channel)
    // ================================================================
    juce::AudioBuffer<float> playableAccum(request.numChannels, static_cast<int>(windowSamples));
    playableAccum.clear();

    // ================================================================
    // 4. Chunked ARA read �?fail fast on read failure (no silence fill)
    // ================================================================
    constexpr int64_t kChunkSamples = 32768;
    {
        int64_t readOffset = sourceStartSample;
        int64_t accumWriteOffset = 0;
        int64_t remaining = windowSamples;

        while (remaining > 0)
        {
            const int64_t chunkSamples = std::min(kChunkSamples, remaining);
            const int accumOffset = static_cast<int>(accumWriteOffset);

            std::vector<void*> channelPointers(static_cast<size_t>(request.numChannels));
            for (int ch = 0; ch < request.numChannels; ++ch)
                channelPointers[static_cast<size_t>(ch)] = playableAccum.getWritePointer(ch, accumOffset);

            if (!readerLease->readAudioSamples(readOffset,
                                               static_cast<int>(chunkSamples),
                                               channelPointers.data()))
            {
                AppLogger::error("ARA OriginalF0 pipeline: readAudioSamples failed at offset="
                    + juce::String(readOffset) + " chunkSamples=" + juce::String(chunkSamples));
                return std::nullopt;
            }

            readOffset += chunkSamples;
            accumWriteOffset += chunkSamples;
            remaining -= chunkSamples;
        }
    }

    AppLogger::info("ARA OriginalF0 pipeline: playableBuffer44k built samples="
        + juce::String(playableAccum.getNumSamples()));

    // ================================================================
    // 5. Extract ch0 for RMVPE (source sample rate �?RMVPE internally
    //    handles 16kHz downsampling via ResamplingManager)
    // ================================================================
    std::vector<float> channel0Data(static_cast<size_t>(playableAccum.getNumSamples()));
    {
        const float* ch0Read = playableAccum.getReadPointer(0);
        std::copy(ch0Read, ch0Read + playableAccum.getNumSamples(), channel0Data.begin());
    }

    // ================================================================
    // 6. Ensure source exists in SourceStore (metadata-only, no full PCM)
    // ================================================================
    if (!sourceStore_->containsSource(request.sourceId))
    {
        SourceStore::CreateSourceRequest sourceRequest;
        sourceRequest.displayName = sourceName;
        sourceRequest.numChannels = request.numChannels;
        sourceRequest.numSamples = request.numSamples;
        sourceRequest.sampleRate = request.sourceSampleRate > 0.0
            ? request.sourceSampleRate
            : TimeCoordinate::kRenderSampleRate;
        if (sourceStore_->createSource(std::move(sourceRequest), request.sourceId) != request.sourceId)
            return std::nullopt;
    }

    // ================================================================
    // 7. prepareImport (internally resamples to 44.1kHz) then birth
    // ================================================================
    PreparedImport preparedImport;
    if (!prepareImport(std::move(playableAccum), request.sourceSampleRate, sourceName, {}, preparedImport,
                       "ara-hydrate"))
    {
        channel0Data.clear();
        channel0Data.shrink_to_fit();
        return std::nullopt;
    }

    preparedImport.silentGaps = SilentGapDetector::detectAllGapsAdaptive(preparedImport.storedAudioBuffer);

    preparedImport.sourceWindow = SourceWindow{request.sourceId,
                                               request.sourceWindow.sourceStartSeconds,
                                               request.sourceWindow.sourceEndSeconds};

    const uint64_t materializationId = commitPreparedImportAsMaterialization(std::move(preparedImport), request.sourceId);
    if (materializationId == 0)
    {
        channel0Data.clear();
        channel0Data.shrink_to_fit();
        return std::nullopt;
    }

    AppLogger::info("ARA OriginalF0 pipeline: materialization born materializationId="
        + juce::String(static_cast<juce::int64>(materializationId)));

    // Helper: build birth result from the born materialization (revision=0 at birth).
    auto buildBirthResult = [&]() -> AraRegionMaterializationBirthResult
    {
        AraRegionMaterializationBirthResult r;
        r.sourceId = request.sourceId;
        r.materializationId = materializationId;
        r.materializationRevision = 0;
        r.materializationDurationSeconds = getMaterializationAudioDurationById(materializationId);
        return r;
    };

    // ================================================================
    // 8. F0 extraction via RMVPE (source-rate input; internal 16kHz resampling)
    // ================================================================
    if (!ensureF0Ready())
    {
        AppLogger::warn("ARA OriginalF0 pipeline: F0 service not ready");
        setMaterializationOriginalF0StateById(materializationId, OriginalF0State::Failed);
        channel0Data.clear();
        channel0Data.shrink_to_fit();
        return buildBirthResult();
    }

    F0InferenceService* f0Service = nullptr;
    {
        std::lock_guard<std::mutex> lock(f0InitMutex_);
        f0Service = f0Service_.get();
    }
    if (f0Service == nullptr)
    {
        AppLogger::warn("ARA OriginalF0 pipeline: F0 service missing");
        setMaterializationOriginalF0StateById(materializationId, OriginalF0State::Failed);
        channel0Data.clear();
        channel0Data.shrink_to_fit();
        return buildBirthResult();
    }

    auto extraction = f0Service->extractF0(
        channel0Data.data(), channel0Data.size(),
        static_cast<int>(request.sourceSampleRate));

    if (!extraction.ok() || extraction.value().empty())
    {
        AppLogger::error("ARA OriginalF0 pipeline: RMVPE extraction failed");
        setMaterializationOriginalF0StateById(materializationId, OriginalF0State::Failed);
        f0Service->releaseImmediately();
        channel0Data.clear();
        channel0Data.shrink_to_fit();
        return buildBirthResult();
    }

    // ================================================================
    // 9. Commit F0 and energy to materialization
    // ================================================================
    const int hopSize = f0Service->getF0HopSize();
    const int f0SampleRate = f0Service->getF0SampleRate();

    auto pitchCurve = std::make_shared<PitchCurve>();
    pitchCurve->setHopSize(hopSize);
    pitchCurve->setSampleRate(static_cast<double>(f0SampleRate));
    pitchCurve->setOriginalF0(extraction.value());

    // Compute energy from channel0Data at source sample rate
    {
        std::vector<float> energy(extraction.value().size(), 0.0f);
        const double f0SecondsPerFrame = static_cast<double>(hopSize)
            / static_cast<double>(juce::jmax(1, f0SampleRate));
        const int halfRmsWindowSamples = juce::jmax(1,
            static_cast<int>(std::round(request.sourceSampleRate * 0.010)));
        for (size_t i = 0; i < extraction.value().size(); ++i)
        {
            if (!std::isfinite(extraction.value()[i]) || extraction.value()[i] <= 0.0f)
                continue;
            const int centerSample = juce::jlimit(0, static_cast<int>(channel0Data.size()) - 1,
                static_cast<int>(std::round(static_cast<double>(i) * f0SecondsPerFrame * request.sourceSampleRate)));
            const int startSample = juce::jmax(0, centerSample - halfRmsWindowSamples);
            const int endSampleExclusive = juce::jmin(static_cast<int>(channel0Data.size()),
                centerSample + halfRmsWindowSamples);
            if (endSampleExclusive <= startSample) continue;
            double squareSum = 0.0;
            for (int s = startSample; s < endSampleExclusive; ++s)
            {
                const float v = channel0Data[static_cast<size_t>(s)];
                squareSum += static_cast<double>(v) * static_cast<double>(v);
            }
            const double meanSquare = squareSum / static_cast<double>(endSampleExclusive - startSample);
            energy[i] = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::sqrt(meanSquare)));
        }
        pitchCurve->setOriginalEnergy(energy);
    }

    setMaterializationPitchCurveById(materializationId, std::move(pitchCurve));
    detectAndCommitMaterializationKeyIfUnset(materializationId);
    setMaterializationOriginalF0StateById(materializationId, OriginalF0State::Ready);

    AppLogger::info("ARA OriginalF0 pipeline: F0 committed frames="
        + juce::String(static_cast<juce::int64>(extraction.value().size())));

    // ================================================================
    // 10. Release RMVPE immediately, clean temp buffers
    // ================================================================
    f0Service->releaseImmediately();
    channel0Data.clear();
    channel0Data.shrink_to_fit();

    AppLogger::info("ARA OriginalF0 pipeline: complete materializationId="
        + juce::String(static_cast<juce::int64>(materializationId)));

    return buildBirthResult();
}

#endif // JucePlugin_Enable_ARA

// ============================================================================
// requestMaterializationRefresh �?Standalone / regular VST3 F0 refresh.
// F0 only �?does NOT run GAME note generation.
// ============================================================================

bool OpenTuneAudioProcessor::requestMaterializationRefresh(const OpenTuneAudioProcessor::MaterializationRefreshRequest& request)
{
    if (request.materializationId == 0) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    if (getMaterializationAudioBufferById(request.materializationId) == nullptr) {
        return false;
    }

    const bool hasChangedRange = request.preserveCorrectionsOutsideChangedRange
        && request.changedEndSeconds > request.changedStartSeconds;

    if (hasChangedRange) {
        auto notes = getMaterializationNotesById(request.materializationId);
        if (!notes.empty()) {
            NoteSequence sequence;
            sequence.setNotesSorted(notes);
            sequence.eraseRange(request.changedStartSeconds, request.changedEndSeconds);
            setMaterializationNotesById(request.materializationId, sequence.getNotes());
        }

        auto pitchCurve = getMaterializationPitchCurveById(request.materializationId);
        if (pitchCurve != nullptr) {
            const auto snapshot = pitchCurve->getSnapshot();
            const double frameRate = snapshot->getSampleRate()
                / static_cast<double>(juce::jmax(1, snapshot->getHopSize()));
            if (frameRate > 0.0) {
                const int startFrame = juce::jmax(0,
                    static_cast<int>(std::floor(request.changedStartSeconds * frameRate)));
                const int endFrame = juce::jmax(startFrame,
                    static_cast<int>(std::ceil(request.changedEndSeconds * frameRate)));
                auto clearedCurve = pitchCurve->clone();
                clearedCurve->clearCorrectionRange(startFrame, endFrame);
                if (!setMaterializationPitchCurveById(request.materializationId, std::move(clearedCurve))) {
                    return false;
                }
            }
        }

        setMaterializationDetectedKeyById(request.materializationId, DetectedKey{});
    }

    setMaterializationOriginalF0StateById(request.materializationId, OriginalF0State::Extracting);

    if (materializationRefreshService_.isActive(request.materializationId)) {
        materializationRefreshService_.cancel(request.materializationId);
    }

    const auto lifetimeFlag = materializationRefreshAliveFlag_;
    OpenTuneAudioProcessor* const processor = this;
    const auto capturedRequest = request;

    const auto submitResult = materializationRefreshService_.submit(
        request.materializationId,
        [lifetimeFlag, processor, capturedRequest]() -> F0ExtractionService::Result {
            F0ExtractionService::Result result;
            result.materializationId = capturedRequest.materializationId;
            result.requestKey = capturedRequest.materializationId;

            if (!lifetimeFlag->load(std::memory_order_acquire)) {
                result.errorMessage = "processor_destroyed";
                return result;
            }

            MaterializationSnapshot snapshot;
            if (!processor->getMaterializationSnapshotById(capturedRequest.materializationId, snapshot)
                || snapshot.audioBuffer == nullptr) {
                result.errorMessage = "content_snapshot_failed";
                return result;
            }

            result.sourceAudioBuffer = snapshot.audioBuffer;

            std::string errorMessage;
            if (!processor->extractImportedClipOriginalF0(snapshot, result, errorMessage)) {
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

            MaterializationSnapshot currentSnapshot;
            if (!processor->getMaterializationSnapshotById(result.materializationId, currentSnapshot)
                || currentSnapshot.audioBuffer == nullptr
                || result.sourceAudioBuffer == nullptr
                || currentSnapshot.audioBuffer != result.sourceAudioBuffer) {
                AppLogger::log("MaterializationRefresh: stale result dropped materializationId="
                    + juce::String(static_cast<juce::int64>(result.materializationId)));
                return;
            }

            // silentGaps computed at birth (prepareImport) �?no async overwrite needed

            if (!result.success) {
                AppLogger::log("MaterializationRefresh: extraction failed materializationId="
                    + juce::String(static_cast<juce::int64>(result.materializationId))
                    + " reason=" + juce::String(result.errorMessage));
                processor->setMaterializationOriginalF0StateById(result.materializationId, OriginalF0State::Failed);
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
                if (auto previousCurve = processor->getMaterializationPitchCurveById(result.materializationId)) {
                    auto previousSnapshot = previousCurve->getSnapshot();
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
                                                              if (segment.startFrame >= maxFrame) {
                                                                  return true;
                                                              }
                                                              const int clampedEnd = juce::jmin(segment.endFrame, maxFrame);
                                                              if (clampedEnd <= segment.startFrame) {
                                                                  return true;
                                                              }
                                                              return clampedEnd > changedStartFrame
                                                                  && segment.startFrame < changedEndFrame;
                                                          }),
                                           segments.end());
                        } else {
                            segments.erase(std::remove_if(segments.begin(),
                                                          segments.end(),
                                                          [maxFrame](const CorrectedSegment& segment) {
                                                              if (segment.startFrame >= maxFrame) {
                                                                  return true;
                                                              }
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

            if (!processor->setMaterializationPitchCurveById(result.materializationId, pitchCurve)) {
                AppLogger::log("MaterializationRefresh: pitch curve commit failed materializationId="
                    + juce::String(static_cast<juce::int64>(result.materializationId)));
                processor->setMaterializationOriginalF0StateById(result.materializationId, OriginalF0State::Failed);
                return;
            }

            // F0Alignment diagnostic
            {
                const auto& sw = currentSnapshot.sourceWindow;
                AppLogger::log("F0Alignment: materializationId="
                    + juce::String(static_cast<juce::int64>(result.materializationId))
                    + " audioDuration=" + juce::String(result.audioDurationSeconds, 6)
                    + " firstAudibleTime=" + juce::String(result.firstAudibleTimeSeconds, 6)
                    + " firstVoicedFrame=" + juce::String(result.firstVoicedFrame)
                    + " firstVoicedTime=" + juce::String(result.firstVoicedTimeSeconds, 6)
                    + " f0FrameCount=" + juce::String(static_cast<int>(result.f0.size()))
                    + " expectedInferenceFrameCount=" + juce::String(result.expectedInferenceFrameCount)
                    + " sourceWindow=[" + juce::String(static_cast<juce::int64>(sw.sourceId))
                    + "," + juce::String(sw.sourceStartSeconds, 6)
                    + "," + juce::String(sw.sourceEndSeconds, 6) + "]");
            }

            processor->detectAndCommitMaterializationKeyIfUnset(result.materializationId);

            processor->setMaterializationOriginalF0StateById(result.materializationId, OriginalF0State::Ready);

        });

    if (submitResult != F0ExtractionService::SubmitResult::Accepted) {
        AppLogger::log("MaterializationRefresh: submit rejected materializationId="
            + juce::String(static_cast<juce::int64>(request.materializationId)));
        setMaterializationOriginalF0StateById(request.materializationId, OriginalF0State::Failed);
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

std::shared_ptr<PitchCurve> OpenTuneAudioProcessor::getMaterializationPitchCurveById(uint64_t materializationId) const
{
    if (materializationId == 0) {
        return nullptr;
    }

    std::shared_ptr<PitchCurve> curve;
    jassert(materializationStore_ != nullptr);
    return materializationStore_->getPitchCurve(materializationId, curve) ? curve : nullptr;
}

bool OpenTuneAudioProcessor::setMaterializationPitchCurveById(uint64_t materializationId, std::shared_ptr<PitchCurve> curve)
{
    if (materializationId == 0) {
        return false;
    }

    jassert(materializationStore_ != nullptr);
    if (!materializationStore_->setPitchCurve(materializationId, std::move(curve))) {
        return false;
    }

    // §7 (Phase E): pitch edit invalidates Stage 2 (handled inside
    // setPitchCurve via TimeStretchCache::invalidate).  If the materialization
    // currently has a non-identity TimeGrid, the user is hearing stretched
    // audio �?we must enqueue a Stage 2 rebuild so the new pitch is reflected.
    // For identity grids, Stage 2 is unused; skip the rebuild.
    {
        std::shared_ptr<const TimeGridSnapshot> tg;
        if (materializationStore_->getTimeGrid(materializationId, tg)
            && tg != nullptr && !tg->isIdentity()) {
            requestStage2Rebuild(materializationId);
        }
    }
    return true;
}

OriginalF0State OpenTuneAudioProcessor::getMaterializationOriginalF0StateById(uint64_t materializationId) const
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0
        ? materializationStore_->getOriginalF0State(materializationId)
        : OriginalF0State::NotRequested;
}

bool OpenTuneAudioProcessor::setMaterializationOriginalF0StateById(uint64_t materializationId, OriginalF0State state)
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0 && materializationStore_->setOriginalF0State(materializationId, state);
}

void OpenTuneAudioProcessor::detectAndCommitMaterializationKeyIfUnset(uint64_t materializationId)
{
    const auto existingKey = getMaterializationDetectedKeyById(materializationId);
    if (existingKey.confidence > 0.0f) {
        return;
    }

    auto audioBuffer = getMaterializationAudioBufferById(materializationId);
    if (audioBuffer == nullptr || audioBuffer->getNumSamples() <= 0) {
        return;
    }

    ChromaKeyDetector detector;
    const auto key = detector.detect(audioBuffer->getReadPointer(0),
                                     audioBuffer->getNumSamples(),
                                     44100);
    setMaterializationDetectedKeyById(materializationId, key);
}

DetectedKey OpenTuneAudioProcessor::getMaterializationDetectedKeyById(uint64_t materializationId) const
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0 ? materializationStore_->getDetectedKey(materializationId) : DetectedKey{};
}

bool OpenTuneAudioProcessor::setMaterializationDetectedKeyById(uint64_t materializationId, const DetectedKey& key)
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0 && materializationStore_->setDetectedKey(materializationId, key);
}

// ============================================================================
// vocal-time-stretch §3.6 �?TimeGrid processor accessors
// ============================================================================

std::shared_ptr<const TimeGridSnapshot>
OpenTuneAudioProcessor::getMaterializationTimeGridById(uint64_t materializationId) const
{
    if (materializationId == 0) return nullptr;
    jassert(materializationStore_ != nullptr);
    std::shared_ptr<const TimeGridSnapshot> snapshot;
    return materializationStore_->getTimeGrid(materializationId, snapshot) ? snapshot : nullptr;
}

uint64_t OpenTuneAudioProcessor::getMaterializationTimeGridRevisionById(uint64_t materializationId) const
{
    if (materializationId == 0) return 0;
    jassert(materializationStore_ != nullptr);
    return materializationStore_->getTimeGridRevision(materializationId);
}

bool OpenTuneAudioProcessor::ensureTimeToolAnchorSeed(uint64_t materializationId)
{
    if (materializationId == 0 || materializationStore_ == nullptr) {
        return false;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!materializationStore_->getSnapshot(materializationId, snapshot)) {
        return false;
    }

    const auto existingGrid = snapshot.timeGrid;
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

    ReferenceFeatureSet features;
    const bool featuresReady = materializationStore_->getReferenceFeatures(materializationId, features)
        && features.isReady()
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snapshot.renderRevision)
        && features.hasTimingAnchors();

    if (!featuresReady) {
        // Dispatch async GAME analysis via the already-existing async path.
        // This posts to ReferenceAnalysisService worker thread and returns immediately.
        // The UI will show overlay/badge during extraction (via PluginEditor busy aggregation).
        const auto preheatStatus = preheatReferenceAlignmentFeatures(materializationId);
        if (preheatStatus == ReferenceAnalysisPreheatStatus::InvalidMaterialization
            || preheatStatus == ReferenceAnalysisPreheatStatus::AnalysisFailed) {
            return false;
        }
        // Queued or AlreadyReady: return false now, anchors will appear via async listener.
        return false;
    }

    if (!features.isReady()
        || features.producer != ReferenceFeatureProducer::Game
        || !features.hasTimingAnchors()) {
        return false;
    }

    const double durationSeconds = snapshot.timeGrid != nullptr
        ? snapshot.timeGrid->totalDurationSeconds()
        : TimeCoordinate::samplesToSeconds(snapshot.audioBuffer != nullptr ? snapshot.audioBuffer->getNumSamples() : 0,
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

    const int64_t affectedEndFrame = static_cast<int64_t>(
        std::ceil(durationSeconds * TimeGridSnapshot::kSourceSpacingFrameRate));
    return setMaterializationTimeGridById(materializationId,
                                          std::move(seededGrid),
                                          0,
                                          affectedEndFrame);
}

bool OpenTuneAudioProcessor::setMaterializationTimeGridById(uint64_t materializationId,
                                                              std::shared_ptr<const TimeGridSnapshot> snapshot,
                                                              int64_t affectedSrcStartFrame,
                                                              int64_t affectedSrcEndFrame)
{
    if (materializationId == 0 || snapshot == nullptr) return false;
    jassert(materializationStore_ != nullptr);
    if (!materializationStore_->setTimeGrid(materializationId, std::move(snapshot))) {
        return false;
    }

    // §7 (Phase D MVP): TimeStretchCache invalidation is performed inside
    // setTimeGrid (§6.5).  Here we additionally enqueue a Stage 2 rebuild on
    // the dedicated worker thread �?it will read the freshest snapshot and
    // produce a new TimeStretchCache entry asynchronously.  affectedSrcStart/End
    // bounds are reserved for future region-scoped re-stretch optimization
    // (current Offline mode is clip-wide).
    juce::ignoreUnused(affectedSrcStartFrame, affectedSrcEndFrame);
    requestStage2Rebuild(materializationId);
    return true;
}

void OpenTuneAudioProcessor::setPitchShiftSettings(uint64_t materializationId, const PitchShiftSettings& settings)
{
    if (materializationId == 0) return;

    materializationStore_->setPitchShiftSettings(materializationId, settings);

    // Trigger full re-render for this materialization
    MaterializationSnapshot snap;
    if (getMaterializationSnapshotById(materializationId, snap) && snap.audioBuffer) {
        const int numSamples = snap.audioBuffer->getNumSamples();
        const double durationSec = static_cast<double>(numSamples) / 44100.0;
        if (durationSec > 0.0) {
            materializationStore_->enqueuePartialRender(materializationId, 0.0, durationSec, 512);
            ensureChunkRenderWorkerStarted();
            schedulerCv_.notify_one();
        }
    }
}

std::shared_ptr<RenderCache> OpenTuneAudioProcessor::getMaterializationRenderCacheById(uint64_t materializationId) const
{
    if (materializationId == 0) {
        return nullptr;
    }

    std::shared_ptr<RenderCache> renderCache;
    jassert(materializationStore_ != nullptr);
    return materializationStore_->getRenderCache(materializationId, renderCache) ? renderCache : nullptr;
}

std::vector<Note> OpenTuneAudioProcessor::getMaterializationNotesById(uint64_t materializationId) const
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0 ? materializationStore_->getNotes(materializationId) : std::vector<Note>{};
}

OpenTuneAudioProcessor::MaterializationNotesSnapshot OpenTuneAudioProcessor::getMaterializationNotesSnapshotById(uint64_t materializationId) const
{
    MaterializationNotesSnapshot snapshot;
    jassert(materializationStore_ != nullptr);
    if (materializationId == 0) {
        return snapshot;
    }

    materializationStore_->getNotesSnapshot(materializationId, snapshot);
    return snapshot;
}

bool OpenTuneAudioProcessor::setMaterializationNotesById(uint64_t materializationId, const std::vector<Note>& notes)
{
    jassert(materializationStore_ != nullptr);
    return materializationId != 0 && materializationStore_->setNotes(materializationId, normalizeStoredNotes(notes));
}

bool OpenTuneAudioProcessor::setMaterializationCorrectedSegmentsById(uint64_t materializationId,
                                                              const std::vector<CorrectedSegment>& segments)
{
    if (materializationId == 0) {
        return false;
    }

    std::shared_ptr<PitchCurve> pitchCurve;
    jassert(materializationStore_ != nullptr);
    if (!materializationStore_->getPitchCurve(materializationId, pitchCurve) || pitchCurve == nullptr) {
        return false;
    }

    auto committedCurve = clonePitchCurveWithCorrectedSegments(pitchCurve, segments);
    return committedCurve != nullptr && materializationStore_->setPitchCurve(materializationId, std::move(committedCurve));
}

bool OpenTuneAudioProcessor::commitMaterializationNotesAndSegmentsById(uint64_t materializationId,
                                                                const std::vector<Note>& notes,
                                                                const std::vector<CorrectedSegment>& segments)
{
    if (materializationId == 0) {
        return false;
    }

    const auto normalizedNotes = normalizeStoredNotes(notes);
    std::shared_ptr<PitchCurve> pitchCurve;
    jassert(materializationStore_ != nullptr);
    if (!materializationStore_->getPitchCurve(materializationId, pitchCurve) || pitchCurve == nullptr) {
        return false;
    }

    auto committedCurve = clonePitchCurveWithCorrectedSegments(pitchCurve, segments);
    const bool committed = committedCurve != nullptr
        && materializationStore_->commitNotesAndPitchCurve(materializationId,
                                                           normalizedNotes,
                                                           std::move(committedCurve));
    if (committed) {
        // §7 (Phase E): pitch edit �?Stage 2 rebuild if non-identity grid
        std::shared_ptr<const TimeGridSnapshot> tg;
        if (materializationStore_->getTimeGrid(materializationId, tg)
            && tg != nullptr && !tg->isIdentity()) {
            requestStage2Rebuild(materializationId);
        }
    }
    return committed;
}

OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus
OpenTuneAudioProcessor::preheatReferenceAlignmentFeatures(uint64_t materializationId)
{
    if (materializationStore_ == nullptr || materializationId == 0) {
        return ReferenceAnalysisPreheatStatus::InvalidMaterialization;
    }

    MaterializationStore::MaterializationSnapshot snapshot;
    if (!materializationStore_->getSnapshot(materializationId, snapshot)) {
        return ReferenceAnalysisPreheatStatus::InvalidMaterialization;
    }

    ReferenceFeatureSet features;
    if (materializationStore_->getReferenceFeatures(materializationId, features)
        && features.isReady()
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snapshot.renderRevision)) {
        return ReferenceAnalysisPreheatStatus::AlreadyReady;
    }

    if (features.status == ReferenceFeatureStatus::Failed
        && features.producer == ReferenceFeatureProducer::Game
        && features.inputFingerprint == static_cast<int64_t>(snapshot.renderRevision)) {
        return ReferenceAnalysisPreheatStatus::AnalysisFailed;
    }

    if (features.status != ReferenceFeatureStatus::Extracting
        || features.producer != ReferenceFeatureProducer::Game
        || features.inputFingerprint != static_cast<int64_t>(snapshot.renderRevision)) {
        ReferenceFeatureSet extracting;
        extracting.producer = ReferenceFeatureProducer::Game;
        extracting.status = ReferenceFeatureStatus::Extracting;
        extracting.inputFingerprint = static_cast<int64_t>(snapshot.renderRevision);
        extracting.sourceDurationSeconds = snapshot.audioBuffer != nullptr
            ? TimeCoordinate::samplesToSeconds(snapshot.audioBuffer->getNumSamples(),
                                               TimeCoordinate::kRenderSampleRate)
            : 0.0;
        materializationStore_->setReferenceFeatures(materializationId, extracting);
    }

    referenceAnalysisService_.submitAnalysis(materializationId,
                                             static_cast<int64_t>(snapshot.renderRevision));
    return ReferenceAnalysisPreheatStatus::Queued;
}

OpenTuneAudioProcessor::ReferenceAlignmentResult
OpenTuneAudioProcessor::executeReferenceAlignmentForPlacement(uint64_t targetPlacementId)
{
    ReferenceAlignmentResult result;

    if (targetPlacementId == 0 || standaloneArrangement_ == nullptr || materializationStore_ == nullptr) {
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

    MaterializationStore::MaterializationSnapshot targetSnapshot;
    MaterializationStore::MaterializationSnapshot referenceSnapshot;
    if (!materializationStore_->getSnapshot(targetPlacement.materializationId, targetSnapshot)) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target analysis could not read materialization";
        return result;
    }
    if (!materializationStore_->getSnapshot(referencePlacement.materializationId, referenceSnapshot)) {
        result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
        result.message = "AUTO Ref reference analysis could not read materialization";
        return result;
    }

    ReferenceFeatureSet targetFeatures;
    if (!materializationStore_->getReferenceFeatures(targetPlacement.materializationId, targetFeatures)
        || !targetFeatures.isReady()
        || targetFeatures.producer != ReferenceFeatureProducer::Game
        || targetFeatures.inputFingerprint != static_cast<int64_t>(targetSnapshot.renderRevision)) {
        targetFeatures = buildReferenceFeatureSet(targetSnapshot);
        materializationStore_->setReferenceFeatures(targetPlacement.materializationId, targetFeatures);
    }
    if (!targetFeatures.isReady()) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = targetFeatures.errorMessage.isNotEmpty()
            ? targetFeatures.errorMessage
            : "AUTO Ref target alignment features are not ready";
        return result;
    }

    ReferenceFeatureSet referenceFeatures;
    if (!materializationStore_->getReferenceFeatures(referencePlacement.materializationId, referenceFeatures)
        || !referenceFeatures.isReady()
        || referenceFeatures.producer != ReferenceFeatureProducer::Game
        || referenceFeatures.inputFingerprint != static_cast<int64_t>(referenceSnapshot.renderRevision)) {
        referenceFeatures = buildReferenceFeatureSet(referenceSnapshot);
        materializationStore_->setReferenceFeatures(referencePlacement.materializationId, referenceFeatures);
    }
    if (!referenceFeatures.isReady()) {
        result.status = ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady;
        result.message = referenceFeatures.errorMessage.isNotEmpty()
            ? referenceFeatures.errorMessage
            : "AUTO Ref reference alignment features are not ready";
        return result;
    }

    const auto oldNotes = materializationStore_->getNotes(targetPlacement.materializationId);
    std::shared_ptr<PitchCurve> oldCurve;
    if (!materializationStore_->getPitchCurve(targetPlacement.materializationId, oldCurve) || oldCurve == nullptr) {
        result.status = ReferenceAlignmentResult::Status::TargetAnalysisNotReady;
        result.message = "AUTO Ref target pitch curve is not available";
        return result;
    }

    const auto oldSegments = copyCorrectedSegments(oldCurve);
    const double targetDurationSeconds = targetSnapshot.audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(targetSnapshot.audioBuffer->getNumSamples(),
                                           TimeCoordinate::kRenderSampleRate)
        : targetFeatures.sourceDurationSeconds;
    const double referenceDurationSeconds = referenceSnapshot.audioBuffer != nullptr
        ? TimeCoordinate::samplesToSeconds(referenceSnapshot.audioBuffer->getNumSamples(),
                                           TimeCoordinate::kRenderSampleRate)
        : referenceFeatures.sourceDurationSeconds;
    if (!(targetDurationSeconds > 0.0) || !(referenceDurationSeconds > 0.0)) {
        result.status = ReferenceAlignmentResult::Status::InvalidTimeGrid;
        result.message = "AUTO Ref requires positive target and reference durations";
        return result;
    }

    auto oldTimeGrid = targetSnapshot.timeGrid;
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
    request.target.materializationId = targetPlacement.materializationId;
    request.target.timelineStartSeconds = targetPlacement.timelineStartSeconds;
    request.target.timelineEndSeconds = targetPlacement.timelineEndSeconds();
    request.reference.placementId = referencePlacement.placementId;
    request.reference.materializationId = referencePlacement.materializationId;
    request.reference.timelineStartSeconds = referencePlacement.timelineStartSeconds;
    request.reference.timelineEndSeconds = referencePlacement.timelineEndSeconds();
    request.targetTimeMap = EffectiveTimeMap::fromTimeGrid(targetSnapshot.timeGrid, targetDurationSeconds);
    request.referenceTimeMap = EffectiveTimeMap::fromTimeGrid(referenceSnapshot.timeGrid, referenceDurationSeconds);
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
        result.targetMaterializationId = targetPlacement.materializationId;
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
    if (!materializationStore_->commitReferenceAlignmentPatch(targetPlacement.materializationId,
                                                             normalizedNotes,
                                                             std::move(newCurve),
                                                             timeGridAfter)) {
        result.status = ReferenceAlignmentResult::Status::CommitFailed;
        result.message = "AUTO Ref could not commit patch";
        return result;
    }

    auto composite = std::make_unique<CompositeUndoAction>("AUTO (Ref)");
    if (patch.pitchChanged) {
        composite->addAction(std::make_unique<PianoRollEditAction>(
            *this,
            targetPlacement.materializationId,
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
            *this,
            targetPlacement.materializationId,
            "AUTO (Ref) Time",
            oldTimeGrid,
            timeGridAfter,
            patch.affectedStartFrame,
            patch.affectedEndFrame));
    }
    if (composite->getNumActions() > 0) {
        undoManager_.addAction(std::move(composite));
    }

    const double editStartSec = static_cast<double>(patch.affectedStartFrame)
                              / TimeGridSnapshot::kSourceSpacingFrameRate;
    const double editEndSec = static_cast<double>(patch.affectedEndFrame)
                            / TimeGridSnapshot::kSourceSpacingFrameRate;
    enqueueMaterializationPartialRenderById(targetPlacement.materializationId, editStartSec, editEndSec);
    if (patch.timingChanged) {
        requestStage2Rebuild(targetPlacement.materializationId);
    }

    result.status = ReferenceAlignmentResult::Status::Succeeded;
    result.message = "AUTO Ref alignment applied";
    result.targetMaterializationId = targetPlacement.materializationId;
    result.affectedStartFrame = patch.affectedStartFrame;
    result.affectedEndFrame = patch.affectedEndFrame;
    return result;
}

bool OpenTuneAudioProcessor::commitAutoTuneGeneratedNotesByMaterializationId(uint64_t materializationId,
                                                                       const std::vector<Note>& generatedNotes,
                                                                       int startFrame,
                                                                       int endFrameExclusive,
                                                                      float retuneSpeed,
                                                                      float vibratoDepth,
                                                                      float vibratoRate,
                                                                      double audioSampleRate)
{
    AppLogger::log("AutoTune: commitAutoTuneGenerated entry matId=" + juce::String(static_cast<juce::int64>(materializationId))
        + " notes=" + juce::String(static_cast<int>(generatedNotes.size()))
        + " range=[" + juce::String(startFrame) + "," + juce::String(endFrameExclusive) + ")");

    if (materializationId == 0 || endFrameExclusive <= startFrame) {
        return false;
    }

    const auto normalizedNotes = normalizeStoredNotes(generatedNotes);
    if (normalizedNotes.empty()) {
        AppLogger::log("AutoTune: commitAutoTuneGenerated abort - normalizedNotes empty");
        return false;
    }

    std::shared_ptr<PitchCurve> sharedCurve;
    jassert(materializationStore_ != nullptr);
    if (!materializationStore_->getPitchCurve(materializationId, sharedCurve) || sharedCurve == nullptr) {
        AppLogger::log("AutoTune: commitAutoTuneGenerated abort - no pitchCurve in store");
        return false;
    }

    const double secondsPerFrame = static_cast<double>(sharedCurve->getHopSize()) / sharedCurve->getSampleRate();
    const double rangeStartTime = static_cast<double>(startFrame) * secondsPerFrame;
    const double rangeEndTime = static_cast<double>(endFrameExclusive) * secondsPerFrame;

    auto existingSnapshot = getMaterializationNotesSnapshotById(materializationId);

    std::vector<Note> mergedNotes;
    mergedNotes.reserve(existingSnapshot.notes.size() + normalizedNotes.size());

    for (const auto& note : existingSnapshot.notes) {
        if (note.endTime <= rangeStartTime || note.startTime >= rangeEndTime) {
            mergedNotes.push_back(note);
        }
    }

    for (const auto& note : normalizedNotes) {
        mergedNotes.push_back(note);
    }

    std::sort(mergedNotes.begin(), mergedNotes.end(),
        [](const Note& a, const Note& b) { return a.startTime < b.startTime; });

    AppLogger::log("AutoTune: cloning curve and applying correction");
    auto derivedCurve = sharedCurve->clone();
    derivedCurve->applyCorrectionToRange(mergedNotes,
                                         startFrame,
                                         endFrameExclusive,
                                         retuneSpeed,
                                         vibratoDepth,
                                         vibratoRate,
                                         audioSampleRate);
    AppLogger::log("AutoTune: applyCorrectionToRange completed");

    if (getMaterializationPitchCurveById(materializationId) != sharedCurve) {
        AppLogger::log("AutoTune: commitAutoTuneGenerated abort - curve replaced during correction (TOCTOU)");
        return false;
    }

    AppLogger::log("AutoTune: committing notes and curve to store");
    bool result = materializationStore_->commitNotesAndPitchCurve(materializationId,
                                                                    mergedNotes,
                                                                    std::move(derivedCurve));
    AppLogger::log("AutoTune: commitNotesAndPitchCurve result=" + juce::String(result ? "true" : "false"));
    if (result) {
        // §7 (Phase E): pitch edit �?Stage 2 rebuild if non-identity grid
        std::shared_ptr<const TimeGridSnapshot> tg;
        if (materializationStore_->getTimeGrid(materializationId, tg)
            && tg != nullptr && !tg->isIdentity()) {
            requestStage2Rebuild(materializationId);
        }
    }
    return result;
}

bool OpenTuneAudioProcessor::enqueueMaterializationPartialRenderById(uint64_t materializationId,
                                                        double relStartSeconds,
                                                        double relEndSeconds)
{
    AppLogger::log("AutoTune: enqueueMaterializationPartialRender matId=" + juce::String(static_cast<juce::int64>(materializationId))
        + " range=[" + juce::String(relStartSeconds, 3) + "s," + juce::String(relEndSeconds, 3) + "s]");

    if (materializationId == 0 || relEndSeconds <= relStartSeconds) {
        return false;
    }
    jassert(materializationStore_ != nullptr);

    int hopSize = 512;
    if (vocoderDomain_) {
        const int currentHopSize = vocoderDomain_->getVocoderHopSize();
        if (currentHopSize > 0) {
            hopSize = currentHopSize;
        }
    }

    const bool requested = materializationStore_->enqueuePartialRender(materializationId, relStartSeconds, relEndSeconds, hopSize);
    if (!requested) {
        return false;
    }

    ensureChunkRenderWorkerStarted();
    schedulerCv_.notify_one();
    return true;
}


// ============================================================================
// 分块渲染工作线程
// ============================================================================

void OpenTuneAudioProcessor::chunkRenderWorkerLoop()
{
    AppLogger::log("RenderWorker: chunkRenderWorkerLoop started");

    struct WorkerRenderJob {
        MaterializationStore::PendingRenderJob coreJob;
        FrozenRenderBoundaries boundaries;
    };

    // 复用 scratch buffer，避免每 chunk 重新堆分�?
    std::vector<float> monoAudio;
    std::vector<float> sourceF0;
    std::vector<float> correctedF0;

    while (true) {
        // 1. �?Pending Chunk
        std::shared_ptr<WorkerRenderJob> job;

        {
            std::unique_lock<std::mutex> lock(schedulerMutex_);
            schedulerCv_.wait_for(lock, std::chrono::seconds(10), [this]() {
                return !chunkRenderWorkerRunning_.load(std::memory_order_acquire)
                    || (chunkRenderJobsInFlight_.load(std::memory_order_acquire) == 0
                        && materializationStore_ != nullptr && materializationStore_->hasPendingRenderJobs());
            });

            if (!chunkRenderWorkerRunning_) {
                return;
            }

            // �?materialization store render queue 拉取下一�?pending chunk
            if (materializationStore_ != nullptr) {
                MaterializationStore::PendingRenderJob pendingJob;
                if (materializationStore_->pullNextPendingRenderJob(pendingJob)) {
                    job = std::make_shared<WorkerRenderJob>();
                    job->coreJob = std::move(pendingJob);
                }
            }

        }

        if (!job) {
            // �?Pending 任务，等待唤�?
            continue;
        }

        const double relChunkStartSec = job->coreJob.startSeconds;
        auto& boundaries = job->boundaries;

        // 2. 准备渲染数据（读�?Clip 中的音频�?PitchCurve�?
        const auto& coreJob = job->coreJob;
        std::shared_ptr<PitchCurve> pitchCurve = coreJob.pitchCurve;
        int numFrames = 0;
        bool clipFound = false;
        bool boundariesFrozen = false;

        if (coreJob.audioBuffer != nullptr) {
            const int audioNumSamples = coreJob.audioBuffer->getNumSamples();
            const int audioNumChannels = coreJob.audioBuffer->getNumChannels();
            int workerHopSize = 512;
            if (vocoderDomain_) {
                const int currentHopSize = vocoderDomain_->getVocoderHopSize();
                if (currentHopSize > 0) {
                    workerHopSize = currentHopSize;
                }
            }

            MaterializationSampleRange materializationRange{0, audioNumSamples};
            if (freezeRenderBoundaries(materializationRange,
                                       coreJob.startSample,
                                       coreJob.endSampleExclusive,
                                       workerHopSize,
                                       boundaries)) {
                boundariesFrozen = true;

                if (audioNumChannels > 0) {
                    numFrames = boundaries.frameCount;
                    monoAudio.resize(static_cast<size_t>(boundaries.synthSampleCount), 0.0f);

                    // Per channel-layout-policy spec: vocoder mel input is unconditionally
                    // sourced from channel 0 (L) of the stored audio. No averaging, no
                    // active-channel detection. Storage is guaranteed 1 or 2 channels, and
                    // ch 0 is "L" in stereo or "the mono channel" in mono �?either way the
                    // canonical mono input.
                    const float* ch0 = coreJob.audioBuffer->getReadPointer(0);
                    for (int64_t i = 0; i < boundaries.publishSampleCount; ++i) {
                        monoAudio[static_cast<size_t>(i)] = ch0[static_cast<int>(boundaries.trueStartSample + i)];
                    }
                    ChannelLayoutLog::logChunkRender(
                        static_cast<juce::int64>(coreJob.materializationId),
                        audioNumChannels);
                    clipFound = true;
                }
            }
        }

        if (!clipFound || !pitchCurve || monoAudio.empty() || numFrames <= 0 || !boundariesFrozen) {
            coreJob.renderCache->completeChunkRender(relChunkStartSec, coreJob.targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        const double trueStartSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueStartSample,
                                                                         TimeCoordinate::kRenderSampleRate);
        const double trueEndSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueEndSample,
                                                                        TimeCoordinate::kRenderSampleRate);
        const double hopDuration = static_cast<double>(boundaries.hopSize) / RenderCache::kSampleRate;

        auto snap = pitchCurve->getSnapshot();
        if (!snap->hasRenderableCorrectedF0()) {
            coreJob.renderCache->markChunkAsBlank(relChunkStartSec);
            schedulerCv_.notify_one();
            continue;
        }

        const int f0HopSize = snap->getHopSize();
        const double f0SampleRate = snap->getSampleRate();
        if (f0HopSize <= 0 || f0SampleRate <= 0.0) {
            coreJob.renderCache->completeChunkRender(relChunkStartSec, coreJob.targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        const double f0FrameRate = f0SampleRate / static_cast<double>(f0HopSize);

        // 3. 构�?F0 数据
        const int f0StartFrame = static_cast<int>(std::floor(trueStartSeconds * f0FrameRate));
        const int f0EndFrame = static_cast<int>(std::ceil(trueEndSeconds * f0FrameRate)) + 1;
        const int numF0Frames = std::max(1, f0EndFrame - f0StartFrame);

        sourceF0.assign(static_cast<size_t>(numF0Frames), 0.0f);
        snap->renderF0Range(f0StartFrame, f0EndFrame,
            [&sourceF0, f0StartFrame](int frameIndex, const float* data, int length) {
                if (!data || length <= 0) return;
                const int offset = frameIndex - f0StartFrame;
                if (offset < 0) return;
                const int copyLen = std::min(length, static_cast<int>(sourceF0.size()) - offset);
                if (copyLen > 0) {
                    std::copy(data, data + copyLen, sourceF0.begin() + offset);
                }
            });

        // ===== Pitch Shift render modifier: apply global F0 offset =====
        // PitchShiftSettings is a clip-level modifier that shifts all F0 values
        // by a constant ratio BEFORE the AutoTune/vocoder split decision.
        // This ensures both paths (light pitch correction and vocoder) see the
        // shifted F0 as their "corrected" target.
        if (materializationStore_ != nullptr) {
            const auto pitchShiftSettings = materializationStore_->getPitchShiftSettings(coreJob.materializationId);
            if (!pitchShiftSettings.isIdentity()) {
                const float pitchRatio = static_cast<float>(pitchShiftSettings.getPitchRatio());
                for (auto& f0Val : sourceF0) {
                    if (f0Val > 0.0f) {
                        f0Val *= pitchRatio;
                    }
                }
            }
        }

        bool hasValidF0 = false;
        for (float f : sourceF0) {
            if (f > 0.0f) {
                hasValidF0 = true;
                break;
            }
        }

        if (!hasValidF0) {
            coreJob.renderCache->markChunkAsBlank(relChunkStartSec);
            schedulerCv_.notify_one();
            continue;
        }

        // ===== AutoTune 轻量修音分流 =====
        const bool lightPitchEnabled = appPreferences_ != nullptr
            && appPreferences_->getState().shared.lightPitchCorrectionEnabled;

        if (lightPitchEnabled) {
            const auto& originalF0Full = snap->getOriginalF0();
            const int originalF0Size = static_cast<int>(originalF0Full.size());

            // 安全检查：f0StartFrame 必须在原�?F0 范围�?
            if (f0StartFrame >= 0 && f0StartFrame < originalF0Size) {
                const bool needsVocoder = chunkNeedsVocoder(
                    sourceF0.data(), numF0Frames, originalF0Full, f0StartFrame);

                if (!needsVocoder) {
                    // === AutoTune cycle-resampling pitch-shift 路径 ===
                    if (!autoTuneShifter_) {
                        autoTuneShifter_ = std::make_unique<AutoTunePitchShifter>(
                            RenderCache::kSampleRate);
                    } else {
                        autoTuneShifter_->reset();
                    }

                    const int safeNumF0Frames = std::min(numF0Frames, originalF0Size - f0StartFrame);
                    auto shiftedAudio = autoTuneShifter_->shiftChunk(
                        monoAudio.data(),
                        static_cast<int>(boundaries.publishSampleCount),
                        originalF0Full.data() + f0StartFrame,
                        sourceF0.data(),
                        safeNumF0Frames,
                        f0FrameRate);

                    // AutoTune 保证输出长度 == 输入长度，防御性截�?
                    if (static_cast<int64_t>(shiftedAudio.size()) != boundaries.publishSampleCount) {
                        shiftedAudio.resize(static_cast<size_t>(boundaries.publishSampleCount), 0.0f);
                    }

                    const bool added = coreJob.renderCache->addChunk(
                        boundaries.trueStartSample, boundaries.trueEndSample,
                        std::move(shiftedAudio), coreJob.targetRevision);

                    if (added) {
                        coreJob.renderCache->completeChunkRender(relChunkStartSec,
                            coreJob.targetRevision, RenderCache::CompletionResult::Succeeded);
                    } else {
                        coreJob.renderCache->completeChunkRender(relChunkStartSec,
                            coreJob.targetRevision, RenderCache::CompletionResult::TerminalFailure);
                    }

                    // Stage 2 失效（与声码器路径一致）
                    const uint64_t rbMatId = coreJob.materializationId;
                    if (rbMatId != 0 && materializationStore_ != nullptr) {
                        materializationStore_->getTimeStretchCache().invalidate(rbMatId);
                        requestStage2Rebuild(rbMatId);
                    }

                    AppLogger::debug("RenderWorker: AutoTune pitch-shift chunk matId="
                        + juce::String(static_cast<juce::int64>(coreJob.materializationId))
                        + " start=" + juce::String(relChunkStartSec, 3));

                    schedulerCv_.notify_one();
                    continue;  // 跳过声码器路�?
                }
            }
        }

        if (!ensureVocoderReady()) {
            AppLogger::log("RenderWorker: ensureVocoderReady FAILED");
            coreJob.renderCache->completeChunkRender(relChunkStartSec, coreJob.targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        // 4. 构�?Mel Spectrogram
        MelSpectrogramConfig melConfig;
        melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
        melConfig.nMels = vocoderDomain_->getMelBins();
        melConfig.fMax = vocoderDomain_->getFMax();

        auto melResult = computeLogMelSpectrogram(monoAudio.data(), static_cast<int>(monoAudio.size()), numFrames, melConfig);
        if (!melResult.ok() || melResult.value().empty()) {
            coreJob.renderCache->completeChunkRender(relChunkStartSec, coreJob.targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        auto mel = std::move(melResult).value();
        const int actualFrames = static_cast<int>(mel.size() / melConfig.nMels);

        // 5. F0-to-Mel 插�?
        correctedF0.assign(static_cast<size_t>(actualFrames), 0.0f);
        for (int i = 0; i < actualFrames; ++i) {
            const double melTimeSec = trueStartSeconds + i * hopDuration;
            const double srcPos = melTimeSec * f0FrameRate - static_cast<double>(f0StartFrame);
            if (srcPos < 0.0) continue;

            const int srcIdx0 = static_cast<int>(srcPos);
            if (srcIdx0 >= numF0Frames) continue;
            const int srcIdx1 = std::min(srcIdx0 + 1, numF0Frames - 1);
            const double frac = srcPos - static_cast<double>(srcIdx0);

            const float f0_0 = sourceF0[static_cast<size_t>(srcIdx0)];
            const float f0_1 = sourceF0[static_cast<size_t>(srcIdx1)];

            if (f0_0 > 0.0f && f0_1 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
            } else if (f0_0 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = f0_0;
            } else if (f0_1 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = f0_1;
            }
        }

        const bool allowTrailingExtension = !(boundaries.synthSampleCount > boundaries.publishSampleCount);
        OpenTune::fillF0GapsForVocoder(correctedF0,
                                       snap,
                                       trueStartSeconds,
                                       trueEndSeconds,
                                       hopDuration,
                                       f0FrameRate,
                                       allowTrailingExtension);

        // 6. 提交执行
        VocoderDomain::Job vocoderJob;
        vocoderJob.chunkKey = (coreJob.materializationId << 32) | static_cast<uint64_t>(static_cast<uint32_t>(coreJob.startSample));
        vocoderJob.f0 = std::move(correctedF0);
        vocoderJob.mel = std::move(mel);

        auto renderCache = coreJob.renderCache;
        auto targetRevision = coreJob.targetRevision;
        const uint64_t chunkMatId = coreJob.materializationId;
        double jobStartSeconds = TimeCoordinate::samplesToSeconds(job->boundaries.trueStartSample,
                                                                  TimeCoordinate::kRenderSampleRate);
        const FrozenRenderBoundaries frozenBoundaries = job->boundaries;

        chunkRenderJobsInFlight_.fetch_add(1, std::memory_order_acq_rel);

        vocoderJob.onComplete = [this, renderCache, targetRevision, chunkMatId, jobStartSeconds, frozenBoundaries](bool success, const juce::String& error, const std::vector<float>& audio) {
            chunkRenderJobsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
            const auto& boundaries = frozenBoundaries;

            if (success) {
                std::vector<float> publishedAudio;
                if (!preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio)) {
                    AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish matId="
                        + juce::String(static_cast<juce::int64>(chunkMatId))
                        + " start=" + juce::String(jobStartSeconds, 3)
                        + " expectedSynthSamples=" + juce::String(static_cast<juce::int64>(boundaries.synthSampleCount))
                        + " expectedPublishSamples=" + juce::String(static_cast<juce::int64>(boundaries.publishSampleCount))
                        + " actualSamples=" + juce::String(static_cast<juce::int64>(audio.size())));
                    renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::TerminalFailure);
                    schedulerCv_.notify_one();
                    return;
                }

                const bool added = renderCache->addChunk(
                    boundaries.trueStartSample,
                    boundaries.trueEndSample,
                    std::move(publishedAudio),
                    targetRevision);
                if (!added) {
                    AppLogger::error("ChunkRender: RenderCache rejected published chunk matId="
                        + juce::String(static_cast<juce::int64>(chunkMatId))
                        + " start=" + juce::String(jobStartSeconds, 3)
                        + " targetRevision=" + juce::String(static_cast<juce::int64>(targetRevision))
                        + " startSample=" + juce::String(static_cast<juce::int64>(boundaries.trueStartSample))
                        + " endSample=" + juce::String(static_cast<juce::int64>(boundaries.trueEndSample)));
                    renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::TerminalFailure);
                    schedulerCv_.notify_one();
                    return;
                }

                renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::Succeeded);

                // ⚡️ vocal-time-stretch §7 (Journey-1 fix 2026-05-12) �?Stage 2
                // depends on Stage 1's RenderCache via readPlaybackAudio.  When
                // a chunk is freshly published, any prior Stage 2 cache entry
                // captured before this chunk landed is now stale: the worker
                // had read the OLD RenderCache (or an empty slot for that chunk)
                // and produced stretched output that doesn't include the new
                // pitch correction.
                //
                // Without this trigger, the user-visible symptom (per Journey 1
                // testing): "edit pitch then time-stretched, can't hear pitch
                // change until I edit again to bump revisions and retrigger
                // Stage 2".
                //
                // Fix: invalidate Stage 2 cache + enqueue a rebuild so the
                // freshly rendered chunk gets composed into a new Stage 2 PCM.
                // requestStage2Rebuild dedups on matId, so multiple chunk
                // completions in quick succession collapse to a single rebuild.
                // For identity-TimeGrid clips, runStage2RebuildForMaterialization
                // bails out early after invalidating �?net cost is a worker
                // wakeup + early return.
                if (chunkMatId != 0 && materializationStore_ != nullptr) {
                    materializationStore_->getTimeStretchCache().invalidate(chunkMatId);
                    requestStage2Rebuild(chunkMatId);
                }
            } else {
                AppLogger::error("ChunkRender: vocoder failed matId="
                    + juce::String(static_cast<juce::int64>(chunkMatId))
                    + " start=" + juce::String(jobStartSeconds, 3)
                    + " targetRevision=" + juce::String(static_cast<juce::int64>(targetRevision))
                    + " error=" + error);
                renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::TerminalFailure);
            }
            schedulerCv_.notify_one();
        };

        vocoderDomain_->submit(std::move(vocoderJob));
    }
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
    // When a non-identity TimeGrid is published AND the Stage 2 worker has
    // populated the TimeStretchCache for this (materializationId, pitchRev,
    // timeGridRev), serve the audio directly from cache.  This bypasses the
    // dry-source resample + RenderCache piecewise-replace overlay path entirely.
    //
    // On any miss (cache not yet populated, locked, or revision mismatch),
    // fall through to the existing dry-path �?perceptually the user hears
    // unstretched audio for the brief moment until Stage 2 finishes.
    // ============================================================
    if (!request.source.timeGridIsIdentity
        && request.source.timeStretchCache != nullptr
        && request.source.materializationId != 0) {
        const int wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.materializationId,
            request.readStartSeconds,
            destination,
            destinationStartSample,
            writableSamples,
            static_cast<int>(request.targetSampleRate));
        if (wrote > 0) {
            return wrote;
        }
        // Cache miss �?fall through to the dry-path (silent-stretch fallback)
        // until Stage 2 finishes populating the cache for this revision.
    }

    const auto& srcBuffer = *request.source.audioBuffer;
    const int srcChannels = srcBuffer.getNumChannels();
    const int64_t srcLengthSamples = srcBuffer.getNumSamples();
    constexpr double srcSampleRate = TimeCoordinate::kRenderSampleRate;
    if (srcChannels <= 0 || srcLengthSamples <= 0) {
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
    // at `MaterializationStore::createMaterialization`). The `srcCh = ch % srcChannels`
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
// Clipboard �?materialization range copy for paste/duplicate
// ============================================================================

uint64_t OpenTuneAudioProcessor::copyMaterializationRange(uint64_t sourceMaterializationId,
                                                           double offsetSeconds,
                                                           double durationSeconds,
                                                           const juce::String& newName)
{
    juce::ignoreUnused(newName);
    if (materializationStore_ == nullptr || durationSeconds <= 0.0)
        return 0;

    MaterializationStore::MaterializationSnapshot sourceSnap;
    if (!materializationStore_->getSnapshot(sourceMaterializationId, sourceSnap))
        return 0;
    if (!sourceSnap.audioBuffer || sourceSnap.audioBuffer->getNumSamples() == 0)
        return 0;

    const int64_t totalSamples = sourceSnap.audioBuffer->getNumSamples();
    constexpr double sr = 44100.0;
    const int64_t offsetSamples = static_cast<int64_t>(offsetSeconds * sr);
    const int64_t durSamples = static_cast<int64_t>(durationSeconds * sr);

    if (offsetSamples < 0 || durSamples <= 0 || offsetSamples + durSamples > totalSamples)
        return 0;

    // Slice audio buffer
    auto newBuffer = std::make_shared<juce::AudioBuffer<float>>(
        sourceSnap.audioBuffer->getNumChannels(),
        static_cast<int>(durSamples));
    for (int ch = 0; ch < sourceSnap.audioBuffer->getNumChannels(); ++ch)
        newBuffer->copyFrom(ch, 0, *sourceSnap.audioBuffer, ch,
                           static_cast<int>(offsetSamples), static_cast<int>(durSamples));

    // Build request
    MaterializationStore::CreateMaterializationRequest req;
    req.sourceId = sourceSnap.sourceId;
    req.audioBuffer = newBuffer;
    req.renderCache = std::make_shared<RenderCache>();
    req.sourceWindow = SourceWindow{
        sourceSnap.sourceWindow.sourceId,
        sourceSnap.sourceWindow.sourceStartSeconds + offsetSeconds,
        sourceSnap.sourceWindow.sourceStartSeconds + offsetSeconds + durationSeconds
    };
    req.originalF0State = sourceSnap.originalF0State;
    req.detectedKey = sourceSnap.detectedKey;
    req.lineageParentMaterializationId = sourceMaterializationId;

    // Copy pitch curve range (slice originalF0/originalEnergy by frame domain)
    if (sourceSnap.pitchCurve && sourceSnap.pitchCurve->getSnapshot() != nullptr
        && !sourceSnap.pitchCurve->isEmpty())
    {
        auto snap = sourceSnap.pitchCurve->getSnapshot();
        const double frameRate = snap->getSampleRate() / snap->getHopSize();
        const int startFrame = static_cast<int>(std::round(offsetSeconds * frameRate));
        const int endFrame   = static_cast<int>(std::round((offsetSeconds + durationSeconds) * frameRate));
        const int numFrames  = endFrame - startFrame;

        if (startFrame >= 0 && endFrame <= static_cast<int>(snap->size()) && numFrames > 0)
        {
            const auto& origF0 = snap->getOriginalF0();
            const auto& origEnergy = snap->getOriginalEnergy();

            std::vector<float> slicedF0(origF0.begin() + startFrame, origF0.begin() + endFrame);

            std::vector<float> slicedEnergy;
            if (!origEnergy.empty()
                && static_cast<size_t>(endFrame) <= origEnergy.size())
            {
                slicedEnergy.assign(origEnergy.begin() + startFrame,
                                    origEnergy.begin() + endFrame);
            }

            auto slicedCurve = std::make_shared<PitchCurve>();
            slicedCurve->setHopSize(snap->getHopSize());
            slicedCurve->setSampleRate(snap->getSampleRate());
            slicedCurve->setOriginalF0(slicedF0);
            if (!slicedEnergy.empty())
                slicedCurve->setOriginalEnergy(slicedEnergy);

            req.pitchCurve = slicedCurve;
        }
    }

    // Copy notes shifted by offsetSeconds
    if (!sourceSnap.notes.empty())
    {
        req.notes.reserve(sourceSnap.notes.size());
        for (auto note : sourceSnap.notes)
        {
            if (note.endTime <= offsetSeconds || note.startTime >= offsetSeconds + durationSeconds)
                continue;

            note.startTime = std::max(0.0, note.startTime - offsetSeconds);
            note.endTime = std::min(durationSeconds, note.endTime - offsetSeconds);
            if (note.endTime > note.startTime)
                req.notes.push_back(note);
        }
    }

    // Copy silent gaps shifted by offsetSamples
    if (!sourceSnap.silentGaps.empty())
    {
        req.silentGaps.reserve(sourceSnap.silentGaps.size());
        for (auto gap : sourceSnap.silentGaps)
        {
            if (gap.endSampleExclusive <= offsetSamples
                || gap.startSample >= offsetSamples + durSamples)
                continue;

            gap.startSample = std::max<int64_t>(0, gap.startSample - offsetSamples);
            gap.endSampleExclusive = std::min<int64_t>(durSamples,
                                                        gap.endSampleExclusive - offsetSamples);
            if (gap.endSampleExclusive > gap.startSample)
                req.silentGaps.push_back(gap);
        }
    }

    // Copy corrected segments (pitch correction data)
    if (sourceSnap.pitchCurve)
    {
        auto sourceSnapCurve = sourceSnap.pitchCurve->getSnapshot();
        if (sourceSnapCurve && !sourceSnapCurve->getCorrectedSegments().empty())
        {
            std::vector<CorrectedSegment> segments;
            segments.reserve(sourceSnapCurve->getCorrectedSegments().size());
            for (auto seg : sourceSnapCurve->getCorrectedSegments())
            {
                if (seg.endFrame <= offsetSamples || seg.startFrame >= offsetSamples + durSamples)
                    continue;
                seg.startFrame = static_cast<int>(std::max<int64_t>(0, seg.startFrame - offsetSamples));
                seg.endFrame   = static_cast<int>(std::min<int64_t>(durSamples, seg.endFrame - offsetSamples));
                if (seg.endFrame > seg.startFrame)
                    segments.push_back(std::move(seg));
            }
            if (!segments.empty() && req.pitchCurve)
                req.pitchCurve->replaceCorrectedSegments(segments);
        }
    }

    // Copy time grid (v7 vocal-time-stretch data) �?offset handles to new origin
    if (sourceSnap.timeGrid && !sourceSnap.timeGrid->empty())
    {
        const auto& srcHandles = sourceSnap.timeGrid->handles();
        std::vector<TimeHandle> newHandles;
        newHandles.reserve(srcHandles.size());
        for (const auto& h : srcHandles)
        {
            TimeHandle nh = h;
            nh.source_seconds -= offsetSeconds;
            nh.output_seconds -= offsetSeconds;
            if (nh.source_seconds >= 0.0 && nh.source_seconds <= durationSeconds)
                newHandles.push_back(std::move(nh));
        }
        if (!newHandles.empty())
            req.timeGrid = TimeGridSnapshot::makeFromHandles(std::move(newHandles),
                                                              sourceSnap.timeGrid->revision());
    }

    return materializationStore_->createMaterialization(std::move(req));
}

uint64_t OpenTuneAudioProcessor::cloneMaterialization(uint64_t sourceMaterializationId,
                                                      const juce::String& newName)
{
    juce::ignoreUnused(newName);
    if (materializationStore_ == nullptr) {
        return 0;
    }

    MaterializationStore::MaterializationSnapshot sourceSnap;
    if (!materializationStore_->getSnapshot(sourceMaterializationId, sourceSnap)) {
        return 0;
    }

    if (sourceSnap.audioBuffer == nullptr || sourceSnap.audioBuffer->getNumSamples() == 0) {
        return 0;
    }

    MaterializationStore::CreateMaterializationRequest request;
    request.sourceId = sourceSnap.sourceId;
    request.lineageParentMaterializationId = sourceMaterializationId;
    request.sourceWindow = sourceSnap.sourceWindow;
    request.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(*sourceSnap.audioBuffer);
    request.pitchCurve = sourceSnap.pitchCurve != nullptr ? sourceSnap.pitchCurve->clone() : nullptr;
    request.originalF0State = sourceSnap.originalF0State;
    request.detectedKey = sourceSnap.detectedKey;
    request.renderCache = std::make_shared<RenderCache>();
    request.notes = sourceSnap.notes;
    request.silentGaps = sourceSnap.silentGaps;
    request.renderRevision = sourceSnap.renderRevision;
    request.timeGrid = sourceSnap.timeGrid;
    return materializationStore_->createMaterialization(std::move(request));
}

} // namespace OpenTune

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenTune::OpenTuneAudioProcessor();
}
