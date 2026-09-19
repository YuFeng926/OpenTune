// Vst3ProcessorStateCodec (OTST outer payload) tests.
// Covers v11 roundtrip with/without capture tail, released v10/v9 legacy
// arrangement skipping (variable-length NUL-terminated names + tail), and
// rejection of truncated / unknown-version / invalid-count payloads.
// Links juce_core for MemoryBlock / stream I/O.

#include "../Source/Plugin/Vst3ProcessorStateCodec.h"

#include <cstdio>
#include <cstring>

using namespace OpenTune;

namespace {

constexpr int kOtstMagic = 0x4F545354;
constexpr int kLegacyTrackCount = 12;

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

bool blocksEqual(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    return a.getSize() == b.getSize()
        && (a.getSize() == 0 || std::memcmp(a.getData(), b.getData(), a.getSize()) == 0);
}

juce::MemoryBlock makeTail(int seed)
{
    juce::MemoryBlock tail;
    juce::MemoryOutputStream output(tail, false);
    output.writeInt(static_cast<int>(0x4341507A)); // 'CAPz'
    output.writeInt(12);
    output.writeInt(seed);
    output.writeInt(seed * 7 + 1);
    output.flush();
    return tail;
}

// Mirror of the released v9/v10 writer: fixed per-track fields plus placements
// whose names are raw UTF-8 followed by a NUL terminator (JUCE writeString).
void writeLegacyArrangement(juce::MemoryOutputStream& output, int placementsPerTrack)
{
    output.writeInt(0); // activeTrackId
    output.writeInt(kLegacyTrackCount);
    for (int track = 0; track < kLegacyTrackCount; ++track) {
        output.writeInt64(0);       // selectedPlacementId
        output.writeBool(false);    // muted
        output.writeBool(false);    // solo
        output.writeFloat(1.0f);    // volume
        output.writeInt(placementsPerTrack);

        for (int placement = 0; placement < placementsPerTrack; ++placement) {
            const juce::String name = (placement % 3 == 0) ? juce::String("")
                : (placement % 3 == 1) ? juce::String("clip_") + juce::String(track)
                : juce::String::fromUTF8("\xE4\xB8\xAD\xE6\x96\x87\xE5\x90\x8D\xE7\xA7\xB0");
            output.writeInt64(static_cast<juce::int64>(track * 100 + placement + 1));
            output.writeInt(0); // domainKind
            output.writeInt64(static_cast<juce::int64>(track + 1));
            output.writeInt64(0); // sourceWindowDiscriminator
            output.writeInt64(0); // mappingRevision
            output.writeDouble(1.5);
            output.writeDouble(2.5);
            output.writeFloat(0.75f);
            output.writeDouble(0.1);
            output.writeDouble(0.2);
            output.writeDouble(0.05);
            output.writeString(name);
        }
    }
}

juce::MemoryBlock makeLegacyPayload(int version, int zoomSlot, int trackHeight,
                                    const juce::MemoryBlock& tail, int placementsPerTrack)
{
    juce::MemoryBlock payload;
    juce::MemoryOutputStream output(payload, false);
    output.writeInt(kOtstMagic);
    output.writeInt(version);
    if (version == Vst3ProcessorStateCodec::kLegacyVersion10)
        output.writeInt(zoomSlot);
    else
        output.writeDouble(static_cast<double>(zoomSlot)); // legacy timeline zoom
    output.writeInt(trackHeight);
    writeLegacyArrangement(output, placementsPerTrack);
    if (tail.getSize() > 0)
        output.write(tail.getData(), tail.getSize());
    output.flush();
    return payload;
}

void testV11RoundTrip()
{
    Vst3ProcessorOuterState state;
    state.uiZoomPercent = 125;
    state.trackHeight = 140;

    const auto encoded = Vst3ProcessorStateCodec::encode(state);
    check(encoded.getSize() == 4 * 4, "v11 without tail is a 16-byte header");

    Vst3ProcessorOuterState decoded;
    check(Vst3ProcessorStateCodec::decode(encoded.getData(), static_cast<int>(encoded.getSize()), decoded),
          "v11 without tail decodes");
    check(decoded.sourceVersion == 11, "v11 source version");
    check(decoded.uiZoomPercent == 125 && decoded.trackHeight == 140, "v11 settings roundtrip");
    check(decoded.captureTail.getSize() == 0, "v11 absent tail stays empty");
}

void testV11RoundTripWithTail()
{
    Vst3ProcessorOuterState state;
    state.uiZoomPercent = 90;
    state.trackHeight = 200;
    state.captureTail = makeTail(42);

    const auto encoded = Vst3ProcessorStateCodec::encode(state);
    check(encoded.getSize() == 16 + state.captureTail.getSize(), "v11 with tail size");

    Vst3ProcessorOuterState decoded;
    check(Vst3ProcessorStateCodec::decode(encoded.getData(), static_cast<int>(encoded.getSize()), decoded),
          "v11 with tail decodes");
    check(decoded.uiZoomPercent == 90 && decoded.trackHeight == 200, "v11 tail settings roundtrip");
    check(blocksEqual(decoded.captureTail, state.captureTail), "v11 tail bytes roundtrip");
}

void testLegacyV10Skip()
{
    const auto tail = makeTail(7);
    const auto payload = makeLegacyPayload(10, 110, 160, tail, 3);

    Vst3ProcessorOuterState decoded;
    check(Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
          "v10 legacy payload decodes");
    check(decoded.sourceVersion == 10, "v10 source version");
    check(decoded.uiZoomPercent == 110 && decoded.trackHeight == 160, "v10 outer settings restored");
    check(blocksEqual(decoded.captureTail, tail), "v10 capture tail recovered after arrangement skip");
}

void testLegacyV9Skip()
{
    const auto tail = makeTail(99);
    const auto payload = makeLegacyPayload(9, 250, 175, tail, 2);

    Vst3ProcessorOuterState decoded;
    check(Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
          "v9 legacy payload decodes");
    check(decoded.sourceVersion == 9, "v9 source version");
    check(decoded.uiZoomPercent == 100, "v9 legacy timeline zoom maps to default UI zoom");
    check(decoded.trackHeight == 175, "v9 track height restored");
    check(blocksEqual(decoded.captureTail, tail), "v9 capture tail recovered after arrangement skip");
}

void testLegacyWithoutTail()
{
    const auto payload = makeLegacyPayload(10, 100, 120, juce::MemoryBlock(), 0);
    Vst3ProcessorOuterState decoded;
    check(Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
          "legacy payload without capture tail is legal");
    check(decoded.captureTail.getSize() == 0, "legacy absent tail stays empty");
}

void testRejections()
{
    Vst3ProcessorOuterState decoded;

    check(!Vst3ProcessorStateCodec::decode(nullptr, 16, decoded), "null data rejected");
    check(!Vst3ProcessorStateCodec::decode("x", 0, decoded), "zero size rejected");

    // Unknown magic and unknown versions.
    {
        juce::MemoryBlock payload;
        juce::MemoryOutputStream output(payload, false);
        output.writeInt(0x4F545355);
        output.writeInt(11);
        output.writeInt(100);
        output.writeInt(120);
        output.flush();
        check(!Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
              "unknown magic rejected");
    }
    for (const int version : { 8, 12, 100 })
    {
        juce::MemoryBlock bytes = makeLegacyPayload(10, 100, 120, juce::MemoryBlock(), 0);
        std::memcpy(static_cast<char*>(bytes.getData()) + 4, &version, 4);
        check(!Vst3ProcessorStateCodec::decode(bytes.getData(), static_cast<int>(bytes.getSize()), decoded),
              "unknown version rejected");
    }

    // Truncated v11 header (missing trackHeight) and full-truncation payloads.
    {
        Vst3ProcessorOuterState state;
        const auto encoded = Vst3ProcessorStateCodec::encode(state);
        check(!Vst3ProcessorStateCodec::decode(encoded.getData(), 15, decoded), "truncated v11 header rejected");
    }

    // Rejected decode is transactional: the caller's output is left untouched.
    {
        Vst3ProcessorOuterState preserved;
        preserved.uiZoomPercent = 77;
        preserved.trackHeight = 88;
        preserved.captureTail = makeTail(5);
        const auto preservedTail = preserved.captureTail;

        Vst3ProcessorOuterState state;
        const auto encoded = Vst3ProcessorStateCodec::encode(state);
        check(!Vst3ProcessorStateCodec::decode(encoded.getData(), 15, preserved),
              "truncated v11 header rejected (preserved output)");
        check(preserved.uiZoomPercent == 77 && preserved.trackHeight == 88
                  && blocksEqual(preserved.captureTail, preservedTail),
              "failed decode leaves output untouched");
    }

    // Truncated legacy arrangement: cut inside the last placement's name.
    {
        const auto payload = makeLegacyPayload(10, 100, 120, juce::MemoryBlock(), 1);
        check(!Vst3ProcessorStateCodec::decode(payload.getData(),
                                               static_cast<int>(payload.getSize()) - 6, decoded),
              "truncated legacy arrangement rejected");
    }

    // Legacy track count other than the released fixed 12.
    {
        juce::MemoryBlock payload = makeLegacyPayload(10, 100, 120, juce::MemoryBlock(), 0);
        const int bogusTrackCount = 11;
        std::memcpy(static_cast<char*>(payload.getData()) + 20, &bogusTrackCount, 4);
        check(!Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
              "legacy track count other than 12 rejected");
    }

    // Negative and unreasonable placement counts.
    {
        juce::MemoryBlock payload = makeLegacyPayload(10, 100, 120, juce::MemoryBlock(), 0);
        // First track record ends with placementCount at offset 16 + 4 + 4 + 14 = 38.
        const int negativeCount = -1;
        std::memcpy(static_cast<char*>(payload.getData()) + 38, &negativeCount, 4);
        check(!Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
              "negative placement count rejected");

        const int hugeCount = 1000000;
        std::memcpy(static_cast<char*>(payload.getData()) + 38, &hugeCount, 4);
        check(!Vst3ProcessorStateCodec::decode(payload.getData(), static_cast<int>(payload.getSize()), decoded),
              "unreasonable placement count rejected");
    }
}

} // namespace

int main()
{
    testV11RoundTrip();
    testV11RoundTripWithTail();
    testLegacyV10Skip();
    testLegacyV9Skip();
    testLegacyWithoutTail();
    testRejections();

    if (failures == 0) { std::puts("All Vst3ProcessorStateCodec tests passed."); return 0; }
    std::fprintf(stderr, "%d test(s) FAILED.\n", failures);
    return 1;
}
