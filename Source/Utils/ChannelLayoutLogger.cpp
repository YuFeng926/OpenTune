#include "ChannelLayoutLogger.h"

namespace OpenTune::ChannelLayoutLog {

namespace {

juce::String makeLine(const juce::String& tail)
{
    return juce::String(kTag) + " " + tail;
}

juce::String escapeName(const juce::String& displayName)
{
    // Display names contain spaces / special chars. Wrap in quotes and replace
    // any embedded quotes so a tester can grep "displayName=\"...\"" reliably.
    if (displayName.isEmpty())
        return {};
    return "\"" + displayName.replaceCharacter('"', '\'') + "\"";
}

}  // namespace

void logEntry(const char* source, int declared, int stored,
              const juce::String& displayName, juce::int64 sourceId)
{
    juce::String line;
    line << "event=entry source=" << source
         << " declared=" << declared
         << " stored=" << stored;
    if (sourceId != 0)
        line << " sourceId=" << sourceId;
    if (displayName.isNotEmpty())
        line << " displayName=" << escapeName(displayName);
    AppLogger::log(makeLine(line));
}

void logReject(const char* source, int declared, const char* reason,
               const juce::String& displayName, juce::int64 sourceId)
{
    juce::String line;
    line << "event=entry source=" << source
         << " REJECT declared=" << declared
         << " reason=" << reason;
    if (sourceId != 0)
        line << " sourceId=" << sourceId;
    if (displayName.isNotEmpty())
        line << " displayName=" << escapeName(displayName);
    AppLogger::log(makeLine(line));
}

void logSessionReconfig(int oldChannels, int newChannels)
{
    juce::String line;
    line << "event=session-reconfig captureChannels old=" << oldChannels
         << " new=" << newChannels;
    AppLogger::log(makeLine(line));
}

void logSegmentArm(juce::int64 segmentId, int captureChannels)
{
    juce::String line;
    line << "event=segment-arm id=" << segmentId
         << " captureChannels=" << captureChannels;
    AppLogger::log(makeLine(line));
}

void logSegmentFinalize(juce::int64 segmentId, int captureChannels,
                        double durationSeconds)
{
    juce::String line;
    line << "event=segment-finalize id=" << segmentId
         << " captureChannels=" << captureChannels
         << " durationSeconds=" << juce::String(durationSeconds, 4);
    AppLogger::log(makeLine(line));
}

void logMaterializationCreate(juce::int64 materializationId, int channels)
{
    juce::String line;
    line << "event=materialization-create materializationId=" << materializationId
         << " channels=" << channels;
    AppLogger::log(makeLine(line));
}

void logMaterializationReject(int requestChannels)
{
    juce::String line;
    line << "event=materialization-create REJECT requestChannels=" << requestChannels;
    AppLogger::log(makeLine(line));
}

void logChunkRender(juce::int64 materializationId, int stored)
{
    juce::String line;
    line << "event=chunk-render materializationId=" << materializationId
         << " stored=" << stored
         << " vocoderSource=ch0";
    AppLogger::log(makeLine(line));
}

void logPlaybackRead(juce::int64 materializationId, int stored, int dest,
                     const char* mode)
{
    juce::String line;
    line << "event=playback-read materializationId=" << materializationId
         << " stored=" << stored
         << " dest=" << dest
         << " mode=" << mode;
    AppLogger::log(makeLine(line));
}

void logNumericGuard(int zeroedSamples, int channel)
{
    // Throttle to ~1 Hz to avoid log flooding when a host injects continuous garbage.
    static std::atomic<juce::int64> lastEmitMs { 0 };
    const auto nowMs = juce::Time::currentTimeMillis();
    const auto prev = lastEmitMs.load(std::memory_order_relaxed);
    if (nowMs - prev < 1000)
        return;
    if (!lastEmitMs.compare_exchange_strong(const_cast<juce::int64&>(prev),
                                             nowMs, std::memory_order_relaxed))
        return;

    juce::String line;
    line << "event=numeric-guard zeroedSamples=" << zeroedSamples
         << " ch=" << channel;
    AppLogger::log(makeLine(line));
}

void logPersistenceSerialize(int segmentCount)
{
    juce::String line;
    line << "event=persistence-serialize segmentCount=" << segmentCount;
    AppLogger::log(makeLine(line));
}

void logPersistenceDeserializeReject(juce::uint32 magic)
{
    juce::String line;
    line << "event=persistence-deserialize REJECT magic=0x"
         << juce::String::toHexString(static_cast<int>(magic))
         << " reason=incompatible-format";
    AppLogger::log(makeLine(line));
}

void logLoadAuditReject(juce::int64 materializationId, int channels)
{
    juce::String line;
    line << "event=load-audit REJECT materializationId=" << materializationId
         << " channels=" << channels;
    AppLogger::log(makeLine(line));
}

}  // namespace OpenTune::ChannelLayoutLog
