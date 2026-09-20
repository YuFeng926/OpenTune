#include "StandaloneProcessorStateCodec.h"

namespace OpenTune {

juce::MemoryBlock StandaloneProcessorStateCodec::encode(const StandaloneProcessorSettings& settings)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream output(out, false);
    output.writeInt(static_cast<int>(kMagic));
    output.writeInt(kCurrentVersion);
    output.writeDouble(settings.bpm);
    output.writeInt(settings.timeSigNumerator);
    output.writeInt(settings.timeSigDenominator);
    output.writeInt(settings.uiZoomPercent);
    output.writeInt(settings.trackHeight);
    output.flush();
    return out;
}

bool StandaloneProcessorStateCodec::decode(const void* data, int sizeInBytes,
                                           StandaloneProcessorSettings& out, juce::String& error)
{
    if (data == nullptr || sizeInBytes <= 0) {
        error = "empty standalone state payload";
        return false;
    }

    juce::MemoryInputStream input(data, static_cast<size_t>(sizeInBytes), false);
    const int magic = input.readInt();
    const int version = input.readInt();

    if (magic != static_cast<int>(kMagic)) {
        error = "unsupported standalone state payload (version=" + juce::String(version) + ")";
        return false;
    }

    // Accept current v3 (uiZoomPercent int32) and released v2 (legacy timeline
    // zoom double, discarded on load; UI zoom semantics are unrelated).
    if (version != kCurrentVersion && version != kLegacyVersion2) {
        error = "unsupported standalone settings version " + juce::String(version)
            + " (expect " + juce::String(kCurrentVersion)
            + " or " + juce::String(kLegacyVersion2) + ")";
        return false;
    }

    StandaloneProcessorSettings decoded;
    decoded.bpm = input.readDouble();
    decoded.timeSigNumerator = input.readInt();
    decoded.timeSigDenominator = input.readInt();
    if (version == kCurrentVersion)
        decoded.uiZoomPercent = input.readInt();
    else
        (void) input.readDouble(); // legacy timeline zoom, discarded
    decoded.trackHeight = input.readInt();

    if (input.getNumBytesRemaining() != 0) {
        error = "standalone settings payload not fully consumed, trailing bytes="
            + juce::String(static_cast<int>(input.getNumBytesRemaining()));
        return false;
    }

    out = decoded;
    return true;
}

} // namespace OpenTune
