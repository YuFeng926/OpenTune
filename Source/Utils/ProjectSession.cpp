#include "ProjectSession.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "../PluginProcessor.h"
#include "AppLogger.h"
#include "AppPreferences.h"
#include "ProjectPersistence.h"

namespace OpenTune {

// ============================================================================
// 构造 / 析构
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
    if (!hasProjectPath()) { return "Untitled"; }
    return currentProjectFile_.getFileNameWithoutExtension();
}

// ============================================================================
// 脏状态
// ============================================================================

bool ProjectSession::isDirty() const noexcept { return dirty_; }

void ProjectSession::markDirty() { dirty_ = true; }

void ProjectSession::clearDirty() { dirty_ = false; }

// ============================================================================
// 快照抓取
// ============================================================================

ProjectSnapshot ProjectSession::captureSnapshot() const
{
    ProjectSnapshot snap;

    // Header
    snap.header.projectFormatVersion = ProjectPersistence::kCurrentProjectFormatVersion;
    snap.header.appVersion = "1.5.0";
    snap.header.projectName = getProjectName();

    // 工程身份固化：首次保存时生成，后续保存复用
    if (cachedProjectId_.isEmpty()) {
        cachedProjectId_ = ProjectSnapshot::generateProjectId();
        cachedCreatedAt_ = ProjectSnapshot::generateTimestamp();
    }
    snap.header.projectId = cachedProjectId_;
    snap.header.createdAt = cachedCreatedAt_;
    snap.header.lastSavedAt = ProjectSnapshot::generateTimestamp();

    // Sources (from SourceStore)
    auto* sourceStore = processorRef_.getSourceStore();
    if (sourceStore) {
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
            // Compute content hash from audio data
            if (srcSnap.audioBuffer && srcSnap.audioBuffer->getNumSamples() > 0) {
                const auto numSamples = srcSnap.audioBuffer->getNumSamples();
                const auto numChannels = srcSnap.audioBuffer->getNumChannels();
                // Simple hash: combine sample count, channel count, and first N samples
                juce::int64 hash = numSamples ^ (static_cast<juce::int64>(numChannels) << 32);
                const int sampleCount = std::min(numSamples, 1024);
                for (int ch = 0; ch < numChannels; ++ch) {
                    const auto* data = srcSnap.audioBuffer->getReadPointer(ch);
                    for (int i = 0; i < sampleCount; ++i) {
                        hash = hash * 31 + static_cast<juce::int64>(data[i] * 1000000.0f);
                    }
                }
                entry.contentHash = juce::String::toHexString(hash);
            }
            entry.fileSizeBytes = 0; // Unknown until file copy

            snap.sources.push_back(entry);
        }
    }

    // Materializations (from MaterializationStore)
    auto* matStore = processorRef_.getMaterializationStore();
    if (matStore) {
        const auto matIds = matStore->getAllActiveMaterializationIds();
        for (auto mid : matIds) {
            MaterializationStore::MaterializationSnapshot matSnap;
            if (!matStore->getSnapshot(mid, matSnap)) { continue; }

            ProjectMaterializationEntry entry;
            entry.materializationId = matSnap.materializationId;
            entry.sourceId = matSnap.sourceId;
            entry.retired = false;
            entry.renderRevision = matSnap.renderRevision;
            entry.lineageParentMaterializationId = matSnap.lineageParentMaterializationId;
            entry.sourceWindow = matSnap.sourceWindow;
            entry.detectedKey = matSnap.detectedKey;
            entry.notes = matSnap.notes;

            // Extract corrected segments from pitch curve
            if (matSnap.pitchCurve) {
                auto snap = matSnap.pitchCurve->getSnapshot();
                const auto& segments = snap->getCorrectedSegments();
                for (const auto& seg : segments) {
                    ProjectMaterializationEntry::SegmentEntry segEntry;
                    segEntry.startFrame = seg.startFrame;
                    segEntry.endFrame = seg.endFrame;
                    segEntry.source = static_cast<uint8_t>(seg.source);
                    segEntry.retuneSpeed = seg.retuneSpeed;
                    segEntry.vibratoDepth = seg.vibratoDepth;
                    segEntry.vibratoRate = seg.vibratoRate;
                    // F0 data not serialized (reconstructed on load)
                    entry.correctedSegments.push_back(segEntry);
                }
            }

            // TimeGrid
            if (matSnap.timeGrid) {
                entry.timeGrid.revision = matSnap.timeGridRevision;
                for (const auto& handle : matSnap.timeGrid->handles()) {
                    ProjectMaterializationEntry::TimeGridEntry::HandleEntry he;
                    he.id = static_cast<int>(handle.id);
                    he.kind = static_cast<uint8_t>(handle.kind);
                    he.sourceSeconds = handle.source_seconds;
                    he.outputSeconds = handle.output_seconds;
                    he.confidence = static_cast<float>(static_cast<uint8_t>(handle.confidence));
                    he.isUserAdded = (handle.kind == HandleKind::UserAdded);
                    entry.timeGrid.handles.push_back(he);
                }
            }

            snap.materializations.push_back(entry);
            }
        }

    // Tracks and Placements (from StandaloneArrangement)
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement) {
        const int numTracks = arrangement->getNumTracks();
        for (int trackId = 0; trackId < numTracks; ++trackId) {
            ProjectTrackEntry trackEntry;
            trackEntry.trackId = trackId;
            trackEntry.gain = arrangement->getTrackVolume(trackId);
            trackEntry.mute = arrangement->isTrackMuted(trackId);
            trackEntry.solo = arrangement->isTrackSolo(trackId);

            const int numPlacements = arrangement->getNumPlacements(trackId);
            for (int pi = 0; pi < numPlacements; ++pi) {
                StandaloneArrangement::Placement placement;
                if (!arrangement->getPlacementByIndex(trackId, pi, placement)) { continue; }
                if (placement.isRetired) { continue; }

                ProjectPlacementEntry pEntry;
                pEntry.placementId = placement.placementId;
                pEntry.materializationId = placement.materializationId;
                pEntry.mappingRevision = placement.mappingRevision;
                pEntry.timelineStartSeconds = placement.timelineStartSeconds;
                pEntry.timelineDurationSeconds = placement.durationSeconds;
                pEntry.clipGain = placement.gain;
                pEntry.fadeInDurationSeconds = placement.fadeInDuration;
                pEntry.fadeOutDurationSeconds = placement.fadeOutDuration;
                pEntry.name = placement.name;
                pEntry.colour = placement.colour;
                trackEntry.placements.push_back(pEntry);
            }

            snap.tracks.push_back(trackEntry);
        }

        // Selected track/placement for session state
        snap.recentSessionState.selectedTrackId = arrangement->getActiveTrackId();
    }

    // Settings
    snap.settings.bpm = processorRef_.getBpm();
    snap.settings.sampleRate = processorRef_.getSampleRate();

    return snap;
}

// ============================================================================
// 快照应用
// ============================================================================

static std::shared_ptr<juce::AudioBuffer<float>> loadAudioFile(const juce::File& file, double& outSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto* reader = formatManager.createReaderFor(file);
    if (!reader) { return nullptr; }

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(
        static_cast<int>(reader->numChannels),
        static_cast<int>(reader->lengthInSamples));
    reader->read(buffer.get(), 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    outSampleRate = reader->sampleRate;
    delete reader;
    return buffer;
}

Result<void> ProjectSession::applySnapshot(const ProjectSnapshot& snapshot)
{
    // 清空当前状态
    auto* sourceStore = processorRef_.getSourceStore();
    auto* matStore = processorRef_.getMaterializationStore();
    auto* arrangement = processorRef_.getStandaloneArrangement();

    if (sourceStore) { sourceStore->clear(); }
    if (matStore) { matStore->clear(); }
    if (arrangement) { arrangement->clear(); }

    processorRef_.getUndoManager().clear();

    // 获取工程文件所在目录（用于解析相对路径）
    const auto projectDir = currentProjectFile_.getParentDirectory();

    // 1. 重建 Sources
    for (const auto& srcEntry : snapshot.sources) {
        juce::File audioFile;

        // 优先从相对路径加载
        if (srcEntry.relativeMediaPath.isNotEmpty() && projectDir != juce::File{}) {
            audioFile = projectDir.getChildFile(srcEntry.relativeMediaPath);
        }

        // 回退到原始路径
        if (!audioFile.existsAsFile() && srcEntry.originalImportPath.isNotEmpty()) {
            audioFile = juce::File(srcEntry.originalImportPath);
        }

        double loadedSampleRate = 0.0;
        std::shared_ptr<juce::AudioBuffer<float>> audioBuffer;

        if (audioFile.existsAsFile()) {
            audioBuffer = loadAudioFile(audioFile, loadedSampleRate);
        }

        if (!audioBuffer) {
            AppLogger::log("ProjectSession: Failed to load audio for source "
                + juce::String(srcEntry.sourceId) + " (" + srcEntry.displayName + "), skipping");
            continue;
        }

        SourceStore::CreateSourceRequest req;
        req.displayName = srcEntry.displayName;
        req.audioBuffer = audioBuffer;
        req.sampleRate = loadedSampleRate > 0.0 ? loadedSampleRate : srcEntry.sampleRate;

        sourceStore->createSource(req, srcEntry.sourceId);
    }

    // 2. 重建 Materializations
    for (const auto& matEntry : snapshot.materializations) {
        // Get source buffer for this materialization
        std::shared_ptr<const juce::AudioBuffer<float>> sourceBuf;
        if (sourceStore && !sourceStore->getAudioBuffer(matEntry.sourceId, sourceBuf)) {
            AppLogger::log("ProjectSession: Source " + juce::String(matEntry.sourceId)
                + " not found for materialization " + juce::String(matEntry.materializationId) + ", skipping");
            continue;
        }

        MaterializationStore::CreateMaterializationRequest req;
        req.sourceId = matEntry.sourceId;
        req.lineageParentMaterializationId = matEntry.lineageParentMaterializationId;
        req.sourceWindow = matEntry.sourceWindow;
        req.audioBuffer = sourceBuf;
        req.detectedKey = matEntry.detectedKey;
        req.notes = matEntry.notes;
        req.renderRevision = matEntry.renderRevision;

        auto pitchCurve = std::make_shared<PitchCurve>();
        req.pitchCurve = pitchCurve;

        matStore->createMaterialization(req, matEntry.materializationId);

        // Restore HandDraw/LineAnchor correctedSegments with f0Data
        for (const auto& seg : matEntry.correctedSegments) {
            if (!seg.f0Data.empty()) {
                pitchCurve->setManualCorrectionRange(
                    seg.startFrame, seg.endFrame, seg.f0Data,
                    static_cast<CorrectedSegment::Source>(seg.source));
            }
        }

        // Restore TimeGrid
        if (!matEntry.timeGrid.handles.empty()) {
            std::vector<TimeHandle> handles;
            for (const auto& he : matEntry.timeGrid.handles) {
                TimeHandle th;
                th.id = static_cast<uint64_t>(he.id);
                th.kind = static_cast<HandleKind>(he.kind);
                th.source_seconds = he.sourceSeconds;
                th.output_seconds = he.outputSeconds;
                th.confidence = static_cast<Confidence>(static_cast<uint8_t>(he.confidence));
                // isUserAdded is derived from kind (HandleKind::UserAdded), no separate field on TimeHandle
                handles.push_back(th);
            }
            auto tgSnapshot = TimeGridSnapshot::makeFromHandles(
                std::move(handles), matEntry.timeGrid.revision);
            matStore->setTimeGrid(matEntry.materializationId, tgSnapshot);
        }
    }

    // 3. 重建 Tracks & Placements
    if (arrangement) {
        for (const auto& trackEntry : snapshot.tracks) {
            const int trackId = trackEntry.trackId;
            arrangement->setTrackVolume(trackId, trackEntry.gain);
            arrangement->setTrackMuted(trackId, trackEntry.mute);
            arrangement->setTrackSolo(trackId, trackEntry.solo);

            for (const auto& pEntry : trackEntry.placements) {
                // Verify materialization exists
                if (matStore && !matStore->containsMaterialization(pEntry.materializationId)) {
                    AppLogger::log("ProjectSession: Materialization "
                        + juce::String(pEntry.materializationId)
                        + " not found for placement " + juce::String(pEntry.placementId) + ", skipping");
                    continue;
                }

                StandaloneArrangement::Placement placement;
                placement.placementId = pEntry.placementId;
                placement.materializationId = pEntry.materializationId;
                placement.mappingRevision = pEntry.mappingRevision;
                placement.timelineStartSeconds = pEntry.timelineStartSeconds;
                placement.durationSeconds = pEntry.timelineDurationSeconds;
                placement.gain = pEntry.clipGain;
                placement.fadeInDuration = pEntry.fadeInDurationSeconds;
                placement.fadeOutDuration = pEntry.fadeOutDurationSeconds;
                placement.name = pEntry.name;
                placement.colour = pEntry.colour;

                arrangement->insertPlacement(trackId, placement);
            }
        }

        arrangement->setActiveTrack(snapshot.recentSessionState.selectedTrackId);
    }

    // 恢复工程设置
    processorRef_.setBpm(snapshot.settings.bpm);

    return Result<void>::success();
}

// ============================================================================
// 工程操作
// ============================================================================

Result<void> ProjectSession::openProject(const juce::File& file)
{
    ProjectPersistence persistence;
    auto result = persistence.readProjectFile(file);
    if (!result.ok()) {
        return Result<void>::failure(result.error());
    }

    currentProjectFile_ = file;
    auto applyResult = applySnapshot(result.value());
    if (!applyResult.ok()) {
        return applyResult;
    }

    clearDirty();
    pushRecentProject(file);
    return Result<void>::success();
}

Result<void> ProjectSession::saveProject()
{
    if (!hasProjectPath()) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::InvalidParameter,
                "No project path set; use Save Project As... first"));
    }

    auto snapshot = captureSnapshot();
    auto mediaResult = copyMediaToProjectDirectory(snapshot);
    if (!mediaResult.ok()) {
        return mediaResult;
    }

    ProjectPersistence persistence;
    if (!persistence.writeProjectFile(snapshot, currentProjectFile_)) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                ("Failed to write project file: " + currentProjectFile_.getFullPathName()).toStdString()));
    }

    clearDirty();
    pushRecentProject(currentProjectFile_);
    return Result<void>::success();
}

Result<void> ProjectSession::saveProjectAs(const juce::File& file)
{
    auto oldFile = currentProjectFile_;
    currentProjectFile_ = file;
    auto result = saveProject();
    if (!result.ok()) {
        currentProjectFile_ = oldFile;
    }
    return result;
}

void ProjectSession::newProject()
{
    auto* sourceStore = processorRef_.getSourceStore();
    auto* matStore = processorRef_.getMaterializationStore();
    auto* arrangement = processorRef_.getStandaloneArrangement();

    if (sourceStore) { sourceStore->clear(); }
    if (matStore) { matStore->clear(); }
    if (arrangement) { arrangement->clear(); }

    processorRef_.getUndoManager().clear();

    cachedProjectId_ = {};
    cachedCreatedAt_ = {};
    currentProjectFile_ = juce::File{};
    clearDirty();
}

// ============================================================================
// 媒体复制
// ============================================================================

juce::File ProjectSession::getProjectMediaDirectory() const
{
    if (!hasProjectPath()) { return {}; }
    return currentProjectFile_.getParentDirectory().getChildFile(kMediaDirectoryName);
}

juce::String ProjectSession::generateMediaFileName(const ProjectSourceEntry& source) const
{
    // Stable naming: content hash + display name extension
    juce::String base = source.contentHash.isNotEmpty()
        ? source.contentHash.substring(0, 16)
        : juce::String(source.sourceId);

    // Determine extension from original import path or default to .wav
    juce::String extension = ".wav";
    if (source.originalImportPath.isNotEmpty()) {
        extension = juce::File(source.originalImportPath).getFileExtension();
        if (extension.isEmpty()) { extension = ".wav"; }
    }

    return base + extension;
}

Result<void> ProjectSession::copyMediaToProjectDirectory(ProjectSnapshot& snapshot)
{
    if (!hasProjectPath()) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::InvalidParameter, "Cannot copy media: no project path set"));
    }

    const auto mediaDir = getProjectMediaDirectory();
    if (!mediaDir.createDirectory().wasOk()) {
        return Result<void>::failure(
            Error::fromCode(ErrorCode::UnknownError,
                ("Failed to create media directory: " + mediaDir.getFullPathName()).toStdString()));
    }

    for (auto& srcEntry : snapshot.sources) {
        juce::File sourceFile;

        if (srcEntry.originalImportPath.isNotEmpty()) {
            sourceFile = juce::File(srcEntry.originalImportPath);
        }

        if (!sourceFile.existsAsFile()) {
            AppLogger::log("ProjectSession: Source file not found for copy: "
                + srcEntry.originalImportPath);
            continue;
        }

        const auto destFileName = generateMediaFileName(srcEntry);
        const auto destFile = mediaDir.getChildFile(destFileName);

        if (!destFile.existsAsFile() || sourceFile.getLastModificationTime() > destFile.getLastModificationTime()) {
            if (!sourceFile.copyFileTo(destFile)) {
                return Result<void>::failure(
                    Error::fromCode(ErrorCode::UnknownError,
                        ("Failed to copy media file: " + sourceFile.getFullPathName()
                         + " -> " + destFile.getFullPathName()).toStdString()));
            }
        }

        srcEntry.relativeMediaPath = juce::String(kMediaDirectoryName) + "/" + destFileName;
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
