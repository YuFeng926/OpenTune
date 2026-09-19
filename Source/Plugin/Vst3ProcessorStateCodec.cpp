#include "Vst3ProcessorStateCodec.h"

namespace OpenTune {

namespace {

constexpr uint32_t kProcessorStateMagic = 0x4F545354; // 'OTST'

// Legacy v9/v10 arrangement section (released layout, fixed at 12 tracks):
//   activeTrackId, trackCount
//   per track: selectedPlacementId, muted, solo, volume, placementCount
//   per placement: id/domainKind/objectId/sourceWindowDiscriminator/mappingRevision,
//                  timelineStart/duration/gain/fadeIn/fadeOut/clipIn,
//                  NUL-terminated UTF-8 name
constexpr int64_t kLegacyTrackFixedBytes = 8 + 1 + 1 + 4; // without placementCount
constexpr int64_t kLegacyPlacementFixedBytes =
    8 + 4 + 8 + 8 + 8      // placementId, domainKind, objectId, sourceWindowDiscriminator, mappingRevision
    + 8 + 8 + 4 + 8 + 8 + 8; // timelineStartSeconds, durationSeconds, gain, fadeIn, fadeOut, clipInSeconds
constexpr int64_t kLegacyPlacementMinBytes = kLegacyPlacementFixedBytes + 1; // name writes at least its NUL

bool hasBytes(juce::MemoryInputStream& input, int64_t numBytes)
{
    return numBytes >= 0 && input.getNumBytesRemaining() >= numBytes;
}

bool readInt(juce::MemoryInputStream& input, int& value)
{
    if (!hasBytes(input, 4))
        return false;
    value = input.readInt();
    return true;
}

bool skipBytes(juce::MemoryInputStream& input, int64_t numBytes)
{
    if (!hasBytes(input, numBytes))
        return false;
    input.skipNextBytes(numBytes);
    return true;
}

// OutputStream::writeString emits raw UTF-8 followed by a NUL terminator (no
// length prefix); locate that terminator within the remaining payload only.
bool skipNulTerminatedName(juce::MemoryInputStream& input)
{
    while (input.getNumBytesRemaining() > 0) {
        if (input.readByte() == 0)
            return true;
    }
    return false;
}

// Bounded skip of the released v9/v10 arrangement section. Values are discarded;
// the layout is never materialised into a StandaloneArrangement.
bool skipLegacyArrangement(juce::MemoryInputStream& input)
{
    if (!skipBytes(input, 4)) // activeTrackId
        return false;

    int trackCount = 0;
    if (!readInt(input, trackCount)
        || trackCount != Vst3ProcessorStateCodec::kLegacyTrackCount) {
        return false;
    }

    for (int track = 0; track < trackCount; ++track) {
        if (!skipBytes(input, kLegacyTrackFixedBytes))
            return false;

        int placementCount = 0;
        if (!readInt(input, placementCount))
            return false;
        if (placementCount < 0
            || placementCount > input.getNumBytesRemaining() / kLegacyPlacementMinBytes) {
            return false;
        }

        for (int placement = 0; placement < placementCount; ++placement) {
            if (!skipBytes(input, kLegacyPlacementFixedBytes) || !skipNulTerminatedName(input))
                return false;
        }
    }
    return true;
}

} // namespace

juce::MemoryBlock Vst3ProcessorStateCodec::encode(const Vst3ProcessorOuterState& state)
{
    juce::MemoryBlock out;
    juce::MemoryOutputStream output(out, false);
    output.writeInt(static_cast<int>(kProcessorStateMagic));
    output.writeInt(kCurrentVersion);
    output.writeInt(state.uiZoomPercent);
    output.writeInt(state.trackHeight);
    if (state.captureTail.getSize() > 0)
        output.write(state.captureTail.getData(), state.captureTail.getSize());
    output.flush();
    return out;
}

bool Vst3ProcessorStateCodec::decode(const void* data, int sizeInBytes, Vst3ProcessorOuterState& out)
{
    if (data == nullptr || sizeInBytes <= 0)
        return false;

    juce::MemoryInputStream input(data, static_cast<size_t>(sizeInBytes), false);

    int magic = 0;
    int version = 0;
    if (!readInt(input, magic) || magic != static_cast<int>(kProcessorStateMagic))
        return false;
    if (!readInt(input, version))
        return false;

    Vst3ProcessorOuterState decoded;
    decoded.sourceVersion = version;

    if (version == kCurrentVersion) {
        if (!readInt(input, decoded.uiZoomPercent) || !readInt(input, decoded.trackHeight))
            return false;
    } else if (version == kLegacyVersion10 || version == kLegacyVersion9) {
        if (version == kLegacyVersion10) {
            if (!readInt(input, decoded.uiZoomPercent))
                return false;
        } else {
            // v9 stored the legacy timeline zoom as a double; the UI zoom
            // semantics have no equivalent, so it is discarded.
            if (!skipBytes(input, 8))
                return false;
        }
        if (!readInt(input, decoded.trackHeight) || !skipLegacyArrangement(input))
            return false;
    } else {
        return false;
    }

    const auto remaining = static_cast<int>(input.getNumBytesRemaining());
    if (remaining > 0) {
        decoded.captureTail.setSize(static_cast<size_t>(remaining));
        input.read(decoded.captureTail.getData(), remaining);
    }

    out = std::move(decoded);
    return true;
}

} // namespace OpenTune
