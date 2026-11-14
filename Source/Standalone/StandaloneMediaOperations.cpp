// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StandaloneMediaOperations.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Inference/RenderCache.h"
#include "Render/ContentRenderService.h"
#include "Render/PlaybackReadSource.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/PlacementFade.h"
#include "Utils/PlaybackAudioReader.h"
#include "Utils/TimeCoordinate.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <map>
#include <utility>

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
            if (snapshot == nullptr)
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
    return plan;
}

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
