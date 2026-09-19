#pragma once


#include <juce_core/juce_core.h>

namespace OpenTune::Capture {

class CaptureSession;

/**
 * Capture session ↔ binary state codec (CAPz format).
 *
 * Layout:
 *   [u32 CAPTURE_MAGIC = 'CAPz' (0x4341507A)]
 *   [i32 version]
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
 *       [i64 hostStartSample]    (v12+: authoritative absolute sample position)
 *       [i64 hostSampleCount]    (v12+: authoritative sample count)
 *       [PCM audio]
 *       [i32 originalF0State]
 *       [i32 detectedKeyRoot]
 *       [i32 detectedKeyScale]
 *       [f32 detectedKeyConfidence]
 *       [i32 detectedKeyOrigin]
 *       [pitch curve payload]
 *   ]
 *   [u32 CAPTURE_END_MAGIC = 'xCAP' (0x78434150)]
 *
 * CaptureSegmentContent is the persisted content owner. ContentRenderService is
 * republished from the restored owner snapshot; it is not persistence state.
 *
 * v12 replaces all prior versions: adds authoritative hostStartSample/
 * hostSampleCount per segment. Older files are rejected on load (no migration).
 */
class CapturePersistence
{
public:
    /// Current archive version. Older versions are rejected (no migration).
    static constexpr int kArchiveVersion = 12;

    static juce::MemoryBlock serialize(const CaptureSession& session);
    static bool deserialize(CaptureSession& session, const juce::MemoryBlock& block);
    static bool validate(const juce::MemoryBlock& block);

    /** Header probe only: returns the CAPz archive version, or 0 when the block
     *  is not a CAPz archive. Payload contents are not validated. */
    static int peekArchiveVersion(const juce::MemoryBlock& block);
};

}  // namespace OpenTune::Capture
