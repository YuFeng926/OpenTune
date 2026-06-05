#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

#include "../MaterializationStore.h"
#include "../Utils/PitchShiftSettings.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class OpenTuneDocumentController;

// ============================================================================
// MaterializationContentAccess — read-only content access interface
// ============================================================================
class MaterializationContentAccess
{
public:
    virtual ~MaterializationContentAccess() = default;

    virtual MaterializationStore::MaterializationSnapshot getSnapshot(uint64_t materializationId) const = 0;
    virtual double getMaterializationDuration(uint64_t materializationId) const = 0;
    virtual uint64_t getSourceId(uint64_t materializationId) const = 0;
    virtual PitchShiftSettings getPitchShift(uint64_t materializationId) const = 0;
    virtual bool hasMaterialization(uint64_t materializationId) const = 0;

    virtual std::shared_ptr<const juce::AudioBuffer<float>> getAudioBuffer(uint64_t materializationId) const = 0;
    virtual std::shared_ptr<PitchCurve> getPitchCurve(uint64_t materializationId) const = 0;
    virtual OriginalF0State getOriginalF0State(uint64_t materializationId) const = 0;
    virtual DetectedKey getDetectedKey(uint64_t materializationId) const = 0;
    virtual std::vector<Note> getNotes(uint64_t materializationId) const = 0;
    virtual MaterializationStore::MaterializationNotesSnapshot getNotesSnapshot(uint64_t materializationId) const = 0;
    virtual uint64_t getNotesRevision(uint64_t materializationId) const = 0;

    virtual std::shared_ptr<const TimeGridSnapshot> getTimeGrid(uint64_t materializationId) const = 0;
    virtual uint64_t getTimeGridRevision(uint64_t materializationId) const = 0;

    virtual RenderCache::ChunkStats getChunkStats(uint64_t materializationId) const = 0;
    virtual bool getChunkBoundaries(uint64_t materializationId, std::vector<double>& outSeconds) const = 0;
};

// ============================================================================
// MaterializationContentCommands — state-change content interface
// ============================================================================
class MaterializationContentCommands
{
public:
    virtual ~MaterializationContentCommands() = default;

    virtual void setDetectedKey(uint64_t materializationId, const DetectedKey& key) = 0;
    virtual void setPitchShiftSettings(uint64_t materializationId, const PitchShiftSettings& settings) = 0;
    virtual void enqueuePartialRender(uint64_t materializationId, double relStartSeconds, double relEndSeconds) = 0;
    virtual uint64_t createMaterialization(uint64_t sourceId, double sampleRate, int channels,
                                           const juce::AudioBuffer<float>& buffer,
                                           const juce::String& name) = 0;
    virtual bool commitAutoTuneGeneratedNotes(uint64_t materializationId,
                                               const std::vector<Note>& generatedNotes,
                                               int startFrame,
                                               int endFrameExclusive,
                                               float retuneSpeed,
                                               float vibratoDepth,
                                               float vibratoRate,
                                               double audioSampleRate) = 0;
};

// ============================================================================
// Factory functions
// ============================================================================
std::unique_ptr<MaterializationContentAccess> makeDocumentControllerAccess(OpenTuneDocumentController* dc);
std::unique_ptr<MaterializationContentCommands> makeDocumentControllerCommands(OpenTuneDocumentController* dc);
std::unique_ptr<MaterializationContentAccess> makeProcessorAccess(OpenTuneAudioProcessor* processor);
std::unique_ptr<MaterializationContentCommands> makeProcessorCommands(OpenTuneAudioProcessor* processor);

} // namespace OpenTune
