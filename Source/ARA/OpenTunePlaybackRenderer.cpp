#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "../Utils/PlaybackAudioReader.h"

#include <algorithm>

namespace OpenTune {

bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                   bool rendererIsPlaying) noexcept
{
    if (realtime != juce::AudioProcessor::Realtime::yes)
        return true;

    return rendererIsPlaying;
}

namespace {
    double mapPlaybackTimeToContentTime(const OpenTunePlaybackRenderer::PlaybackRegionRenderItem& region,
                                                double playbackTimeSeconds) noexcept
    {
        if (region.durationInPlaybackTime <= 0.0 || region.durationInModificationTime <= 0.0)
            return 0.0;

        const double playbackOffset = playbackTimeSeconds - region.startInPlaybackTime;
        const double modificationOffset = playbackOffset
            * (region.durationInModificationTime / region.durationInPlaybackTime);
        const double modificationTime = region.startInModificationTime + modificationOffset;
        const double contentOffset = modificationTime - region.contentWindow.sourceStartSeconds;

        return juce::jlimit(0.0,
                            juce::jmax(0.0, region.contentDurationSeconds),
                            contentOffset);
    }

    void mixScratchInto(juce::AudioBuffer<float>& destination,
                        const juce::AudioBuffer<float>& source,
                        int destinationStartSample,
                        int samplesToMix) noexcept
    {
        const int channels = juce::jmin(destination.getNumChannels(), source.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = destination.getWritePointer(ch, destinationStartSample);
            const auto* src = source.getReadPointer(ch);
            for (int sample = 0; sample < samplesToMix; ++sample)
                dest[sample] += src[sample];
        }
    }

    juce::ARAPlaybackRegion* toJucePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
    {
        return static_cast<juce::ARAPlaybackRegion*>(playbackRegion);
    }
}

OpenTunePlaybackRenderer::OpenTunePlaybackRenderer(ARA::PlugIn::DocumentController* araDc,
                                                   OpenTuneDocumentController* docController)
    : juce::ARAPlaybackRenderer(araDc)
    , documentController_(docController)
{
    std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                                docController ? docController->getContentRenderServiceShared() : nullptr,
                                std::memory_order_release);
}

OpenTunePlaybackRenderer::~OpenTunePlaybackRenderer()
{
    // After owner-driven detach, destructor becomes no-op.
    // Only unregister when documentController_ is still live (relationship not revoked).
    if (documentController_ != nullptr)
        documentController_->unregisterPlaybackRenderer(*this);
}

void OpenTunePlaybackRenderer::detachDocumentController(OpenTuneDocumentController& owner)
{
    // Atomically clear documentController_ only for the matching owner.
    // Per architecture: teardown is owner-driven. Renderer cannot call into destroyed DC.
    if (documentController_ == &owner)
    {
        // 1. Publish empty render plan FIRST to stop audio thread from entering render loop
        currentPlan_.store(std::make_shared<RenderPlan>(), std::memory_order_release);
        // 2. Clear CRS snapshot atomically so audio thread sees nullptr
        std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                                   std::shared_ptr<ContentRenderService>(),
                                   std::memory_order_release);
        // 3. Finally clear documentController_ pointer
        documentController_ = nullptr;
    }
}

void OpenTunePlaybackRenderer::didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    auto plan = currentPlan_.load(std::memory_order_acquire);
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    if (jucePlaybackRegion != nullptr
        && std::find(playbackRegions.begin(), playbackRegions.end(), jucePlaybackRegion)
            == playbackRegions.end())
    {
        playbackRegions.push_back(jucePlaybackRegion);
    }

    publishRenderPlanFor(std::move(playbackRegions));
}

void OpenTunePlaybackRenderer::willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    auto plan = currentPlan_.load(std::memory_order_acquire);
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    playbackRegions.erase(std::remove(playbackRegions.begin(),
                                      playbackRegions.end(),
                                      jucePlaybackRegion),
                          playbackRegions.end());
    publishRenderPlanFor(std::move(playbackRegions));
}

void OpenTunePlaybackRenderer::refreshRenderPlanFromDocument()
{
    auto plan = currentPlan_.load(std::memory_order_acquire);
    publishRenderPlanFor(plan != nullptr ? plan->playbackRegions
                                         : std::vector<juce::ARAPlaybackRegion*>{});
}

void OpenTunePlaybackRenderer::setContentRenderService(std::shared_ptr<ContentRenderService> crs)
{
    std::atomic_store_explicit(&contentRenderServiceSnapshot_, crs, std::memory_order_release);
}

std::shared_ptr<const OpenTunePlaybackRenderer::RenderPlan> OpenTunePlaybackRenderer::buildRenderPlan(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions) const
{
    auto nextPlan = std::make_shared<RenderPlan>();
    nextPlan->playbackRegions = std::move(playbackRegions);
    if (documentController_ == nullptr)
        return nextPlan;

    const auto projections = documentController_->getPlaybackRegionProjectionsFor(nextPlan->playbackRegions);
    nextPlan->items.reserve(projections.size());

    for (const auto& projection : projections)
    {
        if (!projection.isPlaybackRenderable())
            continue;

        PlaybackRegionRenderItem item;
        item.playbackRegion = projection.playbackRegion;
        item.contentWindow = projection.contentWindow;
        item.contentKey = projection.contentKey;
        item.startInPlaybackTime = projection.startInPlaybackTime;
        item.startInModificationTime = projection.startInModificationTime;
        item.durationInPlaybackTime = projection.durationInPlaybackTime;
        item.durationInModificationTime = projection.durationInModificationTime;
        item.contentDurationSeconds = projection.contentDurationSeconds;
        nextPlan->items.push_back(item);
    }

    return nextPlan;
}

void OpenTunePlaybackRenderer::publishRenderPlanFor(std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    currentPlan_.store(buildRenderPlan(std::move(playbackRegions)), std::memory_order_release);
}

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
    renderBuffer_.setSize(juce::jmax(1, numChannels_),
                          juce::jmax(1, maximumSamplesPerBlock_),
                          false,
                          true,
                          true);

    // Crossfade window between passthrough and rendered content: 10 ms.
    crossfadeTotal_ = juce::jlimit(128, 2048, static_cast<int>(sampleRate * 0.01));
    crossfadeRemaining_ = 0;
    outputMode_ = RenderOutputMode::Passthrough;

    // 设备率切换 → 准备所有 CRS caches（各 cache 使用自有 resampler）
    if (contentRenderServiceSnapshot_) {
        contentRenderServiceSnapshot_->preparePlaybackSampleRate(sampleRate);
    }
}

void OpenTunePlaybackRenderer::releaseResources()
{
    playbackScratch_.setSize(0, 0);
    renderBuffer_.setSize(0, 0);
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::AudioProcessor::Realtime realtime,
                                             const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const int numSamples = buffer.getNumSamples();

    // Use CRS snapshot instead of chasing documentController_ pointer
    // to avoid TOCTOU race with DocumentController destruction.
    auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_, std::memory_order_acquire);
    const auto plan = currentPlan_.load(std::memory_order_acquire);
    const auto positionTime = positionInfo.getTimeInSeconds();

    const bool hasRenderContent = crs != nullptr
        && plan != nullptr && !plan->items.empty() && positionTime.hasValue();
    const bool wantRender = hasRenderContent
        && shouldRenderAraPlaybackBlock(realtime, positionInfo.getIsPlaying());

    // Mode transition → start a crossfade window so the switch between
    // passthrough (host input) and rendered content stays click-free.
    if (wantRender != (outputMode_ == RenderOutputMode::Rendering))
    {
        outputMode_ = wantRender ? RenderOutputMode::Rendering : RenderOutputMode::Passthrough;
        crossfadeRemaining_ = crossfadeTotal_;
    }

    const bool inTransition = crossfadeRemaining_ > 0;

    // Pure passthrough: leave the host input untouched.
    if (outputMode_ == RenderOutputMode::Passthrough && !inTransition)
        return true;

    // Render the block into renderBuffer_ (needed for Rendering mode and for
    // both crossfade directions).
    if (outputMode_ == RenderOutputMode::Rendering || inTransition)
    {
        renderBuffer_.clear();
        if (crs != nullptr && plan != nullptr && positionTime.hasValue())
        {
            const double blockStartSeconds = *positionTime;
            for (const auto& region : plan->items)
            {
                const auto overlap = computeRegionBlockRenderSpan(blockStartSeconds,
                                                                   numSamples,
                                                                   hostSampleRate_,
                                                                   region.startInPlaybackTime,
                                                                   region.endInPlaybackTime());
                if (!overlap.has_value())
                    continue;

                PlaybackReadSource readSource;
                if (!crs->getPlaybackReadSource(region.contentKey, readSource))
                    continue;

                const double readStartSeconds = mapPlaybackTimeToContentTime(region,
                                                                             overlap->overlapStartSeconds);
                const int64_t readStartSample = TimeCoordinate::secondsToSamples(readStartSeconds, hostSampleRate_);
                const ::OpenTune::PlaybackReadRequest request(readSource,
                                                              readStartSample,
                                                              hostSampleRate_,
                                                              overlap->samplesToCopy);

                playbackScratch_.clear();
                const int copied = readPlaybackAudio(request, playbackScratch_, 0);
                if (copied <= 0)
                    continue;

                const int samplesToMix = juce::jmin(copied, overlap->samplesToCopy);
                mixScratchInto(renderBuffer_, playbackScratch_, overlap->destinationStartSample, samplesToMix);
            }
        }
    }

    if (inTransition)
    {
        // 淡出方向需要渲染信号参与交叉。宿主停止后若不再提供有效时间
        // （timeInSeconds 缺失），渲染内容不可得——此时宿主输入通常已为
        // 静音，直接直通输入，不做无效淡化。
        if (outputMode_ == RenderOutputMode::Passthrough && !positionTime.hasValue())
        {
            crossfadeRemaining_ = 0;
            return true;
        }

        // Crossfade: output = input * (1 - renderWeight) + render * renderWeight.
        // 淡入 renderWeight 从 0→1（纯输入→纯渲染），淡出从 1→0（纯渲染→纯输入）；
        // 窗口最后一帧强制到达终点值，两端均无端点残留。
        const int fadeSamples = juce::jmin(crossfadeRemaining_, numSamples);
        const int elapsedBase = crossfadeTotal_ - crossfadeRemaining_;
        const bool fadingIntoRender = (outputMode_ == RenderOutputMode::Rendering);
        const int channels = juce::jmin(buffer.getNumChannels(), renderBuffer_.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* out = buffer.getWritePointer(ch);
            const auto* in = buffer.getReadPointer(ch);
            const auto* render = renderBuffer_.getReadPointer(ch);
            for (int s = 0; s < fadeSamples; ++s)
            {
                const int windowFrame = elapsedBase + s;
                float weight = static_cast<float>(windowFrame) / static_cast<float>(crossfadeTotal_);
                if (windowFrame + 1 == crossfadeTotal_)
                    weight = 1.0f;
                const float renderWeight = fadingIntoRender ? weight : (1.0f - weight);
                out[s] = in[s] * (1.0f - renderWeight) + render[s] * renderWeight;
            }
        }
        crossfadeRemaining_ -= fadeSamples;

        // Remainder of the block after the crossfade window ends.
        if (fadeSamples < numSamples)
        {
            if (outputMode_ == RenderOutputMode::Rendering)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* out = buffer.getWritePointer(ch, fadeSamples);
                    const auto* render = renderBuffer_.getReadPointer(ch, fadeSamples);
                    juce::FloatVectorOperations::copy(out, render, numSamples - fadeSamples);
                }
            }
            // Passthrough: remainder keeps the host input untouched.
        }
        return true;
    }

    // Rendering, no transition: replace the host input with the rendered
    // content (ARA 2 playback renderer semantics, ARAInterface.h:3512-3518).
    buffer.clear();
    mixScratchInto(buffer, renderBuffer_, 0, numSamples);
    return true;
}

} // namespace OpenTune
