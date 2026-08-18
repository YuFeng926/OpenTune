
#include "CapturePersistence.h"

#include "CaptureSession.h"
#include "../../Content/EditableContentSnapshot.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/ChannelLayoutLogger.h"
#include "../../Utils/PitchCurve.h"
#include "../../Utils/NoteEqSettings.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstring>
#include <vector>

namespace OpenTune::Capture {

namespace {
    // CAPz v6: per-segment fixed bytes + embedded PCM audio + unified VolumeEnvelope.
    // Audio travels with CaptureSegmentContent.
    constexpr uint32_t kCaptureMagic    = 0x4341507A;  // 'CAPz' little-endian
    constexpr uint32_t kCaptureEndMagic = 0x78434150;  // 'xCAP' little-endian
    constexpr int kCaptureArchiveVersion = 11;  // v11: EqFilter.bypassed (per-filter bypass)
    constexpr int kCaptureArchiveVersionMin = 4;  // v4 files load with pitchDriftScale=1.0

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
            stream.writeFloat(segment.pitchDriftScale);
            stream.writeFloat(segment.vibratoDepth);
            stream.writeFloat(segment.vibratoRate);
        }
    }

    std::shared_ptr<PitchCurve> readPitchCurve(juce::MemoryInputStream& stream, bool hasPitchDriftScale)
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
            if (hasPitchDriftScale)
                segment.pitchDriftScale = stream.readFloat();
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
            segNode.setProperty("noteTopologyInitialized",
                                seg->content->editable().noteTopologyInitialized ? 1 : 0, nullptr);
            root.appendChild(segNode, nullptr);
            ++persistedCount;
        }
    }

    // ── 2. Magic + metadata XML + per-segment records ────────────────────
    stream.writeInt(static_cast<int>(kCaptureMagic));
    stream.writeInt(kCaptureArchiveVersion);

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
            stream.writeInt(static_cast<int>(snap->detectedKey.origin));
            writePitchCurve(stream, snap->pitchCurve);

            stream.writeInt(static_cast<int>(snap->notes.size()));
            for (const auto& note : snap->notes) {
                stream.writeDouble(note.startTime);
                stream.writeDouble(note.endTime);
                stream.writeFloat(note.pitch);
                stream.writeFloat(note.originalPitch);
                stream.writeFloat(note.pitchOffset);
                stream.writeFloat(note.retuneSpeed);
                stream.writeFloat(note.pitchDriftScale);
                stream.writeFloat(note.vibratoDepth);
                stream.writeFloat(note.vibratoRate);
                stream.writeFloat(note.outputGainDb);
                stream.writeInt(note.isVoiced ? 1 : 0);

                // v10: Per-note EQ settings — filter count + per-filter type/frequency/gain/q/slot
                stream.writeInt(note.eq.has_value() ? 1 : 0);
                if (note.eq.has_value()) {
                    const auto& eq = *note.eq;
                    stream.writeInt(eq.active ? 1 : 0);
                    const int filterCount = static_cast<int>(eq.filters.size());
                    stream.writeInt(filterCount);
                    for (int fi = 0; fi < filterCount; ++fi) {
                        const auto& f = eq.filters[static_cast<size_t>(fi)];
                        stream.writeInt(static_cast<int>(f.type));
                        stream.writeFloat(f.frequencyHz);
                        stream.writeFloat(f.gainDb);
                        stream.writeFloat(f.q);
                        stream.writeInt(f.paletteSlot);
                        stream.writeInt(f.bypassed ? 1 : 0);
                    }
                }
            }
            const auto& envelopePoints = snap->volumeEnvelope.points();
            stream.writeInt(static_cast<int>(envelopePoints.size()));
            for (const auto& point : envelopePoints) {
                stream.writeDouble(point.timeSeconds);
                stream.writeFloat(point.gainDb);
            }
            stream.writeInt(snap->pitchShiftSettings.semitone);
            stream.writeInt(snap->pitchShiftSettings.cents);
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
    const int fileVersion = stream.readInt();
    if (fileVersion < kCaptureArchiveVersionMin || fileVersion > kCaptureArchiveVersion)
        return false;
    const bool hasPitchDriftScale = (fileVersion >= 5);
    const bool hasUnifiedVolumeEnvelope = (fileVersion >= 6);
    const bool hasDetectedKeyOrigin = (fileVersion >= 7);
    const bool hasPerNoteEq = (fileVersion >= 8);
    const bool hasDynamicEqFilters = (fileVersion >= 9);
    const bool hasPaletteSlot = (fileVersion >= 10);
    const bool hasPerFilterBypassed = (fileVersion >= 11);

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
        std::vector<Note> notes;
        AutomationLane volumeEnvelope;
        PitchShiftSettings pitchShiftSettings;
        bool noteTopologyInitialized{false};
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
        // v6 及更旧数据无 origin：按 confidence 迁移
        p.detectedKey.origin = hasDetectedKeyOrigin
            ? static_cast<Origin>(stream.readInt())
            : DetectedKey::originFromLegacyConfidence(p.detectedKey.confidence);
        p.pitchCurve = readPitchCurve(stream, hasPitchDriftScale);

        const int noteCount = stream.readInt();
        if (noteCount < 0)
            return false;
        p.notes.reserve(static_cast<size_t>(noteCount));
        for (int noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
            Note note;
            note.startTime = stream.readDouble();
            note.endTime = stream.readDouble();
            note.pitch = stream.readFloat();
            note.originalPitch = stream.readFloat();
            note.pitchOffset = stream.readFloat();
            note.retuneSpeed = stream.readFloat();
            if (hasPitchDriftScale)
                note.pitchDriftScale = stream.readFloat();
            note.vibratoDepth = stream.readFloat();
            note.vibratoRate = stream.readFloat();
            note.outputGainDb = stream.readFloat();
            note.isVoiced = stream.readInt() != 0;

            // v8+: Per-note EQ settings
            if (hasPerNoteEq && stream.readInt() == 1) {
                EqSettings eq;
                eq.active = stream.readInt() != 0;

                if (hasDynamicEqFilters) {
                    // v9+: filter count + per-filter type/frequency/gain/q [+ v10 slot]
                    const int filterCount = stream.readInt();
                    if (filterCount <= 0 || filterCount > EqSettings::kMaxFilters)
                        return false;
                    eq.filters.clear();
                    eq.filters.reserve(static_cast<size_t>(filterCount));
                    for (int fi = 0; fi < filterCount; ++fi) {
                        const int typeInt = stream.readInt();
                        const float freq = stream.readFloat();
                        const float gain = stream.readFloat();
                        const float q = stream.readFloat();
                        if (typeInt < 0 || typeInt > static_cast<int>(EqFilterType::HighCut))
                            return false;
                        EqFilter f;
                        f.type = static_cast<EqFilterType>(typeInt);
                        f.frequencyHz = freq;
                        f.gainDb = gain;
                        f.q = q;
                        // v10: paletteSlot；v9 旧数据按索引确定性补 slot
                        f.paletteSlot = hasPaletteSlot ? stream.readInt() : fi;
                        if (f.paletteSlot < 0 || f.paletteSlot >= EqSettings::kMaxFilters)
                            return false;
                        if (hasPerFilterBypassed)
                            f.bypassed = stream.readInt() != 0;
                        eq.filters.push_back(f);
                    }
                } else {
                    // v8 legacy: 旧 8 float 字段 → 5 个固定过滤器 + 确定性 paletteSlot 0..4
                    // (active 已在上方读取，此处紧跟 8 个 float)
                    const float lowCutFreq = stream.readFloat();
                    const float lowShelfFreq = stream.readFloat();
                    const float lowShelfGain = stream.readFloat();
                    const float peakFreq = stream.readFloat();
                    const float peakGain = stream.readFloat();
                    const float highShelfFreq = stream.readFloat();
                    const float highShelfGain = stream.readFloat();
                    const float highCutFreq = stream.readFloat();

                    eq.filters = {
                        { EqFilterType::LowCut,   lowCutFreq,   0.0f,    0.707f, 0 },
                        { EqFilterType::LowShelf, lowShelfFreq, lowShelfGain, 2.0f, 1 },
                        { EqFilterType::Peak,     peakFreq,     peakGain, 2.0f, 2 },
                        { EqFilterType::HighShelf,highShelfFreq,highShelfGain, 2.0f, 3 },
                        { EqFilterType::HighCut,  highCutFreq,  0.0f,    0.707f, 4 }
                    };
                }

                if (!eq.isValid())
                    return false;
                note.eq = eq;
            }

            p.notes.push_back(note);
        }
        // 旧归档无该 property 时按 notes 是否为空推断，避免覆盖已有音符拓扑事实。
        p.noteTopologyInitialized = segNode.hasProperty("noteTopologyInitialized")
            ? static_cast<int>(segNode.getProperty("noteTopologyInitialized", 0)) != 0
            : !p.notes.empty();
        const int envelopeCount = stream.readInt();
        if (envelopeCount < 0)
            return false;
        std::vector<AutomationPoint> envelopePoints;
        envelopePoints.reserve(static_cast<size_t>(envelopeCount));
        for (int envIndex = 0; envIndex < envelopeCount; ++envIndex) {
            AutomationPoint point;
            point.timeSeconds = stream.readDouble();
            point.gainDb = stream.readFloat();
            envelopePoints.push_back(point);
        }
        if (hasUnifiedVolumeEnvelope) {
            p.volumeEnvelope = AutomationLane::fromSnapshot(envelopePoints);
        } else {
            p.volumeEnvelope = AutomationLane::sum(
                AutomationLane::fromLegacyNoteGains(p.notes),
                AutomationLane::fromLegacyStepPoints(envelopePoints));
        }
        p.pitchShiftSettings.semitone = stream.readInt();
        p.pitchShiftSettings.cents = stream.readInt();
        if (p.pitchCurve == nullptr
            && (!p.notes.empty() || !p.pitchShiftSettings.isIdentity()))
            return false;

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
            seg->content->applyAudioBuffer(*p.audio, p.captureSampleRate);
        }
        seg->content->applyDetectedKey(p.detectedKey);
        if (p.pitchCurve) {
            const auto pitchSnapshot = p.pitchCurve->getSnapshot();
            PitchShiftEditState pitchShiftState;
            pitchShiftState.settings = p.pitchShiftSettings;
            pitchShiftState.notes = std::move(p.notes);
            pitchShiftState.segments = pitchSnapshot->getCorrectionSegments();
            seg->content->applyPitchCurve(std::move(p.pitchCurve));
            seg->content->applyPitchShiftState(pitchShiftState);
        }
        seg->content->applyOriginalF0State(p.originalF0State);

        // Envelope revision 不落盘，恢复端由 owner 推进新 revision。
        seg->content->applyVolumeEnvelope(std::move(p.volumeEnvelope));

        // 持久化的拓扑初始化标志最终覆盖恢复路径中写 notes 但未置位该标志的中间步骤（applyPitchShiftState）。
        seg->content->editable().noteTopologyInitialized = p.noteTopologyInitialized;

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
