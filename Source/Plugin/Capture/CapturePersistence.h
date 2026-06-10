#pragma once


#include <juce_core/juce_core.h>

namespace OpenTune::Capture {

class CaptureSession;

/**
 * Capture session ↔ binary state codec (CAPz format).
 *
 * Layout:
 *   [u32 CAPTURE_MAGIC = 'CAPz' (0x4341507A)]
 *   [i32 metadata_xml_length]
 *   [UTF-8 metadata XML (ValueTree::toXmlString)]
 *   [for each non-Capturing segment:
 *       [i64 id]
 *       [i64 creationOrder]
 *       [f64 T_start]
 *       [f64 durationSeconds]
 *       [f64 captureSampleRate]
 *       [i32 captureChannels]
 *       [i32 segmentState]
 *       [PCM audio]
 *       [i32 originalF0State]
 *       [i32 detectedKeyRoot]
 *       [i32 detectedKeyScale]
 *       [f32 detectedKeyConfidence]
 *       [pitch curve payload]
 *   ]
 *   [u32 CAPTURE_END_MAGIC = 'xCAP' (0x78434150)]
 *
 * CaptureSegmentContent is the persisted content owner. ContentRenderService is
 * republished from the restored owner snapshot; it is not persistence state.
 *
 * Format incompatible with old 'CAPy' (FLAC-embedded) blocks: deserialize
 * rejects them via ChannelLayoutLog::logPersistenceDeserializeReject. Pre-fix
 * builds in non-ARA hosts never persisted reachable state, so no migration
 * path is required.
 */
class CapturePersistence
{
public:
    static juce::MemoryBlock serialize(const CaptureSession& session);
    static bool deserialize(CaptureSession& session, const juce::MemoryBlock& block);
};

}  // namespace OpenTune::Capture

