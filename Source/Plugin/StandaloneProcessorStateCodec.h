#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {

/**
 * Decoded OTSS standalone settings payload.
 *
 * The Standalone project format owns arrangement/content persistence; host
 * state carries settings only. No absolute-time conversion happens here.
 */
struct StandaloneProcessorSettings
{
    double bpm = 0.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    int uiZoomPercent = 100;
    int trackHeight = 120;
};

/**
 * OTSS settings-only codec for Standalone processor state.
 *
 * Current write layout (v3):
 *   [i32 magic 'OTSS']
 *   [i32 version = 3]
 *   [f64 bpm]
 *   [i32 timeSigNumerator]
 *   [i32 timeSigDenominator]
 *   [i32 uiZoomPercent]
 *   [i32 trackHeight]
 *
 * decode() also accepts the released v2 layout, which stored the legacy
 * timeline zoom as a double where v3 stores uiZoomPercent; that double is
 * consumed and discarded (uiZoomPercent stays 100). The legacy value is not
 * migrated. Payloads with trailing bytes or an unknown version are rejected.
 *
 * The codec is pure data: it does not touch the processor, threads, logging or
 * global state. The processor owns the runtime/deferred lifecycle and writes
 * the decoded values into its own state.
 */
class StandaloneProcessorStateCodec
{
public:
    static constexpr uint32_t kMagic = 0x4F545353; // 'OTSS' (OpenTune Standalone Settings)
    static constexpr int kCurrentVersion = 3;      // v3: timeline zoom double -> uiZoomPercent int32
    static constexpr int kLegacyVersion2 = 2;      // released v2 payloads still accepted and migrated

    static juce::MemoryBlock encode(const StandaloneProcessorSettings& settings);

    /**
     * Decodes a complete OTSS payload into `out` on success. On failure `out`
     * is left untouched and `error` describes the rejected payload so the
     * owner can log it.
     */
    static bool decode(const void* data, int sizeInBytes,
                       StandaloneProcessorSettings& out, juce::String& error);
};

} // namespace OpenTune
