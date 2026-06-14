#pragma once

#include "AppLogger.h"

#include <atomic>
#include <cstdint>

namespace OpenTune::ChannelLayoutLog {

/**
 * Single greppable tag every channel-layout-decision log line carries.
 * Format on the wire (as written through AppLogger::log):
 *   [ChannelLayout] event=<name> key=val key=val ...
 *
 * See specs/channel-layout-policy/spec.md "Structured channel layout logging"
 * for the complete event taxonomy.
 */
inline constexpr const char* kTag = "[ChannelLayout]";

void logEntry(const char* source, int declared, int stored,
              const juce::String& displayName = {},
              juce::int64 sourceId = 0);

void logReject(const char* source, int declared, const char* reason,
               const juce::String& displayName = {},
               juce::int64 sourceId = 0);

void logSessionReconfig(int oldChannels, int newChannels);

void logSegmentArm(juce::int64 segmentId, int captureChannels);

void logSegmentFinalize(juce::int64 segmentId, int captureChannels,
                        double durationSeconds);

void logContentCreate(juce::int64 contentId, int channels);

void logContentReject(int requestChannels);

void logChunkRender(juce::int64 contentId, int stored);

/** Throttled to ~1 Hz internally (caller MAY invoke per block; the helper
 *  drops calls that arrive faster than that). */
void logNumericGuard(int zeroedSamples, int channel);

void logPersistenceSerialize(int segmentCount);

void logPersistenceDeserializeReject(juce::uint32 magic);

}  // namespace OpenTune::ChannelLayoutLog
