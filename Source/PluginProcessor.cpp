#include "PluginProcessor.h"
#if JucePlugin_Build_Standalone
#include "SourceStore.h"
#include "StandaloneArrangement.h"
#include "Utils/PlacementActions.h"
#endif
#include "Editor/EditorFactory.h"
#include "Editor/ConfirmDialogContent.h"
#include "Standalone/UI/UIColors.h"
#include "DSP/ResamplingManager.h"
#include "DSP/MelSpectrogram.h"
#include "DSP/F0KeyDetector.h"
#include "Services/F0ExtractionService.h"
#include "Services/ImportedClipF0Extraction.h"
#include "Runtime/ProcessF0Runtime.h"
#include "Utils/ModelPathResolver.h"
#include "Utils/AppLogger.h"
#include "Utils/ExternalEditSession.h"
#include "Utils/ChannelLayoutLogger.h"
#if !JucePlugin_Build_Standalone
#include "Plugin/Capture/CaptureSession.h"
#include "Plugin/Capture/CapturePersistence.h"
#include "Plugin/Vst3ProcessorStateCodec.h"
#elif JucePlugin_Build_Standalone
#include "Plugin/StandaloneProcessorStateCodec.h"
#endif
#include "Utils/TimeCoordinate.h"
#include "Utils/PlaybackSampleGeometry.h"
#include "Utils/CaptureContentSlice.h"
#include "Utils/LocalizationManager.h"
#include "Utils/PlacementFade.h"
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/ScaleUiMapping.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchUtils.h"
#include "Utils/TuningConfig.h"
#include "Utils/VolumeEnvelopeEditAction.h"
#include "Utils/PianoRollNotePatchAction.h"
#include "Utils/NoteDraftEdit.h"
#include "Utils/ScissorsUndoAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/TimeGridEditAction.h"
#include "Render/Stage2TimeStretchRebuilder.h"
#include "Render/ContentRenderService.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <numeric>

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif
#include "Content/ContentEditCommands.h"
#include "Content/ContentPatchGeometry.h"
#include "Content/ContentSnapshotProjection.h"
#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
#include "Control/ControlService.h"
#endif

namespace OpenTune {

namespace {

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
        key, std::move(snapshot), payload.nativeSource, payload.audioBuffer,
        payload.sampleRate, payload.firstCanonicalSample,
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

    // TimeGrid 由各 owner 入口（import/split/merge/copy/restore）按权威
    // source window duration 显式初始化；这里只提交完整 payload，不再补造或改写。
    clip->content() = std::move(payload);
    publishStandalonePlaybackSource(crs, key, clip->content());
    return key;
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

    // 全覆盖范围直接共享不可变存储，切片入口不复制整段 canonical。
    if (clampedStart == 0 && clampedEnd == audioBuffer->getNumSamples()) {
        return audioBuffer;
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
    if (!canMergePlacementPitchCurves(leadingCurve, leadingState, trailingCurve, trailingState)) {
        return nullptr;
    }

    if (leadingCurve == nullptr)
        return {};

    const auto& leadingSnapshot = *leadingCurve;
    const auto& trailingSnapshot = *trailingCurve;

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
    if (!leadingSnapshot.getErasedRanges().empty())
        mergedCurve->replaceErasedRangesInRange(
            0, leadingFrameCount, leadingSnapshot.getErasedRanges());
    if (!trailingSnapshot.getErasedRanges().empty()) {
        std::vector<F0FrameRange> trailingRanges = trailingSnapshot.getErasedRanges();
        for (auto& range : trailingRanges) {
            range.startFrame += leadingFrameCount;
            range.endFrameExclusive += leadingFrameCount;
        }
        mergedCurve->replaceErasedRangesInRange(
            leadingFrameCount,
            leadingFrameCount + static_cast<int>(trailingOriginalF0.size()),
            trailingRanges);
    }
    mergedCurve->replaceCorrectionSegments(mergedSegments);
    return mergedCurve;
}
#endif // JucePlugin_Build_Standalone

} // anonymous namespace


#if JucePlugin_Build_Standalone
namespace {

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
// settings payload (encoded/decoded by StandaloneProcessorStateCodec), while
// host-plugin targets own the OTST processor state payload (encoded/decoded by
// Vst3ProcessorStateCodec).

ReferenceFeatureSet OpenTuneAudioProcessor::buildStandardAutoReferenceFeatureSet(
    const EditableContentSnapshot& snapshot, int analysisRevision)
{
    ReferenceFeatureSet failed;
    failed.status = ReferenceFeatureStatus::Failed;
    failed.producer = ReferenceFeatureProducer::StandardAuto;
    failed.inputFingerprint = static_cast<int64_t>(snapshot.audioRevision);
    failed.analysisRevision = analysisRevision;

    if (snapshot.originalF0State != OriginalF0State::Ready || snapshot.pitchCurve == nullptr) {
        failed.errorMessage = "Reference standard analysis requires Original F0";
        return failed;
    }

    const auto pitchSnapshot = snapshot.pitchCurve;
    const auto& originalF0 = pitchSnapshot->getOriginalF0();
    const auto& originalEnergy = pitchSnapshot->getOriginalEnergy();
    const int hopSize = pitchSnapshot->getHopSize();
    const double sampleRate = pitchSnapshot->getSampleRate();
    if (originalF0.empty() || hopSize <= 0 || sampleRate <= 0.0) {
        failed.errorMessage = "Reference standard analysis requires valid Original F0";
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
        "Reference standard analysis found no notes or timing anchors");
}

void OpenTuneAudioProcessor::analysisFinished(
    ContentKey key,
    const ReferenceFeatureSet& result)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;

    if (result.inputFingerprint != static_cast<int64_t>(snap->audioRevision)
        || result.producer != ReferenceFeatureProducer::StandardAuto) {
        return;
    }

    setContentReferenceFeatures(key, result);
#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
    controlService_->wakeJobs();
#endif

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
    undoManager_.setHistoryPrunedCallback([this] {
        synchronizeStandaloneHistory();
        scheduleReclaimSweep();
    });
#endif
#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
    // Start the external service after construction, guarded by its liveness token.
    controlService_ = std::make_unique<ControlService>(*this);
    controlService_->scheduleStart();
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
        1, 100, [] { return ProcessF0Runtime::getInstance().getF0Service(); });
    auto refSvc = std::make_unique<ReferenceAnalysisService>();
    auto appPreferences = std::make_unique<AppPreferences>();

    // 资源所有权：ARA 绑定实例的渲染全部走 DC 自己的 CRS（DC 构造时安装自己的
    // ExecutionLease），processor 不得再持有 local CRS / CaptureSession，避免出现
    // 第二个不可达的渲染所有者。未绑定的普通宿主插件 / Standalone 实例行为不变。
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

            if (job.renderCache == nullptr) {
                ProcessRenderRuntime::notifyApplicationRenderFailure(
                    job.contentKey, job.contentRevision, job.targetRevision,
                    "Render job has no RenderCache", {}, completionGate_);
#if !JucePlugin_Build_Standalone
                const auto gate = completionGate_;
                const auto failedKey = job.contentKey;
                const bool posted = juce::MessageManager::callAsync(
                    [this, gate, failedKey]()
                    {
                        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                        if (!callbackLease)
                            return;
                        notifyRenderTerminal(failedKey, false);
                    });
                if (!posted)
                    AppLogger::error("PluginProcessor: failed to dispatch render error");
#else
                const auto gate = completionGate_;
                const auto failedKey = job.contentKey;
                juce::MessageManager::callAsync([this, gate, failedKey] {
                    ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                    if (callbackLease)
                        notifyRenderTerminal(failedKey, false);
                });
#endif
                return;
            }

            auto contentSnap = job.contentSnapshot;
            if (!contentSnap) {
                if (job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
                {
                    const auto failureReason = juce::String("Render job has no content snapshot");
                    ProcessRenderRuntime::notifyApplicationRenderFailure(
                        job.contentKey, job.contentRevision, job.targetRevision,
                        failureReason, job.renderCache, completionGate_);
#if !JucePlugin_Build_Standalone
                    auto gate = completionGate_;
                    const auto failedKey = job.contentKey;
                    const bool posted = juce::MessageManager::callAsync(
                        [this, gate, failedKey]()
                        {
                            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                            if (!callbackLease)
                                return;
                            notifyRenderTerminal(failedKey, false);
#if !JucePlugin_Build_Standalone
                            if (auto* session = getCaptureSession())
                                session->onRenderFailed(failedKey, "Render job has no content snapshot");
#endif
                        });
                    if (!posted)
                        AppLogger::error("PluginProcessor: failed to dispatch render error");
#else
                    const auto gate = completionGate_;
                    const auto failedKey = job.contentKey;
                    juce::MessageManager::callAsync([this, gate, failedKey] {
                        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                        if (callbackLease)
                            notifyRenderTerminal(failedKey, false);
                    });
#endif
                }
                return;
            }
            ProcessRenderRuntime::CompletionContext completion;
            completion.gate = completionGate_;
            completion.applicationFailure = [gate = completionGate_](
                ContentKey key, uint64_t contentRevision, uint64_t targetRevision,
                const juce::String& reason, std::shared_ptr<RenderCache> renderCache) {
                ProcessRenderRuntime::notifyApplicationRenderFailure(
                    key, contentRevision, targetRevision, reason, std::move(renderCache), gate);
            };
            completion.chunkSettled = [this](
                                          ContentKey key,
                                          std::shared_ptr<const EditableContentSnapshot> snapshot,
                                          std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                                          double audioSampleRate) {
#if JucePlugin_Build_Standalone
                // Stage2 must enter the worker queue before the Runtime callback
                // returns; export drain() cannot wait for a MessageManager hop
                // while running on the render thread.
                enqueueStage2WhenCanonicalSettled(
                    key, snapshot, audioBuffer, audioSampleRate);
#endif
                handleStage1ChunkSettled(key, std::move(snapshot),
                                         std::move(audioBuffer), audioSampleRate);
            };
            completion.chunkFailed = [this](ContentKey key, const juce::String& reason) {
                notifyRenderTerminal(key, false);
#if !JucePlugin_Build_Standalone
                if (auto* session = getCaptureSession())
                    session->onRenderFailed(key, reason);
#else
                juce::ignoreUnused(key);
#endif
            };
            const bool lightPitchEnabled = appPreferences_->getState().shared.lightPitchCorrectionEnabled;
            ProcessRenderRuntime::getInstance().processChunkRenderJob(
                contentRenderService_, job,
                lightPitchEnabled, std::move(completion));
        };
        const auto renderFailureGate = completionGate_;
        lease.renderFailureCallback = [this, renderFailureGate](
            const RenderJob& job, const juce::String& reason) {
            ProcessRenderRuntime::notifyApplicationRenderFailure(
                job.contentKey, job.contentRevision, job.targetRevision,
                reason, job.renderCache, renderFailureGate);
#if !JucePlugin_Build_Standalone
            const bool posted = juce::MessageManager::callAsync(
                [this, renderFailureGate, key = job.contentKey, reason]() {
                    ProcessRenderRuntime::CompletionCallbackLease callbackLease(*renderFailureGate);
                    if (!callbackLease)
                        return;
                    notifyRenderTerminal(key, false);
                    if (auto* session = getCaptureSession())
                        session->onRenderFailed(key, reason);
                });
            if (!posted)
                AppLogger::error("RenderWorker callback failure could not be dispatched to CaptureSession");
#else
            juce::MessageManager::callAsync(
                [this, renderFailureGate, key = job.contentKey]() {
                    ProcessRenderRuntime::CompletionCallbackLease callbackLease(*renderFailureGate);
                    if (callbackLease)
                        notifyRenderTerminal(key, false);
                });
#endif
        };
        const auto backendChangeGate = completionGate_;
        lease.backendChangedCallback = [this, backendChangeGate]() {
            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*backendChangeGate);
            if (callbackLease)
                handleSharedBackendChanged();
        };
        crs->attachExecutionLease(std::move(lease));
    }
    class ProcessorContentCommands final : public ContentEditCommands
    {
    public:
        explicit ProcessorContentCommands(OpenTuneAudioProcessor& proc) noexcept : proc_(proc) {}

        std::optional<ContentDetectedKeyEditResult> setDetectedKey(
            ContentKey key, const DetectedKey& detectedKey) override
        {
            const auto snapshot = proc_.getContentSnapshot(key);
            if (snapshot == nullptr)
                return std::nullopt;
            ContentDetectedKeyEditResult result{snapshot->detectedKey, false};
            if (!proc_.setContentDetectedKey(key, detectedKey))
                return std::nullopt;
            const auto after = proc_.getContentSnapshot(key);
            if (after == nullptr)
                return std::nullopt;
            result.changed = result.before.root != after->detectedKey.root
                             || result.before.scale != after->detectedKey.scale
                             || result.before.origin != after->detectedKey.origin
                             || result.before.confidence != after->detectedKey.confidence;
            if (result.changed) {
                proc_.getUndoManager().clearRedo();
                proc_.notifyContentEditCompleted(key);
            }
            return result;
        }

        bool applyPitchShiftState(ContentKey key, const PitchShiftEditState& state) override
        {
            return proc_.applyContentPitchShiftState(key, state);
        }

        PitchShiftEditAction* commitPitchShiftEdit(
            ContentKey key,
            const PitchShiftSettings& newSettings) override
        {
            return proc_.commitPitchShiftEdit(key, newSettings);
        }

        ContentEditCommitResult commitAutoTuneContentRangeUndoable(
            ContentKey key,
            int startFrame,
            int endFrameExclusive,
            const NoteGeneratorParams& params,
            const std::optional<ScaleSnapConfig>& scaleSnap,
            const juce::String& description) override
        {
            const auto before = proc_.getContentSnapshot(key);
            if (before == nullptr || before->pitchCurve == nullptr
                || !proc_.autoTuneContentRangeByContentKey(key, startFrame, endFrameExclusive, params, scaleSnap))
                return {};

            const auto after = proc_.getContentSnapshot(key);
            if (after == nullptr || after->pitchCurve == nullptr)
                return {};
            const auto affected = PitchCurve::expandNoteBasedCorrectionRange(
                startFrame, endFrameExclusive, static_cast<int>(before->pitchCurve->size()));
            auto action = createPianoRollEditAction(
                key, before->notes, before->pitchCurve->getCorrectionSegments(), after,
                std::vector<ContentEditRangeFrames>{
                    {affected.startFrame, affected.endFrameExclusive}}, description);
            if (action == nullptr)
                return {};
            auto* actionPtr = action.get();
            proc_.getUndoManager().addAction(std::move(action));
            proc_.notifyContentEditCompleted(key);
            return {after, actionPtr};
        }

        bool generateNotesOnly(ContentKey key, const NoteGeneratorParams& params) override
        {
            const auto snapshot = proc_.getContentSnapshot(key);
            if (snapshot == nullptr)
                return false;
            auto patch = planGenerateNotesOnly(*snapshot, params);
            if (!patch || !commitGeneratedNotesOnly(key, std::move(*patch)))
                return false;
            return true;
        }

        bool commitGeneratedNotesOnly(ContentKey key, ContentNoteRangePatch patch) override
        {
            if (proc_.commitContentNoteTopologyPatch(key, std::move(patch)) == nullptr)
                return false;
            proc_.getUndoManager().clearRedo();
            proc_.notifyContentEditCompleted(key);
            return true;
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

        ContentCommitSnapshot commitNotesAndSegmentsInRanges(
            ContentKey key,
            std::vector<Note> notes,
            std::vector<PitchCorrectionSegment> segments,
            const std::vector<ContentEditRangeFrames>& affectedRanges) override
        {
            return proc_.commitContentNotesAndSegmentsInRanges(
                key, std::move(notes), std::move(segments), affectedRanges);
        }

        ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
            ContentKey key,
            std::vector<Note> notes,
            std::vector<PitchCorrectionSegment> segments,
            ContentEditRangeFrames affectedRange,
            std::vector<float> originalF0InRange,
            std::optional<std::vector<F0FrameRange>> erasedRanges) override
        {
            return proc_.commitContentNotesAndSegmentsWithOriginalF0(
                key, std::move(notes), std::move(segments), affectedRange,
                std::move(originalF0InRange), std::move(erasedRanges));
        }

        ContentEditCommitResult commitNotesAndSegmentsUndoable(
            ContentKey key,
            std::vector<Note> notes,
            std::vector<PitchCorrectionSegment> segments,
            ContentEditRangeFrames affectedRange,
            const juce::String& description) override
        {
            return commitNotesAndSegmentsUndoableInRanges(
                key, std::move(notes), std::move(segments),
                std::vector<ContentEditRangeFrames>{affectedRange}, description);
        }

        ContentEditCommitResult commitNotesAndSegmentsUndoableInRanges(
            ContentKey key,
            std::vector<Note> notes,
            std::vector<PitchCorrectionSegment> segments,
            const std::vector<ContentEditRangeFrames>& affectedRanges,
            const juce::String& description) override
        {
            if (affectedRanges.empty())
                return {};

            const auto before = proc_.getContentSnapshot(key);
            if (before == nullptr || before->pitchCurve == nullptr)
                return {};
            auto committed = proc_.commitContentNotesAndSegmentsInRanges(
                key, std::move(notes), std::move(segments), affectedRanges);
            if (committed == nullptr || committed->pitchCurve == nullptr)
                return {};

            auto action = createPianoRollEditAction(
                key, before->notes, before->pitchCurve->getCorrectionSegments(),
                committed, affectedRanges, description);
            if (action == nullptr)
                return {};
            auto* actionPtr = action.get();
            proc_.getUndoManager().addAction(std::move(action));
            proc_.notifyContentEditCompleted(key);
            return {std::move(committed), actionPtr};
        }

        std::unique_ptr<PianoRollEditAction> createPianoRollEditAction(
            ContentKey key,
            const std::vector<Note>& beforeNotes,
            const std::vector<PitchCorrectionSegment>& beforeSegments,
            ContentCommitSnapshot after,
            const std::vector<ContentEditRangeFrames>& affectedRanges,
            const juce::String& description) const override
        {
            if (after == nullptr || after->pitchCurve == nullptr || affectedRanges.empty())
                return {};
            return std::make_unique<PianoRollEditAction>(
                proc_.getContentCommands(), key,
                description.isNotEmpty() ? description : LOC_RAW("Edit"),
                notesInRanges(beforeNotes, *after->pitchCurve, affectedRanges),
                notesInRanges(after->notes, *after->pitchCurve, affectedRanges),
                segmentsInRanges(beforeSegments, affectedRanges),
                segmentsInRanges(after->pitchCurve->getCorrectionSegments(), affectedRanges),
                affectedRanges);
        }

        std::optional<ContentNoteCorrectionPlan> planNoteCorrection(
            const EditableContentSnapshot& snapshot,
            const std::vector<Note>& notes,
            F0FrameRange editRange,
            const NoteGeneratorParams& params) const override
        {
            if (snapshot.pitchCurve == nullptr || editRange.isEmpty())
                return std::nullopt;
            const auto& curve = *snapshot.pitchCurve;
            const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())};
            auto scratch = PitchCurve::fromSnapshot(snapshot.pitchCurve);
            auto corrected = scratch->applyCorrectionToRange(
                notes, editRange.startFrame, editRange.endFrameExclusive,
                static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
                params.retuneSpeed, params.vibratoDepth, params.vibratoRate);
            const auto affected = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame, editRange.endFrameExclusive, timeline.endFrameExclusive());
            if (affected.isEmpty() || corrected == nullptr)
                return std::nullopt;

            ContentNoteCorrectionPlan plan;
            plan.affectedRange = affected;
            plan.notes = notes;
            plan.correctedCurve = corrected;
            for (const auto& segment : corrected->getCorrectionSegments())
                if (segment.startFrame < affected.endFrameExclusive && segment.endFrame > affected.startFrame)
                    plan.segments.push_back(segment);
            return plan;
        }

        F0FrameRange noteFrameRange(
            const PitchCurveSnapshot& curve,
            const std::vector<Note>& notes,
            const std::vector<std::size_t>& positions) const override
        {
            double start = 1e30;
            double end = -1e30;
            for (const auto position : positions) {
                if (position >= notes.size())
                    continue;
                start = std::min(start, notes[position].startTime);
                end = std::max(end, notes[position].endTime);
            }
            if (end <= start)
                return {};
            return F0Timeline{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())}
                .rangeForTimes(start, end);
        }

        ContentAutoSnapPlan planAutoSnap(
            const EditableContentSnapshot& snapshot,
            const std::optional<std::vector<std::size_t>>& targets,
            const ScaleSnapConfig& scaleSnap,
            const NoteGeneratorParams& params) const override
        {
            ContentAutoSnapPlan plan;
            plan.notes = snapshot.notes;
            if (snapshot.pitchCurve == nullptr)
                return plan;

            const auto& curve = *snapshot.pitchCurve;
            const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())};
            if (timeline.isEmpty()) {
                plan.status = ContentAutoSnapPlan::Status::EmptyRange;
                return plan;
            }

            const bool selectionActive = targets.has_value();
            std::vector<std::size_t> indices;
            if (selectionActive) {
                for (const auto index : *targets)
                    if (index < plan.notes.size())
                        indices.push_back(index);
            } else {
                indices.resize(plan.notes.size());
                std::iota(indices.begin(), indices.end(), std::size_t{0});
            }
            if (indices.empty())
                return plan;

            double dirtyStart = 1e30;
            double dirtyEnd = -1e30;
            bool pitchChanged = false;
            for (const auto index : indices) {
                auto& note = plan.notes[index];
                const float baseMidi = PitchUtils::freqToMidi(note.pitch);
                const float offset = scaleSnap.quantizeMidiToActiveScale(baseMidi) - baseMidi;
                if (std::abs(offset - note.pitchOffset) < 0.001f)
                    continue;
                note.pitchOffset = offset;
                note.dirty = true;
                pitchChanged = true;
                dirtyStart = std::min(dirtyStart, note.startTime);
                dirtyEnd = std::max(dirtyEnd, note.endTime);
            }

            std::vector<Note> targetNotes;
            double targetStart = 1e30;
            double targetEnd = -1e30;
            targetNotes.reserve(indices.size());
            for (const auto index : indices) {
                const auto& note = plan.notes[index];
                targetStart = std::min(targetStart, note.startTime);
                targetEnd = std::max(targetEnd, note.endTime);
                targetNotes.push_back(note);
            }
            const auto noteFrames = timeline.rangeForTimes(targetStart, targetEnd);
            if (!pitchChanged && fullyCorrected(curve, noteFrames.startFrame, noteFrames.endFrameExclusive))
                return plan;

            if (pitchChanged) {
                dirtyStart = std::min(dirtyStart, targetStart);
                dirtyEnd = std::max(dirtyEnd, targetEnd);
            } else {
                dirtyStart = targetStart;
                dirtyEnd = targetEnd;
            }
            const auto editRange = timeline.rangeForTimes(dirtyStart, dirtyEnd);
            if (editRange.isEmpty()) {
                plan.status = ContentAutoSnapPlan::Status::EmptyRange;
                return plan;
            }

            auto scratch = PitchCurve::fromSnapshot(snapshot.pitchCurve);
            const auto corrected = scratch->applyCorrectionToRange(
                targetNotes, editRange.startFrame, editRange.endFrameExclusive,
                static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
                params.retuneSpeed, params.vibratoDepth, params.vibratoRate);
            const auto affected = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame, editRange.endFrameExclusive, timeline.endFrameExclusive());

            std::vector<PitchCorrectionSegment> segments;
            if (!selectionActive) {
                for (const auto& segment : corrected->getCorrectionSegments())
                    if (segment.startFrame < affected.endFrameExclusive && segment.endFrame > affected.startFrame)
                        segments.push_back(segment);
            } else {
                std::vector<F0FrameRange> noteFrames;
                for (const auto& note : targetNotes) {
                    const int start = timeline.exclusiveFrameAt(note.startTime);
                    const int end = timeline.exclusiveFrameAt(note.endTime);
                    if (end > start)
                        noteFrames.push_back({start, end});
                }
                std::sort(noteFrames.begin(), noteFrames.end(),
                          [](const F0FrameRange& a, const F0FrameRange& b) {
                              return a.startFrame < b.startFrame;
                          });
                std::vector<F0FrameRange> targetFrames;
                for (const auto& frames : noteFrames) {
                    if (targetFrames.empty() || targetFrames.back().endFrameExclusive < frames.startFrame)
                        targetFrames.push_back(frames);
                    else
                        targetFrames.back().endFrameExclusive =
                            std::max(targetFrames.back().endFrameExclusive, frames.endFrameExclusive);
                }

                for (const auto& segment : corrected->getCorrectionSegments())
                    for (const auto& frames : targetFrames)
                        if (auto cut = cutSegment(segment, frames.startFrame, frames.endFrameExclusive))
                            segments.push_back(std::move(*cut));

                for (const auto& segment : curve.getCorrectionSegments()) {
                    if (segment.endFrame <= affected.startFrame || segment.startFrame >= affected.endFrameExclusive)
                        continue;
                    std::vector<F0FrameRange> spans{{segment.startFrame, segment.endFrame}};
                    for (const auto& frames : targetFrames) {
                        std::vector<F0FrameRange> remainder;
                        for (const auto& span : spans) {
                            if (frames.startFrame >= span.endFrameExclusive || frames.endFrameExclusive <= span.startFrame) {
                                remainder.push_back(span);
                                continue;
                            }
                            if (span.startFrame < frames.startFrame)
                                remainder.push_back({span.startFrame, std::min(span.endFrameExclusive, frames.startFrame)});
                            if (frames.endFrameExclusive < span.endFrameExclusive)
                                remainder.push_back({std::max(span.startFrame, frames.endFrameExclusive), span.endFrameExclusive});
                        }
                        spans = std::move(remainder);
                        if (spans.empty())
                            break;
                    }
                    for (const auto& span : spans)
                        if (auto kept = cutSegment(segment, span.startFrame, span.endFrameExclusive))
                            segments.push_back(std::move(*kept));
                }
            }

            plan.status = ContentAutoSnapPlan::Status::Commit;
            plan.correction.affectedRange = affected;
            plan.correction.notes = plan.notes;
            plan.correction.segments = std::move(segments);
            plan.correction.correctedCurve = corrected;
            return plan;
        }

        std::optional<std::vector<Note>> planAutoTuneContentRange(
            const EditableContentSnapshot& snapshot,
            int startFrame,
            int endFrameExclusive,
            const NoteGeneratorParams& params,
            const std::optional<ScaleSnapConfig>& scaleSnap) const override
        {
            if (snapshot.pitchCurve == nullptr || endFrameExclusive <= startFrame)
                return std::nullopt;
            const auto& curve = *snapshot.pitchCurve;
            const auto& f0 = curve.getOriginalF0();
            const int count = static_cast<int>(f0.size());
            if (startFrame < 0 || endFrameExclusive > count || count == 0)
                return std::nullopt;
            auto generated = LegacyNoteGenerator::generate(
                f0.data(), count, nullptr, startFrame, endFrameExclusive,
                curve.getHopSize(), curve.getSampleRate(), params);
            if (!LegacyNoteGenerator::validate(generated))
                return std::nullopt;
            if (scaleSnap)
                scaleSnap->applyToNotes(generated);
            auto normalized = normalizeStoredNotes(std::move(generated));
            if (normalized.empty())
                return std::nullopt;

            const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), count};
            const double rangeStart = timeline.timeAtFrame(startFrame);
            const double rangeEnd = timeline.timeAtFrame(endFrameExclusive);
            std::vector<Note> merged;
            merged.reserve(snapshot.notes.size() + normalized.size());
            for (const auto& note : snapshot.notes)
                if (note.endTime <= rangeStart || note.startTime >= rangeEnd)
                    merged.push_back(note);
            merged.insert(merged.end(), normalized.begin(), normalized.end());
            std::sort(merged.begin(), merged.end(),
                      [](const Note& a, const Note& b) { return a.startTime < b.startTime; });
            return normalizeStoredNotes(std::move(merged));
        }

        std::optional<ContentNoteRangePatch> planGenerateNotesOnly(
            const EditableContentSnapshot& snapshot,
            const NoteGeneratorParams& params) const override
        {
            if (snapshot.pitchCurve == nullptr)
                return std::nullopt;
            const auto& curve = *snapshot.pitchCurve;
            const auto& f0 = curve.getOriginalF0();
            const int count = static_cast<int>(f0.size());
            if (count == 0)
                return std::nullopt;
            auto generated = LegacyNoteGenerator::generate(
                f0.data(), count, nullptr, 0, count, curve.getHopSize(), curve.getSampleRate(), params);
            if (!LegacyNoteGenerator::validate(generated))
                return std::nullopt;
            for (auto& note : generated)
                if (note.originalPitch > 0.0f)
                    note.pitch = note.originalPitch;

            const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), count};
            ContentNoteRangePatch patch;
            patch.affectedRange = {0.0, timeline.timeAtFrame(count)};
            for (const auto& note : generated) {
                patch.affectedRange.startSeconds = std::min(patch.affectedRange.startSeconds, note.startTime);
                patch.affectedRange.endSeconds = std::max(patch.affectedRange.endSeconds, note.endTime);
            }
            patch.afterNotesInRange = std::move(generated);
            return patch;
        }

        bool setTimeGrid(ContentKey key,
                          std::shared_ptr<const TimeGridSnapshot> grid) override
        {
            return proc_.setContentTimeGrid(key, std::move(grid));
        }

        TimeGridEditAction* commitTimeGridEdit(
            ContentKey key,
            std::shared_ptr<const TimeGridSnapshot> before,
            std::shared_ptr<const TimeGridSnapshot> after,
            const juce::String& description) override
        {
            if (!key.isValid() || before == nullptr || after == nullptr)
                return {};
            auto action = std::make_unique<TimeGridEditAction>(
                proc_.getContentCommands(), key,
                description.isNotEmpty() ? description : LOC_RAW("Edit time grid"), before, after);
            if (!proc_.setContentTimeGrid(key, std::move(after)))
                return nullptr;
            auto* actionPtr = action.get();
            proc_.getUndoManager().addAction(std::move(action));
            proc_.notifyContentEditCompleted(key);
            return actionPtr;
        }

    private:
        static std::pair<double, double> secondsRange(
            const PitchCurveSnapshot& curve, ContentEditRangeFrames range)
        {
            const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())};
            return {timeline.isEmpty() ? 0.0 : timeline.timeAtFrame(range.startFrame),
                    timeline.isEmpty() ? 0.0 : timeline.timeAtFrame(range.endFrameExclusive)};
        }

        static std::vector<Note> notesInRange(const std::vector<Note>& notes, double start, double end)
        {
            std::vector<Note> result;
            for (const auto& note : notes)
                if (note.startTime < end && note.endTime > start)
                    result.push_back(note);
            return result;
        }

        static std::vector<NoteRangeSeconds> secondsRanges(
            const PitchCurveSnapshot& curve,
            const std::vector<ContentEditRangeFrames>& ranges)
        {
            std::vector<NoteRangeSeconds> result;
            result.reserve(ranges.size());
            for (const auto& range : ranges) {
                const auto seconds = secondsRange(curve, range);
                result.push_back({seconds.first, seconds.second});
            }
            return result;
        }

        static std::vector<Note> notesInRanges(
            const std::vector<Note>& notes,
            const PitchCurveSnapshot& curve,
            const std::vector<ContentEditRangeFrames>& ranges)
        {
            const auto timeRanges = secondsRanges(curve, ranges);
            std::vector<Note> result;
            for (const auto& note : notes)
                if (std::any_of(timeRanges.begin(), timeRanges.end(), [&note](const NoteRangeSeconds& range) {
                        return note.startTime < range.endSeconds && note.endTime > range.startSeconds;
                    }))
                    result.push_back(note);
            return result;
        }

        static std::vector<PitchCorrectionSegment> segmentsInRange(
            const std::vector<PitchCorrectionSegment>& segments, int start, int end)
        {
            std::vector<PitchCorrectionSegment> result;
            for (const auto& segment : segments) {
                if (segment.endFrame <= start || segment.startFrame >= end)
                    continue;
                const int clipStart = std::max(segment.startFrame, start);
                const int clipEnd = std::min(segment.endFrame, end);
                const int offset = clipStart - segment.startFrame;
                if (clipEnd <= clipStart || offset + clipEnd - clipStart > static_cast<int>(segment.f0Data.size()))
                    continue;
                auto clipped = segment;
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(segment.f0Data.begin() + offset,
                                      segment.f0Data.begin() + offset + clipEnd - clipStart);
                result.push_back(std::move(clipped));
            }
            return result;
        }

        static std::vector<PitchCorrectionSegment> segmentsInRanges(
            const std::vector<PitchCorrectionSegment>& segments,
            const std::vector<ContentEditRangeFrames>& ranges)
        {
            std::vector<PitchCorrectionSegment> result;
            for (const auto& range : ranges) {
                auto clipped = segmentsInRange(segments, range.startFrame, range.endFrameExclusive);
                result.insert(result.end(),
                              std::make_move_iterator(clipped.begin()),
                              std::make_move_iterator(clipped.end()));
            }
            return result;
        }

        static bool fullyCorrected(const PitchCurveSnapshot& curve, int start, int end)
        {
            if (start < 0 || end > static_cast<int>(curve.size()) || end <= start)
                return false;
            bool covered = true;
            curve.forEachCorrectionF0Span(start, end, [&](int, const float* values, int) {
                if (values == nullptr)
                    covered = false;
            });
            return covered;
        }

        static std::optional<PitchCorrectionSegment> cutSegment(
            const PitchCorrectionSegment& segment, int start, int end)
        {
            const int clipStart = std::max(segment.startFrame, start);
            const int clipEnd = std::min(segment.endFrame, end);
            const int offset = clipStart - segment.startFrame;
            if (clipEnd <= clipStart || offset + clipEnd - clipStart > static_cast<int>(segment.f0Data.size()))
                return std::nullopt;
            auto clipped = segment;
            clipped.startFrame = clipStart;
            clipped.endFrame = clipEnd;
            clipped.f0Data.assign(segment.f0Data.begin() + offset,
                                  segment.f0Data.begin() + offset + clipEnd - clipStart);
            return clipped;
        }

        OpenTuneAudioProcessor& proc_;
    };
    auto commands = std::make_shared<ProcessorContentCommands>(*this);
    auto resampler = std::make_shared<ResamplingManager>();

#if !JucePlugin_Build_Standalone
    // Capture session（仅未绑定的宿主插件实例）：先局部构造，attach 成功后发布。
    // ARA 绑定实例的采集由 DC 的 ARA 对象模型承担，不创建 CaptureSession，也不启动 tick timer。
    std::unique_ptr<Capture::CaptureSession> capture;
    if (!araBound)
    {
        Capture::ProcessorBindings bindings;

        bindings.replaceWithRendered = [this](juce::AudioBuffer<float>& buffer,
                                               int destStart, int numSamples,
                                               const PlaybackReadSource& readSource,
                                               int64_t readStartSample,
                                               double outputStartSeconds,
                                               double targetSampleRate) {
            // Frozen with the replacement geometry at view publication.
            if (!readSource.hasAudio())
                return;
            const PlaybackReadRequest req(readSource, readStartSample,
                                          targetSampleRate, numSamples, outputStartSeconds);
            readPlaybackAudio(req, buffer, destStart);
        };

        bindings.getPlaybackReadSource = [this](ContentKey key, PlaybackReadSource& out) {
            return contentRenderService_ != nullptr
                && contentRenderService_->getPlaybackReadSource(key, out);
        };

        bindings.retireSegment = [this](ContentKey segmentContentKey) {
            // CaptureSegmentContent owns content; CRS cache is derived.
            if (contentRenderService_) {
                contentRenderService_->removePlaybackSource(segmentContentKey);
                contentRenderService_->removeRenderCache(segmentContentKey);
                contentRenderService_->removeStretcher(segmentContentKey);
                contentRenderService_->getTimeStretchCache().invalidate(segmentContentKey);
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

                const auto submitResult = f0ExtractionService_->submit(
                    F0RequestKey{segContentKey},
                    [audio, sr, segContentKey](const std::shared_ptr<F0RunOwnerState>& runOwnerState) -> F0ExtractionService::Result {
                        F0ExtractionService::Result result;
                        result.contentKey = segContentKey;
                        if (!audio || audio->getNumSamples() == 0
                            || audio->getNumChannels() <= 0) {
                            result.errorMessage = !audio
                                ? "Capture F0 extraction has no audio buffer"
                                : "Capture F0 audio buffer has no usable samples or channels (samples="
                                    + std::to_string(audio->getNumSamples()) + ", channels="
                                    + std::to_string(audio->getNumChannels()) + ")";
                            return result;
                        }
                        // Lazy initialize F0 runtime and resolve service inside worker.
                        auto& f0Runtime = ProcessF0Runtime::getInstance();
                        std::string initializationError;
                        if (!f0Runtime.isReady())
                            f0Runtime.initialize(ModelPathResolver::getModelsDirectory(), &initializationError);
                        auto f0Svc = f0Runtime.getF0Service();
                        if (!f0Svc) {
                            result.errorMessage = initializationError.empty()
                                ? "F0 runtime has no inference service after initialization"
                                : initializationError;
                            return result;
                        }
                        // F0 is mono; stereo input uses channel 0 (left).
                        const float* src = audio->getReadPointer(0);
                        const int numSamples = audio->getNumSamples();
                        auto extraction = f0Svc->extractF0(
                            src, static_cast<size_t>(numSamples),
                            static_cast<int>(sr), runOwnerState);
                        if (!extraction.ok()) {
                            result.errorMessage = extraction.error().fullMessage();
                            return result;
                        }
                        if (extraction.value().empty()) {
                            result.errorMessage = "F0 inference returned an empty result";
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
                        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                        if (!callbackLease)
                            return;
                        if (result.requestKey != F0RequestKey{segContentKey})
                            return;
                        if (auto* session = getCaptureSession()) {
                            if (!result.success) {
                                const auto reason = result.errorMessage.empty()
                                    ? std::string("Capture F0 extraction failed without an error detail")
                                    : result.errorMessage;
                                session->commitSegmentF0Result(
                                    segContentKey, nullptr,
                                    OriginalF0State::Failed,
                                    reason);
                                recordOriginalF0FailureEvent(segContentKey, juce::String(reason));
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
                    },
                    [this, gate, segContentKey](F0ExtractionService::Result&& result) {
                        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
                        if (!callbackLease
                            || result.requestKey != F0RequestKey{segContentKey})
                            return;
                        if (auto* session = getCaptureSession())
                        {
                            const auto reason = result.errorMessage.empty()
                                ? std::string("F0 completion dispatcher rejected the result without an error detail")
                                : result.errorMessage;
                            session->commitSegmentF0Result(
                                segContentKey, nullptr, OriginalF0State::Failed,
                                reason);
                            recordOriginalF0FailureEvent(segContentKey, juce::String(reason));
                        }
                    });
                if (submitResult != F0ExtractionService::SubmitResult::Accepted
                    && submitResult != F0ExtractionService::SubmitResult::AlreadyInProgress
                    && submitResult != F0ExtractionService::SubmitResult::ShuttingDown) {
                    const auto reason = submitResult == F0ExtractionService::SubmitResult::QueueFull
                        ? std::string("F0 extraction queue is full")
                        : std::string("F0 extraction submission was rejected: invalid task");
                    if (auto* session = getCaptureSession())
                    {
                        session->commitSegmentF0Result(
                            segContentKey, nullptr, OriginalF0State::Failed, reason);
                        recordOriginalF0FailureEvent(segContentKey, juce::String(reason));
                    }
                }
            }
        };

        // Capture 播放源的唯一汇聚点：CaptureSegmentContent 保留采集时原率 PCM
        // （不回写 owner），此处以 native 描述派生 CRS canonical 播放源。
        // canonical 全局原点由 snapshot.sourceWindow 决定；同率时共享原生 PCM，
        // 异率从 native 到 44.1kHz 一次范围转换。采样率使用实际 double 值。
        bindings.publishPlaybackSource = [this](const ContentKey& key,
                                                 std::shared_ptr<const EditableContentSnapshot> snapshot,
                                                 std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                 double sampleRate) {
            jassert(contentRenderService_ != nullptr);
            juce::ignoreUnused(audio, sampleRate);
            if (snapshot == nullptr || !key.isValid())
                return;

            const auto& native = snapshot->nativeSource;
            if (native.buffer == nullptr || native.sampleRate <= 0.0)
                return;

            const double canonicalRate = TimeCoordinate::kRenderSampleRate;
            // Owner initialization established the authoritative source window:
            // both canonical boundaries project from its absolute seconds.
            const int64_t firstCanonicalSample = PlaybackSampleGeometry::projectToSample(
                snapshot->sourceWindow.sourceStartSeconds * canonicalRate);
            const int64_t canonicalEndSample = std::max(firstCanonicalSample + 1,
                PlaybackSampleGeometry::projectToSample(
                    snapshot->sourceWindow.sourceEndSeconds * canonicalRate));

            const int canonicalLength = static_cast<int>(canonicalEndSample - firstCanonicalSample);
            int64_t canonicalFirstSample = firstCanonicalSample;
            std::shared_ptr<const juce::AudioBuffer<float>> canonicalAudio;
            if (native.sampleRate == canonicalRate) {
                const int64_t nativeTotal = native.buffer->getNumSamples();
                const int64_t nativeOffset = firstCanonicalSample - native.firstSourceSample;
                if (nativeOffset == 0 && static_cast<int64_t>(canonicalLength) == nativeTotal) {
                    canonicalAudio = native.buffer;
                    canonicalFirstSample = native.firstSourceSample;
                } else {
                    const int64_t begin = juce::jlimit<int64_t>(0, nativeTotal, nativeOffset);
                    const int64_t end = juce::jlimit<int64_t>(begin, nativeTotal,
                                                              nativeOffset + canonicalLength);
                    if (end <= begin)
                        return;
                    auto sliced = std::make_shared<juce::AudioBuffer<float>>(
                        native.buffer->getNumChannels(), static_cast<int>(end - begin));
                    for (int ch = 0; ch < sliced->getNumChannels(); ++ch) {
                        sliced->copyFrom(ch, 0, *native.buffer, ch,
                                         static_cast<int>(begin), sliced->getNumSamples());
                    }
                    canonicalAudio = std::move(sliced);
                    canonicalFirstSample = native.firstSourceSample + begin;
                }
            } else {
                auto derived = std::make_shared<juce::AudioBuffer<float>>(
                    native.buffer->getNumChannels(), canonicalLength);
                for (int ch = 0; ch < derived->getNumChannels(); ++ch) {
                    const auto resampled = resamplingManager_->resampleRange(
                        native.buffer->getReadPointer(ch),
                        static_cast<size_t>(native.buffer->getNumSamples()),
                        native.sampleRate, canonicalRate,
                        native.firstSourceSample, canonicalFirstSample,
                        canonicalLength);
                    derived->copyFrom(ch, 0, resampled.data(), canonicalLength);
                }
                canonicalAudio = std::move(derived);
            }

            auto readSource = makePlaybackReadSource(
                key, std::move(snapshot), std::move(native), std::move(canonicalAudio),
                canonicalRate, canonicalFirstSample,
                contentRenderService_->getOrCreateRenderCache(key),
                contentRenderService_->getTimeStretchCache());
            contentRenderService_->publishPlaybackSource(key, readSource);
        };

        bindings.requestFullRender = [this](ContentKey key) {
            requestFullContentRender(key);
        };

        capture = std::make_unique<Capture::CaptureSession>(std::move(bindings));

        // Log before publishing members so a throwing logger cannot leave partial state.
        AppLogger::log("OpenTuneAudioProcessor: host-plugin capture session created processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(this)));
    }
#endif // !JucePlugin_Build_Standalone

    // 完整成功后一次性发布成员（noexcept 移动）。
    f0ExtractionService_ = std::move(f0SvcOwner);
    referenceAnalysisService_ = std::move(refSvc);
    appPreferences_ = std::move(appPreferences);
#if JucePlugin_Build_Standalone
    sourceStore_ = std::move(srcStore);
#endif
    contentRenderService_ = std::move(crs);
#if JucePlugin_Build_Standalone
    standaloneContentRepository_ = std::move(repo);
#endif
    contentCommands_ = std::move(commands);
    resamplingManager_ = std::move(resampler);
#if !JucePlugin_Build_Standalone
    if (capture)
    {
        captureSession_ = std::move(capture);
        startTimerHz(30);
    }
#endif

    ProcessRenderRuntime::getInstance().retainOwner();
    runtimeStateInitialized_.store(true, std::memory_order_release);
}

OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
#if JucePlugin_Enable_ARA
    if (renderTerminalSubscription_ != 0)
    {
        if (auto* dc = getDocumentController())
            dc->unsubscribeRenderTerminalCallback(renderTerminalSubscription_);
        renderTerminalSubscription_ = 0;
    }
#endif
#if JucePlugin_Build_Standalone
    contentChangedCallback_ = {};
#endif
#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
    // Stop external requests before releasing processor state.
    controlService_->stop();
#endif
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
#if !JucePlugin_Build_Standalone
    stopTimer();
#endif

#if JucePlugin_Enable_ARA
    // Before noteAudition_ goes away: clear the routing atomics the audio callback reads.
    if (auto* route = auditionRoute_.load(std::memory_order_acquire))
    {
        auto* expected = noteAudition_.get();
        route->keyboardAudition.compare_exchange_strong(expected, nullptr,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_relaxed);
        // Release the mixer claim only when this instance owns it; clearing it
        // unconditionally would let another renderer claim and mix the same
        // NoteAudition while the owner's audio callback is still draining.
        if (auto* editorRenderer = getEditorRenderer())
        {
            auto* expectedMixer = editorRenderer;
            route->keyboardMixer.compare_exchange_strong(expectedMixer, nullptr,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed);
            releaseOwnedNotePitchMixer(route->notePitchMixer,
                                       route->notePitchAudition,
                                       editorRenderer);
        }
    }
    auditionRoute_.store(nullptr, std::memory_order_release);
    sharedPlayHeadStateForAudio_.store(nullptr, std::memory_order_release);
    if (renderTerminalSink_ != nullptr)
        renderTerminalSink_->callback = {};
    renderTerminalSink_.reset();
#endif

    // 析构最前段生命周期动作（此后再无异步任务访问裸 this）：
    // 1) 关闭 completion gate：与持锁进入的 F0 commit / chunkSettled / 模型切换
    //    completion 回调互斥。已进入者完成后才置 closed；此后进入者持锁见
    //    closed 即返回，不访问 owner。
    completionGate_->closeAndWait();

    // 2) 仅当运行时已初始化时才关闭 F0 / Reference owner 服务、解除 CRS execution
    //    lease；未初始化（scanner-only 生命周期）时不得构造进程 runtime 单例 —
    //    getInstance() 会启动 control worker。
    // F0 / Reference shutdown 关闭 owner、丢弃排队任务、终止可取消的 F0 Run，
    // 并 join 各自 worker；不可取消的 Reference 分析在此处完成收敛。
    f0ExtractionService_->shutdown();
    referenceAnalysisService_->shutdown();

    // Phase 2: 解除 CRS execution lease，取消 pending render jobs
    if (contentRenderService_) {
        contentRenderService_->detachExecutionLease(this);
    }
    ProcessRenderRuntime::getInstance().detachDeferredJobs(
        contentRenderService_.get(), completionGate_);

    // Phase 4: 内部清理
#if JucePlugin_Build_Standalone
    cancelPendingUpdate();
#endif

#if !JucePlugin_Build_Standalone
    // Processor 生命周期结束：清理尚未返回的外部编辑会话目录（消息线程，
    // 析构时 MessageManager 已在；Timer 已先停止，不再有 pollReturnedExternalEdit）。
    if (externalEditSessionDir_.isDirectory()) {
        externalEditSessionDir_.deleteRecursively();
        externalEditSessionDir_ = juce::File();
        externalEditContentKey_ = {};
    }
#endif

    ProcessRenderRuntime::getInstance().releaseOwner();

    // Vocoder / F0 / AppLogger 全部是进程级资源（ProcessRenderRuntime /
    // ProcessF0Runtime 单例与进程寿命 logger）：实例析构不 shutdown、不 reset、
    // 不等待推理，后续实例直接复用。
}

bool OpenTuneAudioProcessor::resetInferenceBackend(bool forceCpu,
                                                   ProcessingPreferenceRuntimeCallback completion)
{
    AppLogger::info("[Processor] Resetting inference backend, forceCpu=" 
        + juce::String(forceCpu ? "true" : "false"));
    
    // 耗时 reset（Session 销毁、按当前配置重建、AccelerationDetector
    // resetAndDetect）由 ProcessRenderRuntime 的 control worker 串行执行。
    auto gate = completionGate_;
    return ProcessRenderRuntime::getInstance().resetInferenceBackend(forceCpu, contentRenderService_,
        [completion = std::move(completion)](ProcessRenderRuntime::ControlResult result,
                                            juce::String errorMessage) mutable {
            if (result == ProcessRenderRuntime::ControlResult::Failed) {
                AppLogger::error("[Processor] inference backend control failed: " + errorMessage);
                ConfirmDialogContent::showDiagnostic(
                    nullptr,
                    "Vocoder initialization failed",
                    errorMessage,
                    AppLogger::makeDiagnosticText("Inference backend reset", errorMessage));
            } else {
                AppLogger::info("[Processor] Inference backend reset complete");
            }
            if (completion) {
                const auto status = result == ProcessRenderRuntime::ControlResult::Changed
                    ? ProcessingPreferenceRuntimeStatus::Changed
                    : result == ProcessRenderRuntime::ControlResult::Unchanged
                        ? ProcessingPreferenceRuntimeStatus::Unchanged
                        : ProcessingPreferenceRuntimeStatus::Failed;
                completion(status, errorMessage);
            }
        }, gate);
}

void OpenTuneAudioProcessor::handleSharedBackendChanged()
{
#if JucePlugin_Build_Standalone
    if (contentRenderService_ && standaloneContentRepository_) {
        const auto keys = standaloneContentRepository_->getAllClips();
        for (const auto key : keys) {
            if (auto cache = contentRenderService_->getRenderCache(key))
                cache->clear();
            if (auto* clip = standaloneContentRepository_->findClip(key)) {
                const auto snapshot = clip->snapshotContent();
                if (snapshot != nullptr
                    && snapshot->originalF0State == OriginalF0State::Extracting)
                    setContentOriginalF0State(key,
                        snapshot->hasUsableOriginalF0()
                            ? OriginalF0State::Ready
                            : OriginalF0State::NotRequested);
                requestFullContentRender(key);
            } else {
                requestFullContentRender(key);
            }
        }
        contentRenderService_->getTimeStretchCache().clear();
    }
#endif
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

bool OpenTuneAudioProcessor::setF0ModelType(F0ModelType type, juce::String* failureReason)
{
    auto& f0Runtime = ProcessF0Runtime::getInstance();
    std::string initializationError;
    if (!f0Runtime.isReady()
        && !f0Runtime.initialize(ModelPathResolver::getModelsDirectory(), type, &initializationError))
    {
        if (failureReason != nullptr)
            *failureReason = initializationError.empty()
                ? "F0 runtime initialization failed without an error detail"
                : juce::String(initializationError);
        return false;
    }

    auto f0Svc = ProcessF0Runtime::getInstance().getF0Service();
    if (!f0Svc) {
        if (failureReason != nullptr)
            *failureReason = "F0 runtime has no inference service after initialization";
        AppLogger::warn("[Processor] F0 service not ready, cannot switch model");
        return false;
    }
    std::string modelError;
    if (!f0Svc->setF0Model(type, &modelError)) {
        if (failureReason != nullptr)
            *failureReason = modelError.empty()
                ? "Selected F0 model could not be activated"
                : juce::String(modelError);
        AppLogger::error("[Processor] Failed to switch F0 model");
        return false;
    }
    return true;
}

ProcessingPreferenceRuntimeOperations
OpenTuneAudioProcessor::getProcessingPreferenceRuntimeOperations()
{
    ProcessingPreferenceRuntimeOperations operations;
    operations.setF0ModelType = [this](F0ModelType type, juce::String& failureReason) {
        return setF0ModelType(type, &failureReason);
    };
    operations.resetInferenceBackend = [this](bool forceCpu,
                                               ProcessingPreferenceRuntimeCallback completion) {
        return resetInferenceBackend(forceCpu, std::move(completion));
    };
    operations.invalidateAllContentCaches = [this] {
        invalidateAllContentCaches();
    };
    return operations;
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

    // Any in-flight note pitch audition is bound to the previous device rate; stop it.
    stopNotePitchAudition();

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

#if JucePlugin_Build_Standalone
    // Bind the arrangement's playback snapshot to the prepared CRS and device
    // rate. The snapshot freezes source views and fixed geometry per placement.
    if (standaloneArrangement_ != nullptr) {
        standaloneArrangement_->configurePlaybackSources(contentRenderService_.get(), sampleRate);
    }
#endif

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

#if !JucePlugin_Build_Standalone
    if (auto* captureSession = getCaptureSession())
        captureSession->prepareToPlay(sampleRate, samplesPerBlock, getMainBusNumInputChannels());
#endif
}

void OpenTuneAudioProcessor::releaseResources() {
    // 停止可能仍在进行的音符移调试听（设备即将释放）
    stopNotePitchAudition();

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

#if !JucePlugin_Build_Standalone
    if (auto* captureSession = getCaptureSession())
        captureSession->releaseResources();
#endif
}

void OpenTuneAudioProcessor::collectPlaybackSnapshots()
{
    JUCE_ASSERT_MESSAGE_THREAD
#if JucePlugin_Enable_ARA
    if (auto* dc = getDocumentController())
        dc->collectPlaybackSnapshots();
#endif
#if JucePlugin_Build_Standalone
    if (standaloneArrangement_ != nullptr)
        standaloneArrangement_->collectPlaybackSnapshots();
#endif
    if (contentRenderService_ != nullptr)
        contentRenderService_->collectPlaybackSnapshots();
}

#if !JucePlugin_Build_Standalone
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

bool OpenTuneAudioProcessor::externalEditLaunchRequested(ContentKey key, const EditableContentSnapshot& snapshot)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());

    // 已有活动会话：重新拉起独立编辑器窗口（不新建会话）。
    if (externalEditSessionActive()) {
        ExternalEditSession::launchStandalone(externalEditSessionDir_);
        return true;
    }

    if (!key.isValid() || snapshot.audioBuffer == nullptr
        || snapshot.audioBuffer->getNumSamples() <= 0)
        return false;

    const auto directory = ExternalEditSession::createSessionDirectory();
    if (!ExternalEditSession::writeSession(directory, key, snapshot)
        || !ExternalEditSession::launchStandalone(directory)) {
        directory.deleteRecursively();
        return false;
    }

    externalEditSessionDir_ = directory;
    externalEditContentKey_ = key;
    externalEditCorruptCurveShown_ = false;
    AppLogger::log("External edit launched " + directory.getFullPathName());
    return true;
}
#endif // !JucePlugin_Build_Standalone

#if JucePlugin_Enable_ARA
bool OpenTuneAudioProcessor::releaseOwnedNotePitchMixer(
    std::atomic<juce::ARAEditorRenderer*>& mixer,
    std::atomic<NotePitchAudition*>& auditionSource,
    juce::ARAEditorRenderer* owner) noexcept
{
    auto* expected = owner;
    if (!mixer.compare_exchange_strong(expected, nullptr,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed))
        return false;

    if (auto* audition = auditionSource.load(std::memory_order_acquire))
    {
        audition->setTargetRegion(nullptr);
        audition->stop();
        audition->clearActiveTargetRegion();
    }
    return true;
}

OpenTuneDocumentController* OpenTuneAudioProcessor::getDocumentController() const
{
    auto* controller = AudioProcessorARAExtension::getDocumentController();
    if (controller == nullptr)
    {
        return nullptr;
    }

    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(controller);
}

std::vector<juce::ARAPlaybackRegion*> OpenTuneAudioProcessor::getAssignedPlaybackRegions() const
{
    if (auto* renderer = getPlaybackRenderer())
        return renderer->getPlaybackRegions<juce::ARAPlaybackRegion>();
    return {};
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
#if !JucePlugin_Build_Standalone
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
    ProcessRenderRuntime::getInstance().detachDeferredJobs(
        contentRenderService_.get(), completionGate_);
#if !JucePlugin_Build_Standalone
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

        if (isEditorView())
            dc->setKeyboardAuditionSource(noteAudition_);

        auditionRoute_.store(dc->getAuditionRoute(), std::memory_order_release);
        sharedPlayHeadStateForAudio_.store(&dc->getSharedPlayHeadStateForAudio(),
                                           std::memory_order_release);
        if (isEditorView())
        {
            const std::weak_ptr<RenderTerminalSink> weakSink = renderTerminalSink_;
            renderTerminalSubscription_ = dc->subscribeRenderTerminalCallback(
                [weakSink](ContentKey key, bool completed) {
                    if (auto sink = weakSink.lock())
                    {
                        const auto callback = sink->callback;
                        if (callback)
                            callback(key, completed);
                    }
                });
        }

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

#if JucePlugin_Build_Standalone
    return out == juce::AudioChannelSet::stereo() && in.isDisabled();
#elif JucePlugin_Build_AU
    return (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::mono())
        || (in == juce::AudioChannelSet::stereo() && out == juce::AudioChannelSet::stereo());
#else
    // ARA 宿主只保证渲染实例的输出总线；输入端在 ARA 渲染实例上可为禁用。
    // 约束对齐 JUCE 官方 ARA 示例：只要求输出为立体声。
    return out == juce::AudioChannelSet::stereo()
        && (in == juce::AudioChannelSet::stereo() || in.isDisabled());
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

#if !JucePlugin_Build_Standalone
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
    // ARA2 playback renderers can run ahead of the actual playhead on track
    // processing threads. The realtime editor renderer is the current transport
    // source; only this role publishes to the shared document-level PlayHeadState.
    if (hostPosOpt.hasValue() && isBoundToARA() && isEditorRenderer())
    {
        if (auto* sharedState = sharedPlayHeadStateForAudio_.load(std::memory_order_acquire))
        {
            const auto updateResult = sharedState->update(hostPosOpt);
            if (numSamples > 0 && updateResult.timeChanged)
            {
                if (const auto timeSec = hostPosOpt->getTimeInSeconds())
                {
                    const double blockDuration = static_cast<double>(numSamples)
                        / currentSampleRate_.load(std::memory_order_relaxed);
                    const double nowClock = juce::Time::getMillisecondCounterHiRes() * 0.001;
                    if (sharedState->timeInSeconds.load(std::memory_order_relaxed) == *timeSec)
                        sharedState->presentationProjection.tryPublish(
                            *timeSec, nowClock, *timeSec + blockDuration, updateResult.epoch);
                }
            }
        }
    }
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
#endif // !JucePlugin_Build_Standalone

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
            mixAuditions(buffer, numSamples, static_cast<double>(getSampleRate()));
            outputSpectrumAnalyzer_.push(buffer);
            return;
        }
    }
#endif

#if !JucePlugin_Build_Standalone
    // --- Non-ARA host-plugin capture path: dry pass-through + capture session ---
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
        if (!isNonRealtime() && notePitchAudition_.isActive())
            buffer.clear();
        mixAuditions(buffer, numSamples, static_cast<double>(getSampleRate()));
        outputSpectrumAnalyzer_.push(buffer);
        return;
    }

    // ARA-bound instance whose ARA processor path declined this block: silence the
    // output and keep the post-ARA audition/spectrum publication.
    for (int i = 0; i < totalNumOutputChannels; ++i) {
        buffer.clear(i, 0, numSamples);
    }
    mixAuditions(buffer, numSamples, static_cast<double>(getSampleRate()));
    outputSpectrumAnalyzer_.push(buffer);
#elif JucePlugin_Build_Standalone
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
        mixAuditions(buffer, numSamples, deviceSampleRate);
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

    {
        const auto playbackSnapshot = standaloneArrangement_->loadPlaybackSnapshot();

        for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
            if (!playbackSnapshot) {
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
                if (!isNonRealtime()
                    && notePitchAudition_.isContentActive(placement.contentKey))
                    continue;

                // 快照在 prepare/来源/TimeGrid/编排变化时冻结来源视图与固定几何；
                // 音频线程只做整数半开区间求交与地址平移。
                if (!placement.hasPlayableView()
                    || placement.sampleGeometry.playbackRate != deviceSampleRate) {
                    continue;
                }

                const auto& geometry = placement.sampleGeometry;
                const auto overlap = PlaybackSampleGeometry::intersect(
                    PlaybackSampleGeometry::hostPlaybackSpan(geometry),
                    {blockStartSample, blockEndSample});
                if (overlap.empty()) {
                    continue;
                }

                const int64_t samplesToCopy64 = overlap.end - overlap.begin;
                const int64_t readStartSample = overlap.begin - geometry.offsetSamples;
                const double outputStartSeconds = placement.outputAnchorSeconds
                    + static_cast<double>(overlap.begin - geometry.playbackBeginSample) / deviceSampleRate;
                const int offsetInBlock = static_cast<int>(overlap.begin - blockStartSample);
                const int samplesToCopy = static_cast<int>(samplesToCopy64);

                const float placementGain = placement.gain * trackVolume;
                const double fadeInSeconds = placement.fadeInDuration;
                const double fadeOutSeconds = placement.fadeOutDuration;

                const ::OpenTune::PlaybackReadRequest readRequest(
                    placement.source, readStartSample, deviceSampleRate,
                    samplesToCopy, outputStartSeconds);

                clipReadScratch_.clear();
                const int availableReadSamples = readPlaybackAudio(readRequest, clipReadScratch_, 0);

                for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                    const float* src = clipReadScratch_.getReadPointer(ch);
                    float* dst = trackMixScratch_.getWritePointer(ch, offsetInBlock);

                    for (int s = 0; s < availableReadSamples; ++s) {
                        // 显式放置锚点 A 加相对样本遍历；每个声道独立求值。
                        const double fadeSeconds =
                            static_cast<double>(overlap.begin + s - geometry.playbackBeginSample)
                            / deviceSampleRate;
                        const float fade = placementFadeGain(fadeSeconds,
                                                             placement.durationSeconds,
                                                             fadeInSeconds,
                                                             fadeOutSeconds);
                        dst[s] += src[s] * placementGain * fade;
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

    mixAuditions(buffer, numSamples, deviceSampleRate);

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
#endif // host-plugin / Standalone audio path
}

#if !JucePlugin_Build_Standalone
OpenTuneAudioProcessor::HostTransportSnapshot OpenTuneAudioProcessor::getHostTransportSnapshot() const
{
    HostTransportSnapshot snapshot;
    snapshot.bpm = hostTransportBpm_.load(std::memory_order_relaxed);
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

    if (const auto timeSignature = positionInfo.getTimeSignature()) {
        snapshot.timeSignatureNumerator = timeSignature->numerator;
        snapshot.timeSignatureDenominator = timeSignature->denominator;
    }

    const double newBpm = snapshot.bpm;
    const int newNum = snapshot.timeSignatureNumerator;
    const int newDen = snapshot.timeSignatureDenominator;
    hostTransportBpm_.store(newBpm, std::memory_order_relaxed);
    hostTransportIsRecording_.store(snapshot.isRecording, std::memory_order_relaxed);
    hostTransportTimeSignatureNumerator_.store(newNum, std::memory_order_relaxed);
    hostTransportTimeSignatureDenominator_.store(newDen, std::memory_order_relaxed);

    // RT-SAFE change signalling ONLY: a release atomic generation bump when
    // metadata differs from the last pushed baseline. The audio thread must
    // never post system messages (sendChangeMessage/triggerAsyncUpdate take a
    // message-queue lock); the message thread pulls this counter in the editor
    // heartbeat and pushes UI updates only when the generation moves.
    if (newBpm != lastPushedHostBpm_
        || newNum != lastPushedHostTimeSigNum_
        || newDen != lastPushedHostTimeSigDen_)
    {
        lastPushedHostBpm_ = newBpm;
        lastPushedHostTimeSigNum_ = newNum;
        lastPushedHostTimeSigDen_ = newDen;
        // Release: guarantees the metadata stores above are visible to the
        // editor before it observes the new generation (acquire on its side).
        hostTransportMetadataGeneration_.fetch_add(1, std::memory_order_release);
    }
    return snapshot;
}
#endif // !JucePlugin_Build_Standalone

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
        g.setFont(UIColors::getUIFontExact(14.0f));
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
    // doStoreObjectsToStream/doRestoreObjectsFromStream, NOT via host-plugin processor state.
    // ARA host owns document archive lifecycle.
    // The existing non-Standalone outer-state codec carries UI settings and an
    // unbound plugin's capture tail. Standalone has its own persistence.
    Vst3ProcessorOuterState outerState;
    outerState.uiZoomPercent = getUiZoomPercent();
    outerState.trackHeight = trackHeight_;

    // Append unbound-plugin capture data at end of stream. ARA-bound instances keep
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
    // doRestoreObjectsFromStream, NOT via host-plugin processor state.
    // ARA host owns document archive lifecycle.
    // OTST v11 carries outer UI settings plus the unbound-plugin capture tail; the
    // Standalone arrangement is not embedded. Released v9/v10 arrangement
    // sections are skipped by the codec, never restored.

    // Host-plugin processor state (OTST): outer settings + optional capture tail. The
    // codec accepts current v11 and released v10/v9, skipping the legacy
    // arrangement layout without materialising it.
    Vst3ProcessorOuterState outerState;
    if (!Vst3ProcessorStateCodec::decode(data, sizeInBytes, outerState)) {
        AppLogger::error("StateRestore: unsupported processor state payload sizeBytes="
                         + juce::String(sizeInBytes));
        return false;
    }

    AppLogger::log("StateRestore: host-plugin processor state begin sizeBytes=" + juce::String(sizeInBytes)
                   + " version=" + juce::String(outerState.sourceVersion));

    // Capture tail policy: validate every CAPz version the codec decodes before
    // committing anything. Released OTST v9/v10 payloads may embed an older CAPz
    // archive, which is dropped per the explicit no-migration policy (settings
    // still restored).
    const bool hasCaptureTail = outerState.captureTail.getSize() > 0;
    bool discardCaptureTail = false;
    if (hasCaptureTail) {
        const int captureVersion = Capture::CapturePersistence::peekArchiveVersion(outerState.captureTail);
        if (captureVersion >= 12
            && captureVersion <= Capture::CapturePersistence::kArchiveVersion) {
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

    // Unbound-plugin Capture persistence is a separate owner transaction.
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
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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
    const auto selectionBefore = standaloneArrangement_->getSelectionState();

    const double requestedOffsetSeconds = splitSeconds - originalPlacement.timelineStartSeconds;
    constexpr double minDurationSeconds = 0.1;
    if (requestedOffsetSeconds <= minDurationSeconds
        || requestedOffsetSeconds >= originalPlacement.durationSeconds - minDurationSeconds) {
        AppLogger::log("Split rejected: split point out of valid placement range");
        return std::nullopt;
    }

    const auto originalSnapshot = getContentSnapshot(originalPlacement.contentKey);
    if (!originalSnapshot
        || originalSnapshot->audioBuffer == nullptr
        || originalSnapshot->nativeSource.buffer == nullptr) {
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

    const double canonicalRate = originalSnapshot->audioSampleRate;
    if (!std::isfinite(canonicalRate) || canonicalRate <= 0.0) {
        AppLogger::log("Split rejected: content canonical sample rate unavailable");
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

    const double splitOffsetSeconds = splitSourceSeconds - originalPlacement.clipInSeconds;
    if (splitOffsetSeconds <= minDurationSeconds
        || splitOffsetSeconds >= originalPlacement.durationSeconds - minDurationSeconds) {
        AppLogger::log("Split rejected: snapped split point out of valid placement range");
        return std::nullopt;
    }
    const double snappedSplitSeconds = originalPlacement.timelineStartSeconds + splitOffsetSeconds;

    // 两个子窗口共享唯一权威边界：父窗口起点 + F0 帧边界对应秒。
    const double boundarySourceSeconds =
        originalSnapshot->sourceWindow.sourceStartSeconds + splitSourceSeconds;
    const double parentDurationSeconds = originalSnapshot->sourceWindow.durationSeconds();

    // canonical 裁剪地址来自全局 source 网格，减去父 buffer 的全局起点。
    const int64_t boundaryCanonicalSample =
        PlaybackSampleGeometry::projectToSample(boundarySourceSeconds * canonicalRate);
    const int64_t leadingCanonicalSamples =
        boundaryCanonicalSample - originalSnapshot->firstCanonicalSample;
    const int64_t totalCanonicalSamples = originalSnapshot->audioBuffer->getNumSamples();
    if (leadingCanonicalSamples <= 0 || leadingCanonicalSamples >= totalCanonicalSamples) {
        return std::nullopt;
    }

    ContentState leadingPayload = contentStateFromSnapshot(*originalSnapshot);
    leadingPayload.sourceWindow = originalSnapshot->sourceWindow;
    leadingPayload.sourceWindow.sourceEndSeconds = boundarySourceSeconds;
    leadingPayload.audioBuffer = sliceAudioBuffer(originalSnapshot->audioBuffer, 0, leadingCanonicalSamples);
    leadingPayload.firstCanonicalSample = originalSnapshot->firstCanonicalSample;
    leadingPayload.timeGrid = TimeGridSnapshot::makeIdentity(leadingPayload.sourceWindow.durationSeconds());
    ++leadingPayload.timeGridRevision;
    leadingPayload.analysis.pitchCurve = slicePitchCurveToFrameRange(originalSnapshot->pitchCurve, 0, frameBoundary);
    leadingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes, 0.0, splitSourceSeconds);
    leadingPayload.volumeEnvelope = AutomationLane::sliceToLocalRange(
        originalSnapshot->volumeEnvelope, 0.0, splitSourceSeconds);
    leadingPayload.analysis.silentGaps = sliceSilentGaps(
        originalSnapshot->silentGaps, 0, leadingCanonicalSamples);
    // 派生特征绑定父 content/父绝对窗口，无可靠 slice：清空使其失效并可重新分析。
    leadingPayload.analysis.referenceFeatures.reset();

    ContentState trailingPayload = contentStateFromSnapshot(*originalSnapshot);
    trailingPayload.sourceWindow = originalSnapshot->sourceWindow;
    trailingPayload.sourceWindow.sourceStartSeconds = boundarySourceSeconds;
    trailingPayload.audioBuffer = sliceAudioBuffer(
        originalSnapshot->audioBuffer, leadingCanonicalSamples, totalCanonicalSamples);
    trailingPayload.firstCanonicalSample = boundaryCanonicalSample;
    trailingPayload.timeGrid = TimeGridSnapshot::makeIdentity(trailingPayload.sourceWindow.durationSeconds());
    ++trailingPayload.timeGridRevision;
    trailingPayload.analysis.pitchCurve = slicePitchCurveToFrameRange(
        originalSnapshot->pitchCurve, frameBoundary, f0FrameCount);
    trailingPayload.notes = sliceNotesToLocalRange(originalSnapshot->notes,
                                                   splitSourceSeconds,
                                                   parentDurationSeconds);
    trailingPayload.volumeEnvelope = AutomationLane::sliceToLocalRange(
        originalSnapshot->volumeEnvelope, splitSourceSeconds, parentDurationSeconds);
    trailingPayload.analysis.silentGaps = sliceSilentGaps(
        originalSnapshot->silentGaps, leadingCanonicalSamples, totalCanonicalSamples);
    trailingPayload.analysis.referenceFeatures.reset();

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
    if (!standaloneArrangement_->referencesContentActive(originalPlacement.contentKey))
        standaloneContentRepository_->retireClip(originalPlacement.contentKey);

    auto selectionAfter = selectionBefore;
    selectionAfter.placements.erase({trackId, originalPlacement.placementId});
    selectionAfter.activeTrackIdentity = standaloneArrangement_->getTrackIdentity(trackId);
    selectionAfter.placements.insert({trackId, trailingPlacement.placementId, selectionAfter.activeTrackIdentity});
    for (auto& selectedId : selectionAfter.selectedPlacementIds)
        if (selectedId == originalPlacement.placementId)
            selectedId = trailingPlacement.placementId;
    selectionAfter.activeTrackId = trackId;
    selectionAfter.activeTrackIdentity = standaloneArrangement_->getTrackIdentity(trackId);
    selectionAfter.selectedPlacementIds[static_cast<size_t>(trackId)] = trailingPlacement.placementId;
    selectionAfter.selectedTrackIdentities[static_cast<size_t>(trackId)] = selectionAfter.activeTrackIdentity;
    standaloneArrangement_->setSelectionState(selectionAfter);
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
    outcome.selectionBefore = selectionBefore;
    outcome.selectionAfter = selectionAfter;
    notifyPlacementChanged();
    return outcome;
}

std::optional<MergeOutcome> OpenTuneAudioProcessor::mergePlacements(int trackId,
                                                                    uint64_t leadingPlacementId,
                                                                    uint64_t trailingPlacementId,
                                                                    int targetPlacementIndex)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
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

    const auto leadingSnapshot = getContentSnapshot(leadingPlacement.contentKey);
    const auto trailingSnapshot = getContentSnapshot(trailingPlacement.contentKey);
    if (!leadingSnapshot || !trailingSnapshot) {
        AppLogger::log("Merge rejected: content snapshot unavailable");
        return std::nullopt;
    }

    const auto mergePlan = planPlacementMerge(trackId, leadingPlacement, *leadingSnapshot,
                                              trackId, trailingPlacement, *trailingSnapshot);
    if (mergePlan.issue != PlacementMergeIssue::None) {
        AppLogger::log("Merge rejected: shared placement merge plan refused the inputs");
        return std::nullopt;
    }

    const double leadingCanonicalRate = leadingSnapshot->audioSampleRate;
    const auto selectionBefore = standaloneArrangement_->getSelectionState();

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
    // notes/envelope 的合并位移使用 leading 权威窗口时长，不从派生 PCM 长度反推。
    const double leadingDurationSeconds = leadingSnapshot->sourceWindow.durationSeconds();
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
    // 合并窗口端点直接取 leading start / trailing end（权威秒值），
    // 并保留 leading 的 source identity 与 native 起点。
    mergedPayload.sourceWindow = leadingSnapshot->sourceWindow;
    mergedPayload.sourceWindow.sourceEndSeconds = trailingSnapshot->sourceWindow.sourceEndSeconds;
    mergedPayload.audioBuffer = mergedBuffer;
    mergedPayload.sampleRate = leadingCanonicalRate;
    mergedPayload.nativeSource = leadingSnapshot->nativeSource;
    mergedPayload.firstCanonicalSample = leadingSnapshot->firstCanonicalSample;
    mergedPayload.timeGrid = TimeGridSnapshot::makeIdentity(mergedPayload.sourceWindow.durationSeconds());
    ++mergedPayload.timeGridRevision;
    mergedPayload.analysis.pitchCurve = mergedPitchCurve;
    mergedPayload.analysis.setOriginalF0State(leadingSnapshot->originalF0State);
    mergedPayload.analysis.detectedKey = leadingSnapshot->detectedKey;
    mergedPayload.notes = std::move(mergedNotes);
    mergedPayload.noteTopologyInitialized = leadingSnapshot->noteTopologyInitialized
        || trailingSnapshot->noteTopologyInitialized;
    mergedPayload.analysis.silentGaps = mergeSilentGaps(leadingSnapshot->silentGaps, trailingSnapshot->silentGaps, leadingSamples);
    mergedPayload.pitchShiftSettings = leadingSnapshot->pitchShiftSettings;
    mergedPayload.volumeEnvelope = AutomationLane::mergeContiguous(
        leadingSnapshot->volumeEnvelope, trailingSnapshot->volumeEnvelope, leadingDurationSeconds);

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
    if (!standaloneArrangement_->referencesContentActive(leadingPlacement.contentKey))
        standaloneContentRepository_->retireClip(leadingPlacement.contentKey);
    if (!standaloneArrangement_->referencesContentActive(trailingPlacement.contentKey))
        standaloneContentRepository_->retireClip(trailingPlacement.contentKey);

    auto selectionAfter = selectionBefore;
    selectionAfter.placements.erase({trackId, leadingPlacementId});
    selectionAfter.placements.erase({trackId, trailingPlacementId});
    selectionAfter.activeTrackIdentity = standaloneArrangement_->getTrackIdentity(trackId);
    selectionAfter.placements.insert({trackId, mergedPlacement.placementId, selectionAfter.activeTrackIdentity});
    for (auto& selectedId : selectionAfter.selectedPlacementIds)
        if (selectedId == leadingPlacementId || selectedId == trailingPlacementId)
            selectedId = mergedPlacement.placementId;
    selectionAfter.activeTrackId = trackId;
    selectionAfter.activeTrackIdentity = standaloneArrangement_->getTrackIdentity(trackId);
    selectionAfter.selectedPlacementIds[static_cast<size_t>(trackId)] = mergedPlacement.placementId;
    selectionAfter.selectedTrackIdentities[static_cast<size_t>(trackId)] = selectionAfter.activeTrackIdentity;
    standaloneArrangement_->setSelectionState(selectionAfter);

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
    outcome.selectionBefore = selectionBefore;
    outcome.selectionAfter = selectionAfter;
    notifyPlacementChanged();
    return outcome;
}

std::optional<DeleteOutcome> OpenTuneAudioProcessor::deletePlacement(int trackId, int placementIndex)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    StandaloneArrangement::Placement placement;
    if (!standaloneArrangement_->getPlacementByIndex(trackId, placementIndex, placement)) {
        return std::nullopt;
    }

    uint64_t sourceId = 0;
    if (const auto snap = getContentSnapshot(placement.contentKey)) {
        sourceId = snap->sourceWindow.sourceId;
    }

    standaloneArrangement_->retirePlacement(trackId, placement.placementId);
    if (!standaloneArrangement_->referencesContentActive(placement.contentKey))
        standaloneContentRepository_->retireClip(placement.contentKey);
    scheduleReclaimSweep();

    DeleteOutcome outcome;
    outcome.trackId = trackId;
    outcome.sourceId = sourceId;
    outcome.placementId = placement.placementId;
    outcome.contentKey = placement.contentKey;
    notifyPlacementChanged();
    return outcome;
}

bool OpenTuneAudioProcessor::deleteTrack(int trackId, int visibleTrackCount)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto* arrangement = standaloneArrangement_.get();
    auto* repository = standaloneContentRepository_.get();
    if (arrangement == nullptr || repository == nullptr || visibleTrackCount <= 1
        || trackId < 0 || trackId >= visibleTrackCount || trackId >= arrangement->getNumTracks())
        return false;

    const auto trackIdentity = arrangement->getTrackIdentity(trackId);
    if (trackIdentity == 0)
        return false;

    std::vector<ContentKey> contents;
    const auto addContent = [&contents](ContentKey key) {
        if (key.isValid() && std::find(contents.begin(), contents.end(), key) == contents.end())
            contents.push_back(key);
    };

    for (int index = 0; index < arrangement->getNumPlacements(trackId); ++index) {
        StandaloneArrangement::Placement placement;
        if (!arrangement->getPlacementByIndex(trackId, index, placement))
            continue;
        addContent(placement.contentKey);
    }
    for (const auto& retired : arrangement->getRetiredPlacements()) {
        if (retired.trackIdentity != trackIdentity)
            continue;
        addContent(retired.contentKey);
    }

    if (!arrangement->removeTrackAndShift(trackId, visibleTrackCount))
        return false;

    for (const auto key : contents)
        if (!arrangement->referencesContentActive(key))
            repository->retireClip(key);

    settleStandaloneHistory();
    scheduleReclaimSweep();
    notifyPlacementChanged();
    return true;
}

void OpenTuneAudioProcessor::scheduleReclaimSweep()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    triggerAsyncUpdate();
}

void OpenTuneAudioProcessor::handleAsyncUpdate()
{
    collectPlaybackSnapshots();
    runReclaimSweepOnMessageThread();
}
#endif // JucePlugin_Build_Standalone

#if !JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::timerCallback()
{
    collectPlaybackSnapshots();
    if (auto* session = getCaptureSession())
        session->tick();

    pollReturnedExternalEdit();
}
#endif // !JucePlugin_Build_Standalone

#if !JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::pollReturnedExternalEdit()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (!externalEditSessionActive())
        return;

    const auto check = ExternalEditSession::checkReturned(externalEditSessionDir_);
    if (check.result == ExternalCurveCheckResult::Pending)
        return;

    if (check.result == ExternalCurveCheckResult::CorruptCurve) {
        // 一次性提示：返回的 curve.bin 存在却损坏。会话保持打开，外部修复后
        // 重试（返回检查会再次运行）。使用无父组件依赖的异步消息框，插件窗口
        // 关闭后提示仍然可见。
        if (!externalEditCorruptCurveShown_) {
            externalEditCorruptCurveShown_ = true;
            AppLogger::log("External edit: returned curve.bin is corrupt "
                           + externalEditSessionDir_.getFullPathName());
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle(LOC(kExternalEdit))
                    .withMessage(LOC(kExternalEditCurveCorrupt)),
                nullptr);
        }
        return;
    }

    // Ok：应用返回结果；成功后清理会话状态。
    const bool applied = applyReturnedExternalEdit(externalEditContentKey_, check.document);
    if (applied) {
        const auto completedPath = externalEditSessionDir_.getFullPathName();
        externalEditSessionDir_.deleteRecursively();
        externalEditSessionDir_ = juce::File();
        externalEditContentKey_ = {};
        externalEditCorruptCurveShown_ = false;
        ++externalEditAppliedCount_;
        AppLogger::log("External edit returned and applied "
                       + completedPath);
    }
}
#endif // !JucePlugin_Build_Standalone


#if JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::runReclaimSweepOnMessageThread()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    cancelPendingUpdate();
    settleStandaloneHistory();

    const auto retiredPlacements = standaloneArrangement_->getRetiredPlacements();
    for (const auto& entry : retiredPlacements) {
        if (undoManager_.retainsRetiredPlacement(entry.trackIdentity, entry.placementId))
            continue;
        standaloneArrangement_->deletePlacementById(entry.trackId, entry.placementId, nullptr, nullptr);
    }

    const auto retiredClips = standaloneContentRepository_->getRetiredClips();
    for (const auto& key : retiredClips) {
        if (standaloneArrangement_->referencesContentAnyState(key)
            || undoManager_.retainsRetiredContent(key)) {
            continue;
        }

        uint64_t sourceId = 0;
        if (const auto* retired = standaloneContentRepository_->getRetiredContent(key))
            sourceId = retired->content.sourceWindow.sourceId;

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
            && !standaloneContentRepository_->isSourceReferenced(sourceId)) {
            sourceStore_->retireSource(sourceId);
        }
    }

    if (sourceStore_ != nullptr) {
        const auto retiredSourceIds = sourceStore_->getRetiredSourceIds();
        for (const uint64_t sourceId : retiredSourceIds) {
            if (standaloneContentRepository_->isSourceReferenced(sourceId)) {
                continue;
            }
            sourceStore_->physicallyDeleteIfReclaimable(sourceId);
        }
    }
}

UndoReplayState OpenTuneAudioProcessor::captureStandaloneReplayState() const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    UndoReplayState state;
    const auto* arrangement = standaloneArrangement_.get();
    jassert(arrangement != nullptr);
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        const auto trackIdentity = arrangement->getTrackIdentity(trackId);
        state.trackIdentities.insert(trackIdentity);
        for (int index = 0; index < arrangement->getNumPlacements(trackId); ++index) {
            StandaloneArrangement::Placement placement;
            const bool found = arrangement->getPlacementByIndex(trackId, index, placement);
            jassertquiet(found);
            state.placements.emplace(placement.placementId,
                                     UndoReplayState::Placement{trackIdentity, true});
        }
    }
    for (const auto& placement : arrangement->getRetiredPlacements())
        state.placements.emplace(placement.placementId,
                                 UndoReplayState::Placement{placement.trackIdentity, false});
    return state;
}

void OpenTuneAudioProcessor::settleStandaloneHistory()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto* arrangement = standaloneArrangement_.get();
    auto* repository = standaloneContentRepository_.get();
    jassert(arrangement != nullptr && repository != nullptr);

    if (undoManager_.size() > 0) {
        const auto replayState = captureStandaloneReplayState();
        undoManager_.settleStructure(replayState);

        // Split, merge and delete retain every content they depend on. Control
        // entries wrap one such action or only in-place placement edits. The
        // content prune therefore removes only edits that neither retain
        // content nor change the replay state of either history direction.
        std::vector<ContentKey> releasableContents;
        for (const auto key : repository->getRetiredClips())
            if (!arrangement->referencesContentActive(key) && !undoManager_.retainsRetiredContent(key))
                releasableContents.push_back(key);
        [[maybe_unused]] const int removed = undoManager_.pruneActionsDependingOnContents(releasableContents);
#if JUCE_DEBUG
        if (removed > 0) {
            const int invalid = undoManager_.settleStructure(replayState);
            jassertquiet(invalid == 0);
        }
#endif
    }
    synchronizeStandaloneHistory();
}

void OpenTuneAudioProcessor::synchronizeStandaloneHistory()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
#if defined(OPENTUNE_ENABLE_CONTROL_API)
    if (controlService_ != nullptr && standaloneContentRepository_ != nullptr)
        controlService_->synchronizeHistory(undoManager_, standaloneContentRepository_->getContentEpoch());
#endif
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
#if !JucePlugin_Build_Standalone
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
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture:
#endif
            return contentRenderService_.get();
        default:
            break;
    }
    return nullptr;
}

void OpenTuneAudioProcessor::setRenderTerminalCallback(RenderTerminalCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (renderTerminalSink_ != nullptr)
        renderTerminalSink_->callback = std::move(callback);
}

void OpenTuneAudioProcessor::setPlacementChangedCallback(PlacementChangedCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    placementChangedCallback_ = std::move(callback);
}

void OpenTuneAudioProcessor::notifyPlacementChanged()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (placementChangedCallback_)
        placementChangedCallback_();
}

#if JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::setContentChangedCallback(ContentChangedCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    contentChangedCallback_ = std::move(callback);
}

void OpenTuneAudioProcessor::setContentEditCompletedCallback(ContentEditCompletedCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    contentEditCompletedCallback_ = std::move(callback);
}

void OpenTuneAudioProcessor::setExportReadinessChangedCallback(ExportReadinessChangedCallback callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    exportReadinessChangedCallback_ = std::move(callback);
    updateContentRenderStateChangedCallback();
}

void OpenTuneAudioProcessor::updateContentRenderStateChangedCallback()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (contentRenderService_ == nullptr)
        return;

    auto exportCallback = exportReadinessChangedCallback_;
#if defined(OPENTUNE_ENABLE_CONTROL_API)
    auto controlCallback = controlJobWakeCallback_;
    contentRenderService_->setStateChangedCallback(
        [exportCallback = std::move(exportCallback), controlCallback = std::move(controlCallback)] {
            if (exportCallback)
                exportCallback();
            if (controlCallback)
                controlCallback();
        });
#else
    contentRenderService_->setStateChangedCallback(std::move(exportCallback));
#endif
}
#endif

#if JucePlugin_Build_Standalone && defined(OPENTUNE_ENABLE_CONTROL_API)
void OpenTuneAudioProcessor::setControlJobWakeCallback(std::function<void()> callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    controlJobWakeCallback_ = std::move(callback);
    updateContentRenderStateChangedCallback();
}
#endif

void OpenTuneAudioProcessor::notifyContentChanged(ContentKey key)
{
#if JucePlugin_Build_Standalone
    JUCE_ASSERT_MESSAGE_THREAD
#endif
    collectPlaybackSnapshots();
#if JucePlugin_Build_Standalone
    if (contentChangedCallback_)
        contentChangedCallback_(key);
    if (exportReadinessChangedCallback_)
        exportReadinessChangedCallback_();
#if defined(OPENTUNE_ENABLE_CONTROL_API)
    controlService_->wakeJobs();
#endif
#else
    juce::ignoreUnused(key);
#endif
}

void OpenTuneAudioProcessor::notifyRenderTerminal(ContentKey key, bool completed, uint64_t renderGeneration)
{
    JUCE_ASSERT_MESSAGE_THREAD
    collectPlaybackSnapshots();
    if (renderTerminalSink_ == nullptr || !renderTerminalSink_->callback)
        return;
    if (completed)
    {
        const auto it = lastCompletedRenderGeneration_.find(key);
        if (it != lastCompletedRenderGeneration_.end()
            && it->second == renderGeneration)
            return;
        lastCompletedRenderGeneration_[key] = renderGeneration;
    }
    renderTerminalSink_->callback(key, completed);
}

void OpenTuneAudioProcessor::mixDocumentAuditions(juce::AudioBuffer<float>& buffer,
                                                  int numSamples,
                                                  double sampleRate) noexcept
{
#if JucePlugin_Enable_ARA
    if (isNonRealtime())
        return;

    auto* route = auditionRoute_.load(std::memory_order_acquire);
    auto* editorRenderer = getEditorRenderer();
    if (route == nullptr || editorRenderer == nullptr)
        return;

    if (auto* audition = route->notePitchAudition.load(std::memory_order_acquire))
        if (route->notePitchMixer.load(std::memory_order_acquire) == editorRenderer)
            audition->mixIntoBuffer(buffer, numSamples, sampleRate);

    if (auto* audition = route->keyboardAudition.load(std::memory_order_acquire))
    {
        // One mixer per document: a second one would advance the clip twice per block.
        juce::ARAEditorRenderer* expected = nullptr;
        route->keyboardMixer.compare_exchange_strong(expected, editorRenderer,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed);
        if (route->keyboardMixer.load(std::memory_order_acquire) == editorRenderer)
            audition->mixIntoBuffer(buffer, numSamples, sampleRate);
    }
#else
    juce::ignoreUnused(buffer, numSamples, sampleRate);
#endif
}

void OpenTuneAudioProcessor::mixAuditions(juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
{
    pianoKeyAudition_.mixIntoBuffer(buffer, numSamples, sampleRate);
#if JucePlugin_Enable_ARA
    if (isBoundToARA())
    {
        mixDocumentAuditions(buffer, numSamples, sampleRate);
        return;
    }
#endif
    noteAudition_->mixIntoBuffer(buffer, numSamples, sampleRate);
    // 音符移调预览仅在实时处理时混入；离线（宿主 bounce/export）渲染静音。
    if (!isNonRealtime())
        notePitchAudition_.mixIntoBuffer(buffer, numSamples, sampleRate);
}

void OpenTuneAudioProcessor::auditionNotePitch(ContentKey key, double startSeconds, double endSeconds, float targetHz)
{
    // UI 线程入口：不投递 render job，不触碰 RenderCache，只读取已发布的 playback read source。
    if (!appPreferences_->getNoteTransposePreviewEnabled() || !key.isValid() || targetHz <= 0.0f) {
        stopNotePitchAudition();
        return;
    }

    const auto* crs = resolveReadableContentRenderService(key);
    if (crs == nullptr) {
        stopNotePitchAudition();
        return;
    }

    PlaybackReadSource source;
    if (!crs->getPlaybackReadSource(key, source) || source.canonicalBuffer == nullptr) {
        stopNotePitchAudition();
        return;
    }

#if JucePlugin_Enable_ARA
    if (isBoundToARA())
    {
        auto* dc = getDocumentController();
        if (dc == nullptr
            || !dc->selectNotePitchAuditionRendererForRegion(notePitchAuditionTargetRegion_))
        {
            stopNotePitchAudition();
            return;
        }

        auto* route = auditionRoute_.load(std::memory_order_acquire);
        auto* audition = route != nullptr
            ? route->notePitchAudition.load(std::memory_order_acquire) : nullptr;
        if (audition == nullptr)
        {
            stopNotePitchAudition();
            return;
        }

        audition->request(std::move(source), startSeconds, endSeconds, targetHz,
                          static_cast<double>(getSampleRate()));
        return;
    }
#endif
    notePitchAudition_.request(std::move(source), startSeconds, endSeconds, targetHz,
                               static_cast<double>(getSampleRate()));
}

void OpenTuneAudioProcessor::stopNotePitchAudition()
{
#if JucePlugin_Enable_ARA
    if (isBoundToARA())
    {
        auto* route = auditionRoute_.load(std::memory_order_acquire);
        if (route == nullptr)
            return;
        if (auto* audition = route->notePitchAudition.load(std::memory_order_acquire))
        {
            audition->setTargetRegion(nullptr);
            audition->stop();
        }
        return;
    }
#endif
    notePitchAudition_.stop();
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
#if !JucePlugin_Build_Standalone
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

void OpenTuneAudioProcessor::onContentLocalMutationCompleted(
    ContentKey key,
    const std::vector<ContentEditRangeFrames>& affectedRanges)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve || affectedRanges.empty()) return;
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    std::vector<NoteRangeSeconds> secondsRanges;
    secondsRanges.reserve(affectedRanges.size());
    for (const auto& range : affectedRanges)
        secondsRanges.push_back({static_cast<double>(range.startFrame) * secondsPerFrame,
                                 static_cast<double>(range.endFrameExclusive) * secondsPerFrame});
    onContentLocalMutationCompletedSeconds(key, secondsRanges, std::move(snap));
}

void OpenTuneAudioProcessor::onContentLocalMutationCompletedSeconds(
    ContentKey key,
    const std::vector<NoteRangeSeconds>& affectedRanges,
    std::shared_ptr<const EditableContentSnapshot> snapshot)
{
#if JucePlugin_Enable_ARA
    if (key.domainKind == DomainKind::ARAAudioModification)
    {
        if (auto* dc = getDocumentController())
            dc->requestModificationRenderRanges(key, affectedRanges, std::move(snapshot));
        return;
    }
#endif

    requestRenderForLocalMutationRanges(key, affectedRanges, std::move(snapshot));
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
        requestRenderForLocalMutationRanges(
            key, std::vector<NoteRangeSeconds>{{0.0, durationSeconds}}, std::move(snapshot));
}

void OpenTuneAudioProcessor::requestFullContentRender(ContentKey key)
{
    auto snap = getContentSnapshot(key);
    if (!snap) return;
    const double durationSeconds = contentDurationSeconds(*snap);
    if (durationSeconds <= 0.0) return;
    requestRenderForLocalMutationRanges(
        key, std::vector<NoteRangeSeconds>{{0.0, durationSeconds}}, std::move(snap));
}

void OpenTuneAudioProcessor::requestRenderForLocalMutationRanges(
    ContentKey key,
    const std::vector<NoteRangeSeconds>& affectedRanges,
    std::shared_ptr<const EditableContentSnapshot> snap)
{
    if (!snap) return;

    auto* crs = resolveMutableLocalContentRenderService(key);
    if (crs == nullptr) return;

    PlaybackReadSource readSource;
    if (!crs->getPlaybackReadSource(key, readSource)) return;

    const double crsSampleRate = readSource.canonicalSampleRate;
    auto audioBuffer = readSource.canonicalBuffer;
    if (crsSampleRate <= 0.0 || audioBuffer == nullptr || audioBuffer->getNumSamples() <= 0) return;

    collectPlaybackSnapshots();
    if (!crs->republishPlaybackSource(key, snap))
        return;

#if JucePlugin_Build_Standalone
    // 内容发布后刷新编排播放快照的冻结来源视图与固定几何。
    if (standaloneArrangement_ != nullptr)
        standaloneArrangement_->refreshPlaybackSnapshot();
#endif

#if !JucePlugin_Build_Standalone
    if (key.domainKind == DomainKind::RegularVST3Capture) {
        if (auto* session = getCaptureSession())
            session->refreshPublishedReplacementGeometry();
    }
#endif

    if (!snap->hasUsableOriginalF0()) return;

    const int64_t totalSamples = audioBuffer->getNumSamples();
    std::vector<PlaybackSampleGeometry::SampleSpan> requestRanges;
    requestRanges.reserve(affectedRanges.size());
    for (const auto& range : affectedRanges)
    {
        const int64_t startSample = juce::jlimit<int64_t>(
            0, totalSamples, TimeCoordinate::secondsToSamplesFloor(range.startSeconds, crsSampleRate));
        const int64_t endSample = juce::jlimit<int64_t>(
            0, totalSamples, TimeCoordinate::secondsToSamplesCeil(range.endSeconds, crsSampleRate));
        if (endSample > startSample)
            requestRanges.push_back({startSample, endSample});
    }
    if (requestRanges.empty()) return;

    RenderJob job;
    job.contentKey = key;
    job.renderCache = crs->getOrCreateRenderCache(key);
    job.audioBuffer = audioBuffer;
    job.audioSampleRate = crsSampleRate;
    job.contentSnapshot = snap;
    job.contentRevision = snap->contentRevision;

    crs->enqueueRender(std::move(job), requestRanges);
}

void OpenTuneAudioProcessor::enqueueStage2WhenCanonicalSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> snapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (key.domainKind == DomainKind::ARAAudioModification)
        return;

    if (snapshot == nullptr
        || audioBuffer == nullptr
        || snapshot->timeGrid == nullptr
        || snapshot->timeGrid->empty()
        || snapshot->timeGrid->isIdentity())
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
    const auto cache = contentRenderService_ != nullptr
        ? contentRenderService_->getRenderCache(key) : nullptr;
    const auto state = cache != nullptr
        ? cache->getStateSnapshot() : RenderCache::StateSnapshot{};
    if (state.canonicalSettled)
        notifyRenderTerminal(key, true, state.renderGeneration);

#if !JucePlugin_Build_Standalone
    // Only transition to Edited when ALL chunks are canonical settled.
    // Partial completion must not promote the segment prematurely.
    if (auto* session = getCaptureSession()) {
        if (state.canonicalSettled)
            session->onRenderComplete(key);
    }
#endif

#if !JucePlugin_Build_Standalone
    if (contentRenderService_ != nullptr
        && key.domainKind != DomainKind::ARAAudioModification)
    {
        enqueueStage2WhenCanonicalSettled(
            key,
            std::move(snapshot),
            std::move(audioBuffer),
            audioSampleRate);
    }
#endif
}

#if JucePlugin_Build_Standalone
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

    // 存储布局与声明完全一致（1 = mono，2 = stereo）。
    ChannelLayoutLog::logEntry(entrySourceTag, declaredChannels, declaredChannels, displayName);

    // 权威来源时长只由原始媒体帧数/率取得一次；之后所有派生 buffer 长度只作存储边界。
    const double sourceDurationSeconds =
        static_cast<double>(inBuffer.getNumSamples()) / inSampleRate;
    auto nativeBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(inBuffer));

    out.displayName = displayName;
    out.sourceFilePath = sourceFilePath;
    out.sourceWindow = SourceWindow{0, juce::String(), 0.0, sourceDurationSeconds};
    out.nativeSource = NativePcmSource{nativeBuffer, inSampleRate, 0, 0};

    // canonical 从 native 直接派生到 44.1 kHz 全局网格；实际率完全相同时共享存储。
    const double canonicalRate = TimeCoordinate::kRenderSampleRate;
    if (inSampleRate == canonicalRate) {
        out.canonicalAudioBuffer = nativeBuffer;
        out.canonicalSampleRate = canonicalRate;
        out.canonicalFirstSample = 0;
    } else {
        ResamplingManager resampler;
        const int numChannels = nativeBuffer->getNumChannels();
        const int canonicalLength = static_cast<int>(juce::jmax<int64_t>(
            1,
            PlaybackSampleGeometry::projectToSample(sourceDurationSeconds * canonicalRate)));
        auto canonicalBuffer = std::make_shared<juce::AudioBuffer<float>>(numChannels, canonicalLength);
        for (int ch = 0; ch < numChannels; ++ch) {
            const auto resampled = resampler.resampleRange(
                nativeBuffer->getReadPointer(ch),
                static_cast<size_t>(nativeBuffer->getNumSamples()),
                inSampleRate,
                canonicalRate,
                0,
                0,
                canonicalLength);
            canonicalBuffer->copyFrom(ch, 0, resampled.data(), canonicalLength);
        }
        out.canonicalAudioBuffer = std::move(canonicalBuffer);
        out.canonicalSampleRate = canonicalRate;
        out.canonicalFirstSample = 0;
    }

    // 静默段使用 canonical 离散访问坐标。
    out.silentGaps = SilentGapDetector::detectAllGapsAdaptive(*out.canonicalAudioBuffer);

    return true;
}

ContentKey OpenTuneAudioProcessor::ensureSourceAndCreateStandaloneClip(PreparedImport&& prepared, uint64_t& sourceId)
{
    jassert(sourceStore_ != nullptr);

    // SourceStore 始终保存原率原始 PCM 与媒体实际采样率。
    SourceStore::CreateSourceRequest sourceRequest;
    sourceRequest.displayName = prepared.displayName;
    sourceRequest.sourceFilePath = prepared.sourceFilePath;
    sourceRequest.audioBuffer = prepared.nativeSource.buffer;
    sourceRequest.sampleRate = prepared.nativeSource.sampleRate;
    sourceId = sourceStore_->createSource(std::move(sourceRequest));
    if (sourceId == 0) return {};

    SourceWindow sw = prepared.sourceWindow;
    sw.sourceId = sourceId;
    sw.sourcePersistentId.clear();

    ContentState payload;
    payload.sourceWindow = sw;
    payload.nativeSource = prepared.nativeSource;
    payload.audioBuffer = prepared.canonicalAudioBuffer;
    payload.sampleRate = prepared.canonicalSampleRate;
    payload.firstCanonicalSample = prepared.canonicalFirstSample;
    payload.timeGrid = TimeGridSnapshot::makeIdentity(sw.durationSeconds());
    ++payload.timeGridRevision;
    payload.analysis.setOriginalF0State(OriginalF0State::NotRequested);
    payload.analysis.silentGaps = std::move(prepared.silentGaps);

    const ContentKey key = createStandaloneClipOwner(*standaloneContentRepository_,
                                                     *contentRenderService_,
                                                     std::move(payload));
    if (!key.isValid()) {
        sourceStore_->deleteSource(sourceId);
        sourceId = 0;
    }
    return key;
}

OpenTuneAudioProcessor::CommittedPlacement OpenTuneAudioProcessor::commitPreparedImportAsPlacement(
    PreparedImport&& prepared, const ImportPlacement& placement, bool selectPlacement)
{
    if (!placement.isValid()) return {};
    jassert(standaloneArrangement_ != nullptr);

    const juce::String displayName = prepared.displayName;
    uint64_t sourceId = 0;
    const ContentKey clipKey = ensureSourceAndCreateStandaloneClip(std::move(prepared), sourceId);
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
        sourceStore_->deleteSource(sourceId);
        return {};
    }
    if (selectPlacement)
        standaloneArrangement_->selectPlacement(placement.trackId, importedPlacement.placementId);

    return { sourceId, clipKey, importedPlacement.placementId };
}
#endif // JucePlugin_Build_Standalone

// ============================================================================
// requestContentRefresh -- Standalone / unbound-plugin F0 refresh.
// Normally refreshes F0 only.
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
    if (snap == nullptr) {
        setContentOriginalF0State(request.contentKey, OriginalF0State::Failed);
        setContentOriginalF0FailureReason(request.contentKey,
            "F0 extraction could not read the content snapshot");
        return false;
    }
    if (snap->audioBuffer == nullptr) {
        setContentOriginalF0State(request.contentKey, OriginalF0State::Failed);
        setContentOriginalF0FailureReason(request.contentKey,
            "F0 extraction content snapshot has no audio buffer");
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
            if (standaloneContentRepository_->findClip(request.contentKey) != nullptr)
                setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting);
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification:
            // Handled by DC path in setContentOriginalF0State
            break;
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture:
            setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting);
            break;
#endif
        default:
            break;
    }

    snap = getContentSnapshot(request.contentKey);
    if (snap == nullptr || snap->audioBuffer == nullptr) {
        setContentOriginalF0State(request.contentKey, OriginalF0State::Failed);
        setContentOriginalF0FailureReason(request.contentKey,
            "F0 extraction could not read the current content snapshot after request admission");
        return false;
    }

    if (f0ExtractionService_->isActive(F0RequestKey{request.contentKey})) {
        f0ExtractionService_->cancel(F0RequestKey{request.contentKey});
        f0RequestTokens_.erase(request.contentKey);
    }

    auto gate = completionGate_;
    OpenTuneAudioProcessor* const processor = this;
    const auto capturedRequest = request;
    const uint64_t capturedContentRevision = snap->contentRevision;
    const auto capturedAudioBuffer = snap->audioBuffer;
    auto requestToken = std::make_shared<uint64_t>(0);
    // 提交时捕获不可变内容快照：worker 只执行纯数据 F0 计算，owner 销毁后
    // 绝不访问裸 processor（快照 shared_ptr 与进程级 F0 服务随 worker 存活）。
    const auto capturedSnap = snap;

    const auto submitResult = f0ExtractionService_->submit(
        F0RequestKey{request.contentKey},
        [capturedSnap, capturedRequest](
            const std::shared_ptr<F0RunOwnerState>& runOwnerState) -> F0ExtractionService::Result {
            F0ExtractionService::Result result;
            result.contentKey = capturedRequest.contentKey;

            if (!capturedSnap || capturedSnap->audioBuffer == nullptr) {
                result.errorMessage = !capturedSnap
                    ? "F0 extraction could not read the content snapshot"
                    : "F0 extraction content snapshot has no audio buffer";
                return result;
            }

            result.sourceAudioBuffer = capturedSnap->audioBuffer;

            // 进程级 F0 服务（进程寿命，owner 销毁后仍安全）；惰性初始化保留原语义。
            auto& f0Runtime = ProcessF0Runtime::getInstance();
            std::string initializationError;
            if (!f0Runtime.isReady())
                f0Runtime.initialize(ModelPathResolver::getModelsDirectory(), &initializationError);
            auto f0Service = f0Runtime.getF0Service();
            if (!f0Service) {
                result.errorMessage = initializationError.empty()
                    ? "F0 runtime has no inference service after initialization"
                    : initializationError;
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
        [processor, gate, capturedRequest, capturedContentRevision,
         capturedAudioBuffer, requestToken]
        (F0ExtractionService::Result&& result) {
            // Both the normal commit and the dispatcher-failure mailbox invoke
            // this callback on the message thread.  Do not add a second
            // dispatcher hop after the mailbox has already recovered the
            // failed post.
            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
            if (!callbackLease)
                return;

            const auto tokenIt = processor->f0RequestTokens_.find(capturedRequest.contentKey);
            if (result.requestKey != F0RequestKey{capturedRequest.contentKey}
                || result.requestToken != *requestToken
                || tokenIt == processor->f0RequestTokens_.end()
                || tokenIt->second != result.requestToken)
                return;

            auto currentSnap = processor->getContentSnapshot(capturedRequest.contentKey);
            if (!currentSnap
                || currentSnap->contentRevision != capturedContentRevision
                || currentSnap->audioBuffer == nullptr
                || currentSnap->audioBuffer != capturedAudioBuffer
                || (result.sourceAudioBuffer != nullptr
                    && result.sourceAudioBuffer != capturedAudioBuffer)) {
                processor->f0RequestTokens_.erase(tokenIt);
                AppLogger::log("ContentRefresh: stale result dropped contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                return;
            }

            if (!result.success) {
                processor->f0RequestTokens_.erase(capturedRequest.contentKey);
                AppLogger::log("ContentRefresh: extraction failed contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId))
                    + " reason=" + juce::String(result.errorMessage));
                processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Failed);
                processor->setContentOriginalF0FailureReason(
                    capturedRequest.contentKey,
                    result.errorMessage.empty()
                        ? "F0 extraction failed without an error detail"
                        : result.errorMessage);
                return;
            }

            auto pitchCurve = std::make_shared<PitchCurve>();
            pitchCurve->setHopSize(result.hopSize);
            pitchCurve->setSampleRate(static_cast<double>(result.f0SampleRate));
            pitchCurve->setOriginalF0(result.f0);
            if (!result.energy.empty()) {
                pitchCurve->setOriginalEnergy(result.energy);
            }

            // Re-extraction replaces the curve object, but must not turn the
            // user's erased frames back into ordinary F0 gaps.
            if (currentSnap->pitchCurve != nullptr
                && !currentSnap->pitchCurve->getErasedRanges().empty()) {
                pitchCurve->replaceErasedRangesInRange(
                    0, static_cast<int>(result.f0.size()),
                    currentSnap->pitchCurve->getErasedRanges());
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
                            pitchCurve->retainVoicedCorrectionSegments();
                        }
                    }
                }
            }

            if (!processor->writeOriginalF0ToOwner(capturedRequest.contentKey, pitchCurve)) {
                processor->f0RequestTokens_.erase(capturedRequest.contentKey);
                AppLogger::log("ContentRefresh: OriginalF0 commit failed contentKey objectId="
                    + juce::String(static_cast<juce::int64>(capturedRequest.contentKey.objectId)));
                processor->setContentOriginalF0State(capturedRequest.contentKey, OriginalF0State::Failed);
                processor->setContentOriginalF0FailureReason(capturedRequest.contentKey,
                    "F0 result could not be committed to the content owner");
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

            processor->f0RequestTokens_.erase(capturedRequest.contentKey);
            if (processor->pendingTimeToolSeedKeys_.count(capturedRequest.contentKey) != 0)
                processor->ensureTimeToolAnchorSeed(capturedRequest.contentKey);

            // F0 就绪后请求完整渲染：此前因无 F0 而 Blank 的 chunk 需重新渲染
            processor->requestFullContentRender(capturedRequest.contentKey);
        },
        [processor, gate, capturedRequest, capturedContentRevision, requestToken]
        (F0ExtractionService::Result&& result) {
            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
            if (!callbackLease)
                return;
            const auto tokenIt = processor->f0RequestTokens_.find(capturedRequest.contentKey);
            if (result.requestKey != F0RequestKey{capturedRequest.contentKey}
                || result.requestToken != *requestToken
                || tokenIt == processor->f0RequestTokens_.end()
                || tokenIt->second != result.requestToken)
                return;
            const auto currentSnap = processor->getContentSnapshot(capturedRequest.contentKey);
            if (!currentSnap || currentSnap->contentRevision != capturedContentRevision) {
                processor->f0RequestTokens_.erase(tokenIt);
                return;
            }
            processor->f0RequestTokens_.erase(tokenIt);
            processor->setContentOriginalF0State(
                capturedRequest.contentKey, OriginalF0State::Failed);
            processor->setContentOriginalF0FailureReason(
                capturedRequest.contentKey,
                result.errorMessage.empty()
                    ? "F0 completion dispatcher rejected the result without an error detail"
                    : result.errorMessage);
        }, requestToken.get());

    if (submitResult != F0ExtractionService::SubmitResult::Accepted) {
        AppLogger::log("ContentRefresh: submit rejected contentKey objectId="
            + juce::String(static_cast<juce::int64>(request.contentKey.objectId)));
        if (submitResult == F0ExtractionService::SubmitResult::AlreadyInProgress)
            return true;
        if (submitResult == F0ExtractionService::SubmitResult::ShuttingDown)
            return false;
        f0RequestTokens_.erase(request.contentKey);
        setContentOriginalF0State(request.contentKey, OriginalF0State::Failed);
        setContentOriginalF0FailureReason(request.contentKey,
            submitResult == F0ExtractionService::SubmitResult::QueueFull
                ? "F0 extraction queue is full"
                : "F0 extraction submission was rejected: invalid task");
        return false;
    }

    f0RequestTokens_[request.contentKey] = *requestToken;

    return true;
}

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

    constexpr auto desiredProducer = ReferenceFeatureProducer::StandardAuto;
    auto& features = snap->referenceFeatures;
    const bool featuresReady = features.isReady()
        && features.producer == desiredProducer
        && features.inputFingerprint == static_cast<int64_t>(snap->audioRevision)
        && features.hasTimingAnchors();

    if (!featuresReady) {
        pendingTimeToolSeedKeys_.insert(key);
        const auto preheatStatus = preheatReferenceFeatures(key);
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

    if (handles.size() == 1)
        return false;
    pushHandle(durationSeconds,
               durationSeconds,
               HandleKind::ClipEnd);

    auto seededGrid = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (seededGrid == nullptr) {
        return false;
    }

    const auto& beforeHandles = existingGrid->handles();
    const auto& afterHandles = seededGrid->handles();
    const bool unchanged = beforeHandles.size() == afterHandles.size()
        && std::equal(beforeHandles.begin(), beforeHandles.end(), afterHandles.begin(),
                      [](const TimeHandle& before, const TimeHandle& after) {
                          return before.id == after.id
                              && before.source_seconds == after.source_seconds
                              && before.output_seconds == after.output_seconds
                              && before.kind == after.kind
                              && before.confidence == after.confidence;
                      });
    if (unchanged)
        return true;
    if (!setContentTimeGrid(key, std::move(seededGrid)))
        return false;
    undoManager_.clearRedo();
    notifyContentEditCompleted(key);
    return true;
}

OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus
OpenTuneAudioProcessor::preheatReferenceFeatures(ContentKey key)
{
    if (!key.isValid() || key.domainKind != DomainKind::StandaloneClip) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    auto snap = getContentSnapshot(key);
    if (!snap) {
        return ReferenceAnalysisPreheatStatus::InvalidContent;
    }

    constexpr auto producer = ReferenceFeatureProducer::StandardAuto;
    const auto inputFingerprint = static_cast<int64_t>(snap->audioRevision);
    auto& features = snap->referenceFeatures;
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

    auto gate = completionGate_;
    auto* processor = this;
    auto completion = [processor, gate](const ReferenceAnalysisService::AnalysisJobKey& jobKey,
                                         const ReferenceFeatureSet& result) {
        // ReferenceAnalysisService posts this completion to the message thread.
        ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
        if (!callbackLease)
            return;
        processor->analysisFinished(jobKey.contentKey, result);
    };

    auto analysis = [analysisSnap, analysisRevision](const ReferenceAnalysisService::AnalysisJobKey& jobKey) {
            ReferenceFeatureSet failed;
            failed.producer = jobKey.producer;
            failed.inputFingerprint = jobKey.inputFingerprint;
            failed.analysisRevision = analysisRevision;

            // StandardAuto：提交时捕获的纯数据快照。
            if (!analysisSnap) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "Reference analysis could not read content snapshot";
                return failed;
            }
            if (static_cast<int64_t>(analysisSnap->audioRevision) != jobKey.inputFingerprint) {
                failed.status = ReferenceFeatureStatus::Failed;
                failed.errorMessage = "Reference analysis job is stale";
                return failed;
            }
            return buildStandardAutoReferenceFeatureSet(*analysisSnap, analysisRevision);
        };

    referenceAnalysisService_->submitAnalysis(key, inputFingerprint, producer,
                                              std::move(analysis), std::move(completion));
    return ReferenceAnalysisPreheatStatus::Queued;
}

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
#if !JucePlugin_Build_Standalone
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

    notifyContentChanged(key);

    // 发布由调用方统一负责：Scissors 提交事务（applyScissorsNotes）内
    // republishPlaybackSource；requestContentRefresh 经 onContentFullMutationCompleted
    // → render 请求内 requestRenderForLocalMutationRanges 的 republishPlaybackSource
    // 重建包络。此处不再发布，避免同一变更两次 publish（不含包络的中间态双发布）。
    return true;
}

bool OpenTuneAudioProcessor::applyReturnedExternalEdit(ContentKey key, const ExternalEditDocument& document)
{
    if (!key.isValid())
        return false;

#if JucePlugin_Build_Standalone
    if (key.domainKind == DomainKind::StandaloneClip) {
        commitVolumeEnvelope(key, document.envelope);

        if (document.pitchCurve != nullptr) {
            if (!writePitchCurveToOwner(key, PitchCurve::fromSnapshot(document.pitchCurve->getSnapshot())))
                return false;
            if (document.pitchCurve->hasUsableOriginalF0())
                setContentOriginalF0State(key, OriginalF0State::Ready);

            PitchShiftEditState shift;
            shift.settings = document.pitchShift;
            shift.notes = document.notes;
            shift.segments = document.pitchCurve->copyCorrectionSegments();
            return applyContentPitchShiftState(key, shift);
        }

        if (!replaceContentNotesForFullMutation(key, document.notes))
            return false;
        onContentFullMutationCompleted(key);
        return true;
    }
#endif

    // Plugin external edits are Capture-Session-only: apply notes + curve +
    // pitch-shift state + envelope as a single owner commit, then republish once.
#if !JucePlugin_Build_Standalone
    if (key.domainKind == DomainKind::RegularVST3Capture) {
        auto* session = getCaptureSession();
        if (session == nullptr)
            return false;
        auto curve = document.pitchCurve != nullptr
            ? PitchCurve::fromSnapshot(document.pitchCurve->getSnapshot())
            : nullptr;
        if (!session->applyExternalEdit(key,
                                        document.notes,
                                        std::move(curve),
                                        document.envelope,
                                        document.pitchShift))
            return false;
        onContentFullMutationCompleted(key);
        return true;
    }
#endif

    return false;
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotesAndSegments(ContentKey key,
                                                             std::vector<Note> notesInRange,
                                                             std::vector<PitchCorrectionSegment> segments,
                                                             ContentEditRangeFrames affectedRange)
{
    return commitContentNotesAndSegmentsInternal(
        key, std::move(notesInRange), std::move(segments),
        std::vector<ContentEditRangeFrames>{affectedRange}, {}, std::nullopt);
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotesAndSegmentsInRanges(
    ContentKey key,
    std::vector<Note> notes,
    std::vector<PitchCorrectionSegment> segments,
    const std::vector<ContentEditRangeFrames>& affectedRanges)
{
    return commitContentNotesAndSegmentsInternal(
        key, std::move(notes), std::move(segments), affectedRanges, {}, std::nullopt);
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotesAndSegmentsWithOriginalF0(
    ContentKey key,
    std::vector<Note> notesInRange,
    std::vector<PitchCorrectionSegment> segments,
    ContentEditRangeFrames affectedRange,
    std::vector<float> originalF0InRange,
    std::optional<std::vector<F0FrameRange>> erasedRanges)
{
    return commitContentNotesAndSegmentsInternal(
        key, std::move(notesInRange), std::move(segments),
        std::vector<ContentEditRangeFrames>{affectedRange},
        std::move(originalF0InRange), std::move(erasedRanges));
}

SelectedNoteParameterEditResult OpenTuneAudioProcessor::editSelectedNoteParameter(
    ContentKey key,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    AudioEditingScheme::ParameterId parameterId,
    float value,
    float retuneSpeedDefault,
    float vibratoDepthDefault,
    float vibratoRateDefault,
    juce::String undoDescription)
{
    SelectedNoteParameterEditResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread()) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || snapshot->pitchCurve == nullptr || contentCommands_ == nullptr) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }
    const auto patch = planSelectedNoteParameterEdit(
        *contentCommands_, key, *snapshot, selectedNoteIndices, expectedNotesRevision, parameterId, value,
        retuneSpeedDefault, vibratoDepthDefault, vibratoRateDefault);
    if (!patch) {
        if (snapshot->notesRevision != expectedNotesRevision) {
            result.status = AudioEditingScheme::ParameterEditStatus::Failed;
            result.contentSnapshot = snapshot;
            return result;
        }
        const bool hasSelectedNote = std::any_of(
            selectedNoteIndices.begin(), selectedNoteIndices.end(),
            [&snapshot](int index) {
                return index >= 0 && index < static_cast<int>(snapshot->notes.size());
            });
        result.status = hasSelectedNote
            ? AudioEditingScheme::ParameterEditStatus::Failed
            : AudioEditingScheme::ParameterEditStatus::NoTarget;
        return result;
    }

    result.status = AudioEditingScheme::ParameterEditStatus::Applied;
    result.changed = patch->changed;
    result.affectedRange = patch->affectedRange;
    if (!patch->changed || patch->affectedRange.isEmpty())
        return result;

    const ContentEditRangeFrames affectedRange{
        patch->affectedRange.startFrame,
        patch->affectedRange.endFrameExclusive
    };
    const auto edit = contentCommands_->commitNotesAndSegmentsUndoable(
        patch->contentKey, patch->afterNotes, patch->afterSegments, affectedRange, undoDescription);
    if (edit.snapshot == nullptr || edit.undoAction == nullptr) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }
    result.contentSnapshot = edit.snapshot;
    return result;
}

OpenTuneAudioProcessor::SelectedNoteEqEditResult OpenTuneAudioProcessor::editSelectedNoteEq(
    ContentKey key,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    const std::optional<EqSettings>& settings,
    juce::String undoDescription)
{
    SelectedNoteEqEditResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    auto plan = planSelectedNoteEqEdit(
        key, *snapshot, selectedNoteIndices, expectedNotesRevision, settings);
    result.status = plan.status;
    if (plan.status == SelectedNoteEqEditStatus::StaleRevision) {
        result.contentSnapshot = snapshot;
        return result;
    }
    if (plan.status != SelectedNoteEqEditStatus::Ready)
        return result;

    result.contentSnapshot = contentCommands_->commitNoteTopologyPatch(key, plan.afterPatch);
    if (result.contentSnapshot == nullptr) {
        result.status = SelectedNoteEqEditStatus::Failed;
        return result;
    }

    undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
        contentCommands_, key, std::move(undoDescription),
        std::move(plan.beforePatch), std::move(plan.afterPatch)));
    result.status = SelectedNoteEqEditStatus::Applied;
    return result;
}

SelectedNoteDeleteResult OpenTuneAudioProcessor::deleteSelectedPianoRollNotes(
    ContentKey key,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    SelectedNoteDeleteResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    auto plan = planSelectedNoteDelete(*snapshot, selectedNoteIndices, expectedNotesRevision);
    result.status = plan.status;
    if (plan.status == SelectedNoteDeleteStatus::StaleRevision) {
        result.contentSnapshot = snapshot;
        return result;
    }
    if (plan.status != SelectedNoteDeleteStatus::Ready)
        return result;

    result.beforeNotes = snapshot->notes;
    if (auto* edit = std::get_if<SelectedNoteTopologyDelete>(&plan.edit)) {
        result.contentSnapshot = contentCommands_->commitNoteTopologyPatch(key, edit->afterPatch);
        if (result.contentSnapshot == nullptr) {
            result.status = SelectedNoteDeleteStatus::Failed;
            return result;
        }
        undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
            contentCommands_, key, std::move(undoDescription),
            std::move(edit->beforePatch), std::move(edit->afterPatch)));
    } else {
        auto& curveEdit = std::get<SelectedNotePitchCurveDelete>(plan.edit);
        result.affectedRange = curveEdit.affectedRange;
        result.pitchCurveEdited = true;
        const ContentEditRangeFrames affectedRange{
            curveEdit.affectedRange.startFrame,
            curveEdit.affectedRange.endFrameExclusive
        };
        result.contentSnapshot = contentCommands_->commitNotesAndSegments(
            key, curveEdit.afterNotes, curveEdit.afterSegments, affectedRange);
        if (result.contentSnapshot == nullptr) {
            result.status = SelectedNoteDeleteStatus::Failed;
            return result;
        }
        undoManager_.addAction(std::make_unique<PianoRollEditAction>(
            contentCommands_, key, std::move(undoDescription),
            std::move(curveEdit.beforeNotes), std::move(curveEdit.afterNotes),
            std::move(curveEdit.beforeSegments), std::move(curveEdit.afterSegments),
            std::vector<ContentEditRangeFrames>{affectedRange}));
    }

    result.status = SelectedNoteDeleteStatus::Applied;
    return result;
}

OpenTuneAudioProcessor::PasteNotesResult OpenTuneAudioProcessor::pasteNotesAt(
    ContentKey key,
    const std::vector<Note>& clipboardNotes,
    double pasteStartTime,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    PasteNotesResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    auto plan = planPasteNotesEdit(*snapshot, clipboardNotes, pasteStartTime, expectedNotesRevision);
    result.status = plan.status;
    if (plan.status == PasteNotesStatus::StaleRevision
        || plan.status == PasteNotesStatus::EmptyClipboard) {
        result.contentSnapshot = snapshot;
        return result;
    }
    if (plan.status != PasteNotesStatus::Ready)
        return result;

    result.pastedNotes = plan.pastedNotes;
    result.contentSnapshot = contentCommands_->commitNoteTopologyPatch(key, plan.afterPatch);
    if (result.contentSnapshot == nullptr) {
        result.status = PasteNotesStatus::Failed;
        return result;
    }

    undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
        contentCommands_, key, std::move(undoDescription),
        std::move(plan.beforePatch), std::move(plan.afterPatch)));
    result.status = PasteNotesStatus::Applied;
    return result;
}

OpenTuneAudioProcessor::NoteDraftCommitResult OpenTuneAudioProcessor::commitNoteDraft(
    ContentKey key,
    uint64_t expectedNotesRevision,
    const std::vector<Note>& workingNotes,
    juce::String undoDescription)
{
    NoteDraftCommitResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;
    result.contentSnapshot = snapshot;
    if (snapshot->notesRevision != expectedNotesRevision) {
        result.status = NoteDraftCommitStatus::StaleRevision;
        return result;
    }

    auto plan = planNoteDraftEdit(snapshot->notes, workingNotes);
    if (plan.status == NoteDraftEditStatus::NoChange) {
        result.status = NoteDraftCommitStatus::NoChange;
        return result;
    }

    const auto committed = contentCommands_->commitNoteTopologyPatch(key, plan.afterPatch);
    if (committed == nullptr)
        return result;

    undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
        contentCommands_, key, std::move(undoDescription),
        std::move(plan.beforePatch), std::move(plan.afterPatch)));
    result.contentSnapshot = committed;
    result.status = NoteDraftCommitStatus::Applied;
    return result;
}

OpenTuneAudioProcessor::NoteDraftCommitResult OpenTuneAudioProcessor::commitNoteBasedCorrectionDraft(
    ContentKey key,
    uint64_t expectedNotesRevision,
    const std::vector<Note>& workingNotes,
    F0FrameRange editRange,
    NoteGeneratorParams params,
    juce::String undoDescription)
{
    NoteDraftCommitResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;
    result.contentSnapshot = snapshot;
    if (snapshot->notesRevision != expectedNotesRevision) {
        result.status = NoteDraftCommitStatus::StaleRevision;
        return result;
    }

    const auto draftPlan = planNoteDraftEdit(snapshot->notes, workingNotes);
    if (draftPlan.status == NoteDraftEditStatus::NoChange) {
        result.status = NoteDraftCommitStatus::NoChange;
        return result;
    }

    const auto correction = contentCommands_->planNoteCorrection(
        *snapshot, workingNotes, editRange, params);
    if (!correction)
        return result;

    const auto edit = contentCommands_->commitNotesAndSegmentsUndoable(
        key, correction->notes, correction->segments,
        ContentEditRangeFrames{correction->affectedRange.startFrame,
                               correction->affectedRange.endFrameExclusive},
        undoDescription);
    if (edit.snapshot == nullptr || edit.undoAction == nullptr)
        return result;

    result.contentSnapshot = edit.snapshot;
    result.status = NoteDraftCommitStatus::Applied;
    return result;
}

DrawNoteCommitResult OpenTuneAudioProcessor::drawNote(
    ContentKey key,
    const DrawNoteEditInput& input,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    DrawNoteCommitResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    auto plan = planDrawNoteEdit(*snapshot, input, expectedNotesRevision);
    if (plan.status == DrawNotePlanStatus::StaleRevision) {
        result.status = DrawNoteCommitStatus::StaleRevision;
        result.contentSnapshot = snapshot;
        return result;
    }
    if (plan.status == DrawNotePlanStatus::NoChange) {
        result.status = DrawNoteCommitStatus::NoChange;
        result.contentSnapshot = snapshot;
        return result;
    }

    result.insertedNote = plan.insertedNote;
    result.pitchCurveEdited = plan.pitchCurveEdited;
    result.affectedRange = plan.affectedRange;
    if (!plan.pitchCurveEdited) {
        result.contentSnapshot = contentCommands_->commitNoteTopologyPatch(key, plan.afterPatch);
        if (result.contentSnapshot == nullptr) {
            result.contentSnapshot = snapshot;
            return result;
        }

        undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
            contentCommands_, key, std::move(undoDescription),
            std::move(plan.beforePatch), std::move(plan.afterPatch)));
    } else {
        result.contentSnapshot = contentCommands_->commitNotesAndSegments(
            key,
            plan.afterNotes,
            plan.segmentsToCommit,
            ContentEditRangeFrames{ plan.affectedRange.startFrame,
                                    plan.affectedRange.endFrameExclusive });
        if (result.contentSnapshot == nullptr) {
            result.contentSnapshot = snapshot;
            return result;
        }

        const auto& curve = snapshot->pitchCurve;
        const F0Timeline timeline(curve->getHopSize(), curve->getSampleRate(),
                                  static_cast<int>(curve->size()));
        const double rangeStartSeconds = timeline.timeAtFrame(plan.affectedRange.startFrame);
        const double rangeEndSeconds = timeline.timeAtFrame(plan.affectedRange.endFrameExclusive);
        auto afterNotes = filterNotesToRange(
            result.contentSnapshot->notes, rangeStartSeconds, rangeEndSeconds);
        auto afterSegments = clipSegmentsToFrameRange(
            result.contentSnapshot->pitchCurve->getCorrectionSegments(),
            plan.affectedRange.startFrame,
            plan.affectedRange.endFrameExclusive);

        undoManager_.addAction(std::make_unique<PianoRollEditAction>(
            contentCommands_, key, std::move(undoDescription),
            std::move(plan.beforeNotesInPitchRange), std::move(afterNotes),
            std::move(plan.beforeSegments), std::move(afterSegments),
            std::vector<ContentEditRangeFrames>{{plan.affectedRange.startFrame,
                                                 plan.affectedRange.endFrameExclusive}}));
    }

    result.status = DrawNoteCommitStatus::Applied;
    return result;
}

OpenTuneAudioProcessor::NoteResizeCommitResult OpenTuneAudioProcessor::resizePianoRollNote(
    ContentKey key,
    int noteIndex,
    NoteResizeEdge edge,
    double clampedTime,
    double minDuration,
    uint64_t expectedNotesRevision,
    const std::vector<PitchCorrectionSegment>& segments,
    F0FrameRange affectedRange,
    juce::String undoDescription)
{
    NoteResizeCommitResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;
    result.contentSnapshot = snapshot;
    if (snapshot->notesRevision != expectedNotesRevision) {
        result.status = NoteResizeCommitStatus::StaleRevision;
        return result;
    }

    const auto plan = planNoteResizeEdit(
        snapshot->notes, noteIndex, edge, clampedTime, minDuration);
    if (affectedRange.isEmpty()) {
        ContentNoteRangePatch beforePatch;
        ContentNoteRangePatch afterPatch;
        beforePatch.affectedRange = { plan.dirtyStartTime, plan.dirtyEndTime };
        afterPatch.affectedRange = beforePatch.affectedRange;
        beforePatch.afterNotesInRange = filterNotesToRange(
            snapshot->notes, plan.dirtyStartTime, plan.dirtyEndTime);
        afterPatch.afterNotesInRange = filterNotesToRange(
            plan.afterNotes, plan.dirtyStartTime, plan.dirtyEndTime);

        result.contentSnapshot = contentCommands_->commitNoteTopologyPatch(key, afterPatch);
        if (result.contentSnapshot == nullptr) {
            result.contentSnapshot = snapshot;
            return result;
        }
        undoManager_.addAction(std::make_unique<PianoRollNotePatchAction>(
            contentCommands_, key, std::move(undoDescription),
            std::move(beforePatch), std::move(afterPatch)));
        notifyContentEditCompleted(key);
        result.status = NoteResizeCommitStatus::Applied;
        return result;
    }

    if (snapshot->pitchCurve == nullptr)
        return result;

    const auto& curve = snapshot->pitchCurve;
    const F0Timeline timeline(curve->getHopSize(), curve->getSampleRate(),
                              static_cast<int>(curve->size()));
    const double rangeStartSeconds = timeline.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSeconds = timeline.timeAtFrame(affectedRange.endFrameExclusive);
    const ContentEditRangeFrames ownerRange{
        affectedRange.startFrame,
        affectedRange.endFrameExclusive
    };
    const auto edit = contentCommands_->commitNotesAndSegmentsUndoable(
        key,
        filterNotesToRange(plan.afterNotes, rangeStartSeconds, rangeEndSeconds),
        clipSegmentsToFrameRange(segments, affectedRange.startFrame, affectedRange.endFrameExclusive),
        ownerRange,
        undoDescription);
    if (edit.snapshot == nullptr || edit.undoAction == nullptr) {
        result.contentSnapshot = snapshot;
        return result;
    }

    result.contentSnapshot = edit.snapshot;
    result.status = NoteResizeCommitStatus::Applied;
    result.affectedRange = affectedRange;
    result.pitchCurveEdited = true;
    return result;
}

ScissorsCommitResult OpenTuneAudioProcessor::applyScissorsNotes(
    ContentKey key,
    const std::vector<Note>& afterNotes,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    ScissorsCommitResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    // Planner 已在 UI 侧按同一 snapshot 的 revision 计算计划；stale check 只对
    // 提交前的 snapshot revision，外部事务变化则拒绝，避免错内容提交。
    if (snapshot->notesRevision != expectedNotesRevision) {
        result.status = ScissorsCommitStatus::StaleRevision;
        result.contentSnapshot = snapshot;
        return result;
    }

    if (!replaceContentNotesForFullMutation(key, afterNotes))
        return result;

    republishPlaybackSource(key);

    // 提交成功后不与 expectedNotesRevision 比较（notesRevision 已推进）；
    // postcommit snapshot 作为事务结果，其 notes（normalized data）作为 undo after。
    const auto afterSnapshot = getContentSnapshot(key);
    if (afterSnapshot == nullptr)
        return result;

    result.contentSnapshot = afterSnapshot;
    undoManager_.addAction(std::make_unique<ScissorsUndoAction>(
        contentCommands_, key, std::move(undoDescription),
        snapshot->notes, afterSnapshot->notes));
    result.status = ScissorsCommitStatus::Applied;
    return result;
}

EraserStrokeEditResult OpenTuneAudioProcessor::editEraserStroke(
    ContentKey key,
    F0FrameRange brushRange,
    const std::vector<int>& noteIndices,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    EraserStrokeEditResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    const F0Timeline timeline = snapshot->pitchCurve != nullptr
        ? F0Timeline{ snapshot->pitchCurve->getHopSize(),
                      snapshot->pitchCurve->getSampleRate(),
                      static_cast<int>(snapshot->pitchCurve->size()) }
        : F0Timeline{};
    auto plan = planEraserStrokeEdit(
        *snapshot, timeline, brushRange, noteIndices, expectedNotesRevision);
    result.status = plan.status;
    if (plan.status == EraserStrokeEditStatus::StaleRevision) {
        result.contentSnapshot = snapshot;
        return result;
    }
    if (plan.status != EraserStrokeEditStatus::Ready)
        return result;

    result.beforeNotes = std::move(plan.beforeNotes);
    result.affectedRange = plan.affectedRange;
    result.contentSnapshot = contentCommands_->commitNotesAndSegmentsWithOriginalF0(
        key,
        plan.afterNotes,
        plan.afterSegments,
        ContentEditRangeFrames{ plan.affectedRange.startFrame,
                                plan.affectedRange.endFrameExclusive },
        plan.afterOriginalF0,
        plan.afterErasedRanges);
    if (result.contentSnapshot == nullptr) {
        result.status = EraserStrokeEditStatus::Failed;
        return result;
    }

    const auto& committedCurve = result.contentSnapshot->pitchCurve;
    std::vector<float> afterOriginalF0(
        committedCurve->getOriginalF0().begin() + plan.affectedRange.startFrame,
        committedCurve->getOriginalF0().begin() + plan.affectedRange.endFrameExclusive);
    std::vector<F0FrameRange> afterErasedRanges;
    for (const auto& range : committedCurve->getErasedRanges()) {
        const int overlapStart = std::max(range.startFrame, plan.affectedRange.startFrame);
        const int overlapEnd = std::min(range.endFrameExclusive, plan.affectedRange.endFrameExclusive);
        if (overlapStart < overlapEnd)
            afterErasedRanges.push_back({ overlapStart, overlapEnd });
    }

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_, key, std::move(undoDescription),
        result.beforeNotes, result.contentSnapshot->notes,
        plan.beforeSegments, plan.afterSegments,
        std::vector<ContentEditRangeFrames>{{plan.affectedRange.startFrame,
                                             plan.affectedRange.endFrameExclusive}},
        plan.beforeOriginalF0, std::move(afterOriginalF0),
        plan.beforeErasedRanges, std::move(afterErasedRanges));
    action->setErasedRangePatch();
    undoManager_.addAction(std::move(action));
    result.status = EraserStrokeEditStatus::Applied;
    return result;
}

ManualCorrectionEditResult OpenTuneAudioProcessor::applyManualCorrection(
    ContentKey key,
    const std::vector<ManualCorrectionOp>& ops,
    int dirtyStartFrame,
    int dirtyEndFrame,
    juce::String undoDescription)
{
    ManualCorrectionEditResult result;
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || snapshot->pitchCurve == nullptr
        || contentCommands_ == nullptr || ops.empty() || dirtyEndFrame < dirtyStartFrame)
        return result;

    auto scratchCurve = PitchCurve::fromSnapshot(snapshot->pitchCurve);
    for (const auto& op : ops) {
        if (op.endFrameExclusive <= op.startFrame)
            continue;
        scratchCurve->setManualCorrectionRange(
            op.startFrame,
            op.endFrameExclusive,
            op.f0Data,
            op.source,
            op.parameterSnapshot);
    }

    const ContentEditRangeFrames affectedRange{ dirtyStartFrame, dirtyEndFrame + 1 };
    const auto curveSnapshot = scratchCurve->getSnapshot();
    const F0Timeline timeline{ curveSnapshot->getHopSize(), curveSnapshot->getSampleRate(),
                               static_cast<int>(curveSnapshot->size()) };
    const double rangeStartSeconds = timeline.isEmpty()
        ? 0.0
        : timeline.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSeconds = timeline.isEmpty()
        ? 0.0
        : timeline.timeAtFrame(affectedRange.endFrameExclusive);

    auto scopedNotes = filterNotesToRange(snapshot->notes, rangeStartSeconds, rangeEndSeconds);
    auto beforeNotes = scopedNotes;
    auto beforeSegments = clipSegmentsToFrameRange(
        snapshot->pitchCurve->getCorrectionSegments(),
        affectedRange.startFrame,
        affectedRange.endFrameExclusive);
    auto committedSnapshot = contentCommands_->commitNotesAndSegments(
        key,
        std::move(scopedNotes),
        clipSegmentsToFrameRange(curveSnapshot->getCorrectionSegments(),
                                 affectedRange.startFrame,
                                 affectedRange.endFrameExclusive),
        affectedRange);
    if (committedSnapshot == nullptr)
        return result;

    auto afterNotes = filterNotesToRange(
        committedSnapshot->notes, rangeStartSeconds, rangeEndSeconds);
    auto afterSegments = clipSegmentsToFrameRange(
        committedSnapshot->pitchCurve->getCorrectionSegments(),
        affectedRange.startFrame,
        affectedRange.endFrameExclusive);
    undoManager_.addAction(std::make_unique<PianoRollEditAction>(
        contentCommands_, key, std::move(undoDescription),
        std::move(beforeNotes), std::move(afterNotes),
        std::move(beforeSegments), std::move(afterSegments),
        std::vector<ContentEditRangeFrames>{affectedRange}));

    result.status = ManualCorrectionEditStatus::Applied;
    result.contentSnapshot = std::move(committedSnapshot);
    return result;
}

ContentCommitSnapshot OpenTuneAudioProcessor::commitContentNotesAndSegmentsInternal(
    ContentKey key,
    std::vector<Note> notesInRange,
    std::vector<PitchCorrectionSegment> segments,
    std::vector<ContentEditRangeFrames> affectedRanges,
    std::vector<float> originalF0InRange,
    std::optional<std::vector<F0FrameRange>> erasedRanges)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve || affectedRanges.empty()) return {};

    // Remove and replace notes overlapping any affected range, preserving notes
    // in the gaps between disjoint ranges.
    const double secondsPerFrame = static_cast<double>(snap->pitchCurve->getHopSize())
                                 / snap->pitchCurve->getSampleRate();
    std::vector<NoteRangeSeconds> secondsRanges;
    secondsRanges.reserve(affectedRanges.size());
    for (const auto& range : affectedRanges)
        secondsRanges.push_back({static_cast<double>(range.startFrame) * secondsPerFrame,
                                 static_cast<double>(range.endFrameExclusive) * secondsPerFrame});
    const auto overlapsAffectedRanges = [&secondsRanges](const Note& note) {
        return std::any_of(secondsRanges.begin(), secondsRanges.end(), [&note](const NoteRangeSeconds& range) {
            return note.endTime > range.startSeconds && note.startTime < range.endSeconds;
        });
    };

    std::vector<Note> mergedNotes;
    mergedNotes.reserve(snap->notes.size() + notesInRange.size());
    for (const auto& note : snap->notes)
        if (!overlapsAffectedRanges(note))
            mergedNotes.push_back(note);
    for (const auto& note : notesInRange)
        if (overlapsAffectedRanges(note))
            mergedNotes.push_back(note);
    auto normalizedNotes = normalizeStoredNotes(std::move(mergedNotes));

    auto mergedSegments = snap->pitchCurve->getCorrectionSegments();
    for (const auto& range : affectedRanges) {
        mergedSegments = replaceSegmentsInFrameRange(
            mergedSegments,
            clipSegmentsToFrameRange(segments, range.startFrame, range.endFrameExclusive),
            range.startFrame,
            range.endFrameExclusive);
    }

    auto newCurve = PitchCurve::fromSnapshot(snap->pitchCurve);
    if (!originalF0InRange.empty())
        newCurve->setOriginalF0Range(static_cast<size_t>(affectedRanges.front().startFrame), originalF0InRange);
    if (erasedRanges)
        newCurve->replaceErasedRangesInRange(
            affectedRanges.front().startFrame, affectedRanges.front().endFrameExclusive, *erasedRanges);
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
            if (!dc->applyNotesAndPitchCurveToModification(
                    key, std::move(normalizedNotes), std::move(newCurve)))
                return {};
            ok = true;
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
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
        notifyContentChanged(key);
        auto committedSnap = getContentSnapshot(key);
        jassert(committedSnap != nullptr);
        onContentLocalMutationCompletedSeconds(key, secondsRanges, committedSnap);
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
#if !JucePlugin_Build_Standalone
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
        notifyContentChanged(key);
        // 根因修复：拓扑 patch 提交后按 patch.affectedRange（秒域）调度局部重渲染，
        // Undo/Redo 经同一 command（PianoRollNotePatchAction）自动重渲染。
        // onContentLocalMutationCompletedSeconds 内部刷新/发布 playback source 并调度 Stage1。
        auto committedSnap = getContentSnapshot(key);
        onContentLocalMutationCompletedSeconds(
            key,
            std::vector<NoteRangeSeconds>{patch.affectedRange},
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
#if !JucePlugin_Build_Standalone
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

    notifyContentChanged(key);
    republishPlaybackSource(key);
    return getContentSnapshot(key);
}

VolumeEnvelopeRegionEditResult OpenTuneAudioProcessor::commitVolumeEnvelopeRegionPlan(
    ContentKey key,
    ContentCommitSnapshot snapshot,
    VolumeEnvelopeRegionEditPlan plan,
    juce::String undoDescription)
{
    VolumeEnvelopeRegionEditResult result;
    if (plan.status != VolumeEnvelopeRegionEditStatus::Ready) {
        result.status = plan.status;
        if (plan.status == VolumeEnvelopeRegionEditStatus::StaleRevision)
            result.contentSnapshot = std::move(snapshot);
        return result;
    }

    auto committedSnap = commitVolumeEnvelope(key, plan.afterEnvelope);
    if (committedSnap == nullptr) {
        result.status = VolumeEnvelopeRegionEditStatus::Failed;
        return result;
    }

    undoManager_.addAction(std::make_unique<VolumeEnvelopeEditAction>(
        contentCommands_, key, std::move(undoDescription),
        std::move(plan.beforeEnvelope), std::move(plan.afterEnvelope)));
    result.status = VolumeEnvelopeRegionEditStatus::Applied;
    result.contentSnapshot = std::move(committedSnap);
    return result;
}

VolumeEnvelopeRegionEditResult OpenTuneAudioProcessor::editVolumeEnvelopeRegions(
    ContentKey key,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    float deltaGainDb,
    juce::String undoDescription)
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    VolumeEnvelopeRegionEditResult result;
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    return commitVolumeEnvelopeRegionPlan(
        key, snapshot,
        planVolumeEnvelopeRegionDeltaEdit(*snapshot, selectedNoteIndices,
                                          expectedNotesRevision, deltaGainDb),
        std::move(undoDescription));
}

VolumeEnvelopeRegionEditResult OpenTuneAudioProcessor::resetVolumeEnvelopeRegion(
    ContentKey key,
    int noteIndex,
    uint64_t expectedNotesRevision,
    juce::String undoDescription)
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    VolumeEnvelopeRegionEditResult result;
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return result;

    const auto snapshot = getContentSnapshot(key);
    if (snapshot == nullptr || contentCommands_ == nullptr)
        return result;

    return commitVolumeEnvelopeRegionPlan(
        key, snapshot,
        planVolumeEnvelopeRegionResetEdit(*snapshot, noteIndex, expectedNotesRevision),
        std::move(undoDescription));
}

void OpenTuneAudioProcessor::republishPlaybackSource(ContentKey key)
{
    collectPlaybackSnapshots();
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
#if JucePlugin_Build_Standalone
    // 内容发布后刷新编排播放快照的冻结来源视图与固定几何。
    if (standaloneArrangement_ != nullptr)
        standaloneArrangement_->refreshPlaybackSnapshot();
#endif
#if !JucePlugin_Build_Standalone
    // Capture 替换条目与来源同版冻结：内容发布后重建一次视图。
    if (key.domainKind == DomainKind::RegularVST3Capture) {
        if (auto* session = getCaptureSession())
            session->refreshPublishedReplacementGeometry();
    }
#endif
}

#if JucePlugin_Build_Standalone
void OpenTuneAudioProcessor::refreshStandalonePlaybackSnapshot()
{
    if (standaloneArrangement_ != nullptr)
        standaloneArrangement_->refreshPlaybackSnapshot();
}
#endif

bool OpenTuneAudioProcessor::writePitchCurveToOwner(ContentKey key,
                                                      std::shared_ptr<PitchCurve> curve)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyPitchCurve(std::move(curve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyPitchCurveToModification(key, std::move(curve));
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr || !session->applyPitchCurve(key, std::move(curve)))
                return false;
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok)
        notifyContentChanged(key);
    return ok;
}

bool OpenTuneAudioProcessor::writeOriginalF0ToOwner(
    ContentKey key,
    std::shared_ptr<PitchCurve> curve)
{
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyOriginalF0(std::move(curve));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyOriginalF0ToModification(key, std::move(curve));
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            if (session == nullptr) return false;
            auto* seg = session->findSegmentByContentKey(key);
            if (!seg || !seg->content) return false;
            seg->content->applyOriginalF0(std::move(curve));
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok)
        notifyContentChanged(key);
    return ok;
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
#if !JucePlugin_Build_Standalone
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
        notifyContentChanged(key);
        auto snapshot = getContentSnapshot(key);
        auto* crs = resolveMutableLocalContentRenderService(key);
        if (key.domainKind != DomainKind::ARAAudioModification
            && crs != nullptr
            && snapshot != nullptr)
        {
            // TimeGrid affects only Stage2. Reuse settled Stage1 when available;
            // a pending Stage1 completion will enqueue Stage2 after it settles.
            if (!crs->republishPlaybackSource(key, snapshot))
                return ok;
#if JucePlugin_Build_Standalone
            // TimeGrid 改变来源视图映射，刷新快照中的冻结几何。
            if (standaloneArrangement_ != nullptr)
                standaloneArrangement_->refreshPlaybackSnapshot();
#endif
#if !JucePlugin_Build_Standalone
            // Capture 替换几何随 TimeGrid 输出域重建，与来源发布同一消息回调内完成。
            if (key.domainKind == DomainKind::RegularVST3Capture) {
                if (auto* session = getCaptureSession())
                    session->refreshPublishedReplacementGeometry();
            }
#endif
            crs->getTimeStretchCache().invalidate(key);
            PlaybackReadSource source;
            if (crs->getPlaybackReadSource(key, source))
                enqueueStage2WhenCanonicalSettled(
                    key, std::move(snapshot),
                    source.canonicalBuffer, source.canonicalSampleRate);
        }
        else if (key.domainKind == DomainKind::ARAAudioModification
                 && crs != nullptr
                 && snapshot != nullptr)
            crs->republishPlaybackSource(key, std::move(snapshot));
    }
    return ok;
}

bool OpenTuneAudioProcessor::applyTimeGridEdit(
    ContentKey key,
    std::shared_ptr<const TimeGridSnapshot> before,
    std::shared_ptr<const TimeGridSnapshot> after,
    juce::String description)
{
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        return false;
    if (!key.isValid() || before == nullptr || after == nullptr || contentCommands_ == nullptr)
        return false;

    auto action = contentCommands_->commitTimeGridEdit(key, std::move(before), std::move(after), description);
    if (action == nullptr)
        return false;

    return true;
}

bool OpenTuneAudioProcessor::setContentDetectedKey(ContentKey key, const DetectedKey& detectedKey)
{
    const auto current = getContentSnapshot(key);
    if (current == nullptr)
        return false;
    const auto& previous = current->detectedKey;
    if (previous.origin == Origin::Manual && previous.root == detectedKey.root
        && previous.scale == detectedKey.scale)
        return true;

    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyDetectedKey(detectedKey);
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyDetectedKeyToModification(key, detectedKey);
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            ok = session != nullptr && session->applyDetectedKey(key, detectedKey);
            break;
        }
#endif
        default:
            break;
    }
    if (ok)
        notifyContentChanged(key);
    return ok;
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
#if !JucePlugin_Build_Standalone
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
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            clip->applyOriginalF0State(state);
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyOriginalF0StateToModification(key, state);
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            seg->content->applyOriginalF0State(state);
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok)
        notifyContentChanged(key);
    return ok;
}

bool OpenTuneAudioProcessor::setContentOriginalF0FailureReason(ContentKey key, std::string reason)
{
#if JucePlugin_Build_Standalone
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
#endif
    bool ok = false;
    switch (key.domainKind) {
#if JucePlugin_Build_Standalone
        case DomainKind::StandaloneClip: {
            auto* clip = standaloneContentRepository_->findClip(key);
            if (!clip) return false;
            recordOriginalF0FailureEvent(key, juce::String(reason));
            clip->applyOriginalF0FailureReason(std::move(reason));
            ok = true;
            break;
        }
#endif
#if JucePlugin_Enable_ARA
        case DomainKind::ARAAudioModification: {
            auto* dc = getDocumentController();
            if (!dc) return false;
            ok = dc->applyOriginalF0FailureReasonToModification(key, std::move(reason));
            break;
        }
#endif
#if !JucePlugin_Build_Standalone
        case DomainKind::RegularVST3Capture: {
            auto* session = getCaptureSession();
            auto* seg = session ? session->findSegmentByContentKey(key) : nullptr;
            if (!seg || !seg->content) return false;
            recordOriginalF0FailureEvent(key, juce::String(reason));
            seg->content->applyOriginalF0FailureReason(std::move(reason));
            ok = true;
            break;
        }
#endif
        default:
            break;
    }
    if (ok)
        notifyContentChanged(key);
    return ok;
}

void OpenTuneAudioProcessor::recordOriginalF0FailureEvent(
    ContentKey key, const juce::String& reason)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    pendingF0FailureEvents_.push_back({key, reason});
}

std::deque<OriginalF0FailureEvent> OpenTuneAudioProcessor::takeOriginalF0FailureEvents()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    std::deque<OriginalF0FailureEvent> events;
    events.swap(pendingF0FailureEvents_);
    return events;
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
#if !JucePlugin_Build_Standalone
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
        notifyContentChanged(key);
        onContentFullMutationCompleted(key);
    }
    return ok;
}

PitchShiftEditAction* OpenTuneAudioProcessor::commitPitchShiftEdit(
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

    auto action = std::make_unique<PitchShiftEditAction>(
        contentCommands_, key, std::move(before), std::move(after));
    auto* actionPtr = action.get();
    undoManager_.addAction(std::move(action));
    notifyContentEditCompleted(key);
    return actionPtr;
}

bool OpenTuneAudioProcessor::generateNotesOnlyByContentKey(
    ContentKey key, const NoteGeneratorParams& params)
{
    auto snap = getContentSnapshot(key);
    if (snap == nullptr || contentCommands_ == nullptr)
        return false;
    auto patch = contentCommands_->planGenerateNotesOnly(*snap, params);
    if (!patch)
        return false;
    return commitContentNoteTopologyPatch(key, std::move(*patch)) != nullptr;
}

void OpenTuneAudioProcessor::notifyContentEditCompleted(ContentKey key)
{
#if JucePlugin_Build_Standalone
    JUCE_ASSERT_MESSAGE_THREAD
    if (contentEditCompletedCallback_)
        contentEditCompletedCallback_(key);
#else
    juce::ignoreUnused(key);
#endif
}

bool OpenTuneAudioProcessor::autoTuneContentRangeByContentKey(
    ContentKey key,
    int startFrame,
    int endFrameExclusive,
    const NoteGeneratorParams& params,
    const std::optional<ScaleSnapConfig>& scaleSnap)
{
    auto snap = getContentSnapshot(key);
    if (snap == nullptr || snap->pitchCurve == nullptr || contentCommands_ == nullptr)
        return false;
    auto plannedNotes = contentCommands_->planAutoTuneContentRange(
        *snap, startFrame, endFrameExclusive, params, scaleSnap);
    if (!plannedNotes)
        return false;
    return commitAutoTunePlanByContentKey(
        key, std::move(*plannedNotes), startFrame, endFrameExclusive,
        params.retuneSpeed, params.vibratoDepth, params.vibratoRate);
}

bool OpenTuneAudioProcessor::commitAutoTunePlanByContentKey(ContentKey key,
                                                             std::vector<Note> mergedNotes,
                                                             int startFrame,
                                                             int endFrameExclusive,
                                                             float retuneSpeed,
                                                             float vibratoDepth,
                                                             float vibratoRate)
{
    auto snap = getContentSnapshot(key);
    if (!snap || !snap->pitchCurve) return false;

    if (endFrameExclusive <= startFrame || mergedNotes.empty()) return false;

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
#if !JucePlugin_Build_Standalone
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

    notifyContentChanged(key);

    // 渲染请求范围列表必须覆盖 applyCorrectionToRange 的 calculationRange（两侧各
    // expand getCorrectedF0BoundaryContextFrames 帧），否则边界处 chunk 保留旧
    // 音频，与新 correction segment 产生不连续 → click。
    const int maxFrame = static_cast<int>(snap->pitchCurve->getOriginalF0().size());
    const auto expandedRange = PitchCurve::expandNoteBasedCorrectionRange(
        startFrame, endFrameExclusive, maxFrame);
    onContentLocalMutationCompleted(
        key, std::vector<ContentEditRangeFrames>{{expandedRange.startFrame, expandedRange.endFrameExclusive}});
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
    if (!sourceContentKey.isValid() || !std::isfinite(offsetSeconds)
        || !std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        return ContentKey{};
    }

    const auto sourceSnap = getContentSnapshot(sourceContentKey);
    if (!sourceSnap
        || sourceSnap->audioBuffer == nullptr
        || sourceSnap->audioBuffer->getNumSamples() == 0
        || sourceSnap->nativeSource.buffer == nullptr) {
        return ContentKey{};
    }

    const double canonicalRate = sourceSnap->audioSampleRate;
    if (!std::isfinite(canonicalRate) || canonicalRate <= 0.0) {
        return ContentKey{};
    }

    const int64_t totalCanonicalSamples = sourceSnap->audioBuffer->getNumSamples();
    // offsetSeconds/durationSeconds 是父 content-local 权威时间；只派生
    // source-absolute 窗口，原始秒值不投影回写。
    SourceWindow requestedWindow = sourceSnap->sourceWindow;
    requestedWindow.sourceStartSeconds += offsetSeconds;
    requestedWindow.sourceEndSeconds = requestedWindow.sourceStartSeconds + durationSeconds;

    // canonical 裁剪地址来自全局 source 网格与父 buffer 全局起点的差。
    const int64_t cropBeginSample =
        PlaybackSampleGeometry::projectToSample(requestedWindow.sourceStartSeconds * canonicalRate)
        - sourceSnap->firstCanonicalSample;
    const int64_t cropEndSample =
        PlaybackSampleGeometry::projectToSample(requestedWindow.sourceEndSeconds * canonicalRate)
        - sourceSnap->firstCanonicalSample;
    if (cropBeginSample < 0 || cropEndSample <= cropBeginSample
        || cropEndSample > totalCanonicalSamples) {
        return ContentKey{};
    }

    // local seconds 是绝对时间合同；canonical 下标只承担离散访问。
    const double localStartSeconds = offsetSeconds;
    const double localEndSeconds = offsetSeconds + durationSeconds;
    ContentState payload;
    payload.sourceWindow = requestedWindow;
    payload.nativeSource = sourceSnap->nativeSource;
    payload.audioBuffer = sliceAudioBuffer(sourceSnap->audioBuffer, cropBeginSample, cropEndSample);
    payload.sampleRate = canonicalRate;
    payload.firstCanonicalSample =
        PlaybackSampleGeometry::projectToSample(requestedWindow.sourceStartSeconds * canonicalRate);
    payload.analysis.pitchCurve = slicePitchCurveToLocalRange(sourceSnap->pitchCurve,
                                                     localStartSeconds,
                                                     localEndSeconds);
    payload.analysis.setOriginalF0State(sourceSnap->originalF0State);
    payload.analysis.detectedKey = sourceSnap->detectedKey;
    payload.notes = sliceNotesToLocalRange(sourceSnap->notes, localStartSeconds, localEndSeconds);
    payload.noteTopologyInitialized = sourceSnap->noteTopologyInitialized;
    payload.analysis.silentGaps = sliceSilentGaps(sourceSnap->silentGaps, cropBeginSample, cropEndSample);
    payload.pitchShiftSettings = sourceSnap->pitchShiftSettings;
    payload.volumeEnvelope = AutomationLane::sliceToLocalRange(
        sourceSnap->volumeEnvelope, localStartSeconds, localEndSeconds);

    // 截断失败时按权威 local duration 建立 identity；owner 不再补造 grid。
    payload.timeGrid = buildCopiedRangeTimeGrid(*sourceSnap->timeGrid,
                                                localStartSeconds,
                                                durationSeconds);
    if (payload.timeGrid == nullptr) {
        payload.timeGrid = TimeGridSnapshot::makeIdentity(durationSeconds);
    }
    ++payload.timeGridRevision;

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
