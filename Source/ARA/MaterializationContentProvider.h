#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

#include "../MaterializationStore.h"
#include "../Utils/PitchShiftSettings.h"

namespace OpenTune {

// ============================================================================
// [DEPRECATED] MaterializationContentAccess — read-only content access interface
//
// 旧的 DC/Processor 内容访问路径已废弃。
// 新的内容访问统一通过 DomainContentOwner::snapshotContent()。
// 此接口保留仅为避免编译中断；新代码不应依赖此接口。
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
// [DEPRECATED] MaterializationContentCommands — state-change content interface
// 旧的内容写入路径。新代码应通过 DomainContentOwner::applyContentCommand()。
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

    virtual bool setNotes(uint64_t materializationId, const std::vector<Note>& notes) = 0;
    virtual bool commitNotesAndSegments(uint64_t materializationId,
                                        const std::vector<Note>& notes,
                                        const std::vector<CorrectedSegment>& segments) = 0;
    virtual bool setCorrectedSegments(uint64_t materializationId,
                                      const std::vector<CorrectedSegment>& segments) = 0;
    virtual bool setPitchCurve(uint64_t materializationId, std::shared_ptr<PitchCurve> curve) = 0;
    virtual bool setTimeGrid(uint64_t materializationId,
                             std::shared_ptr<const TimeGridSnapshot> grid,
                             int64_t srcStartFrame,
                             int64_t srcEndFrame) = 0;
};

} // namespace OpenTune
