
#include "CapturePersistence.h"

#include "CaptureSession.h"
#include "../../Content/EditableContentSnapshot.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"
#include "../../Utils/PitchCurve.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstring>
#include <vector>

namespace OpenTune::Capture {

namespace {
    // CAPz v2: per-segment fixed bytes + embedded PCM audio.
    // Audio travels with CaptureSegmentContent.
    constexpr uint32_t kCaptureMagic    = 0x4341507A;  // 'CAPz' little-endian
    constexpr uint32_t kCaptureEndMagic = 0x78434150;  // 'xCAP' little-endian

    void writeFloatVector(juce::MemoryOutputStream& stream, const std::vector<float>& values)
    {
        stream.writeInt(static_cast<int>(values.size()));
        if (!values.empty())
            stream.write(values.data(), sizeof(float) * values.size());
    }

    std::vector<float> readFloatVector(juce::MemoryInputStream& stream)
    {
        const int count = stream.readInt();
        std::vector<float> values(static_cast<size_t>(juce::jmax(0, count)));
        if (!values.empty()) {
            const int byteCount = static_cast<int>(sizeof(float) * values.size());
            stream.read(values.data(), byteCount);
        }
        return values;
    }

    void writePitchCurve(juce::MemoryOutputStream& stream, const std::shared_ptr<PitchCurve>& curve)
    {
        stream.writeInt(curve ? 1 : 0);
        if (!curve)
            return;

        const auto snap = curve->getSnapshot();
        stream.writeInt(snap->getHopSize());
        stream.writeDouble(snap->getSampleRate());
        writeFloatVector(stream, snap->getOriginalF0());
        writeFloatVector(stream, snap->getOriginalEnergy());

        const auto& segments = snap->getCorrectionSegments();
        stream.writeInt(static_cast<int>(segments.size()));
        for (const auto& segment : segments) {
            stream.writeInt(segment.startFrame);
            stream.writeInt(segment.endFrame);
            writeFloatVector(stream, segment.f0Data);
            stream.writeInt(static_cast<int>(segment.source));
            stream.writeFloat(segment.retuneSpeed);
            stream.writeFloat(segment.vibratoDepth);
            stream.writeFloat(segment.vibratoRate);
        }
    }

    std::shared_ptr<PitchCurve> readPitchCurve(juce::MemoryInputStream& stream)
    {
        if (stream.readInt() == 0)
            return nullptr;

        auto curve = std::make_shared<PitchCurve>();
        curve->setHopSize(stream.readInt());
        curve->setSampleRate(stream.readDouble());
        curve->setOriginalF0(readFloatVector(stream));
        curve->setOriginalEnergy(readFloatVector(stream));

        const int segmentCount = stream.readInt();
        std::vector<PitchCorrectionSegment> segments;
        segments.reserve(static_cast<size_t>(juce::jmax(0, segmentCount)));
        for (int i = 0; i < segmentCount; ++i) {
            PitchCorrectionSegment segment;
            segment.startFrame = stream.readInt();
            segment.endFrame = stream.readInt();
            segment.f0Data = readFloatVector(stream);
            segment.source = static_cast<PitchCorrectionSegment::Source>(stream.readInt());
            segment.retuneSpeed = stream.readFloat();
            segment.vibratoDepth = stream.readFloat();
            segment.vibratoRate = stream.readFloat();
            segments.push_back(std::move(segment));
        }
        curve->replaceCorrectionSegments(segments);
        return curve;
    }
}  // namespace

juce::MemoryBlock CapturePersistence::serialize(const CaptureSession& session)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream stream(out, false);

    // ── 1. Build ValueTree metadata for each persistable segment ─────────
    juce::ValueTree root("CaptureSession");
    int persistedCount = 0;
    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        for (const auto& seg : session.mutableSegments_) {
            const auto s = seg->state.load(std::memory_order_acquire);
            if (s == SegmentState::Capturing)
                continue;
            juce::ValueTree segNode("Segment");
            segNode.setProperty("id", juce::String(seg->contentKey.objectId), nullptr);
            segNode.setProperty("creationOrder", juce::String(seg->creationOrder), nullptr);
            segNode.setProperty("T_start", seg->T_start.load(std::memory_order_acquire), nullptr);
            segNode.setProperty("durationSeconds", seg->durationSeconds, nullptr);
            segNode.setProperty("captureSampleRate", seg->captureSampleRate, nullptr);
            segNode.setProperty("captureChannels", seg->captureChannels, nullptr);
            root.appendChild(segNode, nullptr);
            ++persistedCount;
        }
    }

    // ── 2. Magic + metadata XML + per-segment records ────────────────────
    stream.writeInt(static_cast<int>(kCaptureMagic));

    const juce::String xml = root.toXmlString();
    const auto xmlUtf8 = xml.toRawUTF8();
    const auto xmlLen = static_cast<int>(std::strlen(xmlUtf8));
    stream.writeInt(xmlLen);
    stream.write(xmlUtf8, static_cast<size_t>(xmlLen));

    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        for (const auto& seg : session.mutableSegments_) {
            const auto s = seg->state.load(std::memory_order_acquire);
            if (s == SegmentState::Capturing)
                continue;
            const auto snap = seg->content->snapshotContent();

            stream.writeInt64(static_cast<juce::int64>(seg->contentKey.objectId));
            stream.writeInt64(static_cast<juce::int64>(seg->creationOrder));
            stream.writeDouble(seg->T_start.load(std::memory_order_acquire));
            stream.writeDouble(seg->durationSeconds);
            stream.writeDouble(seg->captureSampleRate);
            stream.writeInt(seg->captureChannels);
            stream.writeInt(static_cast<int>(s));

            int numSamples = 0;
            int numChannels = 0;
            if (snap->audioBuffer) {
                numSamples = snap->audioBuffer->getNumSamples();
                numChannels = snap->audioBuffer->getNumChannels();
            }
            stream.writeInt(numSamples);
            stream.writeInt(numChannels);
            if (numSamples > 0 && numChannels > 0) {
                for (int ch = 0; ch < numChannels; ++ch) {
                    stream.write(snap->audioBuffer->getReadPointer(ch),
                                 sizeof(float) * static_cast<size_t>(numSamples));
                }
            }

            stream.writeInt(static_cast<int>(snap->originalF0State));
            stream.writeInt(static_cast<int>(snap->detectedKey.root));
            stream.writeInt(static_cast<int>(snap->detectedKey.scale));
            stream.writeFloat(snap->detectedKey.confidence);
            writePitchCurve(stream, snap->pitchCurve);
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

    // ── 2. Read each segment's fixed-bytes record + embedded audio ───────
    struct PersistedSegment {
        uint64_t id;
        uint64_t creationOrder;
        double   T_start;
        double   durationSeconds;
        double   captureSampleRate;
        int      captureChannels;
        SegmentState segmentState{SegmentState::Processing};
        std::shared_ptr<juce::AudioBuffer<float>> audio;
        OriginalF0State originalF0State{OriginalF0State::NotRequested};
        DetectedKey detectedKey;
        std::shared_ptr<PitchCurve> pitchCurve;
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

        const uint64_t fileId = static_cast<uint64_t>(stream.readInt64());
        if (fileId != p.id) return false;
        const uint64_t fileCreationOrder = static_cast<uint64_t>(stream.readInt64());
        if (fileCreationOrder != p.creationOrder) return false;
        p.T_start           = stream.readDouble();
        p.durationSeconds   = stream.readDouble();
        p.captureSampleRate = stream.readDouble();
        p.captureChannels   = stream.readInt();
        p.segmentState      = static_cast<SegmentState>(stream.readInt());

        const int numAudioSamples  = stream.readInt();
        const int numAudioChannels = stream.readInt();
        if (numAudioSamples > 0 && numAudioChannels > 0) {
            p.audio = std::make_shared<juce::AudioBuffer<float>>(numAudioChannels, numAudioSamples);
            for (int ch = 0; ch < numAudioChannels; ++ch) {
                stream.read(p.audio->getWritePointer(ch),
                            sizeof(float) * static_cast<size_t>(numAudioSamples));
            }
        }
        p.originalF0State = static_cast<OriginalF0State>(stream.readInt());
        p.detectedKey.root = static_cast<Key>(stream.readInt());
        p.detectedKey.scale = static_cast<Scale>(stream.readInt());
        p.detectedKey.confidence = stream.readFloat();
        p.pitchCurve = readPitchCurve(stream);

        persisted.push_back(std::move(p));
    }

    // End-magic check.
    const uint32_t endMagic = static_cast<uint32_t>(stream.readInt());
    if (endMagic != kCaptureEndMagic) {
        AppLogger::log("CapturePersistence: invalid end magic");
        return false;
    }

    // ── 3. Rebuild segments ──────────────────────────────────────────────
    uint64_t maxIdSeen = 0;
    int restoredCount = 0;
    std::vector<ContentKey> keysToPublish;
    for (auto& p : persisted) {
        auto seg = std::make_unique<CaptureSegment>();
        seg->contentKey = ContentKey{DomainKind::RegularVST3Capture, p.id, 0};
        seg->creationOrder = p.creationOrder;
        seg->captureSampleRate = p.captureSampleRate;
        seg->captureChannels = p.captureChannels;
        seg->T_start.store(p.T_start, std::memory_order_release);
        seg->anchored.store(true, std::memory_order_release);
        seg->durationSeconds = p.durationSeconds;

        seg->content = std::make_unique<CaptureSegmentContent>(p.id);

        if (p.audio && p.audio->getNumSamples() > 0) {
            seg->content->applyAudioBuffer(p.audio.get(), p.captureSampleRate);
        }
        seg->content->applyDetectedKey(p.detectedKey);
        if (p.pitchCurve)
            seg->content->applyPitchCurve(std::move(p.pitchCurve));
        seg->content->applyOriginalF0State(p.originalF0State);

        const bool ready = p.originalF0State == OriginalF0State::Ready;
        const auto restoredState = ready ? SegmentState::Edited : p.segmentState;
        seg->state.store(restoredState, std::memory_order_release);
        const auto restoredKey = seg->contentKey;

        if (p.creationOrder > maxIdSeen)
            maxIdSeen = p.creationOrder;

        {
            std::lock_guard<std::mutex> lock(session.mutableMutex_);
            if (restoredState == SegmentState::Edited)
                session.activeDisplaySegmentId_ = p.id;
            session.mutableSegments_.push_back(std::move(seg));
        }
        keysToPublish.push_back(restoredKey);
        ++restoredCount;
    }

    {
        std::lock_guard<std::mutex> lock(session.mutableMutex_);
        if (maxIdSeen > session.idCounter_)
            session.idCounter_ = maxIdSeen;
    }
    session.publishSegmentsView();

    // Publish restored audio to CRS
    if (session.bindings_.publishPlaybackSource) {
        for (const ContentKey& key : keysToPublish) {
            auto* seg = session.findSegmentByContentKey(key);
            if (!seg) continue;
            const auto snap = seg->content->snapshotContent();
            if (snap->audioBuffer) {
                session.bindings_.publishPlaybackSource(
                    seg->contentKey,
                    snap->audioBuffer,
                    snap->audioSampleRate);
            }
        }
    }

    // Restore: owner truth (audio + pitch curve + detected key) already persisted.
    // CRS republished above; now immediately rebuild render cache from restored owner truth.
    if (session.bindings_.requestFullRender) {
        for (const ContentKey& key : keysToPublish) {
            auto* seg = session.findSegmentByContentKey(key);
            if (!seg || seg->durationSeconds <= 0.0)
                continue;
            session.bindings_.requestFullRender(key);
        }
    }

    return restoredCount > 0;
}

}  // namespace OpenTune::Capture
