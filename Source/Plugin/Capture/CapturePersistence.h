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
 *   [for each segment with materializationId != 0:
 *       [i64 id]
 *       [i64 creationOrder]
 *       [f64 T_start]
 *       [f64 durationSeconds]
 *       [f64 captureSampleRate]
 *       [i32 captureChannels]
 *       [i64 materializationId]
 *   ]
 *   [u32 CAPTURE_END_MAGIC = 'xCAP' (0x78434150)]
 *
 * Audio + edits are NOT in this block — they travel through standard processor
 * state under their materializationId. On deserialize, segments are bound back
 * to already-restored materializations (Edited state) without re-running render.
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

