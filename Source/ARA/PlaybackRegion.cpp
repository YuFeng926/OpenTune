#include "PlaybackRegion.h"

namespace OpenTune {

void PlaybackRegion::updateFrom(juce::ARAPlaybackRegion* region)
{
    playbackRegion = region;
    if (region == nullptr)
        return;

    if (auto* modification = region->getAudioModification())
        audioModificationPersistentId = juce::String(modification->getPersistentID());
    else
        audioModificationPersistentId.clear();

    startInPlaybackTime = region->getStartInPlaybackTime();
    startInModificationTime = region->getStartInAudioModificationTime();
    durationInPlaybackTime = region->getDurationInPlaybackTime();
    durationInModificationTime = region->getDurationInAudioModificationTime();
    timestretchEnabled = region->isTimestretchEnabled();
    timestretchReflectingTempo = region->isTimeStretchReflectingTempo();
    contentBasedFadeAtHead = region->hasContentBasedFadeAtHead();
    contentBasedFadeAtTail = region->hasContentBasedFadeAtTail();
    updateDisplayColourFrom(region);
    ++placementRevision;
}

void PlaybackRegion::updateDisplayColourFrom(juce::ARAPlaybackRegion* region)
{
    // ARA 标准有效颜色：getEffectiveColor 回退链仅为 region 自身 color →
    // RegionSequence color（ARA_Library/PlugIn/ARAPlug.cpp:588-594），无 musical
    // context 一级。空值表示无颜色。ARAColor 为 0.0f~1.0f 的 RGB float，不透明
    // 投影为 alpha=1.0f 的 juce::Colour。
    if (const ARA::ARAColor* color = region->getEffectiveColor())
        displayColour = juce::Colour::fromFloatRGBA(color->r, color->g, color->b, 1.0f);
    else
        displayColour = std::nullopt;
}

double PlaybackRegion::endInPlaybackTime() const noexcept
{
    return startInPlaybackTime + durationInPlaybackTime;
}

bool PlaybackRegion::hasValidPlacement() const noexcept
{
    return playbackRegion != nullptr
        && audioModificationPersistentId.isNotEmpty()
        && durationInPlaybackTime > 0.0
        && durationInModificationTime > 0.0;
}

} // namespace OpenTune
