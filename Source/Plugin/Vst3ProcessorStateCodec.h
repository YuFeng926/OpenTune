#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {

/**
 * Decoded OTST outer payload of the VST3 processor state.
 *
 * The captureTail is opaque to this codec: it carries the trailing bytes owned
 * by another codec (CAPz archive) and may be empty.
 */
struct Vst3ProcessorOuterState
{
    /// OTST version the payload was decoded from: 11 (current), 10 or 9 (legacy).
    int sourceVersion = 0;
    int uiZoomPercent = 100;
    int trackHeight = 120;
    juce::MemoryBlock captureTail;
};

/**
 * OTST outer-payload codec for VST3 processor state.
 *
 * Current write layout (v11):
 *   [i32 magic 'OTST']
 *   [i32 version = 11]
 *   [i32 uiZoomPercent]
 *   [i32 trackHeight]
 *   [capture tail bytes]  (optional)
 *
 * v11 no longer embeds the Standalone arrangement. decode() also accepts the
 * released v10/v9 layouts and skips their arrangement section in a bounded way
 * (legacy track count fixed at 12, NUL-terminated placement names) so the
 * capture tail can be recovered. The arrangement is never materialised here.
 */
class Vst3ProcessorStateCodec
{
public:
    static constexpr int kCurrentVersion = 11;
    static constexpr int kLegacyVersion10 = 10;
    static constexpr int kLegacyVersion9 = 9;
    /// Released v9/v10 payloads always wrote this fixed track count; the codec
    /// must not depend on the application's current MaxTracks.
    static constexpr int kLegacyTrackCount = 12;

    static juce::MemoryBlock encode(const Vst3ProcessorOuterState& state);
    static bool decode(const void* data, int sizeInBytes, Vst3ProcessorOuterState& out);
};

} // namespace OpenTune
