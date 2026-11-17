// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StandaloneMediaOperations.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "SourceStore.h"
#include "Inference/RenderCache.h"
#include "Render/ContentRenderService.h"
#include "Render/PlaybackReadSource.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/PlacementFade.h"
#include "Utils/PlaybackAudioReader.h"
#include "Utils/TimeCoordinate.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#endif
#endif

namespace OpenTune::StandaloneMediaOperations {

double appendStartSeconds(const OpenTuneAudioProcessor& processor, int trackId)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    const auto* repository = processor.getStandaloneContentRepository();
    double end = 0.0;
    const int count = arrangement != nullptr ? arrangement->getNumPlacements(trackId) : 0;
    for (int index = 0; index < count; ++index) {
        StandaloneArrangement::Placement placement;
        if (!arrangement->getPlacementByIndex(trackId, index, placement) || !placement.contentKey.isValid())
            continue;
        const auto* clip = repository != nullptr ? repository->findClip(placement.contentKey) : nullptr;
        if (clip == nullptr || clip->content().audioBuffer == nullptr)
            continue;
        end = std::max(end, placement.timelineEndSeconds());
    }
    return end;
}

OpenTuneAudioProcessor::ImportPlacement planImportPlacement(
    const OpenTuneAudioProcessor& processor, const ImportPlacementRequest& request)
{
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = request.layout == ImportLayout::SeparateTracks
                            ? request.firstTrackId + static_cast<int>(request.itemIndex)
                            : request.firstTrackId;
    if (request.layout == ImportLayout::Sequential && request.sequentialCursorSec) {
        placement.timelineStartSeconds = *request.sequentialCursorSec;
    } else if (request.startTimelineSec) {
        placement.timelineStartSeconds = *request.startTimelineSec;
    } else if (request.layout == ImportLayout::SeparateTracks) {
        placement.timelineStartSeconds = 0.0;
    } else {
        placement.timelineStartSeconds = appendStartSeconds(processor, placement.trackId);
    }
    return placement;
}

namespace {

constexpr int kExportNumChannels = 1;
constexpr int kExportMasterNumChannels = 2;
constexpr int kExportBitsPerSample = static_cast<int>(sizeof(float) * 8);

bool hasPlaybackAudio(OpenTuneAudioProcessor& processor, const ContentKey& key,
                      PlaybackReadSource* source = nullptr)
{
    const auto* service = processor.getContentRenderService();
    PlaybackReadSource read;
    if (service == nullptr || !service->getPlaybackReadSource(key, read) || !read.hasAudio())
        return false;
    if (source != nullptr)
        *source = std::move(read);
    return true;
}

ExportClip exportClipOf(const StandaloneArrangement::Placement& placement, int trackId, float trackVolume)
{
    ExportClip clip;
    clip.trackId = trackId;
    clip.placementId = placement.placementId;
    clip.trackVolume = trackVolume;
    clip.playback.contentKey = placement.contentKey;
    clip.playback.timelineStartSeconds = placement.timelineStartSeconds;
    clip.playback.durationSeconds = placement.durationSeconds;
    clip.playback.clipInSeconds = placement.clipInSeconds;
    clip.playback.gain = placement.gain;
    clip.playback.fadeInDuration = placement.fadeInDuration;
    clip.playback.fadeOutDuration = placement.fadeOutDuration;
    return clip;
}

std::vector<ExportClip> trackExportClips(const StandaloneArrangement& arrangement, int trackId)
{
    std::vector<ExportClip> clips;
    const float trackVolume = arrangement.getTrackVolume(trackId);
    const int count = arrangement.getNumPlacements(trackId);
    clips.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        StandaloneArrangement::Placement placement;
        if (arrangement.getPlacementByIndex(trackId, index, placement))
            clips.push_back(exportClipOf(placement, trackId, trackVolume));
    }
    return clips;
}

void buildExportClips(OpenTuneAudioProcessor& processor, std::vector<ExportClip>& clips)
{
    clips.erase(
        std::remove_if(clips.begin(), clips.end(), [&processor](ExportClip& clip) {
            if (clip.playback.hasPlayableSource())
                return false;
            PlaybackReadSource source;
            if (!hasPlaybackAudio(processor, clip.playback.contentKey, &source))
                return true;
            clip.playback.source = std::move(source);
            return false;
        }),
        clips.end());

    std::vector<StandaloneArrangement::PlaybackPlacement> playback;
    playback.reserve(clips.size());
    for (const auto& clip : clips)
        playback.push_back(clip.playback);
    StandaloneArrangement::buildPlaybackGeometry(playback, TimeCoordinate::kRenderSampleRate);
    for (std::size_t i = 0; i < clips.size(); ++i)
        clips[i].playback = std::move(playback[i]);
}

void addContents(ExportPlan& plan, const ExportClip& clip)
{
    const auto& key = clip.playback.contentKey;
    if (std::find(plan.contents.begin(), plan.contents.end(), key) == plan.contents.end())
        plan.contents.push_back(key);
}

void updateOutputEnd(ExportPlan& plan, const ExportClip& clip)
{
    plan.outputEndSec = std::max(plan.outputEndSec,
                                 clip.playback.timelineStartSeconds + clip.playback.durationSeconds);
}

ExportPlan::ContentInput contentInputOf(ContentKey key, const PlaybackReadSource& source)
{
    ExportPlan::ContentInput input;
    input.key = key;
    const auto& snapshot = source.contentSnapshot;
    if (snapshot == nullptr)
        return input;

    input.contentRevision = snapshot->contentRevision;
    input.notesRevision = snapshot->notesRevision;
    input.pitchRevision = snapshot->pitchRevision;
    input.timeGridRevision = snapshot->timeGridRevision;
    input.originalF0State = snapshot->originalF0State;
    input.hasUsableOriginalF0 = snapshot->hasUsableOriginalF0();
    input.available = source.hasAudio();
    return input;
}

void captureExportInputs(ExportPlan& plan)
{
    plan.contentInputs.clear();
    plan.contentInputs.reserve(plan.contents.size());
    for (const auto& key : plan.contents) {
        const auto clip = std::find_if(plan.clips.begin(), plan.clips.end(), [&key](const ExportClip& candidate) {
            return candidate.playback.contentKey == key;
        });
        if (clip == plan.clips.end()) {
            ExportPlan::ContentInput missing;
            missing.key = key;
            plan.contentInputs.push_back(std::move(missing));
            continue;
        }
        plan.contentInputs.push_back(contentInputOf(key, clip->playback.source));
    }
}

bool sameContentInput(const ExportPlan::ContentInput& lhs, const ExportPlan::ContentInput& rhs)
{
    return lhs.key == rhs.key && lhs.available == rhs.available
           && lhs.contentRevision == rhs.contentRevision && lhs.notesRevision == rhs.notesRevision
           && lhs.pitchRevision == rhs.pitchRevision && lhs.timeGridRevision == rhs.timeGridRevision;
}

bool sameContentInput(const ExportPlan::ContentInput& expected, ContentKey key,
                      const EditableContentSnapshot& current)
{
    const bool available = current.nativeSource.buffer != nullptr
                           && current.nativeSource.buffer->getNumSamples() > 0;
    return expected.key == key && expected.available == available
           && expected.contentRevision == current.contentRevision
           && expected.notesRevision == current.notesRevision
           && expected.pitchRevision == current.pitchRevision
           && expected.timeGridRevision == current.timeGridRevision;
}

bool isOriginalF0Completion(const ExportPlan::ContentInput& before,
                            const ExportPlan::ContentInput& after)
{
    return before.key == after.key && before.available && after.available
           && before.originalF0State != OriginalF0State::Ready
           && after.originalF0State == OriginalF0State::Ready && after.hasUsableOriginalF0
           && after.contentRevision == before.contentRevision + 1
           && after.pitchRevision == before.pitchRevision + 1
           && after.notesRevision == before.notesRevision
           && after.timeGridRevision == before.timeGridRevision;
}

bool sameExportClip(const ExportClip& lhs, const ExportClip& rhs)
{
    const auto& a = lhs.playback;
    const auto& b = rhs.playback;
    return lhs.trackId == rhs.trackId && lhs.placementId == rhs.placementId && lhs.trackVolume == rhs.trackVolume
           && a.contentKey == b.contentKey && a.timelineStartSeconds == b.timelineStartSeconds
           && a.durationSeconds == b.durationSeconds && a.clipInSeconds == b.clipInSeconds && a.gain == b.gain
           && a.fadeInDuration == b.fadeInDuration && a.fadeOutDuration == b.fadeOutDuration;
}

bool sameExportPlanMix(const ExportPlan& lhs, const ExportPlan& rhs)
{
    if (lhs.target.kind != rhs.target.kind || lhs.target.trackId != rhs.target.trackId
        || lhs.target.placementId != rhs.target.placementId || lhs.targetGone != rhs.targetGone
        || lhs.outputEndSec != rhs.outputEndSec
        || lhs.clips.size() != rhs.clips.size())
        return false;

    for (std::size_t i = 0; i < lhs.clips.size(); ++i)
        if (!sameExportClip(lhs.clips[i], rhs.clips[i]))
            return false;
    return true;
}

void renderExportPlacement(const StandaloneArrangement::PlaybackPlacement& placement,
                           float trackGain,
                           double outputOriginSeconds,
                           juce::AudioBuffer<float>& output,
                           std::int64_t outputLength)
{
    constexpr double sampleRate = TimeCoordinate::kRenderSampleRate;
    if (!placement.contentKey.isValid() || placement.durationSeconds <= 0.0 || !placement.hasPlayableView())
        return;

    const auto& geometry = placement.sampleGeometry;
    const std::int64_t outputOriginSample = PlaybackSampleGeometry::projectToSample(outputOriginSeconds * sampleRate);
    const std::int64_t destinationStart = geometry.playbackBeginSample - outputOriginSample;
    if (destinationStart >= outputLength)
        return;

    const std::int64_t placementLength = geometry.playbackEndSample - geometry.playbackBeginSample;
    const std::int64_t remaining = outputLength - destinationStart;
    const int samplesToRender = static_cast<int>(std::min(placementLength, remaining));
    if (samplesToRender <= 0)
        return;

    juce::AudioBuffer<float> placementAudio(output.getNumChannels(), samplesToRender);
    placementAudio.clear();
    const std::int64_t readStart = geometry.playbackBeginSample - geometry.offsetSamples;
    const PlaybackReadRequest request(placement.source, readStart, sampleRate, samplesToRender,
                                      placement.outputAnchorSeconds);
    const int rendered = readExportPlaybackAudio(request, placementAudio, 0);
    if (rendered <= 0)
        return;

    const float baseGain = trackGain * placement.gain;
    for (int sample = 0; sample < rendered; ++sample) {
        const std::int64_t destination = destinationStart + sample;
        if (destination < 0 || destination >= outputLength)
            continue;
        const float gain = baseGain * placementFadeGain(
            static_cast<double>(sample) / sampleRate, placement.durationSeconds,
            placement.fadeInDuration, placement.fadeOutDuration);
        for (int channel = 0; channel < output.getNumChannels(); ++channel)
            output.addSample(channel, static_cast<int>(destination), placementAudio.getSample(channel, sample) * gain);
    }
}

bool writeBufferToWav(const juce::AudioBuffer<float>& buffer,
                      const juce::File& file,
                      Language language,
                      juce::String& error)
{
    auto outputFile = file;
    if (!outputFile.hasFileExtension(".wav"))
        outputFile = outputFile.withFileExtension(".wav");
    outputFile.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(outputFile.createOutputStream());
    if (!stream) {
        error = Loc::get(language, "Create output file failed");
        return false;
    }

    std::unique_ptr<juce::OutputStream> outputStream(stream.release());
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(TimeCoordinate::kRenderSampleRate)
                             .withNumChannels(buffer.getNumChannels())
                             .withBitsPerSample(kExportBitsPerSample);
    auto writer = wav.createWriterFor(outputStream, options);
    if (!writer) {
        error = Loc::get(language, "Unable to create WAV writer");
        return false;
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

std::filesystem::path nativePath(const juce::File& file)
{
    const auto path = file.getFullPathName();
#if defined(_WIN32)
    return std::filesystem::path(path.toWideCharPointer());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

std::optional<bool> sameFile(const juce::File& lhs, const juce::File& rhs)
{
    const auto a = nativePath(lhs);
    const auto b = nativePath(rhs);
    std::error_code error;
    const bool aExists = std::filesystem::exists(a, error);
    if (error)
        return std::nullopt;
    const bool bExists = std::filesystem::exists(b, error);
    if (error)
        return std::nullopt;
    if (aExists && bExists) {
        const bool same = std::filesystem::equivalent(a, b, error);
        return error ? std::nullopt : std::optional<bool>(same);
    }
    if (aExists != bExists)
        return false;

    const bool aParentExists = std::filesystem::exists(a.parent_path(), error);
    if (error)
        return std::nullopt;
    const bool bParentExists = std::filesystem::exists(b.parent_path(), error);
    if (error)
        return std::nullopt;
    if (!aParentExists || !bParentExists)
        return false;
    const bool sameParent = std::filesystem::equivalent(a.parent_path(), b.parent_path(), error);
    if (error)
        return std::nullopt;
    if (!sameParent)
        return false;
#if defined(_WIN32)
    return lhs.getFileName().equalsIgnoreCase(rhs.getFileName());
#else
    return a.filename() == b.filename();
#endif
}

enum class LoadedMediaStatus {
    Clear,
    Loaded,
    IdentityFailed
};

LoadedMediaStatus checkLoadedMedia(OpenTuneAudioProcessor& processor, const juce::File& destination)
{
    const auto* sources = processor.getSourceStore();
    if (sources == nullptr)
        return LoadedMediaStatus::Clear;

    auto ids = sources->getAllActiveSourceIds();
    const auto retired = sources->getRetiredSourceIds();
    ids.insert(ids.end(), retired.begin(), retired.end());
    for (const auto id : ids) {
        SourceStore::SourceSnapshot source;
        if (!sources->getSnapshot(id, source))
            continue;
        if (source.sourceFilePath.isEmpty())
            continue;
        const auto same = sameFile(destination, juce::File(source.sourceFilePath));
        if (!same)
            return LoadedMediaStatus::IdentityFailed;
        if (*same)
            return LoadedMediaStatus::Loaded;
    }
    return LoadedMediaStatus::Clear;
}

enum class AtomicCommitResult {
    Done,
    TargetExists,
    Failed
};

AtomicCommitResult atomicCommit(const juce::File& temporary, const juce::File& destination, bool replace)
{
#if defined(_WIN32)
    const auto from = temporary.getFullPathName();
    const auto to = destination.getFullPathName();
    if (MoveFileExW(from.toWideCharPointer(), to.toWideCharPointer(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
        return AtomicCommitResult::Done;
    const auto error = GetLastError();
    if (!replace && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS))
        return AtomicCommitResult::TargetExists;
    return AtomicCommitResult::Failed;
#else
    const auto from = temporary.getFullPathName().toStdString();
    const auto to = destination.getFullPathName().toStdString();
    if (replace)
        return std::rename(from.c_str(), to.c_str()) == 0 ? AtomicCommitResult::Done
                                                          : AtomicCommitResult::Failed;
#if defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
        return AtomicCommitResult::Done;
    return errno == EEXIST ? AtomicCommitResult::TargetExists : AtomicCommitResult::Failed;
#else
    if (::link(from.c_str(), to.c_str()) != 0)
        return errno == EEXIST ? AtomicCommitResult::TargetExists : AtomicCommitResult::Failed;
    ::unlink(from.c_str());
    return AtomicCommitResult::Done;
#endif
#endif
}

bool readWavInfo(const juce::File& file, ExportWavInfo& info)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
        return false;
    info.channelCount = static_cast<int>(reader->numChannels);
    info.sampleRateHz = reader->sampleRate;
    info.frameCount = reader->lengthInSamples;
    info.sampleFormat = (reader->usesFloatingPointData ? "float" : "int") + std::to_string(reader->bitsPerSample);
    return info.channelCount > 0 && info.sampleRateHz > 0.0 && info.frameCount >= 0;
}

} // namespace

ExportPlan buildExportPlan(OpenTuneAudioProcessor& processor, const ExportTarget& target)
{
    ExportPlan plan;
    plan.target = target;
    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr) {
        plan.targetGone = target.kind == ExportKind::Clip;
        return plan;
    }

    switch (target.kind) {
        case ExportKind::Clip: {
            StandaloneArrangement::Placement placement;
            int trackId = -1;
            for (int track = 0; track < arrangement->getNumTracks() && trackId < 0; ++track)
                if (arrangement->getPlacementById(track, target.placementId, placement) && !placement.isRetired)
                    trackId = track;
            if (trackId < 0) {
                plan.targetGone = true;
                break;
            }
            plan.clipTrackId = trackId;
            plan.clipStartSec = placement.timelineStartSeconds;
            auto clips = trackExportClips(*arrangement, trackId);
            buildExportClips(processor, clips);
            const auto clip = std::find_if(clips.begin(), clips.end(), [&target](const ExportClip& candidate) {
                return candidate.placementId == target.placementId;
            });
            if (clip != clips.end()) {
                plan.clips.push_back(*clip);
                addContents(plan, *clip);
                updateOutputEnd(plan, *clip);
            }
            break;
        }
        case ExportKind::Track: {
            auto clips = trackExportClips(*arrangement, target.trackId);
            buildExportClips(processor, clips);
            for (const auto& clip : clips) {
                plan.clips.push_back(clip);
                addContents(plan, clip);
                updateOutputEnd(plan, clip);
            }
            break;
        }
        case ExportKind::Master: {
            const auto snapshot = arrangement->loadPlaybackSnapshot();
            if (!snapshot)
                break;
            const int count = std::min(arrangement->getNumTracks(), static_cast<int>(snapshot->tracks.size()));
            for (int trackId = 0; trackId < count; ++trackId) {
                const auto& track = snapshot->tracks[static_cast<std::size_t>(trackId)];
                std::vector<ExportClip> clips;
                clips.reserve(track.placements.size());
                for (const auto& placement : track.placements) {
                    ExportClip clip;
                    clip.trackId = trackId;
                    clip.trackVolume = track.volume;
                    clip.playback = placement;
                    clips.push_back(std::move(clip));
                }
                buildExportClips(processor, clips);
                for (const auto& clip : clips) {
                    updateOutputEnd(plan, clip);
                    if (snapshot->anySoloed ? !track.isSolo : track.isMuted)
                        continue;
                    plan.clips.push_back(clip);
                    addContents(plan, clip);
                }
            }
            break;
        }
    }
    captureExportInputs(plan);
    return plan;
}

namespace {

ExportPlanStatus validateExportPlanInputs(OpenTuneAudioProcessor& processor,
                                          const ExportPlan& plan,
                                          bool allowOriginalF0Completion,
                                          ExportPlan* refreshedPlan)
{
    if (plan.targetGone)
        return ExportPlanStatus::TargetGone;

    auto currentPlan = buildExportPlan(processor, plan.target);
    if (currentPlan.targetGone)
        return ExportPlanStatus::TargetGone;

    if (!sameExportPlanMix(plan, currentPlan)
        || plan.contentInputs.size() != currentPlan.contentInputs.size())
        return ExportPlanStatus::Changed;

    const auto* repository = processor.getStandaloneContentRepository();
    for (std::size_t i = 0; i < plan.contentInputs.size(); ++i) {
        const auto& expected = plan.contentInputs[i];
        if (!expected.available)
            return ExportPlanStatus::ContentUnavailable;
        const auto* clip = repository != nullptr ? repository->findClip(expected.key) : nullptr;
        if (clip == nullptr || clip->isRetired())
            return ExportPlanStatus::ContentUnavailable;
        const auto snapshot = processor.getContentSnapshot(expected.key);
        if (snapshot == nullptr)
            return ExportPlanStatus::ContentUnavailable;
        if (!currentPlan.contentInputs[i].available)
            return ExportPlanStatus::ContentUnavailable;
        const auto& current = currentPlan.contentInputs[i];
        const bool unchanged = sameContentInput(expected, current)
                               && sameContentInput(expected, expected.key, *snapshot);
        const bool completedAnalysis = allowOriginalF0Completion
                                       && isOriginalF0Completion(expected, current)
                                       && sameContentInput(current, expected.key, *snapshot);
        if (!unchanged && !completedAnalysis)
            return ExportPlanStatus::Changed;
    }

    if (refreshedPlan != nullptr)
        *refreshedPlan = std::move(currentPlan);
    return ExportPlanStatus::Current;
}

} // namespace

ExportPlanStatus validateExportPlan(OpenTuneAudioProcessor& processor, const ExportPlan& plan)
{
    return validateExportPlanInputs(processor, plan, false, nullptr);
}

namespace {

ExportPlanStatus refreshExportPlanForReadiness(OpenTuneAudioProcessor& processor, ExportPlan& plan)
{
    return validateExportPlanInputs(processor, plan, true, &plan);
}

} // namespace

bool writeExportWav(const ExportPlan& plan,
                    const juce::File& file,
                    Language language,
                    juce::String& error)
{
    error.clear();
    if (plan.targetGone) {
        error = Loc::get(language, "Placement audio is unavailable");
        return false;
    }
    if (plan.clips.empty()) {
        error = Loc::get(language, "No audio clips to export");
        return false;
    }

    constexpr double sampleRate = TimeCoordinate::kRenderSampleRate;
    const bool clip = plan.target.kind == ExportKind::Clip;
    const bool master = plan.target.kind == ExportKind::Master;
    const std::int64_t outputLength = clip
                                          ? TimeCoordinate::secondsToSamplesCeil(
                                                plan.clips.front().playback.durationSeconds, sampleRate)
                                          : TimeCoordinate::secondsToSamplesCeil(plan.outputEndSec, sampleRate);
    if (outputLength <= 0) {
        error = Loc::get(language, "Total audio length is zero or invalid");
        return false;
    }

    juce::AudioBuffer<float> output(master ? kExportMasterNumChannels : kExportNumChannels,
                                    static_cast<int>(outputLength));
    output.clear();
    const double outputOrigin = clip ? plan.clipStartSec : 0.0;
    for (const auto& clipPlan : plan.clips)
        renderExportPlacement(clipPlan.playback, clipPlan.trackVolume, outputOrigin, output, outputLength);
    return writeBufferToWav(output, file, language, error);
}

struct ExportOperation::State {
    State(OpenTuneAudioProcessor& processorIn, ExportTarget targetIn, juce::File destinationIn,
          ExportDestinationPolicy policyIn, bool allowDry, Language languageIn,
          std::function<void()> busyChangedIn)
        : processor(processorIn)
        , target(targetIn)
        , destination(destinationIn.hasFileExtension(".wav")
                          ? std::move(destinationIn)
                          : destinationIn.withFileExtension(".wav"))
        , policy(policyIn)
        , language(languageIn)
        , readiness(allowDry)
        , busyChanged(std::move(busyChangedIn))
    {}

    OpenTuneAudioProcessor& processor;
    ExportTarget target;
    juce::File destination;
    ExportDestinationPolicy policy;
    Language language;
    ExportReadinessTracker readiness;
    std::function<void()> busyChanged;
    std::function<void()> wake;
    ExportPlan plan;
    ExportOperationProgress progress;
    ExportWavInfo wavInfo;
    juce::String error;
    juce::File temporary;
    std::thread worker;
    std::atomic<bool> workerDone{false};
    bool workerSucceeded = false;
    bool admitted = false;

    void releaseAdmission()
    {
        if (!admitted)
            return;
        admitted = false;
        processor.finishExport();
        if (busyChanged)
            busyChanged();
    }

    void removeTemporary()
    {
        if (temporary != juce::File())
            temporary.deleteFile();
        temporary = juce::File();
    }

    void joinWorker()
    {
        if (worker.joinable())
            worker.join();
    }

    void fail(ExportFailure failure)
    {
        progress.state = ExportOperationState::Failed;
        progress.failure = failure;
        joinWorker();
        removeTemporary();
        releaseAdmission();
    }

    void cancel()
    {
        progress.state = ExportOperationState::Cancelled;
        joinWorker();
        removeTemporary();
        releaseAdmission();
    }
};

ExportOperation::ExportOperation(OpenTuneAudioProcessor& processor,
                                 ExportTarget target,
                                 juce::File destination,
                                 ExportDestinationPolicy destinationPolicy,
                                 bool allowDry,
                                 Language language,
                                 std::function<void()> busyChanged)
    : state_(std::make_unique<State>(processor, target, std::move(destination), destinationPolicy,
                                     allowDry, language, std::move(busyChanged)))
{}

ExportOperation::~ExportOperation()
{
    state_->joinWorker();
    state_->removeTemporary();
    state_->releaseAdmission();
}

ExportFailure ExportOperation::start()
{
    auto& state = *state_;
    if (state.progress.state != ExportOperationState::NotStarted)
        return state.progress.failure;
    if (!state.processor.tryBeginExport()) {
        state.progress.state = ExportOperationState::Failed;
        state.progress.failure = ExportFailure::Busy;
        return state.progress.failure;
    }
    state.admitted = true;
    if (state.busyChanged)
        state.busyChanged();

    state.plan = buildExportPlan(state.processor, state.target);
    if (state.plan.targetGone) {
        state.fail(ExportFailure::TargetGone);
        return state.progress.failure;
    }
    if (state.plan.clips.empty()) {
        state.fail(ExportFailure::NothingToExport);
        return state.progress.failure;
    }

    switch (checkLoadedMedia(state.processor, state.destination)) {
        case LoadedMediaStatus::Clear:
            break;
        case LoadedMediaStatus::Loaded:
            state.fail(ExportFailure::LoadedMedia);
            return state.progress.failure;
        case LoadedMediaStatus::IdentityFailed:
            state.fail(ExportFailure::FileIdentity);
            return state.progress.failure;
    }
    if (!state.destination.getParentDirectory().isDirectory()) {
        state.fail(ExportFailure::DestinationUnavailable);
        return state.progress.failure;
    }
    if (state.policy == ExportDestinationPolicy::FailIfExists && state.destination.exists()) {
        state.fail(ExportFailure::TargetExists);
        return state.progress.failure;
    }

    state.progress.state = ExportOperationState::Waiting;
    state.progress.totalContents = static_cast<int>(state.plan.contents.size());
    return ExportFailure::None;
}

ExportOperationProgress ExportOperation::advance(bool cancelRequested, std::function<bool()> beforeCommit)
{
    auto& state = *state_;
    if (state.progress.state == ExportOperationState::NotStarted
        || state.progress.state == ExportOperationState::Succeeded
        || state.progress.state == ExportOperationState::Failed
        || state.progress.state == ExportOperationState::Cancelled)
        return state.progress;

    if (state.progress.state == ExportOperationState::Waiting) {
        if (cancelRequested) {
            state.cancel();
            return state.progress;
        }

        switch (refreshExportPlanForReadiness(state.processor, state.plan)) {
            case ExportPlanStatus::Current:
                break;
            case ExportPlanStatus::Changed:
                state.fail(ExportFailure::Changed);
                return state.progress;
            case ExportPlanStatus::TargetGone:
                state.fail(ExportFailure::TargetGone);
                return state.progress;
            case ExportPlanStatus::ContentUnavailable:
                state.fail(ExportFailure::ContentUnavailable);
                return state.progress;
        }

        int ready = 0;
        std::vector<ExportDryContent> dryContents;
        for (const auto& key : state.plan.contents) {
            const auto readiness = state.readiness.classify(state.processor, key);
            if (readiness.state == ExportReadiness::State::Fail) {
                state.fail(readiness.failure);
                return state.progress;
            }
            if (readiness.state == ExportReadiness::State::Ready
                || readiness.state == ExportReadiness::State::Dry) {
                ++ready;
                if (readiness.state == ExportReadiness::State::Dry)
                    dryContents.push_back({key, readiness.dryReason});
            }
        }
        state.progress.readyContents = ready;
        state.progress.totalContents = static_cast<int>(state.plan.contents.size());
        if (ready != state.progress.totalContents)
            return state.progress;

        switch (refreshExportPlanForReadiness(state.processor, state.plan)) {
            case ExportPlanStatus::Current:
                break;
            case ExportPlanStatus::Changed:
                state.fail(ExportFailure::Changed);
                return state.progress;
            case ExportPlanStatus::TargetGone:
                state.fail(ExportFailure::TargetGone);
                return state.progress;
            case ExportPlanStatus::ContentUnavailable:
                state.fail(ExportFailure::ContentUnavailable);
                return state.progress;
        }

        state.progress.dryContents = std::move(dryContents);
        const auto directory = state.destination.getParentDirectory();
        if (!directory.isDirectory()) {
            state.fail(ExportFailure::DestinationUnavailable);
            return state.progress;
        }
        state.temporary = directory.getChildFile(
            "." + state.destination.getFileNameWithoutExtension() + "."
            + juce::Uuid().toString() + ".export.wav");

        try {
            auto planForWrite = state.plan;
            const auto temporary = state.temporary;
            const auto language = state.language;
            const auto wake = state.wake;
            state.workerDone.store(false, std::memory_order_relaxed);
            state.worker = std::thread([&state, plan = std::move(planForWrite), temporary, language, wake] {
                try {
                    state.workerSucceeded = writeExportWav(plan, temporary, language, state.error)
                                            && readWavInfo(temporary, state.wavInfo);
                } catch (...) {
                    state.workerSucceeded = false;
                    state.error = Loc::get(language, "Audio export failed");
                }
                state.workerDone.store(true, std::memory_order_release);
                if (wake)
                    wake();
            });
        } catch (...) {
            state.fail(ExportFailure::WriteFailed);
            return state.progress;
        }
        state.progress.state = ExportOperationState::Writing;
        return state.progress;
    }

    if (!state.workerDone.load(std::memory_order_acquire))
        return state.progress;
    state.joinWorker();
    if (cancelRequested) {
        state.cancel();
        return state.progress;
    }
    if (!state.workerSucceeded) {
        state.fail(ExportFailure::WriteFailed);
        return state.progress;
    }
    if (validateExportPlan(state.processor, state.plan) != ExportPlanStatus::Current) {
        state.fail(ExportFailure::Changed);
        return state.progress;
    }

    int lastCandidate = state.policy == ExportDestinationPolicy::Rename ? 999 : 1;
    if (beforeCommit && !beforeCommit()) {
        state.cancel();
        return state.progress;
    }
    for (int number = 1; number <= lastCandidate; ++number) {
        auto candidate = state.destination;
        if (number > 1) {
            candidate = state.destination.getSiblingFile(
                state.destination.getFileNameWithoutExtension() + " (" + juce::String(number) + ").wav");
        }
        switch (checkLoadedMedia(state.processor, candidate)) {
            case LoadedMediaStatus::Clear:
                break;
            case LoadedMediaStatus::Loaded:
                state.fail(ExportFailure::LoadedMedia);
                return state.progress;
            case LoadedMediaStatus::IdentityFailed:
                state.fail(ExportFailure::FileIdentity);
                return state.progress;
        }

        const auto commit = atomicCommit(state.temporary, candidate,
                                         state.policy == ExportDestinationPolicy::Overwrite);
        if (commit == AtomicCommitResult::Done) {
            state.destination = candidate;
            state.temporary.deleteFile();
            if (!state.temporary.exists())
                state.temporary = juce::File();
            state.progress.state = ExportOperationState::Succeeded;
            state.releaseAdmission();
            return state.progress;
        }
        if (commit == AtomicCommitResult::TargetExists
            && state.policy == ExportDestinationPolicy::Rename)
            continue;
        state.fail(commit == AtomicCommitResult::TargetExists ? ExportFailure::TargetExists
                                                              : ExportFailure::WriteFailed);
        return state.progress;
    }
    state.fail(ExportFailure::TargetExists);
    return state.progress;
}

void ExportOperation::setWakeCallback(std::function<void()> wake)
{
    state_->wake = std::move(wake);
}

const ExportPlan& ExportOperation::plan() const noexcept { return state_->plan; }

const ExportWavInfo& ExportOperation::wavInfo() const noexcept { return state_->wavInfo; }

const juce::File& ExportOperation::destination() const noexcept { return state_->destination; }

const juce::String& ExportOperation::error() const noexcept { return state_->error; }

namespace {

bool hasVoicedFrame(const EditableContentSnapshot& snapshot)
{
    if (!snapshot.hasUsableOriginalF0())
        return false;
    const auto& f0 = snapshot.pitchCurve->getOriginalF0();
    return std::any_of(f0.begin(), f0.end(), [](float hz) { return hz > 0.0f; });
}

bool needsRender(const EditableContentSnapshot& snapshot)
{
    if (!snapshot.pitchShiftSettings.isIdentity())
        return true;
    if (snapshot.pitchCurve != nullptr && snapshot.pitchCurve->hasCorrectionLayer())
        return true;
    return std::any_of(snapshot.notes.begin(), snapshot.notes.end(),
                       [](const Note& note) { return note.eq.has_value() && note.eq->active; });
}

} // namespace

struct ExportReadinessTracker::State {
    struct ContentProgress {
        bool renderRequested = false;
        bool analysisRequested = false;
    };

    enum class Stretch {
        Current,
        Pending,
        NoRender,
        Failed
    };

    explicit State(bool allowDryValue) : allowDry(allowDryValue) {}

    bool allowDry;
    std::map<ContentKey, ContentProgress> contentProgress;

    Stretch stretchState(OpenTuneAudioProcessor& processor, ContentKey key)
    {
        const auto snapshot = processor.getContentSnapshot(key);
        if (snapshot == nullptr)
            return Stretch::Failed;
        if (snapshot->timeGrid == nullptr || snapshot->timeGrid->isIdentity())
            return Stretch::Current;
        auto* service = processor.getContentRenderService();
        if (service == nullptr)
            return Stretch::Failed;
        juce::AudioBuffer<float> probe(1, 1);
        if (service->getTimeStretchCache().sliceCanonicalForOutputRange(key, snapshot->contentRevision,
                                                                         snapshot->timeGridRevision, 0, probe, 0, 1)
            > 0)
            return Stretch::Current;
        const auto cache = service->getRenderCache(key);
        if (cache == nullptr)
            return Stretch::NoRender;
        if (!cache->isCanonicalSettled()) {
            if (cache->getChunkStats().hasActiveWork() || service->renderQueueDepth() != 0
                || service->renderInFlight() != 0)
                return Stretch::Pending;
            return Stretch::NoRender;
        }
        if (service->renderQueueDepth() != 0 || service->renderInFlight() != 0) {
            return Stretch::Pending;
        }
        return Stretch::Failed;
    }

    ExportReadiness classifyRender(OpenTuneAudioProcessor& processor, ContentKey key)
    {
        ExportReadiness result;
        auto snapshot = processor.getContentSnapshot(key);
        if (snapshot == nullptr) {
            result.state = ExportReadiness::State::Fail;
            result.failure = ExportFailure::ContentUnavailable;
            return result;
        }
        auto& progress = contentProgress[key];
        switch (snapshot->originalF0State) {
            case OriginalF0State::NotRequested: {
                if (!progress.analysisRequested) {
                    progress.analysisRequested = true;
                    OpenTuneAudioProcessor::ContentRefreshRequest request;
                    request.contentKey = key;
                    if (!processor.requestContentRefresh(request)) {
                        result.state = ExportReadiness::State::Fail;
                        result.failure = ExportFailure::AnalysisFailed;
                        return result;
                    }
                }
                return result;
            }
            case OriginalF0State::Extracting:
                return result;
            case OriginalF0State::Failed:
                if (snapshot->originalF0FailureReason == "f0_empty_or_unvoiced") {
                    result.state = ExportReadiness::State::Dry;
                    result.dryReason = "unvoiced";
                } else if (allowDry) {
                    result.state = ExportReadiness::State::Dry;
                    result.dryReason = "analysisFailed";
                } else {
                    result.state = ExportReadiness::State::Fail;
                    result.failure = ExportFailure::AnalysisFailed;
                }
                return result;
            case OriginalF0State::Ready:
                break;
        }
        if (!hasVoicedFrame(*snapshot)) {
            result.state = ExportReadiness::State::Dry;
            result.dryReason = "unvoiced";
            return result;
        }
        auto* service = processor.getContentRenderService();
        auto cache = service != nullptr ? service->getRenderCache(key) : nullptr;
        auto stats = cache != nullptr ? cache->getChunkStats() : RenderCache::ChunkStats{};
        const bool identityGrid = snapshot->timeGrid == nullptr || snapshot->timeGrid->isIdentity();
        const bool dryWithoutRender = !needsRender(*snapshot) && identityGrid;
        if (stats.total() == 0) {
            if (!progress.renderRequested && !dryWithoutRender) {
                progress.renderRequested = true;
                processor.requestFullContentRender(key);
                snapshot = processor.getContentSnapshot(key);
                if (snapshot == nullptr) {
                    result.state = ExportReadiness::State::Fail;
                    result.failure = ExportFailure::ContentUnavailable;
                    return result;
                }
                cache = service != nullptr ? service->getRenderCache(key) : nullptr;
                stats = cache != nullptr ? cache->getChunkStats() : RenderCache::ChunkStats{};
            }
            if (stats.total() == 0) {
                if (dryWithoutRender) {
                    result.state = ExportReadiness::State::Dry;
                    result.dryReason = "nothingToRender";
                } else {
                    result.state = ExportReadiness::State::Fail;
                    result.failure = ExportFailure::RenderUnavailable;
                }
                return result;
            }
        }
        if (stats.failed > 0) {
            result.state = ExportReadiness::State::Fail;
            result.failure = ExportFailure::RenderChunkFailed;
            return result;
        }
        if (stats.hasActiveWork()) {
            return result;
        }
        if (stats.blank == stats.total()) {
            result.state = ExportReadiness::State::Dry;
            result.dryReason = "nothingToRender";
            return result;
        }
        if (cache->isCanonicalSettled()) {
            result.state = ExportReadiness::State::Ready;
            return result;
        }
        if (!progress.renderRequested) {
            progress.renderRequested = true;
            processor.requestFullContentRender(key);
        } else {
            result.state = ExportReadiness::State::Fail;
            result.failure = ExportFailure::RenderUnavailable;
        }
        return result;
    }

    ExportReadiness classify(OpenTuneAudioProcessor& processor, ContentKey key)
    {
        auto result = classifyRender(processor, key);
        if (result.state != ExportReadiness::State::Ready && result.state != ExportReadiness::State::Dry)
            return result;
        switch (stretchState(processor, key)) {
            case Stretch::Current:
                return result;
            case Stretch::Pending:
                return {};
            case Stretch::NoRender:
                result.state = ExportReadiness::State::Fail;
                result.failure = ExportFailure::DryStretched;
                return result;
            case Stretch::Failed:
                result.state = ExportReadiness::State::Fail;
                result.failure = ExportFailure::StretchFailed;
                return result;
        }
        return result;
    }
};

ExportReadinessTracker::ExportReadinessTracker(bool allowDry) : state_(std::make_unique<State>(allowDry)) {}

ExportReadinessTracker::~ExportReadinessTracker() = default;

ExportReadiness ExportReadinessTracker::classify(OpenTuneAudioProcessor& processor, ContentKey key)
{
    return state_->classify(processor, key);
}

} // namespace OpenTune::StandaloneMediaOperations
