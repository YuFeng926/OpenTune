#include "ProjectSession.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "../PluginProcessor.h"
#include "../Render/PlaybackReadSource.h"
#include "../Standalone/StandaloneArrangementHelpers.h"
#include "AppLogger.h"
#include "AppPreferences.h"
#include "ProjectPersistence.h"
#include "LocalizationManager.h"
#include "PlaybackSampleGeometry.h"
#include "VisibleTrackCount.h"
#include <algorithm>
#include <set>

namespace OpenTune {

// ============================================================================
// 构造/ 析构
// ============================================================================

ProjectSession::ProjectSession(OpenTuneAudioProcessor& processor, AppPreferences& appPreferences)
    : processorRef_(processor)
    , appPreferencesRef_(appPreferences)
    , dirty_(false)
{
}

ProjectSession::~ProjectSession() = default;

// ============================================================================
// 路径查询
// ============================================================================

bool ProjectSession::hasProjectPath() const noexcept
{
    return currentProjectFile_ != juce::File{};
}

const juce::File& ProjectSession::getCurrentProjectFile() const noexcept
{
    return currentProjectFile_;
}

juce::String ProjectSession::getProjectName() const
{
    if (!hasProjectPath()) { return LOC(kUntitled); }
    return currentProjectFile_.getFileNameWithoutExtension();
}

// ============================================================================
// 脏状态
// ============================================================================

bool ProjectSession::isDirty() const noexcept { return dirty_; }

void ProjectSession::markDirty() { dirty_ = true; ++dirtyGeneration_; }

void ProjectSession::clearDirty() { dirty_ = false; }

uint64_t ProjectSession::getDirtyGeneration() const noexcept { return dirtyGeneration_; }

// ============================================================================
// 快照抓取
// ============================================================================

ProjectSnapshot ProjectSession::captureSnapshot() const
{
    ProjectSnapshot snap;

    // Header
    snap.header.projectFormatVersion = ProjectPersistence::kCurrentProjectFormatVersion;
    snap.header.appVersion = OPENTUNE_VERSION;
    snap.header.projectName = getProjectName();

    // 工程身份固化：首次保存时生成，后续保存复用
    if (cachedProjectId_.isEmpty()) {
        cachedProjectId_ = ProjectSnapshot::generateProjectId();
        cachedCreatedAt_ = ProjectSnapshot::generateTimestamp();
    }
    snap.header.projectId = cachedProjectId_;
    snap.header.createdAt = cachedCreatedAt_;
    snap.header.lastSavedAt = ProjectSnapshot::generateTimestamp();

    // Core stores
    auto* sourceStore = processorRef_.getSourceStore();
    auto* contentRepo = processorRef_.getStandaloneContentRepository();
    auto* arrangement = processorRef_.getStandaloneArrangement();

    // All three stores are essential for a valid snapshot
    if (!sourceStore || !contentRepo || !arrangement) {
        AppLogger::error("ProjectSession: Cannot capture snapshot —one or more core stores are unavailable");
        return ProjectSnapshot{};  // Empty snapshot
    }

    // Sources (from SourceStore)
    const auto sourceIds = sourceStore->getAllActiveSourceIds();
    for (auto sid : sourceIds) {
        SourceStore::SourceSnapshot srcSnap;
        if (!sourceStore->getSnapshot(sid, srcSnap)) { continue; }

        ProjectSourceEntry entry;
        entry.sourceId = srcSnap.sourceId;
        entry.displayName = srcSnap.displayName;
        entry.originalImportPath = srcSnap.sourceFilePath.isNotEmpty()
            ? srcSnap.sourceFilePath
            : srcSnap.displayName;
        entry.sampleRate = srcSnap.sampleRate;
        entry.numChannels = srcSnap.numChannels;
        entry.lengthSamples = srcSnap.numSamples;
        entry.lengthSeconds = (srcSnap.sampleRate > 0.0)
            ? static_cast<double>(srcSnap.numSamples) / srcSnap.sampleRate : 0.0;
        entry.contentHash = juce::String::toHexString(static_cast<juce::int64>(srcSnap.sourceId));
        entry.fileSizeBytes = 0; // Unknown until file copy

        snap.sources.push_back(entry);
    }

    // Clips (from StandaloneContentRepository) —collect all ContentKeys referenced by placements
    std::set<ContentKey> activeContentKeys;
    int numTracks = arrangement->getNumTracks();
    for (int trackId = 0; trackId < numTracks; ++trackId) {
        const int numPlacements = arrangement->getNumPlacements(trackId);
        for (int pi = 0; pi < numPlacements; ++pi) {
            StandaloneArrangement::Placement placement;
            if (!arrangement->getPlacementByIndex(trackId, pi, placement)) { continue; }
            if (placement.isRetired) { continue; }
            if (placement.contentKey.domainKind != DomainKind::StandaloneClip) { continue; }
            activeContentKeys.insert(placement.contentKey);
        }
    }

    for (const auto& key : activeContentKeys) {
        auto* clip = contentRepo->findClip(key);
        if (!clip) { continue; }

        const auto& content = clip->content();

        ProjectContentEntry entry;
        entry.contentKey = key;
        entry.sourceId = content.sourceWindow.sourceId;
        entry.retired = false;
        entry.lineageParentContentKey = ContentKey{}; // Invalid key for no parent
        entry.sourceWindow = content.sourceWindow;
        entry.detectedKey = content.analysis.detectedKey;
        entry.notes = content.notes;
        entry.noteTopologyInitialized = content.noteTopologyInitialized;
        entry.volumeEnvelope = content.volumeEnvelope;

        // Extract corrected segments from pitch curve
        if (content.analysis.pitchCurve) {
            auto pcSnap = content.analysis.pitchCurve->getSnapshot();
            const auto& segments = pcSnap->getCorrectionSegments();
            for (const auto& seg : segments) {
                ProjectContentEntry::SegmentEntry segEntry;
                segEntry.startFrame = seg.startFrame;
                segEntry.endFrame = seg.endFrame;
                segEntry.source = static_cast<uint8_t>(seg.source);
                segEntry.retuneSpeed = seg.parameterSnapshot.retuneSpeed;
                segEntry.pitchDriftScale = seg.parameterSnapshot.pitchDriftScale;
                segEntry.vibratoDepth = seg.parameterSnapshot.vibratoDepth;
                segEntry.vibratoRate = seg.parameterSnapshot.vibratoRate;
                // Always serialize f0Data —HandDraw/LineAnchor segments rely on it
                segEntry.f0Data = seg.f0Data;
                entry.correctionSegments.push_back(segEntry);
            }
            entry.erasedRanges = pcSnap->getErasedRanges();
        }

        // TimeGrid: owner invariant 保证 timeGrid 恒非空，无条件写出
        for (const auto& handle : content.timeGrid->handles()) {
            ProjectContentEntry::TimeGridEntry::HandleEntry he;
            he.id = handle.id;
            he.kind = static_cast<uint8_t>(handle.kind);
            he.sourceSeconds = handle.source_seconds;
            he.outputSeconds = handle.output_seconds;
            he.confidence = static_cast<uint8_t>(handle.confidence);
            entry.timeGrid.handles.push_back(he);
        }

        // Original F0 state
        entry.originalF0State = static_cast<uint8_t>(content.analysis.originalF0State);

        // Pitch shift settings
        entry.pitchShiftSettings.semitone = content.pitchShiftSettings.semitone;
        entry.pitchShiftSettings.cents = content.pitchShiftSettings.cents;

        // Silent gaps
        for (const auto& gap : content.analysis.silentGaps) {
            ProjectContentEntry::SilentGapEntry gapEntry;
            gapEntry.startSample = gap.startSample;
            gapEntry.endSampleExclusive = gap.endSampleExclusive;
            gapEntry.minLevel_dB = gap.minLevel_dB;
            entry.silentGaps.push_back(gapEntry);
        }

        // Reference features
        entry.referenceFeatures.analysisRevision = content.analysis.referenceFeatures.analysisRevision;
        entry.referenceFeatures.status = static_cast<uint8_t>(content.analysis.referenceFeatures.status);
        entry.referenceFeatures.producer = static_cast<uint8_t>(content.analysis.referenceFeatures.producer);
        entry.referenceFeatures.inputFingerprint = content.analysis.referenceFeatures.inputFingerprint;
        entry.referenceFeatures.sourceDurationSeconds = content.analysis.referenceFeatures.sourceDurationSeconds;
        entry.referenceFeatures.errorMessage = content.analysis.referenceFeatures.errorMessage;
        entry.referenceFeatures.pitchNotes = content.analysis.referenceFeatures.pitch.notes;
        for (const auto& anchor : content.analysis.referenceFeatures.timing.anchors) {
            ProjectContentEntry::ReferenceFeatureEntry::TimingAnchorEntry anchorEntry;
            anchorEntry.anchorId = anchor.anchorId;
            anchorEntry.sourceSeconds = anchor.sourceSeconds;
            anchorEntry.strength = anchor.strength;
            anchorEntry.kind = static_cast<uint8_t>(anchor.kind);
            anchorEntry.confidence = anchor.confidence;
            entry.referenceFeatures.timingAnchors.push_back(anchorEntry);
        }

        snap.contents.push_back(entry);
    }

    // Tracks and Placements (from StandaloneArrangement)
    for (int trackId = 0; trackId < numTracks; ++trackId) {
        ProjectTrackEntry trackEntry;
        trackEntry.trackId = trackId;
        trackEntry.gain = arrangement->getTrackVolume(trackId);
        trackEntry.mute = arrangement->isTrackMuted(trackId);
        trackEntry.solo = arrangement->isTrackSolo(trackId);
        trackEntry.colour = arrangement->getTrackColour(trackId);

        const int numPlacements = arrangement->getNumPlacements(trackId);
        for (int pi = 0; pi < numPlacements; ++pi) {
            StandaloneArrangement::Placement placement;
            if (!arrangement->getPlacementByIndex(trackId, pi, placement)) { continue; }
            if (placement.isRetired) { continue; }

            ProjectPlacementEntry pEntry;
            pEntry.placementId = placement.placementId;
            pEntry.contentKey = placement.contentKey;
            pEntry.timelineStartSeconds = placement.timelineStartSeconds;
            pEntry.timelineDurationSeconds = placement.durationSeconds;
            pEntry.clipGain = placement.gain;
            pEntry.fadeInDurationSeconds = placement.fadeInDuration;
            pEntry.fadeOutDurationSeconds = placement.fadeOutDuration;
            pEntry.clipInSeconds = placement.clipInSeconds;
            pEntry.name = placement.name;
            trackEntry.placements.push_back(pEntry);
        }

        snap.tracks.push_back(trackEntry);
    }

    // Selected track/placement for session state
    snap.settings.selectedTrackId = arrangement->getActiveTrackId();
    snap.settings.visibleTrackCount = arrangement->getVisibleTrackCount();

    // Settings
    snap.settings.bpm = processorRef_.getBpm();
    snap.settings.timeSignatureNumerator = processorRef_.getTimeSigNumerator();
    snap.settings.timeSignatureDenominator = processorRef_.getTimeSigDenominator();
    snap.settings.sampleRate = processorRef_.getSampleRate();

    return snap;
}

// ============================================================================
// 两阶段打开
// ============================================================================

namespace {
static std::shared_ptr<juce::AudioBuffer<float>> loadAudioFile(const juce::File& file, double& outSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file));
    if (!reader) { return nullptr; }

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(
        static_cast<int>(reader->numChannels),
        static_cast<int>(reader->lengthInSamples));
    reader->read(buffer.get(), 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    outSampleRate = reader->sampleRate;
    return buffer;
}

static std::shared_ptr<const juce::AudioBuffer<float>> cropCanonicalRange(
    const std::shared_ptr<const juce::AudioBuffer<float>>& canonicalBuffer,
    int64_t startSample,
    int64_t endSampleExclusive)
{
    if (canonicalBuffer == nullptr
        || canonicalBuffer->getNumChannels() <= 0
        || canonicalBuffer->getNumSamples() <= 0
        || startSample < 0
        || endSampleExclusive <= startSample
        || startSample >= canonicalBuffer->getNumSamples()) {
        return nullptr;
    }

    const int64_t totalSamples = canonicalBuffer->getNumSamples();
    const int64_t endSample = juce::jmin(totalSamples, endSampleExclusive);
    if (endSample <= startSample)
        return nullptr;

    // 全覆盖范围共享 prepared canonical 存储；子区间才复制切片。
    if (startSample == 0 && endSample == totalSamples)
        return canonicalBuffer;

    const auto rangeSamples = static_cast<int>(endSample - startSample);
    auto rangeBuffer = std::make_shared<juce::AudioBuffer<float>>(
        canonicalBuffer->getNumChannels(), rangeSamples);
    for (int channel = 0; channel < canonicalBuffer->getNumChannels(); ++channel) {
        rangeBuffer->copyFrom(
            channel,
            0,
            *canonicalBuffer,
            channel,
            static_cast<int>(startSample),
            rangeSamples);
    }

    return rangeBuffer;
}
} // namespace

Result<ProjectSession::PreparedOpen> ProjectSession::prepareOpen(const juce::File& file, Language language)
{
    ProjectPersistence persistence(language);
    auto readResult = persistence.readProjectFile(file);
    if (!readResult.ok())
        return Result<PreparedOpen>::failure(readResult.error());

    PreparedOpen preparedOpen;
    preparedOpen.snapshot = std::move(readResult).value();
    preparedOpen.projectFile = file;
    preparedOpen.language = language;
    preparedOpen.sources.reserve(preparedOpen.snapshot.sources.size());

    const auto projectDirectory = file.getParentDirectory();
    for (const auto& sourceEntry : preparedOpen.snapshot.sources) {
        juce::File audioFile;
        if (sourceEntry.relativeMediaPath.isNotEmpty())
            audioFile = projectDirectory.getChildFile(sourceEntry.relativeMediaPath);

        if (!audioFile.existsAsFile() && sourceEntry.originalImportPath.isNotEmpty())
            audioFile = juce::File(sourceEntry.originalImportPath);

        double loadedSampleRate = 0.0;
        auto audioBuffer = audioFile.existsAsFile()
            ? loadAudioFile(audioFile, loadedSampleRate)
            : nullptr;
        if (!audioBuffer) {
            AppLogger::log("ProjectSession: Failed to load audio for source "
                + juce::String(sourceEntry.sourceId) + " (" + sourceEntry.displayName + "), skipping");
            continue;
        }

        OpenTuneAudioProcessor::PreparedImport preparedImport;
        if (!processorRef_.prepareImport(std::move(*audioBuffer), loadedSampleRate,
                                         sourceEntry.displayName, audioFile.getFullPathName(),
                                         preparedImport, "project-open")) {
            AppLogger::log("ProjectSession: prepareImport rejected source "
                + juce::String(sourceEntry.sourceId) + " (" + sourceEntry.displayName + "), skipping");
            continue;
        }

        PreparedProjectSource preparedSource;
        preparedSource.sourceId = sourceEntry.sourceId;
        preparedSource.preparedImport = std::move(preparedImport);
        preparedOpen.sources.push_back(std::move(preparedSource));
    }

    return Result<PreparedOpen>::success(std::move(preparedOpen));
}

Result<void> ProjectSession::commitPreparedOpen(PreparedOpen&& preparedOpen)
{
    const auto& snapshot = preparedOpen.snapshot;

    // 清空当前状态
    auto* sourceStore = processorRef_.getSourceStore();
    auto* contentRepo = processorRef_.getStandaloneContentRepository();
    auto* arrangement = processorRef_.getStandaloneArrangement();

    // All three stores are essential for committing a project.
    if (!sourceStore || !contentRepo || !arrangement) {
        AppLogger::error("ProjectSession: Cannot commit project —one or more core stores are unavailable");
        return Result<void>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                Loc::get(preparedOpen.language, Loc::Keys::kProjectCoreStoresUnavailable).toStdString()));
    }

    if (auto* crs = processorRef_.getContentRenderService())
        crs->drainRenderWorker();

    sourceStore->clear();
    contentRepo->clear();
    arrangement->clear();

    processorRef_.getUndoManager().clear();

    // Clear CRS derived state (playback sources, render caches, stretchers,
    // time stretch cache) to prevent cross-project key collisions.
    if (auto* crs = processorRef_.getContentRenderService())
        crs->clearAll();

    // 1. 重建 Sources（SourceStore 始终保存原率原始 PCM 与实际采样率）
    for (auto& preparedSource : preparedOpen.sources) {
        auto& prepared = preparedSource.preparedImport;

        SourceStore::CreateSourceRequest req;
        req.displayName = prepared.displayName;
        req.sourceFilePath = prepared.sourceFilePath;
        req.audioBuffer = prepared.nativeSource.buffer;
        req.sampleRate = prepared.nativeSource.sampleRate;

        if (sourceStore->createSource(req, preparedSource.sourceId) == 0) {
            AppLogger::log("ProjectSession: Failed to create source "
                + juce::String(preparedSource.sourceId) + " (" + prepared.displayName + "), skipping");
            continue;
        }
    }

    // 2. 重建 Clips (from StandaloneContentRepository)
    for (const auto& contentEntry : snapshot.contents) {
        if (contentEntry.sourceId == 0 || !contentEntry.sourceWindow.isValid()) {
            AppLogger::log("ProjectSession: Invalid source window for clip "
                + juce::String(static_cast<juce::int64>(contentEntry.contentKey.objectId)) + ", skipping");
            continue;
        }

        SourceStore::SourceSnapshot sourceSnapshot;
        if (!sourceStore->getSnapshot(contentEntry.sourceId, sourceSnapshot)
            || sourceSnapshot.sampleRate <= 0.0) {
            AppLogger::log("ProjectSession: Source " + juce::String(contentEntry.sourceId)
                + " not found for clip "
                + juce::String(static_cast<juce::int64>(contentEntry.contentKey.objectId)) + ", skipping");
            continue;
        }

        // 复用 prepareImport 准备好的 per-source canonical，而不是每个 clip 重做整段 SRC。
        const auto preparedIt = std::find_if(
            preparedOpen.sources.begin(), preparedOpen.sources.end(),
            [&contentEntry](const PreparedProjectSource& source) {
                return source.sourceId == contentEntry.sourceId;
            });
        if (preparedIt == preparedOpen.sources.end()
            || preparedIt->preparedImport.canonicalAudioBuffer == nullptr
            || preparedIt->preparedImport.canonicalSampleRate <= 0.0) {
            AppLogger::log("ProjectSession: Prepared audio missing for source "
                + juce::String(contentEntry.sourceId) + ", skipping");
            continue;
        }

        const auto& prepared = preparedIt->preparedImport;
        const double canonicalRate = prepared.canonicalSampleRate;

        // 与 prepareImport 保持相同的最小存储长度：正时长窗口至少保留一个
        // canonical 样本。全覆盖由 cropCanonicalRange 共享 prepared 存储；秒值保持原样。
        const int64_t canonicalBegin = PlaybackSampleGeometry::projectToSample(
            contentEntry.sourceWindow.sourceStartSeconds * canonicalRate) - prepared.canonicalFirstSample;
        const int64_t canonicalEnd = std::max(canonicalBegin + 1,
            PlaybackSampleGeometry::projectToSample(
                contentEntry.sourceWindow.sourceEndSeconds * canonicalRate) - prepared.canonicalFirstSample);
        auto windowedBuffer = cropCanonicalRange(
            prepared.canonicalAudioBuffer, canonicalBegin, canonicalEnd);
        if (windowedBuffer == nullptr) {
            AppLogger::log("ProjectSession: Invalid source window for clip "
                + juce::String(static_cast<juce::int64>(contentEntry.contentKey.objectId)) + ", skipping");
            continue;
        }

        ContentKey clipKey = contentRepo->createClip(contentEntry.contentKey.objectId);
        if (!clipKey.isValid()) {
            AppLogger::log("ProjectSession: Failed to create clip " + juce::String(static_cast<juce::int64>(contentEntry.contentKey.objectId)));
            continue;
        }

        auto* clip = contentRepo->findClip(clipKey);
        if (!clip) { continue; }

        // 应用音频数据（保存的窗口秒值与 native 来源原样恢复）
        clip->content().sourceWindow = contentEntry.sourceWindow;
        clip->content().nativeSource = prepared.nativeSource;
        clip->content().firstCanonicalSample =
            prepared.canonicalFirstSample + canonicalBegin;
        clip->applyAudioBuffer(windowedBuffer, canonicalRate);

        // 应用 notes
        clip->applyNotes(contentEntry.notes);

        // 恢复 note topology 状态（applyNotes 按新契约会置 true，空 notes 工程必须恢复持久值）
        clip->content().noteTopologyInitialized = contentEntry.noteTopologyInitialized;

        // 恢复 envelope（revision 不落盘，恢复端由 owner 推进新 revision）
        clip->applyVolumeEnvelope(contentEntry.volumeEnvelope);

        // 应用 detectedKey
        clip->applyDetectedKey(contentEntry.detectedKey);

        // 恢复 pitch curve 和corrected segments
        auto pitchCurve = std::make_shared<PitchCurve>();
        for (const auto& seg : contentEntry.correctionSegments) {
            if (!seg.f0Data.empty()) {
                // 此时原始音高尚未重新提取。修音段先按保存的 f0 保留，
                // 提取完成后再按新的有声帧切开。
                PitchCorrectionSegment::ParameterSnapshot parameters;
                parameters.retuneSpeed = seg.retuneSpeed;
                parameters.pitchDriftScale = seg.pitchDriftScale;
                parameters.vibratoDepth = seg.vibratoDepth;
                parameters.vibratoRate = seg.vibratoRate;
                pitchCurve->setManualCorrectionRange(
                    seg.startFrame, seg.endFrame, seg.f0Data,
                    static_cast<PitchCorrectionSegment::Source>(seg.source),
                    parameters);
            }
        }
        if (!contentEntry.erasedRanges.empty()) {
            const auto endFrame = std::max_element(
                contentEntry.erasedRanges.begin(), contentEntry.erasedRanges.end(),
                [](const auto& a, const auto& b) {
                    return a.endFrameExclusive < b.endFrameExclusive;
                })->endFrameExclusive;
            pitchCurve->replaceErasedRangesInRange(0, endFrame, contentEntry.erasedRanges);
        }
        clip->applyPitchCurve(pitchCurve);

        // 恢复保存的 TimeGrid（原样恢复，不按窗口时长改写）；
        // handles 缺失时保留 applyAudioBuffer 按权威窗口时长建立的 identity。
        if (!contentEntry.timeGrid.handles.empty()) {
            std::vector<TimeHandle> handles;
            for (const auto& he : contentEntry.timeGrid.handles) {
                TimeHandle th;
                th.id = static_cast<uint64_t>(he.id);
                th.kind = static_cast<HandleKind>(he.kind);
                th.source_seconds = he.sourceSeconds;
                th.output_seconds = he.outputSeconds;
                th.confidence = static_cast<Confidence>(static_cast<uint8_t>(he.confidence));
                handles.push_back(th);
            }
            auto tgSnapshot = TimeGridSnapshot::makeFromHandles(std::move(handles));
            if (tgSnapshot == nullptr) {
                AppLogger::log("ProjectSession: Invalid time grid for clip "
                    + juce::String(static_cast<juce::int64>(contentEntry.contentKey.objectId))
                    + ", keeping audio-derived identity");
            } else {
                clip->applyTimeGrid(std::move(tgSnapshot));
            }
        }

        // 恢复 Original F0 state — Ready 和 Extracting 都归一化为 NotRequested，
        // 让 F0 提取流程重新分析。
        auto restoredF0State = static_cast<OriginalF0State>(contentEntry.originalF0State);
        if (restoredF0State == OriginalF0State::Ready
            || restoredF0State == OriginalF0State::Extracting)
            restoredF0State = OriginalF0State::NotRequested;
        clip->applyOriginalF0State(restoredF0State);

        // 恢复 Pitch shift settings
        PitchShiftSettings pitchShift;
        pitchShift.semitone = contentEntry.pitchShiftSettings.semitone;
        pitchShift.cents = contentEntry.pitchShiftSettings.cents;
        clip->content().pitchShiftSettings = pitchShift;

        // 恢复 Silent gaps（直接写入 content，因为 silentGaps 是分析结果）
        auto& contentRef = clip->content();
        contentRef.analysis.silentGaps.clear();
        for (const auto& gapEntry : contentEntry.silentGaps) {
            SilentGap gap;
            gap.startSample = gapEntry.startSample;
            gap.endSampleExclusive = gapEntry.endSampleExclusive;
            gap.minLevel_dB = gapEntry.minLevel_dB;
            contentRef.analysis.silentGaps.push_back(gap);
        }

        // 恢复 Reference features
        ReferenceFeatureSet refFeatures;
        refFeatures.analysisRevision = contentEntry.referenceFeatures.analysisRevision;
        refFeatures.status = static_cast<ReferenceFeatureStatus>(contentEntry.referenceFeatures.status);
        refFeatures.producer = static_cast<ReferenceFeatureProducer>(contentEntry.referenceFeatures.producer);
        refFeatures.inputFingerprint = contentEntry.referenceFeatures.inputFingerprint;
        refFeatures.sourceDurationSeconds = contentEntry.referenceFeatures.sourceDurationSeconds;
        refFeatures.errorMessage = contentEntry.referenceFeatures.errorMessage;
        refFeatures.pitch.notes = contentEntry.referenceFeatures.pitchNotes;
        for (const auto& anchorEntry : contentEntry.referenceFeatures.timingAnchors) {
            ReferenceTimingAnchor anchor;
            anchor.anchorId = anchorEntry.anchorId;
            anchor.sourceSeconds = anchorEntry.sourceSeconds;
            anchor.strength = anchorEntry.strength;
            anchor.kind = static_cast<ReferenceTimingAnchorKind>(anchorEntry.kind);
            anchor.confidence = anchorEntry.confidence;
            refFeatures.timing.anchors.push_back(anchor);
        }
        clip->applyReferenceFeatures(refFeatures);
    }

    // 3. 重建 Tracks & Placements
    for (const auto& trackEntry : snapshot.tracks) {
        const int trackId = trackEntry.trackId;
        arrangement->setTrackVolume(trackId, trackEntry.gain);
        arrangement->setTrackMuted(trackId, trackEntry.mute);
        arrangement->setTrackSolo(trackId, trackEntry.solo);
        arrangement->setTrackColour(trackId, trackEntry.colour);

        for (const auto& pEntry : trackEntry.placements) {
            // Verify clip exists
            if (!contentRepo->findClip(pEntry.contentKey)) {
                AppLogger::log("ProjectSession: Clip "
                    + juce::String(static_cast<juce::int64>(pEntry.contentKey.objectId))
                    + " not found for placement " + juce::String(pEntry.placementId) + ", skipping");
                continue;
            }

            StandaloneArrangement::Placement placement;
            placement.placementId = pEntry.placementId;
            placement.contentKey = pEntry.contentKey;
            placement.timelineStartSeconds = pEntry.timelineStartSeconds;
            placement.durationSeconds = pEntry.timelineDurationSeconds;
            placement.gain = pEntry.clipGain;
            placement.fadeInDuration = pEntry.fadeInDurationSeconds;
            placement.fadeOutDuration = pEntry.fadeOutDurationSeconds;
            placement.clipInSeconds = pEntry.clipInSeconds;
            placement.name = pEntry.name;

            arrangement->insertPlacement(trackId, placement);
        }
    }

    arrangement->setActiveTrack(snapshot.settings.selectedTrackId);

    std::vector<LoadedTrackVisibilityHint> visibilityHints;
    visibilityHints.reserve(snapshot.tracks.size());
    for (const auto& trackEntry : snapshot.tracks) {
        LoadedTrackVisibilityHint hint;
        hint.trackId = trackEntry.trackId;
        hint.hasPlacements = !trackEntry.placements.empty();
        hint.mute = trackEntry.mute;
        hint.solo = trackEntry.solo;
        hint.gain = trackEntry.gain;
        visibilityHints.push_back(hint);
    }
    arrangement->setVisibleTrackCount(resolveLoadedVisibleTrackCount(
        snapshot.settings.visibleTrackCount,
        snapshot.settings.selectedTrackId,
        visibilityHints));

    // 恢复工程设置
    processorRef_.setBpm(snapshot.settings.bpm);
    processorRef_.setTimeSignature(snapshot.settings.timeSignatureNumerator,
                                   snapshot.settings.timeSignatureDenominator);

    // Rehydrate CRS derived truth from restored owner state.
    // Use requestContentRefresh (preserveCorrectionsOutsideChangedRange=true) to
    // trigger F0 analysis for NotRequested content — cannot just NotRequested and
    // never analyze.
    std::set<ContentKey> uniqueKeys;
    for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId)
    {
        const int placementCount = getStandalonePlacementCount(processorRef_, trackId);
        for (int placementIdx = 0; placementIdx < placementCount; ++placementIdx)
        {
            ContentKey key = getStandaloneContentKey(processorRef_, trackId, placementIdx);
            if (!key.isValid()) continue;

            if (auto snap = processorRef_.getContentSnapshot(key))
            {
                if (snap->nativeSource.buffer && snap->audioBuffer && snap->audioBuffer->getNumSamples() > 0)
                {
                    auto* crs = processorRef_.getContentRenderService();
                    auto readSource = makePlaybackReadSource(
                        key, snap, snap->nativeSource, snap->audioBuffer, snap->audioSampleRate,
                        snap->firstCanonicalSample,
                        crs->getOrCreateRenderCache(key), crs->getTimeStretchCache());
                    crs->publishPlaybackSource(key, std::move(readSource));
                }
            }

            if (uniqueKeys.insert(key).second)
            {
                OpenTuneAudioProcessor::ContentRefreshRequest refreshRequest;
                refreshRequest.contentKey = key;
                refreshRequest.preserveCorrectionsOutsideChangedRange = true;
                processorRef_.requestContentRefresh(refreshRequest);
            }
        }
    }

    // Restored placements were inserted before their CRS source views existed.
    processorRef_.refreshStandalonePlaybackSnapshot();

    currentProjectFile_ = preparedOpen.projectFile;
    cachedProjectId_ = snapshot.header.projectId;
    cachedCreatedAt_ = snapshot.header.createdAt;
    clearDirty();
    pushRecentProject(preparedOpen.projectFile);

    return Result<void>::success();
}

void ProjectSession::setCurrentProjectFile(const juce::File& file)
{
    currentProjectFile_ = file;
}

// ============================================================================
// 分步保存
// ============================================================================

ProjectSession::SaveTask ProjectSession::prepareSave(const juce::File& targetFile)
{
    SaveTask task;
    task.snapshot = captureSnapshot();
    task.snapshot.header.projectName = targetFile.getFileNameWithoutExtension();
    task.targetFile = targetFile;
    task.mediaDirectory = targetFile.getParentDirectory().getChildFile(
        targetFile.getFileNameWithoutExtension() + "_Media");
    task.language = LocalizationManager::getInstance().resolveLanguage();
    return task;
}

Result<void> ProjectSession::executeSaveToFile(SaveTask& task)
{
    // 1. Copy media files (pure file I/O)
    if (task.targetFile != juce::File{} && task.mediaDirectory != juce::File{}) {
        auto mediaResult = copyMediaToProjectDirectory(task.snapshot, task.mediaDirectory, task.language);
        if (!mediaResult.ok()) {
            return mediaResult;
        }
    }

    // 2. Write .otproj (pure file I/O)
    ProjectPersistence persistence(task.language);
    if (!persistence.writeProjectFile(task.snapshot, task.targetFile)) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                Loc::format(Loc::get(task.language, Loc::Keys::kProjectWriteFailed),
                            task.targetFile.getFullPathName()).toStdString()));
    }

    return Result<void>::success();
}

// ============================================================================
// 媒体复制
// ============================================================================

juce::String ProjectSession::generateMediaFileName(const ProjectSourceEntry& source)
{
    // Stable naming: sourceId + sanitized display name extension
    juce::String base = juce::String(source.sourceId);

    juce::String extension = ".wav";
    if (source.originalImportPath.isNotEmpty()) {
        extension = juce::File(source.originalImportPath).getFileExtension();
        if (extension.isEmpty()) { extension = ".wav"; }
    }

    return base + extension;
}

Result<void> ProjectSession::copyMediaToProjectDirectory(ProjectSnapshot& snapshot,
                                                          const juce::File& mediaDir,
                                                          Language language)
{
    if (mediaDir == juce::File{}) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                            Loc::get(language, Loc::Keys::kProjectNoMediaDirectory).toStdString()));
    }

    if (!mediaDir.createDirectory().wasOk()) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                Loc::format(Loc::get(language, Loc::Keys::kProjectMediaDirectoryCreateFailed),
                            mediaDir.getFullPathName()).toStdString()));
    }

    for (auto& srcEntry : snapshot.sources) {
        juce::File sourceFile;

        if (srcEntry.originalImportPath.isNotEmpty()) {
            sourceFile = juce::File(srcEntry.originalImportPath);
        }

        if (!sourceFile.existsAsFile()) {
            return Result<void>::failure(
                Error::fromCode(ErrorCode::UnknownError,
                    Loc::format(Loc::get(language, Loc::Keys::kProjectSourceFileNotFoundForCopy),
                                srcEntry.originalImportPath).toStdString()));
        }

        const auto destFileName = generateMediaFileName(srcEntry);
        const auto destFile = mediaDir.getChildFile(destFileName);

        if (sourceFile != destFile) {
            juce::TemporaryFile temporaryFile(destFile);
            const auto& tempFile = temporaryFile.getFile();
            const auto copied = sourceFile.copyFileTo(tempFile);
            const auto replaced = copied && temporaryFile.overwriteTargetFileWithTemporary();

            if (!replaced) {
                return Result<void>::failure(
                    Error::fromCode(ErrorCode::UnknownError,
                        Loc::format(Loc::get(language, Loc::Keys::kProjectMediaCopyFailed),
                                    sourceFile.getFullPathName(),
                                    destFile.getFullPathName()).toStdString()));
            }
        }

        srcEntry.relativeMediaPath = mediaDir.getFileName() + "/" + destFileName;
        srcEntry.fileSizeBytes = destFile.getSize();
    }

    return Result<void>::success();
}

// ============================================================================
// 最近工程管理
// ============================================================================

void ProjectSession::pushRecentProject(const juce::File& file)
{
    appPreferencesRef_.pushRecentProject(file.getFullPathName());
}

std::vector<juce::File> ProjectSession::getRecentProjects() const
{
    std::vector<juce::File> result;
    const auto paths = appPreferencesRef_.getRecentProjects();
    for (const auto& p : paths) {
        juce::File f(p);
        if (f.existsAsFile()) { result.push_back(f); }
    }
    return result;
}

void ProjectSession::clearRecentProjects()
{
    appPreferencesRef_.clearRecentProjects();
}

} // namespace OpenTune
