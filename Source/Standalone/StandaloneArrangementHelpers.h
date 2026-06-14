#pragma once
#include "../PluginProcessor.h"

namespace OpenTune {

// ============================================================================
// Active track helpers
// ============================================================================

inline int getStandaloneActiveTrack(OpenTuneAudioProcessor& processor)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr ? arrangement->getActiveTrackId() : 0;
}

inline bool setStandaloneActiveTrack(OpenTuneAudioProcessor& processor, int trackId)
{
    if (auto* arrangement = processor.getStandaloneArrangement())
        return arrangement->setActiveTrack(trackId);
    return false;
}

// ============================================================================
// Placement helpers
// ============================================================================

inline int getStandaloneSelectedPlacementIndex(OpenTuneAudioProcessor& processor, int trackId)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr ? arrangement->getSelectedPlacementIndex(trackId) : -1;
}

inline int getStandalonePlacementCount(OpenTuneAudioProcessor& processor, int trackId)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr ? arrangement->getNumPlacements(trackId) : 0;
}

inline bool getStandalonePlacementByIndex(OpenTuneAudioProcessor& processor,
                                          int trackId,
                                          int placementIndex,
                                          StandaloneArrangement::Placement& out)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr
        && trackId >= 0
        && trackId < OpenTuneAudioProcessor::MAX_TRACKS
        && placementIndex >= 0
        && arrangement->getPlacementByIndex(trackId, placementIndex, out);
}

inline bool getStandalonePlacementById(OpenTuneAudioProcessor& processor,
                                       int trackId,
                                       uint64_t placementId,
                                       StandaloneArrangement::Placement& out)
{
    const auto* arrangement = processor.getStandaloneArrangement();
    return arrangement != nullptr
        && trackId >= 0
        && trackId < OpenTuneAudioProcessor::MAX_TRACKS
        && placementId != 0
        && arrangement->getPlacementById(trackId, placementId, out);
}

inline void setStandaloneSelectedPlacementIndex(OpenTuneAudioProcessor& processor, int trackId, int placementIndex)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setSelectedPlacementIndex(trackId, placementIndex);
    }
}

inline ContentKey getStandaloneContentKey(OpenTuneAudioProcessor& processor,
                                               int trackId,
                                               int placementIndex)
{
    StandaloneArrangement::Placement placement;
    return getStandalonePlacementByIndex(processor, trackId, placementIndex, placement)
        ? placement.contentKey
        : ContentKey{};
}

inline bool moveStandalonePlacement(OpenTuneAudioProcessor& processor,
                                    int sourceTrackId,
                                    int targetTrackId,
                                    uint64_t placementId,
                                    double newStartSeconds)
{
    return processor.movePlacementToTrack(sourceTrackId, targetTrackId, placementId, newStartSeconds);
}

inline bool getStandalonePlacementStartSeconds(OpenTuneAudioProcessor& processor,
                                               int trackId,
                                               uint64_t placementId,
                                               double& outStartSeconds)
{
    StandaloneArrangement::Placement placement;
    if (!getStandalonePlacementById(processor, trackId, placementId, placement)) {
        outStartSeconds = 0.0;
        return false;
    }
    outStartSeconds = placement.timelineStartSeconds;
    return true;
}

inline bool getStandalonePlacementGain(OpenTuneAudioProcessor& processor,
                                       int trackId,
                                       uint64_t placementId,
                                       float& outGain)
{
    StandaloneArrangement::Placement placement;
    if (!getStandalonePlacementById(processor, trackId, placementId, placement)) {
        outGain = 1.0f;
        return false;
    }
    outGain = placement.gain;
    return true;
}

// ============================================================================
// Track mute / solo / volume / RMS helpers
// ============================================================================

inline bool getStandaloneTrackMuted(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->isTrackMuted(trackId);
}

inline bool getStandaloneTrackSolo(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->isTrackSolo(trackId);
}

inline float getStandaloneTrackVolume(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getTrackVolume(trackId);
}

inline float getStandaloneTrackRms(OpenTuneAudioProcessor& processor, int trackId)
{
    auto* arrangement = processor.getStandaloneArrangement();
    jassert(arrangement != nullptr);
    return arrangement->getTrackRmsDb(trackId);
}

inline void setStandaloneTrackMuted(OpenTuneAudioProcessor& processor, int trackId, bool muted)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackMuted(trackId, muted);
    }
}

inline void setStandaloneTrackSolo(OpenTuneAudioProcessor& processor, int trackId, bool solo)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackSolo(trackId, solo);
    }
}

inline void setStandaloneTrackVolume(OpenTuneAudioProcessor& processor, int trackId, float volume)
{
    if (auto* arrangement = processor.getStandaloneArrangement()) {
        arrangement->setTrackVolume(trackId, volume);
    }
}

inline juce::Colour getStandaloneTrackColour(OpenTuneAudioProcessor& processor, int trackId)
{
    if (auto* arrangement = processor.getStandaloneArrangement())
        return arrangement->getTrackColour(trackId);
    return juce::Colours::grey;
}

inline void setStandaloneTrackColour(OpenTuneAudioProcessor& processor, int trackId, juce::Colour colour)
{
    if (auto* arrangement = processor.getStandaloneArrangement())
        arrangement->setTrackColour(trackId, colour);
}

} // namespace OpenTune
