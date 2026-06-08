#include "OpenTuneDocumentController.h"

#include "OpenTuneEditorView.h"
#include "OpenTunePlaybackRenderer.h"

#include "../MaterializationStore.h"
#include "../SourceStore.h"
#include "../Inference/F0InferenceService.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/SilentGapDetector.h"
#include "../Utils/PitchCurve.h"
#include "../Inference/RenderCache.h"
#include "../Utils/SourceWindow.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>

namespace OpenTune {

namespace {

constexpr int kContentPayloadArchiveMagic = 0x4F544143;
constexpr int kContentPayloadArchiveVersion = 1;
constexpr int kMaxContentPayloadRecords = 4096;

} // namespace

OpenTuneDocumentController::OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                       const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation(entry, instance)
    , materializationStore_(std::make_shared<MaterializationStore>())
    , sourceStore_(std::make_shared<SourceStore>())
    , resamplingManager_(std::make_shared<ResamplingManager>())
    , reclaimAsyncUpdater_([this] { runContentReclaimSweep(); })
{
}

OpenTuneDocumentController::~OpenTuneDocumentController()
{
    // 撤销服务租约，防止异步 F0 completion 写回已析构的 DC
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    playbackRenderers_.clear();
    // juce::AsyncUpdater in ReclaimAsyncUpdater auto-cancels pending updates on destruction.
}

void OpenTuneDocumentController::attachProcessorServices(ProcessorServices services)
{
    jassert(services.owner != nullptr);
    serviceOwner_ = services.owner;
    f0Service_ = std::move(services.f0Service);
    scheduleAsyncWork_ = std::move(services.scheduleAsyncWork);
    onReclaimNeeded_ = std::move(services.requestReclaimSweep);
    contentRenderService_ = services.contentRenderService;
    asyncLeaseToken_ = std::make_shared<std::atomic<bool>>(true);
}

void OpenTuneDocumentController::detachProcessorServices(const OpenTuneAudioProcessor* owner)
{
    if (serviceOwner_ != owner) return;

    // 撤销服务租约 — 通知所有后台 F0 work 释放
    if (asyncLeaseToken_)
        asyncLeaseToken_->store(false, std::memory_order_release);

    serviceOwner_ = nullptr;
    contentRenderService_ = nullptr;
    f0Service_.reset();
    scheduleAsyncWork_ = nullptr;
    onReclaimNeeded_ = nullptr;
}

void OpenTuneDocumentController::runContentReclaimSweep()
{
    if (materializationStore_ == nullptr || sourceStore_ == nullptr)
        return;

    // 回收 retired materializations
    const auto retiredIds = materializationStore_->getRetiredIds();
    for (uint64_t id : retiredIds)
    {
        const uint64_t sourceId = materializationStore_->getSourceIdAnyState(id);
        if (materializationStore_->physicallyDeleteIfReclaimable(id))
        {
            if (sourceId != 0 && !materializationStore_->hasMaterializationForSource(sourceId)
                && sourceStore_->containsSource(sourceId))
                sourceStore_->retireSource(sourceId);
        }
    }

    // 回收 retired sources
    const auto retiredSourceIds = sourceStore_->getRetiredSourceIds();
    for (uint64_t sourceId : retiredSourceIds)
    {
        if (materializationStore_->hasMaterializationForSourceAnyState(sourceId))
            continue;
        sourceStore_->physicallyDeleteIfReclaimable(sourceId);
    }

    // 清理 retiredContents_ 中已被重新生成内容的 modification 的旧记录
    for (const auto& mod : audioModifications_)
        if (mod.isRenderable())
            releaseRetiredContent(mod.contentKey());

    // 仍触发 onReclaimNeeded_ 通知 processor（processor 扫自己的 standalone arrangement）
    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

void OpenTuneDocumentController::scheduleContentReclaim()
{
    reclaimAsyncUpdater_.triggerAsyncUpdate();
}

// -----------------------------------------------------------------------
// DC-level retired content pool
// -----------------------------------------------------------------------

bool OpenTuneDocumentController::reviveRetiredContentByKey(const ContentKey& key, AudioModification& target)
{
    auto it = std::find_if(retiredContents_.begin(), retiredContents_.end(),
                           [&](const RetiredContentRecord& rec) { return rec.key == key; });
    if (it == retiredContents_.end())
        return false;

    target.content = it->content;
    target.content.lifecycle = ContentLifecycle::Ready;
    ++target.content.contentRevision;
    target.birthState = AudioModificationBirthState::Ready;
    retiredContents_.erase(it);
    return true;
}

void OpenTuneDocumentController::releaseRetiredContent(const ContentKey& key)
{
    auto it = std::find_if(retiredContents_.begin(), retiredContents_.end(),
                           [&](const RetiredContentRecord& rec) { return rec.key == key; });
    if (it != retiredContents_.end())
        retiredContents_.erase(it);
}

// -----------------------------------------------------------------------
// XML persistence helpers (DC-owned store serialization)
// -----------------------------------------------------------------------
namespace {

juce::String audioBufferToBase64(const std::shared_ptr<const juce::AudioBuffer<float>>& buffer)
{
    if (buffer == nullptr || buffer->getNumSamples() <= 0)
        return {};

    juce::MemoryBlock block;
    juce::MemoryOutputStream mos(block, false);
    const int numChannels = buffer->getNumChannels();
    const int numSamples  = buffer->getNumSamples();
    mos.writeInt(numChannels);
    mos.writeInt(numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        mos.write(buffer->getReadPointer(ch), static_cast<size_t>(numSamples) * sizeof(float));
    return juce::Base64::toBase64(block.getData(), block.getSize());
}

std::shared_ptr<const juce::AudioBuffer<float>> audioBufferFromBase64(const juce::String& str)
{
    if (str.isEmpty())
        return nullptr;

    juce::MemoryBlock block;
    {
        juce::MemoryOutputStream mos(block, false);
        if (!juce::Base64::convertFromBase64(mos, str))
            return nullptr;
    }

    juce::MemoryInputStream mis(block, false);
    const int numChannels = mis.readInt();
    const int numSamples  = mis.readInt();
    if (numChannels <= 0 || numSamples <= 0)
        return nullptr;

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(numChannels, numSamples);
    buffer->clear();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto bytesToRead = static_cast<size_t>(numSamples) * sizeof(float);
        if (mis.getNumBytesRemaining() < static_cast<std::int64_t>(bytesToRead))
            return nullptr;
        mis.read(buffer->getWritePointer(ch), bytesToRead);
    }
    return buffer;
}

juce::String floatVectorToBase64(const std::vector<float>& vec)
{
    if (vec.empty())
        return {};
    juce::MemoryBlock block(vec.data(), vec.size() * sizeof(float));
    return juce::Base64::toBase64(block.getData(), block.getSize());
}

std::vector<float> floatVectorFromBase64(const juce::String& str)
{
    if (str.isEmpty())
        return {};

    juce::MemoryBlock block;
    juce::MemoryOutputStream out(block, false);
    if (!juce::Base64::convertFromBase64(out, str))
        return {};

    const auto numBytes = block.getSize();
    if (numBytes == 0 || numBytes % sizeof(float) != 0)
        return {};

    std::vector<float> vec(numBytes / sizeof(float));
    std::memcpy(vec.data(), block.getData(), numBytes);
    return vec;
}

void serializeSourceToXml(const SourceStore::SourceSnapshot& snap, juce::XmlElement& parent)
{
    auto* el = parent.createNewChildElement("source");
    el->setAttribute("id",                juce::String::toHexString(snap.sourceId));
    el->setAttribute("sampleRate",        snap.sampleRate);
    el->setAttribute("numChannels",       snap.numChannels);
    el->setAttribute("numSamples",        static_cast<int>(snap.numSamples));
    el->createNewChildElement("displayName")->addTextElement(snap.displayName);

    const auto audioB64 = audioBufferToBase64(snap.audioBuffer);
    if (audioB64.isNotEmpty())
        el->createNewChildElement("audio")->addTextElement(audioB64);
}

void serializeMaterializationToXml(const MaterializationStore::MaterializationSnapshot& snap,
                                   juce::XmlElement& parent)
{
    auto* el = parent.createNewChildElement("mat");
    el->setAttribute("id",          juce::String::toHexString(snap.materializationId));
    el->setAttribute("sourceId",    juce::String::toHexString(snap.sourceId));
    el->setAttribute("lineageParent",
                     juce::String::toHexString(snap.lineageParentMaterializationId));
    el->setAttribute("f0State",     static_cast<int>(snap.originalF0State));
    el->setAttribute("renderRev",   static_cast<int>(snap.renderRevision));

    // sourceWindow
    auto* sw = el->createNewChildElement("sourceWindow");
    sw->setAttribute("sourceId",    juce::String::toHexString(snap.sourceWindow.sourceId));
    sw->setAttribute("startSec",    snap.sourceWindow.sourceStartSeconds);
    sw->setAttribute("endSec",      snap.sourceWindow.sourceEndSeconds);

    // audio
    const auto audioB64 = audioBufferToBase64(snap.audioBuffer);
    if (audioB64.isNotEmpty())
        el->createNewChildElement("audio")->addTextElement(audioB64);

    // pitchCurve
    {
        auto* pcEl = el->createNewChildElement("pitchCurve");
        if (snap.pitchCurve)
        {
            auto snapshot = snap.pitchCurve->getSnapshot();
            pcEl->setAttribute("hopSize",    snapshot->getHopSize());
            pcEl->setAttribute("sampleRate", snapshot->getSampleRate());

            const auto origF0    = floatVectorToBase64(snapshot->getOriginalF0());
            const auto origEnergy = floatVectorToBase64(snapshot->getOriginalEnergy());
            if (origF0.isNotEmpty())
                pcEl->createNewChildElement("originalF0")->addTextElement(origF0);
            if (origEnergy.isNotEmpty())
                pcEl->createNewChildElement("originalEnergy")->addTextElement(origEnergy);

            const auto& segments = snapshot->getCorrectedSegments();
            if (!segments.empty())
            {
                auto* segsEl = pcEl->createNewChildElement("segments");
                for (const auto& seg : segments)
                {
                    auto* segEl = segsEl->createNewChildElement("seg");
                    segEl->setAttribute("start",       seg.startFrame);
                    segEl->setAttribute("end",         seg.endFrame);
                    segEl->setAttribute("source",      static_cast<int>(seg.source));
                    segEl->setAttribute("retuneSpeed", seg.retuneSpeed);
                    segEl->setAttribute("vibDepth",    seg.vibratoDepth);
                    segEl->setAttribute("vibRate",     seg.vibratoRate);
                    const auto f0B64 = floatVectorToBase64(seg.f0Data);
                    if (f0B64.isNotEmpty())
                        segEl->createNewChildElement("f0")->addTextElement(f0B64);
                }
            }
        }
    }

    // notes
    if (!snap.notes.empty())
    {
        auto* notesEl = el->createNewChildElement("notes");
        for (const auto& note : snap.notes)
        {
            auto* nEl = notesEl->createNewChildElement("n");
            nEl->setAttribute("start",   note.startTime);
            nEl->setAttribute("end",     note.endTime);
            nEl->setAttribute("pitch",   note.pitch);
            nEl->setAttribute("origPitch", note.originalPitch);
            nEl->setAttribute("pitchOff",  note.pitchOffset);
            nEl->setAttribute("retune",    note.retuneSpeed);
            nEl->setAttribute("vibDep",    note.vibratoDepth);
            nEl->setAttribute("vibRate",   note.vibratoRate);
            nEl->setAttribute("vel",       note.velocity);
            nEl->setAttribute("voiced",    note.isVoiced ? 1 : 0);
            nEl->setAttribute("sel",       note.selected ? 1 : 0);
            nEl->setAttribute("dirty",     note.dirty ? 1 : 0);
        }
    }

    // silentGaps
    if (!snap.silentGaps.empty())
    {
        auto* gapsEl = el->createNewChildElement("silentGaps");
        for (const auto& gap : snap.silentGaps)
        {
            auto* gEl = gapsEl->createNewChildElement("gap");
            gEl->setAttribute("start",    static_cast<double>(gap.startSample));
            gEl->setAttribute("end",      static_cast<double>(gap.endSampleExclusive));
            gEl->setAttribute("minLevel", gap.minLevel_dB);
        }
    }

    // detectedKey
    auto* dkEl = el->createNewChildElement("detectedKey");
    dkEl->setAttribute("root",       static_cast<int>(snap.detectedKey.root));
    dkEl->setAttribute("scale",      static_cast<int>(snap.detectedKey.scale));
    dkEl->setAttribute("confidence", snap.detectedKey.confidence);

    // pitchShiftSettings
    auto* psEl = el->createNewChildElement("pitchShift");
    psEl->setAttribute("semitone", snap.pitchShiftSettings.semitone);
    psEl->setAttribute("cents",    snap.pitchShiftSettings.cents);

    // timeGrid
    if (snap.timeGrid)
    {
        auto* tgEl = el->createNewChildElement("timeGrid");
        tgEl->setAttribute("revision", static_cast<int>(snap.timeGridRevision));
        const auto& handles = snap.timeGrid->handles();
        if (!handles.empty())
        {
            auto* hElParent = tgEl->createNewChildElement("handles");
            for (const auto& handle : handles)
            {
                auto* hEl = hElParent->createNewChildElement("handle");
                hEl->setAttribute("id",         static_cast<double>(handle.id));
                hEl->setAttribute("sourceSec",  handle.source_seconds);
                hEl->setAttribute("outputSec",  handle.output_seconds);
                hEl->setAttribute("kind",       static_cast<int>(handle.kind));
                hEl->setAttribute("locked",     handle.locked ? 1 : 0);
                hEl->setAttribute("confidence", static_cast<int>(handle.confidence));
            }
        }
    }
}

// Deserialization helpers

bool deserializeSourceFromXml(const juce::XmlElement& el, SourceStore& store)
{
    const auto sourceIdStr = el.getStringAttribute("id");
    if (sourceIdStr.isEmpty())
        return false;
    const uint64_t sourceId = sourceIdStr.getHexValue64();

    const double sampleRate = el.getDoubleAttribute("sampleRate", 44100.0);
    const int numChannels   = el.getIntAttribute("numChannels", 1);
    const int64_t numSamples = static_cast<int64_t>(el.getIntAttribute("numSamples", 0));

    juce::String displayName;
    if (auto* nameEl = el.getChildByName("displayName"))
        displayName = nameEl->getAllSubText().trim();

    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    if (auto* audioEl = el.getChildByName("audio"))
        audioBuffer = audioBufferFromBase64(audioEl->getAllSubText().trim());

    SourceStore::CreateSourceRequest req;
    req.displayName = displayName;
    req.audioBuffer = std::move(audioBuffer);
    req.sampleRate = sampleRate > 0.0 ? sampleRate : TimeCoordinate::kRenderSampleRate;
    req.numChannels = numChannels;
    req.numSamples = numSamples;
    return store.createSource(std::move(req), sourceId) == sourceId;
}

std::shared_ptr<PitchCurve> deserializePitchCurve(const juce::XmlElement* pcEl)
{
    if (pcEl == nullptr)
        return nullptr;

    const int hopSize    = pcEl->getIntAttribute("hopSize", 512);
    const double sampleRate = pcEl->getDoubleAttribute("sampleRate", 16000.0);
    if (hopSize <= 0 || sampleRate <= 0.0)
        return nullptr;

    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(hopSize);
    curve->setSampleRate(sampleRate);

    if (auto* f0El = pcEl->getChildByName("originalF0"))
    {
        auto f0Data = floatVectorFromBase64(f0El->getAllSubText().trim());
        if (!f0Data.empty())
            curve->setOriginalF0(f0Data);
    }

    if (auto* energyEl = pcEl->getChildByName("originalEnergy"))
    {
        auto energyData = floatVectorFromBase64(energyEl->getAllSubText().trim());
        if (!energyData.empty())
            curve->setOriginalEnergy(energyData);
    }

    if (auto* segsEl = pcEl->getChildByName("segments"))
    {
        for (auto* segEl : segsEl->getChildWithTagNameIterator("seg"))
        {
            CorrectedSegment seg;
            seg.startFrame    = segEl->getIntAttribute("start");
            seg.endFrame      = segEl->getIntAttribute("end");
            seg.source        = static_cast<CorrectedSegment::Source>(
                segEl->getIntAttribute("source", 0));
            seg.retuneSpeed   = segEl->getDoubleAttribute("retuneSpeed", -1.0f);
            seg.vibratoDepth  = segEl->getDoubleAttribute("vibDepth", -1.0f);
            seg.vibratoRate   = segEl->getDoubleAttribute("vibRate", -1.0f);

            if (auto* f0El = segEl->getChildByName("f0"))
                seg.f0Data = floatVectorFromBase64(f0El->getAllSubText().trim());

            if (seg.startFrame < seg.endFrame && !seg.f0Data.empty())
                curve->restoreCorrectedSegment(seg);
        }
    }

    return curve;
}

bool deserializeMaterializationFromXml(const juce::XmlElement& el,
                                       MaterializationStore& store,
                                       SourceStore& sourceStore)
{
    const auto idStr = el.getStringAttribute("id");
    const auto srcIdStr = el.getStringAttribute("sourceId");
    if (idStr.isEmpty() || srcIdStr.isEmpty())
        return false;

    const uint64_t matId   = idStr.getHexValue64();
    const uint64_t sourceId = srcIdStr.getHexValue64();

    if (matId == 0 || sourceId == 0)
        return false;

    // Build createMaterialization request
    MaterializationStore::CreateMaterializationRequest req;
    req.sourceId = sourceId;
    req.lineageParentMaterializationId = juce::String(el.getStringAttribute("lineageParent", "0")).getHexValue64();
    req.originalF0State = static_cast<OriginalF0State>(
        el.getIntAttribute("f0State", static_cast<int>(OriginalF0State::NotRequested)));
    req.renderRevision = static_cast<uint64_t>(el.getIntAttribute("renderRev", 0));

    // sourceWindow
    if (auto* swEl = el.getChildByName("sourceWindow"))
    {
        req.sourceWindow.sourceId = juce::String(swEl->getStringAttribute("sourceId", "0")).getHexValue64();
        req.sourceWindow.sourceStartSeconds = swEl->getDoubleAttribute("startSec", 0.0);
        req.sourceWindow.sourceEndSeconds   = swEl->getDoubleAttribute("endSec", 0.0);
    }

    // audio
    if (auto* audioEl = el.getChildByName("audio"))
        req.audioBuffer = audioBufferFromBase64(audioEl->getAllSubText().trim());

    // pitchCurve
    req.pitchCurve = deserializePitchCurve(el.getChildByName("pitchCurve"));

    // notes
    if (auto* notesEl = el.getChildByName("notes"))
    {
        std::vector<Note> notes;
        for (auto* nEl : notesEl->getChildWithTagNameIterator("n"))
        {
            Note note;
            note.startTime    = nEl->getDoubleAttribute("start");
            note.endTime      = nEl->getDoubleAttribute("end");
            note.pitch        = static_cast<float>(nEl->getDoubleAttribute("pitch"));
            note.originalPitch = static_cast<float>(nEl->getDoubleAttribute("origPitch"));
            note.pitchOffset  = static_cast<float>(nEl->getDoubleAttribute("pitchOff"));
            note.retuneSpeed  = static_cast<float>(nEl->getDoubleAttribute("retune", -1.0));
            note.vibratoDepth = static_cast<float>(nEl->getDoubleAttribute("vibDep", -1.0));
            note.vibratoRate  = static_cast<float>(nEl->getDoubleAttribute("vibRate", -1.0));
            note.velocity     = static_cast<float>(nEl->getDoubleAttribute("vel", 1.0));
            note.isVoiced     = nEl->getIntAttribute("voiced", 1) != 0;
            note.selected     = nEl->getIntAttribute("sel", 0) != 0;
            note.dirty        = nEl->getIntAttribute("dirty", 0) != 0;
            notes.push_back(note);
        }
        req.notes = std::move(notes);
    }

    // silentGaps
    if (auto* gapsEl = el.getChildByName("silentGaps"))
    {
        std::vector<SilentGap> gaps;
        for (auto* gEl : gapsEl->getChildWithTagNameIterator("gap"))
        {
            SilentGap gap;
            gap.startSample      = static_cast<int64_t>(gEl->getDoubleAttribute("start"));
            gap.endSampleExclusive = static_cast<int64_t>(gEl->getDoubleAttribute("end"));
            gap.minLevel_dB      = static_cast<float>(gEl->getDoubleAttribute("minLevel", 0.0));
            gaps.push_back(gap);
        }
        req.silentGaps = std::move(gaps);
    }

    // detectedKey
    if (auto* dkEl = el.getChildByName("detectedKey"))
    {
        DetectedKey dk;
        dk.root       = static_cast<Key>(dkEl->getIntAttribute("root", 0));
        dk.scale      = static_cast<Scale>(dkEl->getIntAttribute("scale", 0));
        dk.confidence = static_cast<float>(dkEl->getDoubleAttribute("confidence", 0.0));
        req.detectedKey = dk;
    }

    // pitchShiftSettings (captured separately — not in CreateMaterializationRequest)
    PitchShiftSettings pitchShift;
    bool hasPitchShift = false;
    if (auto* psEl = el.getChildByName("pitchShift"))
    {
        pitchShift.semitone = psEl->getIntAttribute("semitone", 0);
        pitchShift.cents    = psEl->getIntAttribute("cents", 0);
        hasPitchShift = true;
    }

    // timeGrid
    if (auto* tgEl = el.getChildByName("timeGrid"))
    {
        if (auto* handlesEl = tgEl->getChildByName("handles"))
        {
            std::vector<TimeHandle> handles;
            for (auto* hEl : handlesEl->getChildWithTagNameIterator("handle"))
            {
                TimeHandle handle;
                handle.id              = static_cast<uint64_t>(hEl->getIntAttribute("id"));
                handle.source_seconds  = hEl->getDoubleAttribute("sourceSec");
                handle.output_seconds  = hEl->getDoubleAttribute("outputSec");
                handle.kind            = static_cast<HandleKind>(hEl->getIntAttribute("kind", 0));
                handle.locked          = hEl->getIntAttribute("locked", 0) != 0;
                handle.confidence      = static_cast<Confidence>(hEl->getIntAttribute("confidence", 0));
                handles.push_back(handle);
            }
            if (!handles.empty())
                req.timeGrid = TimeGridSnapshot::makeFromHandles(std::move(handles),
                    static_cast<uint64_t>(tgEl->getIntAttribute("revision", 0)));
        }
    }

    req.renderCache = std::make_shared<RenderCache>();

    const uint64_t createdId = store.createMaterialization(std::move(req), matId);
    if (createdId != matId)
        return false;

    if (hasPitchShift)
        store.setPitchShiftSettings(matId, pitchShift);

    return true;
}

} // namespace

void OpenTuneDocumentController::getContentSnapshot(juce::XmlElement& dest) const
{
    dest.setAttribute("version", 1);

    // Serialize sources
    if (sourceStore_)
    {
        const auto sourceIds = sourceStore_->getAllActiveSourceIds();
        if (!sourceIds.empty())
        {
            auto* sourcesEl = dest.createNewChildElement("sources");
            for (uint64_t sid : sourceIds)
            {
                SourceStore::SourceSnapshot snap;
                if (sourceStore_->getSnapshot(sid, snap))
                    serializeSourceToXml(snap, *sourcesEl);
            }
        }
    }

    // Serialize materializations
    if (materializationStore_)
    {
        const auto matIds = materializationStore_->getAllActiveMaterializationIds();
        if (!matIds.empty())
        {
            auto* matsEl = dest.createNewChildElement("materializations");
            for (uint64_t mid : matIds)
            {
                MaterializationStore::MaterializationSnapshot snap;
                if (materializationStore_->getSnapshot(mid, snap))
                    serializeMaterializationToXml(snap, *matsEl);
            }
        }
    }
}

void OpenTuneDocumentController::restoreContentPayloadInto(const juce::XmlElement& src)
{
    const int version = src.getIntAttribute("version", 0);
    if (version != 1)
        return;

    if (materializationStore_ == nullptr || sourceStore_ == nullptr)
        return;

    // 1. Restore sources first
    if (auto* sourcesEl = src.getChildByName("sources"))
    {
        for (auto* sourceEl : sourcesEl->getChildWithTagNameIterator("source"))
            deserializeSourceFromXml(*sourceEl, *sourceStore_);
    }

    // 2. Restore materializations (must come after sources)
    if (auto* matsEl = src.getChildByName("materializations"))
    {
        for (auto* matEl : matsEl->getChildWithTagNameIterator("mat"))
            deserializeMaterializationFromXml(*matEl, *materializationStore_, *sourceStore_);
    }
}

ContentRenderService* OpenTuneDocumentController::getContentRenderService() const noexcept
{
    return contentRenderService_;
}

SourceStore* OpenTuneDocumentController::getSourceStore() const noexcept
{
    return sourceStore_.get();
}

bool OpenTuneDocumentController::PlaybackRegionProjection::isRenderable() const noexcept
{
    return contentKey.isValid()
        && materializationDurationSeconds > 0.0
        && durationInPlaybackTime > 0.0
        && durationInModificationTime > 0.0;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjections() const
{
    return buildProjections();
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getPlaybackRegionProjectionsFor(
    const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions.size());

    for (auto* playbackRegion : playbackRegions)
    {
        const auto* region = findPlaybackRegion(playbackRegion);
        if (region != nullptr && region->hasValidPlacement())
            projections.push_back(makeProjection(*region));
    }

    return projections;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getEditorSelectionPlaybackRegionProjections() const
{
    return getPlaybackRegionProjectionsFor(editorSelectionPlaybackRegions_);
}

std::optional<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::getFocusedEditorPlaybackRegionProjection() const
{
    const auto projections = getEditorSelectionPlaybackRegionProjections();
    if (projections.empty())
        return std::nullopt;

    return projections.front();
}

int OpenTuneDocumentController::refreshAllAudioModifications()
{
    std::set<juce::String> uniqueModIds;
    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            uniqueModIds.insert(region.audioModificationPersistentId);
    }

    if (uniqueModIds.empty())
        return 0;

    int refreshedCount = 0;

    for (const auto& modId : uniqueModIds)
    {
        auto* modification = findAudioModification(modId);
        if (modification == nullptr)
            continue;

        if (birthMaterializationForModification(*modification))
            ++refreshedCount;
    }

    if (refreshedCount > 0)
        refreshRegisteredRenderers(publishModelChange());

    return refreshedCount;
}

void OpenTuneDocumentController::setEditorViewSelectionPlaybackRegions(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    editorSelectionPlaybackRegions_ = std::move(playbackRegions);
    reconcileEditorSelectionPlaybackRegions();
}

void OpenTuneDocumentController::registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    if (std::find(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer) == playbackRenderers_.end())
        playbackRenderers_.push_back(&renderer);
}

void OpenTuneDocumentController::unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer)
{
    playbackRenderers_.erase(std::remove(playbackRenderers_.begin(), playbackRenderers_.end(), &renderer),
                             playbackRenderers_.end());
}

void OpenTuneDocumentController::didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext)
{
    juce::ignoreUnused(musicalContext);
}

void OpenTuneDocumentController::willBeginEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
}

void OpenTuneDocumentController::didEndEditing(juce::ARADocument* document)
{
    juce::ignoreUnused(document);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification)
{
    auto& modification = ensureAudioModification(audioModification);
    modification.updateIdentity(audioModification);
    if (auto* source = findAudioSource(audioModification != nullptr ? audioModification->getAudioSource() : nullptr))
        modification.attachSource(*source);

    // 如果 modification 被 host 重新激活（undo 删除），检查 retiredContents_ 并恢复
    const auto key = modification.contentKey();
    if (key.isValid() && modification.content.lifecycle <= ContentLifecycle::Empty)
    {
        if (reviveRetiredContentByKey(key, modification))
        {
            if (modification.audioModification != nullptr)
                modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyAudioModification(juce::ARAAudioModification* audioModification)
{
    auto* mod = findAudioModification(audioModification);
    if (mod)
    {
        // Retire active content into DC-level pool before destruction
        if (mod->content.lifecycle >= ContentLifecycle::Loading)
            mod->retireCurrentContent();

        // Transfer all retired records to DC-level pool
        for (auto& rec : mod->retiredContentRecords)
            retiredContents_.push_back(std::move(rec));
        mod->retiredContentRecords.clear();
    }

    const auto persistentId = mod != nullptr ? mod->persistentId : juce::String();
    audioModifications_.erase(std::remove_if(audioModifications_.begin(), audioModifications_.end(),
                                             [audioModification](const AudioModification& record)
                                             {
                                                 return record.audioModification == audioModification;
                                             }),
                              audioModifications_.end());
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [&persistentId](const PlaybackRegion& region)
                                          {
                                              return persistentId.isNotEmpty()
                                                  && region.audioModificationPersistentId == persistentId;
                                          }),
                           playbackRegions_.end());
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

void OpenTuneDocumentController::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion)
{
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

void OpenTuneDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    auto& modification = ensureAudioModification(audioModification);
    auto& region = ensurePlaybackRegion(playbackRegion);
    region.updateFrom(playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    auto& source = ensureAudioSource(audioSource);
    source.updateFrom(audioSource);
    for (auto& modification : audioModifications_)
        if (modification.audioModification != nullptr && modification.audioModification->getAudioSource() == audioSource)
            modification.attachSource(source);

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                                            juce::ARAContentUpdateScopes scopeFlags)
{
    juce::ignoreUnused(scopeFlags);

    if (auto* source = findAudioSource(audioSource))
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.sourcePersistentId == source->getIdentity().persistentId)
                modification.resetContent();
        }
    }

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                    bool enable)
{
    ensureAudioSource(audioSource).setSampleAccessEnabled(enable);
}

void OpenTuneDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    auto& source = ensureAudioSource(audioSource);
    source.setSampleAccessEnabled(enable);
    if (enable)
        source.createReaderLease();

    refreshRegisteredRenderers(publishModelChange());
}

void OpenTuneDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused(audioModification);
    removePlaybackRegion(playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers(publishModelChange());

    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

void OpenTuneDocumentController::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    AudioSource* source = findAudioSource(audioSource);
    const auto persistentId = source != nullptr ? source->getIdentity().persistentId : juce::String();
    audioSources_.erase(std::remove_if(audioSources_.begin(), audioSources_.end(),
                                       [audioSource](const AudioSource& source)
                                       {
                                           return source.matches(audioSource);
                                       }),
                        audioSources_.end());
    if (persistentId.isNotEmpty())
    {
        for (auto& modification : audioModifications_)
        {
            if (modification.sourcePersistentId == persistentId)
                modification.resetContent();
        }
    }

    refreshRegisteredRenderers(publishModelChange());

    if (onReclaimNeeded_)
        onReclaimNeeded_();
}

namespace {
juce::String mapRestoredPersistentId(const juce::String& archivedPersistentId,
                                     const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID(
        archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String() : juce::String::fromUTF8(restoredPersistentId.c_str());
}
} // namespace

bool OpenTuneDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                             const juce::ARARestoreObjectsFilter* filter)
{
    const int magic = input.readInt();
    if (magic != kContentPayloadArchiveMagic)
        return false;

    const int version = input.readInt();
    if (version != kContentPayloadArchiveVersion)
        return false;

    const int bindingCount = input.readInt();
    if (bindingCount < 0 || bindingCount > kMaxContentPayloadRecords)
        return false;

    // Restore retired content records into DC-level pool.
    for (int i = 0; i < bindingCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        juce::ignoreUnused(archivedPersistentId);
        const auto restoredPersistentId = mapRestoredPersistentId(archivedPersistentId, filter);
        juce::ignoreUnused(restoredPersistentId);

        // Read retired content record count
        const int retiredCount = input.readInt();

        for (int r = 0; r < retiredCount; ++r)
        {
            RetiredContentRecord record;
            record.key.domainKind = static_cast<DomainKind>(input.readInt());
            record.key.objectId = static_cast<uint64_t>(input.readInt64());
            record.content.lifecycle = ContentLifecycle::Retired;
            retiredContents_.push_back(std::move(record));
        }
    }

    // 将已恢复的退休记录匹配到现存 modification 上
    for (auto& mod : audioModifications_)
    {
        const auto key = mod.contentKey();
        if (!key.isValid() || mod.audioModification == nullptr)
            continue;
        if (reviveRetiredContentByKey(key, mod))
            mod.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
    }

    refreshRegisteredRenderers(publishModelChange());
    return true;
}

bool OpenTuneDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                         const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<const AudioModification*> bindings;
    bindings.reserve(audioModifications_.size());

    if (filter == nullptr)
    {
        for (const auto& modification : audioModifications_)
            if (modification.persistentId.isNotEmpty() && modification.isRenderable())
                bindings.push_back(&modification);
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        for (const auto& modification : audioModifications_)
        {
            if (modification.persistentId.isEmpty() || !modification.isRenderable())
                continue;

            const auto* araMod = modification.audioModification;
            if (araMod == nullptr)
                continue;

            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*>(araMod);
            if (std::find(modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                bindings.push_back(&modification);
        }
    }

    if (bindings.size() > static_cast<size_t>(kMaxContentPayloadRecords))
        return false;

    bool ok = output.writeInt(kContentPayloadArchiveMagic);
    ok = output.writeInt(kContentPayloadArchiveVersion) && ok;
    ok = output.writeInt(static_cast<int>(bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        ok = output.writeString(modification->persistentId) && ok;

        // Write retired content records count + payload
        const auto retiredCount = static_cast<int>(modification->retiredContentRecords.size());
        ok = output.writeInt(retiredCount) && ok;
        for (const auto& record : modification->retiredContentRecords)
        {
            ok = output.writeInt(static_cast<int>(record.key.domainKind)) && ok;
            ok = output.writeInt64(static_cast<juce::int64>(record.key.objectId)) && ok;
        }
    }

    return ok;
}

juce::ARAPlaybackRenderer* OpenTuneDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new OpenTunePlaybackRenderer(getDocumentController(), this);
    registerPlaybackRenderer(*renderer);
    renderer->refreshRenderPlanFromDocument();
    return renderer;
}

juce::ARAEditorView* OpenTuneDocumentController::doCreateEditorView()
{
    return new OpenTuneEditorView(getDocumentController(), *this);
}

AudioSource* OpenTuneDocumentController::findAudioSource(juce::ARAAudioSource* audioSource)
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [audioSource](const AudioSource& source)
                                 {
                                     return source.matches(audioSource);
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

const AudioSource* OpenTuneDocumentController::findAudioSource(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioSources_.begin(), audioSources_.end(),
                                 [&persistentId](const AudioSource& source)
                                 {
                                     return source.getIdentity().persistentId == persistentId;
                                 });
    return it != audioSources_.end() ? &*it : nullptr;
}

AudioSource& OpenTuneDocumentController::ensureAudioSource(juce::ARAAudioSource* audioSource)
{
    if (auto* existing = findAudioSource(audioSource))
        return *existing;

    AudioSource source;
    source.updateFrom(audioSource);
    audioSources_.push_back(std::move(source));
    return audioSources_.back();
}

AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModification(const juce::String& persistentId) const
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [&persistentId](const AudioModification& modification)
                                 {
                                     return modification.persistentId == persistentId;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

AudioModification* OpenTuneDocumentController::findAudioModification(juce::ARAAudioModification* audioModification)
{
    const auto it = std::find_if(audioModifications_.begin(), audioModifications_.end(),
                                 [audioModification](const AudioModification& modification)
                                 {
                                     return modification.audioModification == audioModification;
                                 });
    return it != audioModifications_.end() ? &*it : nullptr;
}

AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key)
{
    for (auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

const AudioModification* OpenTuneDocumentController::findAudioModificationByContentKey(const ContentKey& key) const
{
    for (const auto& mod : audioModifications_)
        if (mod.contentKey() == key)
            return &mod;
    return nullptr;
}

AudioModification& OpenTuneDocumentController::ensureAudioModification(juce::ARAAudioModification* audioModification)
{
    if (auto* existing = findAudioModification(audioModification))
    {
        return *existing;
    }

    AudioModification modification;
    modification.updateIdentity(audioModification);
    if (audioModification != nullptr)
        if (auto* source = findAudioSource(audioModification->getAudioSource()))
            modification.attachSource(*source);
    audioModifications_.push_back(std::move(modification));
    return audioModifications_.back();
}

PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

const PlaybackRegion* OpenTuneDocumentController::findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const
{
    const auto it = std::find_if(playbackRegions_.begin(), playbackRegions_.end(),
                                 [playbackRegion](const PlaybackRegion& region)
                                 {
                                     return region.playbackRegion == playbackRegion;
                                 });
    return it != playbackRegions_.end() ? &*it : nullptr;
}

PlaybackRegion& OpenTuneDocumentController::ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    if (auto* existing = findPlaybackRegion(playbackRegion))
        return *existing;

    PlaybackRegion region;
    region.updateFrom(playbackRegion);
    playbackRegions_.push_back(std::move(region));
    return playbackRegions_.back();
}

OpenTuneDocumentController::PlaybackRegionProjection
OpenTuneDocumentController::makeProjection(const PlaybackRegion& placement) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = placement.playbackRegion;
    projection.audioModificationPersistentId = placement.audioModificationPersistentId;
    projection.placementRevision = placement.placementRevision;
    projection.startInPlaybackTime = placement.startInPlaybackTime;
    projection.startInModificationTime = placement.startInModificationTime;
    projection.durationInPlaybackTime = placement.durationInPlaybackTime;
    projection.durationInModificationTime = placement.durationInModificationTime;
    projection.timestretchEnabled = placement.timestretchEnabled;
    projection.timestretchReflectingTempo = placement.timestretchReflectingTempo;
    projection.contentBasedFadeAtHead = placement.contentBasedFadeAtHead;
    projection.contentBasedFadeAtTail = placement.contentBasedFadeAtTail;

    const auto* modification = findAudioModification(placement.audioModificationPersistentId);
    if (modification == nullptr)
        return projection;

    projection.contentWindow = modification->contentWindow;
    projection.sourceId = modification->sourceId;
    projection.contentRevision = modification->content.contentRevision;
    projection.materializationDurationSeconds = modification->materializationDurationSeconds;
    projection.contentKey = modification->contentKey();

    const auto* source = findAudioSource(modification->sourcePersistentId);
    if (source != nullptr)
    {
        projection.sampleRate = source->getShape().sourceSampleRate;
        projection.numChannels = source->getShape().numChannels;
    }

    return projection;
}

std::vector<OpenTuneDocumentController::PlaybackRegionProjection>
OpenTuneDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve(playbackRegions_.size());

    for (const auto& region : playbackRegions_)
    {
        if (region.hasValidPlacement())
            projections.push_back(makeProjection(region));
    }

    return projections;
}

std::vector<OpenTunePlaybackRenderer*> OpenTuneDocumentController::publishModelChange()
{
    return playbackRenderers_;
}

void OpenTuneDocumentController::refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        if (renderer != nullptr)
            renderer->refreshRenderPlanFromDocument();
}

void OpenTuneDocumentController::reconcileEditorSelectionPlaybackRegions()
{
    editorSelectionPlaybackRegions_.erase(
        std::remove_if(editorSelectionPlaybackRegions_.begin(),
                       editorSelectionPlaybackRegions_.end(),
                       [this](juce::ARAPlaybackRegion* playbackRegion)
                       {
                           return findPlaybackRegion(playbackRegion) == nullptr;
                       }),
        editorSelectionPlaybackRegions_.end());
}

bool OpenTuneDocumentController::birthMaterializationForModification(AudioModification& modification)
{
    if (modification.persistentId.isEmpty() || modification.sourcePersistentId.isEmpty())
        return false;

    auto* source = findAudioSource(modification.sourcePersistentId);
    if (source == nullptr || !source->canReadSamples())
    {
        modification.birthState = AudioModificationBirthState::WaitingForSource;
        return false;
    }

    if (sourceStore_ == nullptr)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    modification.birthState = AudioModificationBirthState::Rendering;
    ++modification.birthRevision;

    // 1. Determine source ID and window
    const auto sourceId = modification.sourceId != 0
        ? modification.sourceId
        : static_cast<uint64_t>(std::hash<std::string>{}(source->getIdentity().persistentId.toStdString()));

    auto readerLease = source->shareReaderLease();
    if (readerLease == nullptr || source->getShape().numChannels <= 0
        || source->getShape().numSamples <= 0 || source->getShape().sourceSampleRate <= 0.0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    const auto sourceWindow = modification.contentWindow.sourceId != 0
        ? modification.contentWindow
        : SourceWindow{sourceId, 0.0, source->getShape().durationSeconds()};

    const double sourceSampleRate = source->getShape().sourceSampleRate;
    const int64_t numSamples = source->getShape().numSamples;
    const int numChannels = source->getShape().numChannels;
    const juce::String sourceName = source->getAraAudioSource()->getName() != nullptr
        ? juce::String::fromUTF8(source->getAraAudioSource()->getName())
        : juce::String("ARA Source");

    // 2. Read ARA source window
    const int64_t sourceStartSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceStartSeconds * sourceSampleRate));
    const int64_t sourceEndSample = static_cast<int64_t>(
        std::round(sourceWindow.sourceEndSeconds * sourceSampleRate));
    const int64_t windowSamples = std::max<int64_t>(0,
        std::min<int64_t>(sourceEndSample, numSamples)
        - std::max<int64_t>(0, sourceStartSample));

    if (windowSamples <= 0)
    {
        modification.birthState = AudioModificationBirthState::Failed;
        return false;
    }

    juce::AudioBuffer<float> playableAccum(numChannels, static_cast<int>(windowSamples));
    playableAccum.clear();

    constexpr int64_t kChunkSamples = 32768;
    {
        int64_t readOffset = sourceStartSample;
        int64_t accumWriteOffset = 0;
        int64_t remaining = windowSamples;

        while (remaining > 0)
        {
            const int64_t chunkSamples = std::min(kChunkSamples, remaining);
            const int accumOffset = static_cast<int>(accumWriteOffset);

            std::vector<void*> channelPointers(static_cast<size_t>(numChannels));
            for (int ch = 0; ch < numChannels; ++ch)
                channelPointers[static_cast<size_t>(ch)] = playableAccum.getWritePointer(ch, accumOffset);

            if (!readerLease->readAudioSamples(readOffset,
                                                 static_cast<int>(chunkSamples),
                                                 channelPointers.data()))
            {
                modification.birthState = AudioModificationBirthState::Failed;
                return false;
            }

            readOffset += chunkSamples;
            accumWriteOffset += chunkSamples;
            remaining -= chunkSamples;
        }
    }

    // 3. Extract ch0 for async F0
    std::vector<float> channel0Data(static_cast<size_t>(playableAccum.getNumSamples()));
    {
        const float* ch0Read = playableAccum.getReadPointer(0);
        std::copy(ch0Read, ch0Read + playableAccum.getNumSamples(), channel0Data.begin());
    }

    // 4. Ensure source in SourceStore (metadata-only)
    if (!sourceStore_->containsSource(sourceId))
    {
        SourceStore::CreateSourceRequest sourceRequest;
        sourceRequest.displayName = sourceName;
        sourceRequest.numChannels = numChannels;
        sourceRequest.numSamples = numSamples;
        sourceRequest.sampleRate = sourceSampleRate > 0.0
            ? sourceSampleRate
            : TimeCoordinate::kRenderSampleRate;
        if (sourceStore_->createSource(std::move(sourceRequest), sourceId) != sourceId)
        {
            modification.birthState = AudioModificationBirthState::Failed;
            return false;
        }
    }

    // 5. Resample audio to 44.1kHz and populate AudioModification.content
    juce::AudioBuffer<float> storedBuffer;
    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    if (std::abs(sourceSampleRate - targetSampleRate) > 1.0)
    {
        const int storedLen = juce::jmax(1,
            static_cast<int>(TimeCoordinate::secondsToSamples(
                TimeCoordinate::samplesToSeconds(playableAccum.getNumSamples(), sourceSampleRate),
                targetSampleRate)));

        storedBuffer.setSize(numChannels, storedLen);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto resampledData = resamplingManager_->upsampleForHost(
                playableAccum.getReadPointer(ch),
                playableAccum.getNumSamples(),
                static_cast<int>(sourceSampleRate),
                static_cast<int>(targetSampleRate));
            const int toCopy = juce::jmin(storedLen, static_cast<int>(resampledData.size()));
            storedBuffer.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
    }
    else
    {
        storedBuffer = std::move(playableAccum);
    }

    // 6. Detect silent gaps and build content state
    auto silentGaps = SilentGapDetector::detectAllGapsAdaptive(storedBuffer);
    auto storedAudioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(storedBuffer));

    // Fill AudioModification.content (new content root)
    modification.content.sourceWindow = SourceWindow{sourceId,
                                                      sourceWindow.sourceStartSeconds,
                                                      sourceWindow.sourceEndSeconds};
    modification.content.lifecycle = ContentLifecycle::Loading;
    modification.content.analysis.silentGaps = std::move(silentGaps);
    modification.content.analysis.originalF0State = OriginalF0State::NotRequested;

    const double materializationDurationSeconds =
        TimeCoordinate::samplesToSeconds(storedAudioBuffer->getNumSamples(), targetSampleRate);

    // 7. Publish to CRS if available (owns render cache + audio buffer)
    if (contentRenderService_ != nullptr)
    {
        auto renderCache = contentRenderService_->getOrCreateRenderCache(modification.contentKey());
        PlaybackReadSource readSource;
        readSource.renderCache = renderCache;
        readSource.audioBuffer = storedAudioBuffer;
        contentRenderService_->publishPlaybackSource(modification.contentKey(), readSource);
    }

    // 8. Set modification fields and notify ARA host
    modification.sourceId = sourceId;
    modification.contentWindow = SourceWindow{sourceId,
                                               sourceWindow.sourceStartSeconds,
                                               sourceWindow.sourceEndSeconds};
    modification.materializationDurationSeconds = materializationDurationSeconds;
    modification.birthState = AudioModificationBirthState::Ready;
    modification.content.lifecycle = ContentLifecycle::Ready;
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);

    // 9. Schedule async F0 extraction via CRS
    if (contentRenderService_ != nullptr)
        scheduleAsyncF0Extraction(modification.contentKey().objectId, std::move(channel0Data), sourceSampleRate);

    return true;
}

void OpenTuneDocumentController::scheduleAsyncF0Extraction(
    uint64_t contentObjectId,
    std::vector<float> channel0Data,
    double sourceSampleRate)
{
    if (!scheduleAsyncWork_)
        return;

    auto crs = contentRenderService_;
    auto f0Svc = f0Service_;
    const ContentKey key{DomainKind::ARAAudioModification, contentObjectId, 0};

    scheduleAsyncWork_([this, crs, f0Svc, key, leaseToken = asyncLeaseToken_,
                        data = std::move(channel0Data),
                        sourceSampleRate]()
    {
        if (leaseToken && !leaseToken->load(std::memory_order_acquire))
            return;

        if (f0Svc == nullptr || data.empty())
            return;

        auto extraction = f0Svc->extractF0(data.data(), data.size(),
                                            static_cast<int>(sourceSampleRate));

        if (!extraction.ok() || extraction.value().empty())
        {
            f0Svc->releaseImmediately();
            return;
        }

        const int hopSize = f0Svc->getF0HopSize();
        const int f0SampleRate = f0Svc->getF0SampleRate();

        auto pitchCurve = std::make_shared<PitchCurve>();
        pitchCurve->setHopSize(hopSize);
        pitchCurve->setSampleRate(static_cast<double>(f0SampleRate));
        pitchCurve->setOriginalF0(extraction.value());

        // Compute energy from channel0Data
        {
            std::vector<float> energy(extraction.value().size(), 0.0f);
            const double f0SecondsPerFrame = static_cast<double>(hopSize)
                / static_cast<double>(juce::jmax(1, f0SampleRate));
            const int halfRmsWindowSamples = juce::jmax(1,
                static_cast<int>(std::round(sourceSampleRate * 0.010)));
            for (size_t i = 0; i < extraction.value().size(); ++i)
            {
                if (!std::isfinite(extraction.value()[i]) || extraction.value()[i] <= 0.0f)
                    continue;
                const int centerSample = juce::jlimit(0, static_cast<int>(data.size()) - 1,
                    static_cast<int>(std::round(static_cast<double>(i) * f0SecondsPerFrame * sourceSampleRate)));
                const int startSample = juce::jmax(0, centerSample - halfRmsWindowSamples);
                const int endSampleExclusive = juce::jmin(static_cast<int>(data.size()),
                    centerSample + halfRmsWindowSamples);
                if (endSampleExclusive <= startSample) continue;
                double squareSum = 0.0;
                for (int s = startSample; s < endSampleExclusive; ++s)
                {
                    const float v = data[static_cast<size_t>(s)];
                    squareSum += static_cast<double>(v) * static_cast<double>(v);
                }
                const double meanSquare = squareSum / static_cast<double>(endSampleExclusive - startSample);
                energy[i] = juce::jlimit(0.0f, 1.0f, static_cast<float>(std::sqrt(meanSquare)));
            }
            pitchCurve->setOriginalEnergy(energy);
        }

        f0Svc->releaseImmediately();

        // Commit results to AudioModification content on the message thread
        juce::MessageManager::callAsync([this, crs, key, leaseToken,
                                          pc = std::move(pitchCurve)]() mutable
        {
            if (leaseToken && !leaseToken->load(std::memory_order_acquire))
                return;

            if (crs == nullptr)
                return;

            // Wire F0 analysis into AudioModification content state
            if (auto* mod = findAudioModificationByContentKey(key))
            {
                mod->applyF0Analysis(std::move(pc));
                if (mod->audioModification != nullptr)
                    mod->audioModification->notifyContentChanged(juce::ARAContentUpdateScopes(), true);
            }
        });
    });
}

bool OpenTuneDocumentController::removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto oldSize = playbackRegions_.size();
    playbackRegions_.erase(std::remove_if(playbackRegions_.begin(), playbackRegions_.end(),
                                          [playbackRegion](const PlaybackRegion& region)
                                          {
                                              return region.playbackRegion == playbackRegion;
                                          }),
                           playbackRegions_.end());
    return playbackRegions_.size() != oldSize;
}

// RestoredMaterializationBinding removed — ARA archive now uses ContentKey + content payload.
// Legacy archive records are skipped during doRestoreObjectsFromStream.

bool OpenTuneDocumentController::requestSetPlaybackPosition(double timeInSeconds)
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestSetPlaybackPosition(timeInSeconds);
    return true;
}

bool OpenTuneDocumentController::requestStartPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStartPlayback();
    return true;
}

bool OpenTuneDocumentController::requestStopPlayback()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
        return false;

    auto* playbackController = dc->getHostPlaybackController();
    if (playbackController == nullptr)
        return false;

    playbackController->requestStopPlayback();
    return true;
}

// -----------------------------------------------------------------------
// 编辑器只读内容访问器实现
// -----------------------------------------------------------------------

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneDocumentController::readAudioBuffer(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr && contentRenderService_->getPlaybackReadSource(key, crsSrc))
        return crsSrc.audioBuffer;
    return nullptr;
}

std::shared_ptr<PitchCurve> OpenTuneDocumentController::readPitchCurve(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return nullptr;
    return mod->content.analysis.pitchCurve;
}

OriginalF0State OpenTuneDocumentController::readOriginalF0State(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return OriginalF0State::NotRequested;
    return mod->content.analysis.originalF0State;
}

DetectedKey OpenTuneDocumentController::readDetectedKey(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.analysis.detectedKey;
}

std::vector<Note> OpenTuneDocumentController::readNotes(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.editable.notes;
}

MaterializationStore::MaterializationNotesSnapshot OpenTuneDocumentController::readNotesSnapshot(ContentKey key) const
{
    MaterializationStore::MaterializationNotesSnapshot snap;
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod != nullptr)
    {
        snap.notes = mod->content.editable.notes;
        snap.notesRevision = mod->content.editable.notesRevision;
    }
    return snap;
}

uint64_t OpenTuneDocumentController::readNotesRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.editable.notesRevision;
}

std::shared_ptr<const TimeGridSnapshot> OpenTuneDocumentController::readTimeGrid(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return nullptr;
    return mod->content.editable.timeGrid;
}

uint64_t OpenTuneDocumentController::readTimeGridRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.editable.timeGridRevision;
}

PitchShiftSettings OpenTuneDocumentController::readPitchShift(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return {};
    return mod->content.editable.pitchShiftSettings;
}

RenderCache::ChunkStats OpenTuneDocumentController::readChunkStats(ContentKey key) const
{
    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr
        && contentRenderService_->getPlaybackReadSource(key, crsSrc)
        && crsSrc.renderCache != nullptr)
    {
        return crsSrc.renderCache->getChunkStats();
    }
    return {};
}

bool OpenTuneDocumentController::readChunkBoundaries(ContentKey key, std::vector<double>& outSeconds) const
{
    outSeconds.clear();

    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr)
        return false;

    PlaybackReadSource crsSrc;
    if (contentRenderService_ == nullptr || !contentRenderService_->getPlaybackReadSource(key, crsSrc))
        return false;
    if (crsSrc.audioBuffer == nullptr)
        return false;

    const int64_t sampleCount = static_cast<int64_t>(crsSrc.audioBuffer->getNumSamples());
    const auto& silentGaps = mod->content.analysis.silentGaps;
    constexpr int hopSize = 512; // DC 没有 vocoderDomain_，使用默认值

    auto boundaries = MaterializationStore::buildChunkBoundariesFromSilentGaps(
        sampleCount, silentGaps, hopSize);

    outSeconds.reserve(boundaries.size());
    for (auto sample : boundaries)
        outSeconds.push_back(static_cast<double>(sample) / TimeCoordinate::kRenderSampleRate);

    return true;
}

uint64_t OpenTuneDocumentController::readContentRevision(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->content.contentRevision;
}

double OpenTuneDocumentController::readMaterializationDuration(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0.0;
    return mod->materializationDurationSeconds;
}

uint64_t OpenTuneDocumentController::readSourceId(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return 0;
    return mod->sourceId;
}

bool OpenTuneDocumentController::hasContent(ContentKey key) const
{
    const auto* mod = findAudioModificationByContentKey(key);
    return mod != nullptr && mod->content.lifecycle == ContentLifecycle::Ready;
}

MaterializationStore::MaterializationSnapshot OpenTuneDocumentController::readSnapshot(ContentKey key) const
{
    MaterializationStore::MaterializationSnapshot snap;
    const auto* mod = findAudioModificationByContentKey(key);
    if (mod == nullptr) return snap;

    auto editableSnap = mod->snapshotContent();

    snap.sourceId = mod->sourceId;
    snap.sourceWindow = editableSnap->sourceWindow;
    snap.originalF0State = editableSnap->originalF0State;
    snap.detectedKey = editableSnap->detectedKey;
    snap.notes = editableSnap->notes;
    snap.notesRevision = editableSnap->notesRevision;
    snap.timeGrid = editableSnap->timeGrid;
    snap.timeGridRevision = editableSnap->timeGridRevision;
    snap.pitchShiftSettings = editableSnap->pitchShiftSettings;
    snap.pitchShiftRevision = editableSnap->pitchShiftRevision;
    snap.pitchCurve = editableSnap->pitchCurve;

    // Materialization 层特有字段：snapshotContent() 不含这些
    snap.silentGaps = mod->content.analysis.silentGaps;

    PlaybackReadSource crsSrc;
    if (contentRenderService_ != nullptr && contentRenderService_->getPlaybackReadSource(key, crsSrc))
    {
        snap.audioBuffer = crsSrc.audioBuffer;
        snap.renderCache = crsSrc.renderCache;
    }

    return snap;
}

} // namespace OpenTune

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<OpenTune::OpenTuneDocumentController>();
}
