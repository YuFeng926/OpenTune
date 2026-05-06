
#include "CapturePersistence.h"

#include "CaptureSession.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

namespace OpenTune::Capture {

namespace {
    // CAPz: capture block stores per-segment metadata + materializationId reference.
    // PCM and PianoRoll edits travel through standard processor state (writeAudioBuffer,
    // writePitchCurve, writeNotes, writeDetectedKey) under their materialization id;
    // capture block re-binds segments to those restored mats on load.
    // Old format CAPy embedded FLAC PCM and re-ran the render pipeline on load —
    // rejected here so users see a clean failure rather than silent partial restore.
    constexpr uint32_t kCaptureMagic    = 0x4341507A;  // 'CAPz' little-endian
    constexpr uint32_t kCaptureEndMagic = 0x78434150;  // 'xCAP' little-endian
}  // namespace

juce::MemoryBlock CapturePersistence::serialize(const CaptureSession& session)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream stream(out, false);

    // ── 1. Build ValueTree metadata for each persistable segment ─────────
    // Persistable = has a materialization id (segment was at least committed
    // for render). Capturing segments lack a matId and are skipped; Pending /
    // Processing / Edited segments all carry a matId once stopCapture has run.
    juce::ValueTree root("CaptureSession");
    int persistedCount = 0;
    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        for (const auto& seg : session.mutableSegments_) {
            if (seg->materializationId == 0)
                continue;
            juce::ValueTree segNode("Segment");
            segNode.setProperty("id", juce::String(seg->id), nullptr);
            segNode.setProperty("creationOrder", juce::String(seg->creationOrder), nullptr);
            segNode.setProperty("T_start", seg->T_start.load(std::memory_order_acquire), nullptr);
            segNode.setProperty("durationSeconds", seg->durationSeconds, nullptr);
            segNode.setProperty("captureSampleRate", seg->captureSampleRate, nullptr);
            segNode.setProperty("captureChannels", seg->captureChannels, nullptr);
            segNode.setProperty("materializationId",
                                juce::String(static_cast<juce::int64>(seg->materializationId)),
                                nullptr);
            root.appendChild(segNode, nullptr);
            ++persistedCount;
        }
    }

    // ── 2. Magic + metadata XML + per-segment fixed bytes ────────────────
    stream.writeInt(static_cast<int>(kCaptureMagic));

    const juce::String xml = root.toXmlString();
    const auto xmlUtf8 = xml.toRawUTF8();
    const auto xmlLen = static_cast<int>(std::strlen(xmlUtf8));
    stream.writeInt(xmlLen);
    stream.write(xmlUtf8, static_cast<size_t>(xmlLen));

    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        for (const auto& seg : session.mutableSegments_) {
            if (seg->materializationId == 0)
                continue;
            // Fixed 52 bytes per segment, ordered to match deserialize.
            stream.writeInt64(static_cast<juce::int64>(seg->id));
            stream.writeInt64(static_cast<juce::int64>(seg->creationOrder));
            stream.writeDouble(seg->T_start.load(std::memory_order_acquire));
            stream.writeDouble(seg->durationSeconds);
            stream.writeDouble(seg->captureSampleRate);
            stream.writeInt(seg->captureChannels);
            stream.writeInt64(static_cast<juce::int64>(seg->materializationId));
        }
    }

    stream.writeInt(static_cast<int>(kCaptureEndMagic));
    stream.flush();

    ChannelLayoutLog::logPersistenceSerialize(persistedCount);
    return out;
}

bool CapturePersistence::deserialize(CaptureSession& session, const juce::MemoryBlock& block)
{
    if (block.getSize() < sizeof(uint32_t) * 2)
        return false;

    juce::MemoryInputStream stream(block.getData(), block.getSize(), false);
    const uint32_t magic = static_cast<uint32_t>(stream.readInt());
    if (magic != kCaptureMagic) {
        ChannelLayoutLog::logPersistenceDeserializeReject(magic);
        return false;
    }

    // ── 1. Read metadata XML and parse ValueTree ────────────────────────
    const int xmlLen = stream.readInt();
    if (xmlLen <= 0 || xmlLen > 1024 * 1024)  // 1 MB sanity limit
        return false;
    juce::HeapBlock<char> xmlBuf(static_cast<size_t>(xmlLen) + 1);
    if (stream.read(xmlBuf.getData(), xmlLen) != xmlLen)
        return false;
    xmlBuf[xmlLen] = '\0';

    auto xmlElement = juce::XmlDocument::parse(juce::String::fromUTF8(xmlBuf.getData(), xmlLen));
    if (xmlElement == nullptr)
        return false;
    const auto root = juce::ValueTree::fromXml(*xmlElement);
    if (!root.isValid() || root.getType() != juce::Identifier("CaptureSession"))
        return false;

    // ── 2. Read each segment's fixed-bytes record ───────────────────────
    struct PersistedSegment {
        uint64_t id;
        uint64_t creationOrder;
        double   T_start;
        double   durationSeconds;
        double   captureSampleRate;
        int      captureChannels;
        uint64_t materializationId;
    };
    std::vector<PersistedSegment> persisted;
    persisted.reserve(static_cast<size_t>(root.getNumChildren()));

    for (int i = 0; i < root.getNumChildren(); ++i) {
        const auto segNode = root.getChild(i);
        if (segNode.getType() != juce::Identifier("Segment"))
            continue;

        PersistedSegment p {};
        p.id                = static_cast<uint64_t>(segNode.getProperty("id").toString().getLargeIntValue());
        p.creationOrder     = static_cast<uint64_t>(segNode.getProperty("creationOrder").toString().getLargeIntValue());
        p.T_start           = static_cast<double>(segNode.getProperty("T_start"));
        p.durationSeconds   = static_cast<double>(segNode.getProperty("durationSeconds"));
        p.captureSampleRate = static_cast<double>(segNode.getProperty("captureSampleRate"));
        p.captureChannels   = static_cast<int>(segNode.getProperty("captureChannels"));

        // Read fixed-bytes block (sequential, must match serialization order).
        const uint64_t fileId = static_cast<uint64_t>(stream.readInt64());
        if (fileId != p.id) {
            // Stream/metadata mismatch: bail (corrupt or partially written).
            return false;
        }
        const uint64_t fileCreationOrder = static_cast<uint64_t>(stream.readInt64());
        if (fileCreationOrder != p.creationOrder) {
            return false;
        }
        // Override XML scalars with binary values (binary is canonical; XML is index).
        p.T_start           = stream.readDouble();
        p.durationSeconds   = stream.readDouble();
        p.captureSampleRate = stream.readDouble();
        p.captureChannels   = stream.readInt();
        p.materializationId = static_cast<uint64_t>(stream.readInt64());

        persisted.push_back(p);
    }

    // End-magic check (informational; truncated trailers still allow what's parsed).
    const uint32_t endMagic = static_cast<uint32_t>(stream.readInt());
    if (endMagic != kCaptureEndMagic) {
        // Truncated or corrupt; what we extracted above is still kept.
    }

    // ── 3. Rebuild segments and trigger vocoder re-render ──
    // Standard state restoration has populated MaterializationStore with audio +
    // pitchCurve + notes + detectedKey, but RenderCache is fresh empty (intentional,
    // not part of store serialization). Segments enter Processing; refreshMaterialization
    // kicks off vocoder re-render against the existing matId; tick() promotes to Edited
    // via isRenderReady polling.
    uint64_t maxIdSeen = 0;
    int restoredCount = 0;
    std::vector<uint64_t> matIdsToRefresh;
    for (const auto& p : persisted) {
        if (p.materializationId == 0
            || (session.bindings_.containsMaterialization
                && !session.bindings_.containsMaterialization(p.materializationId))) {
            AppLogger::warn("CaptureSession::deserialize: orphan segment id="
                            + juce::String(static_cast<juce::int64>(p.id))
                            + " matId=" + juce::String(static_cast<juce::int64>(p.materializationId))
                            + " dropped");
            continue;
        }

        auto seg = std::make_unique<CaptureSegment>();
        seg->id = p.id;
        seg->creationOrder = p.creationOrder;
        seg->captureSampleRate = p.captureSampleRate;
        seg->captureChannels = p.captureChannels;
        seg->T_start.store(p.T_start, std::memory_order_release);
        seg->anchored.store(true, std::memory_order_release);
        seg->durationSeconds = p.durationSeconds;
        // Audio is owned by MaterializationStore now; capture session no longer
        // needs a private copy in capturedAudio.
        seg->capturedAudio = nullptr;
        seg->materializationId = p.materializationId;
        seg->state.store(SegmentState::Processing, std::memory_order_release);

        if (p.creationOrder > maxIdSeen)
            maxIdSeen = p.creationOrder;

        {
            std::lock_guard<std::mutex> lock(session.mutableMutex_);
            session.mutableSegments_.push_back(std::move(seg));
        }
        matIdsToRefresh.push_back(p.materializationId);
        ++restoredCount;
    }

    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        if (maxIdSeen > session.idCounter_)
            session.idCounter_ = maxIdSeen;
    }
    session.publishSegmentsView();

    // Trigger vocoder re-render for each restored mat. Call outside the mutex —
    // the processor's MaterializationRefreshService is itself thread-safe and
    // marshals to its own worker.
    if (session.bindings_.refreshMaterialization) {
        for (uint64_t matId : matIdsToRefresh)
            session.bindings_.refreshMaterialization(matId);
    }

    return restoredCount > 0;
}

}  // namespace OpenTune::Capture
