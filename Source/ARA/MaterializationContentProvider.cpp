#include "MaterializationContentProvider.h"
#include "OpenTuneDocumentController.h"
#include "../PluginProcessor.h"

namespace OpenTune {

namespace {

// ============================================================================
// DC-backed implementations
// ============================================================================

class DcContentAccess final : public MaterializationContentAccess
{
public:
    explicit DcContentAccess(OpenTuneDocumentController* dc) noexcept : dc_(dc) {}

    MaterializationStore::MaterializationSnapshot getSnapshot(uint64_t id) const override
    {
        MaterializationStore::MaterializationSnapshot snap;
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            dc_->getMaterializationStore()->getSnapshot(id, snap);
        return snap;
    }

    double getMaterializationDuration(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getMaterializationAudioDurationById(id);
        return 0.0;
    }

    uint64_t getSourceId(uint64_t id) const override
    {
        return getSnapshot(id).sourceId;
    }

    PitchShiftSettings getPitchShift(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getPitchShiftSettings(id);
        return {};
    }

    bool hasMaterialization(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->containsMaterialization(id);
        return false;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> getAudioBuffer(uint64_t id) const override
    {
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<const juce::AudioBuffer<float>> buf;
        dc_->getMaterializationStore()->getAudioBuffer(id, buf);
        return buf;
    }

    std::shared_ptr<PitchCurve> getPitchCurve(uint64_t id) const override
    {
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<PitchCurve> curve;
        dc_->getMaterializationStore()->getPitchCurve(id, curve);
        return curve;
    }

    OriginalF0State getOriginalF0State(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getOriginalF0State(id);
        return OriginalF0State::NotRequested;
    }

    DetectedKey getDetectedKey(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getDetectedKey(id);
        return {};
    }

    std::vector<Note> getNotes(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getNotes(id);
        return {};
    }

    MaterializationStore::MaterializationNotesSnapshot getNotesSnapshot(uint64_t id) const override
    {
        MaterializationStore::MaterializationNotesSnapshot snap;
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            dc_->getMaterializationStore()->getNotesSnapshot(id, snap);
        return snap;
    }

    uint64_t getNotesRevision(uint64_t id) const override
    {
        return getNotesSnapshot(id).notesRevision;
    }

    std::shared_ptr<const TimeGridSnapshot> getTimeGrid(uint64_t id) const override
    {
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<const TimeGridSnapshot> tg;
        dc_->getMaterializationStore()->getTimeGrid(id, tg);
        return tg;
    }

    uint64_t getTimeGridRevision(uint64_t id) const override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            return dc_->getMaterializationStore()->getTimeGridRevision(id);
        return 0;
    }

    RenderCache::ChunkStats getChunkStats(uint64_t id) const override
    {
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return {};
        std::shared_ptr<RenderCache> cache;
        if (dc_->getMaterializationStore()->getRenderCache(id, cache) && cache != nullptr)
            return cache->getChunkStats();
        return {};
    }

    bool getChunkBoundaries(uint64_t id, std::vector<double>& outSeconds) const override
    {
        juce::ignoreUnused(id, outSeconds);
        return false;
    }

private:
    OpenTuneDocumentController* dc_;
};

class DcContentCommands final : public MaterializationContentCommands
{
public:
    explicit DcContentCommands(OpenTuneDocumentController* dc) noexcept : dc_(dc) {}

    void setDetectedKey(uint64_t id, const DetectedKey& key) override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            dc_->getMaterializationStore()->setDetectedKey(id, key);
    }

    void setPitchShiftSettings(uint64_t id, const PitchShiftSettings& settings) override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            dc_->getMaterializationStore()->setPitchShiftSettings(id, settings);
    }

    void enqueuePartialRender(uint64_t id, double relStartSeconds, double relEndSeconds) override
    {
        if (dc_ != nullptr && dc_->getMaterializationStore() != nullptr)
            dc_->getMaterializationStore()->enqueuePartialRender(id, relStartSeconds, relEndSeconds, 512);
    }

    uint64_t createMaterialization(uint64_t sourceId, double sampleRate, int channels,
                                    const juce::AudioBuffer<float>& buffer,
                                    const juce::String& name) override
    {
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return 0;

        MaterializationStore::CreateMaterializationRequest request;
        request.sourceId = sourceId;
        request.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(buffer);
        juce::ignoreUnused(sampleRate, channels, name);
        return dc_->getMaterializationStore()->createMaterialization(request);
    }

    bool commitAutoTuneGeneratedNotes(uint64_t materializationId,
                                       const std::vector<Note>& generatedNotes,
                                       int startFrame, int endFrameExclusive,
                                       float retuneSpeed, float vibratoDepth,
                                       float vibratoRate, double audioSampleRate) override
    {
        juce::ignoreUnused(materializationId, generatedNotes, startFrame, endFrameExclusive,
                           retuneSpeed, vibratoDepth, vibratoRate, audioSampleRate);
        // DC-backed: store-level write only; full AutoTune pipeline is processor-level
        if (dc_ == nullptr || dc_->getMaterializationStore() == nullptr) return false;
        return dc_->getMaterializationStore()->commitNotesAndPitchCurve(
            materializationId, generatedNotes, nullptr);
    }

private:
    OpenTuneDocumentController* dc_;
};

// ============================================================================
// Processor-backed implementations
// ============================================================================

class ProcessorContentAccess final : public MaterializationContentAccess
{
public:
    explicit ProcessorContentAccess(OpenTuneAudioProcessor* proc) noexcept : proc_(proc) {}

    MaterializationStore::MaterializationSnapshot getSnapshot(uint64_t id) const override
    {
        MaterializationStore::MaterializationSnapshot snap;
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            proc_->getMaterializationStore()->getSnapshot(id, snap);
        return snap;
    }

    double getMaterializationDuration(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getMaterializationAudioDurationById(id);
        return 0.0;
    }

    uint64_t getSourceId(uint64_t id) const override
    {
        return getSnapshot(id).sourceId;
    }

    PitchShiftSettings getPitchShift(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getPitchShiftSettings(id);
        return {};
    }

    bool hasMaterialization(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->containsMaterialization(id);
        return false;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> getAudioBuffer(uint64_t id) const override
    {
        if (proc_ == nullptr || proc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<const juce::AudioBuffer<float>> buf;
        proc_->getMaterializationStore()->getAudioBuffer(id, buf);
        return buf;
    }

    std::shared_ptr<PitchCurve> getPitchCurve(uint64_t id) const override
    {
        if (proc_ == nullptr || proc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<PitchCurve> curve;
        proc_->getMaterializationStore()->getPitchCurve(id, curve);
        return curve;
    }

    OriginalF0State getOriginalF0State(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getOriginalF0State(id);
        return OriginalF0State::NotRequested;
    }

    DetectedKey getDetectedKey(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getDetectedKey(id);
        return {};
    }

    std::vector<Note> getNotes(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getNotes(id);
        return {};
    }

    MaterializationStore::MaterializationNotesSnapshot getNotesSnapshot(uint64_t id) const override
    {
        MaterializationStore::MaterializationNotesSnapshot snap;
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            proc_->getMaterializationStore()->getNotesSnapshot(id, snap);
        return snap;
    }

    uint64_t getNotesRevision(uint64_t id) const override
    {
        return getNotesSnapshot(id).notesRevision;
    }

    std::shared_ptr<const TimeGridSnapshot> getTimeGrid(uint64_t id) const override
    {
        if (proc_ == nullptr || proc_->getMaterializationStore() == nullptr) return nullptr;
        std::shared_ptr<const TimeGridSnapshot> tg;
        proc_->getMaterializationStore()->getTimeGrid(id, tg);
        return tg;
    }

    uint64_t getTimeGridRevision(uint64_t id) const override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            return proc_->getMaterializationStore()->getTimeGridRevision(id);
        return 0;
    }

    RenderCache::ChunkStats getChunkStats(uint64_t id) const override
    {
        if (proc_ == nullptr || proc_->getMaterializationStore() == nullptr) return {};
        std::shared_ptr<RenderCache> cache;
        if (proc_->getMaterializationStore()->getRenderCache(id, cache) && cache != nullptr)
            return cache->getChunkStats();
        return {};
    }

    bool getChunkBoundaries(uint64_t id, std::vector<double>& outSeconds) const override
    {
        juce::ignoreUnused(id, outSeconds);
        return false;
    }

private:
    OpenTuneAudioProcessor* proc_;
};

class ProcessorContentCommands final : public MaterializationContentCommands
{
public:
    explicit ProcessorContentCommands(OpenTuneAudioProcessor* proc) noexcept : proc_(proc) {}

    void setDetectedKey(uint64_t id, const DetectedKey& key) override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            proc_->getMaterializationStore()->setDetectedKey(id, key);
    }

    void setPitchShiftSettings(uint64_t id, const PitchShiftSettings& settings) override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            proc_->getMaterializationStore()->setPitchShiftSettings(id, settings);
    }

    void enqueuePartialRender(uint64_t id, double relStartSeconds, double relEndSeconds) override
    {
        if (proc_ != nullptr && proc_->getMaterializationStore() != nullptr)
            proc_->getMaterializationStore()->enqueuePartialRender(id, relStartSeconds, relEndSeconds, 512);
    }

    uint64_t createMaterialization(uint64_t sourceId, double sampleRate, int channels,
                                    const juce::AudioBuffer<float>& buffer,
                                    const juce::String& name) override
    {
        if (proc_ == nullptr || proc_->getMaterializationStore() == nullptr) return 0;

        MaterializationStore::CreateMaterializationRequest request;
        request.sourceId = sourceId;
        request.audioBuffer = std::make_shared<juce::AudioBuffer<float>>(buffer);
        juce::ignoreUnused(sampleRate, channels, name);
        return proc_->getMaterializationStore()->createMaterialization(request);
    }

    bool commitAutoTuneGeneratedNotes(uint64_t materializationId,
                                       const std::vector<Note>& generatedNotes,
                                       int startFrame, int endFrameExclusive,
                                       float retuneSpeed, float vibratoDepth,
                                       float vibratoRate, double audioSampleRate) override
    {
        // Processor-backed: delegate to the full processor method
        if (proc_ == nullptr) return false;
        return proc_->commitAutoTuneGeneratedNotesByMaterializationId(
            materializationId, generatedNotes, startFrame, endFrameExclusive,
            retuneSpeed, vibratoDepth, vibratoRate, audioSampleRate);
    }

private:
    OpenTuneAudioProcessor* proc_;
};

} // namespace

// ============================================================================
// Factory functions
// ============================================================================
std::unique_ptr<MaterializationContentAccess> makeDocumentControllerAccess(OpenTuneDocumentController* dc)
{
    return std::make_unique<DcContentAccess>(dc);
}

std::unique_ptr<MaterializationContentCommands> makeDocumentControllerCommands(OpenTuneDocumentController* dc)
{
    return std::make_unique<DcContentCommands>(dc);
}

std::unique_ptr<MaterializationContentAccess> makeProcessorAccess(OpenTuneAudioProcessor* processor)
{
    return std::make_unique<ProcessorContentAccess>(processor);
}

std::unique_ptr<MaterializationContentCommands> makeProcessorCommands(OpenTuneAudioProcessor* processor)
{
    return std::make_unique<ProcessorContentCommands>(processor);
}

} // namespace OpenTune
