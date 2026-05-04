#include "OpenTunePlaybackRenderer.h"
#include "OpenTuneDocumentController.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/MaterializationTimelineProjection.h"

#include <atomic>
#include <cmath>

namespace OpenTune {

bool canRenderPublishedRegionView(const VST3AraSession::PublishedRegionView& view) noexcept
{
    return view.bindingState == VST3AraSession::BindingState::Renderable;
}

int computeSourceSamplesForHostBlock(int hostSamples,
                                     double hostSampleRate,
                                     double sourceSampleRate)
{
    if (hostSamples <= 0)
        return 0;

    if (hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
        return hostSamples;

    const double hostDurationSeconds = static_cast<double>(hostSamples) / hostSampleRate;
    const double sourceSamplesExact = hostDurationSeconds * sourceSampleRate;
    const int sourceSamples = static_cast<int>(std::ceil(sourceSamplesExact));
    return juce::jmax(1, sourceSamples);
}

std::optional<RenderBlockSpan> computeRegionBlockRenderSpan(double blockStartSeconds,
                                                            int blockSamples,
                                                            double hostSampleRate,
                                                            double playbackStartSeconds,
                                                            double playbackEndSeconds) noexcept
{
    if (blockSamples <= 0 || hostSampleRate <= 0.0)
        return std::nullopt;

    const double blockEndSeconds = blockStartSeconds
        + (static_cast<double>(blockSamples) / hostSampleRate);
    const double overlapStartSeconds = juce::jmax(blockStartSeconds, playbackStartSeconds);
    const double overlapEndSeconds = juce::jmin(blockEndSeconds, playbackEndSeconds);
    if (!(overlapEndSeconds > overlapStartSeconds))
        return std::nullopt;

    const int destinationStartSample = juce::jlimit(0,
                                                    blockSamples,
                                                    static_cast<int>(TimeCoordinate::secondsToSamplesFloor(overlapStartSeconds - blockStartSeconds,
                                                                                                           hostSampleRate)));
    const int destinationEndSample = juce::jlimit(destinationStartSample,
                                                  blockSamples,
                                                  static_cast<int>(TimeCoordinate::secondsToSamplesCeil(overlapEndSeconds - blockStartSeconds,
                                                                                                        hostSampleRate)));

    RenderBlockSpan span;
    span.destinationStartSample = destinationStartSample;
    span.samplesToCopy = destinationEndSample - destinationStartSample;
    span.overlapStartSeconds = overlapStartSeconds;
    return span.samplesToCopy > 0 ? std::optional<RenderBlockSpan>(span) : std::nullopt;
}

namespace {
    std::atomic<bool> firstProcessCall{true};
    std::atomic<int> mappingLogCounter{0};

    const VST3AraSession::PublishedRegionView* findRenderableRegionView(
        const VST3AraSession::PublishedSnapshot& snapshot,
        juce::ARAPlaybackRegion* region)
    {
        const auto* publishedView = snapshot.findRegion(region);
        if (publishedView == nullptr)
            return nullptr;

        if (!canRenderPublishedRegionView(*publishedView))
            return nullptr;

        return publishedView;
    }
}

OpenTunePlaybackRenderer::~OpenTunePlaybackRenderer() = default;

void OpenTunePlaybackRenderer::prepareToPlay(double sampleRate,
                                              int maximumSamplesPerBlock,
                                              int numChannels,
                                              juce::AudioProcessor::ProcessingPrecision precision,
                                              AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ignoreUnused(precision, alwaysNonRealtime);
    
    hostSampleRate_ = sampleRate;
    numChannels_ = numChannels;
    maximumSamplesPerBlock_ = maximumSamplesPerBlock;
    playbackScratch_.setSize(juce::jmax(1, numChannels_),
                             juce::jmax(1, maximumSamplesPerBlock_),
                             false,
                             true,
                             true);
    
    AppLogger::log("ARA PlaybackRenderer: prepareToPlay sampleRate=" + juce::String(sampleRate, 0)
        + " maxBlock=" + juce::String(maximumSamplesPerBlock)
        + " channels=" + juce::String(numChannels));
}

void OpenTunePlaybackRenderer::releaseResources()
{
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                              juce::AudioProcessor::Realtime realtime,
                                              const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    juce::ignoreUnused(realtime);
    
    const auto& regions = getPlaybackRegions();
    
    if (firstProcessCall.exchange(false))
    {
        AppLogger::log("ARA PlaybackRenderer: First processBlock call, regions count = " 
            + juce::String(regions.size())
            + " hostSampleRate=" + juce::String(hostSampleRate_, 0));
    }
    
    if (regions.empty())
    {
        buffer.clear();
        return false;
    }
    
    auto* dc = getDocumentController();
    if (!dc)
    {
        buffer.clear();
        return false;
    }
    
    auto* docController = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(dc);
    if (!docController)
    {
        buffer.clear();
        return false;
    }
    
    // Renderer stays read-only: session owns region truth, clip core owns audio truth.
    // Session is owned by DocumentController (document-scoped), not by the processor.
    auto* session = docController->getSession();
    auto* processor = docController->getProcessor();
    if (session == nullptr || processor == nullptr)
    {
        buffer.clear();
        return false;
    }
    auto* materializationStore = processor->getMaterializationStore();
    if (materializationStore == nullptr)
    {
        buffer.clear();
        return false;
    }

    // Phase 25 contract: each audio block consumes one immutable snapshot.
    const auto snapshot = session->loadSnapshot();
    if (!snapshot || snapshot->publishedRegions.empty())
    {
        buffer.clear();
        return false;
    }

    bool processedAny = false;
    buffer.clear();

    jassert(playbackScratch_.getNumChannels() >= buffer.getNumChannels());
    jassert(playbackScratch_.getNumSamples() >= buffer.getNumSamples());

    const double blockStartSeconds = positionInfo.getTimeInSeconds().orFallback(0.0);
    
    for (auto* region : regions)
    {
        const auto* const publishedView = findRenderableRegionView(*snapshot, region);
        if (publishedView == nullptr)
            continue;

        const auto appliedProjection = publishedView->appliedProjection;

        const double playbackStartSeconds = publishedView->playbackStartSeconds;
        const double playbackEndSeconds = publishedView->playbackEndSeconds;
        const double playbackDurationSeconds = playbackEndSeconds - playbackStartSeconds;
        const double materializationDurationSeconds = publishedView->materializationDurationSeconds;

        // Invariant: projection must be isometric (1:1 time mapping)
        if (std::abs(playbackDurationSeconds - materializationDurationSeconds) > 0.001)
        {
            AppLogger::log("InvariantViolation: ARA renderer projection duration mismatch"
                " playbackDuration=" + juce::String(playbackDurationSeconds, 6)
                + " materializationDuration=" + juce::String(materializationDurationSeconds, 6));
            jassertfalse;
            continue;
        }

        const auto overlap = computeRegionBlockRenderSpan(blockStartSeconds,
                                                          buffer.getNumSamples(),
                                                          hostSampleRate_,
                                                          playbackStartSeconds,
                                                          playbackEndSeconds);
        if (!overlap.has_value())
            continue;

        const int destinationStartSample = overlap->destinationStartSample;
        const int requestedSamples = overlap->samplesToCopy;
        if (requestedSamples <= 0)
            continue;

        const double sourceSampleRate = publishedView->sampleRate;

        // Map overlap start to materialization-local time using shared projection
        const MaterializationTimelineProjection projection{
            playbackStartSeconds,
            playbackDurationSeconds,
            materializationDurationSeconds
        };
        const double mappedLocalTime = projection.clampMaterializationTime(
            projection.projectTimelineTimeToMaterialization(overlap->overlapStartSeconds));
        const int64_t mappedLocalSampleForLog = juce::jlimit<int64_t>(0,
            publishedView->numSamples - 1,
            TimeCoordinate::secondsToSamples(mappedLocalTime, sourceSampleRate));

        if (mappingLogCounter.fetch_add(1) < 24)
        {
            AppLogger::log("ARA Mapping: playbackTime=" + juce::String(overlap->overlapStartSeconds, 6)
                + " mappedLocalTime=" + juce::String(mappedLocalTime, 6)
                + " mappedLocalSampleForLog=" + juce::String(static_cast<juce::int64>(mappedLocalSampleForLog))
                + " hostSampleRate=" + juce::String(hostSampleRate_, 1)
                + " sourceSampleRate=" + juce::String(sourceSampleRate, 1));
        }

        MaterializationStore::PlaybackReadSource contentReadSource;
        if (!materializationStore->getPlaybackReadSource(appliedProjection.materializationId, contentReadSource))
        {
            continue;
        }

        OpenTuneAudioProcessor::PlaybackReadSource readSource;
        readSource.renderCache = contentReadSource.renderCache;
        readSource.audioBuffer = contentReadSource.audioBuffer;

        const double clipReadStartSeconds = mappedLocalTime;

        const OpenTuneAudioProcessor::PlaybackReadRequest request(
            readSource,
            clipReadStartSeconds,
            hostSampleRate_,
            requestedSamples);

        playbackScratch_.clear();
        CrossoverMixer* araMixer = readSource.renderCache
            ? &readSource.renderCache->getCrossoverMixer()
            : nullptr;
        const int samplesToCopy = processor->readPlaybackAudio(request, playbackScratch_, 0, araMixer);
        if (samplesToCopy <= 0)
        {
            continue;
        }

        const int samplesToMix = juce::jmin(samplesToCopy, requestedSamples);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* dest = buffer.getWritePointer(ch);
            const float* src = playbackScratch_.getReadPointer(ch);
            for (int sample = 0; sample < samplesToMix; ++sample)
            {
                dest[destinationStartSample + sample] += src[sample];
            }
        }

        processedAny = true;
    }
    
    return processedAny;
}

}
