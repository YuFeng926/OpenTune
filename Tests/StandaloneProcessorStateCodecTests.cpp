// StandaloneProcessorStateCodec (OTSS) tests.
// Covers v3 roundtrip, released v2 decoding, and transactional rejection.

#include "../Source/Plugin/StandaloneProcessorStateCodec.h"

#include <cstdio>
#include <cstring>

using namespace OpenTune;

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

juce::MemoryBlock makeV2Payload()
{
    juce::MemoryBlock payload;
    juce::MemoryOutputStream output(payload, false);
    output.writeInt(static_cast<int>(StandaloneProcessorStateCodec::kMagic));
    output.writeInt(StandaloneProcessorStateCodec::kLegacyVersion2);
    output.writeDouble(123.5);
    output.writeInt(7);
    output.writeInt(8);
    output.writeDouble(2.5); // released timeline zoom, intentionally discarded
    output.writeInt(144);
    output.flush();
    return payload;
}

void testV3RoundTrip()
{
    StandaloneProcessorSettings settings;
    settings.bpm = 123.5;
    settings.timeSigNumerator = 7;
    settings.timeSigDenominator = 8;
    settings.uiZoomPercent = 125;
    settings.trackHeight = 144;

    const auto encoded = StandaloneProcessorStateCodec::encode(settings);
    StandaloneProcessorSettings decoded;
    juce::String error;
    check(StandaloneProcessorStateCodec::decode(
              encoded.getData(), static_cast<int>(encoded.getSize()), decoded, error),
          "v3 payload decodes");
    check(error.isEmpty(), "v3 decode has no error");
    check(decoded.bpm == settings.bpm
              && decoded.timeSigNumerator == settings.timeSigNumerator
              && decoded.timeSigDenominator == settings.timeSigDenominator
              && decoded.uiZoomPercent == settings.uiZoomPercent
              && decoded.trackHeight == settings.trackHeight,
          "v3 settings roundtrip");
}

void testV2Decode()
{
    const auto payload = makeV2Payload();
    StandaloneProcessorSettings decoded;
    juce::String error;
    check(StandaloneProcessorStateCodec::decode(
              payload.getData(), static_cast<int>(payload.getSize()), decoded, error),
          "v2 payload decodes");
    check(decoded.bpm == 123.5
              && decoded.timeSigNumerator == 7
              && decoded.timeSigDenominator == 8
              && decoded.uiZoomPercent == 100
              && decoded.trackHeight == 144,
          "v2 settings preserve fields and discard legacy zoom");
}

void testRejectionsAreTransactional()
{
    StandaloneProcessorSettings preserved;
    preserved.bpm = 88.0;
    preserved.timeSigNumerator = 3;
    preserved.timeSigDenominator = 4;
    preserved.uiZoomPercent = 90;
    preserved.trackHeight = 99;

    const auto encoded = StandaloneProcessorStateCodec::encode(preserved);

    juce::MemoryBlock trailing(encoded);
    trailing.append("x", 1);
    static_cast<char*>(trailing.getData())[trailing.getSize() - 1] = '\0';

    juce::String error;
    check(!StandaloneProcessorStateCodec::decode(
              trailing.getData(), static_cast<int>(trailing.getSize()), preserved, error),
          "trailing bytes are rejected");
    check(preserved.bpm == 88.0
              && preserved.timeSigNumerator == 3
              && preserved.timeSigDenominator == 4
              && preserved.uiZoomPercent == 90
              && preserved.trackHeight == 99,
          "trailing-byte rejection leaves output untouched");

    juce::MemoryBlock unknownVersion(encoded);
    const int version = 99;
    std::memcpy(static_cast<char*>(unknownVersion.getData()) + 4, &version, sizeof(version));
    check(!StandaloneProcessorStateCodec::decode(
              unknownVersion.getData(), static_cast<int>(unknownVersion.getSize()), preserved, error),
          "unknown version is rejected");
}

} // namespace

int main()
{
    testV3RoundTrip();
    testV2Decode();
    testRejectionsAreTransactional();

    if (failures == 0) {
        std::puts("All StandaloneProcessorStateCodec tests passed.");
        return 0;
    }

    std::fprintf(stderr, "%d test(s) FAILED.\n", failures);
    return 1;
}
