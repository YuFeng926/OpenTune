#include "AudioFormatRegistry.h"

namespace OpenTune::AudioFormatRegistry {

namespace {

juce::String decodeFourCC(const char* data)
{
    juce::String text;
    for (int index = 0; index < 4; ++index) {
        const auto byte = static_cast<unsigned char>(data[index]);
        text += juce::CharacterFunctions::isPrintable(static_cast<juce::juce_wchar>(byte))
            ? juce::String::charToString(static_cast<juce::juce_wchar>(byte))
            : "?";
    }

    return text;
}

uint16_t readLittleEndian16(const char* data)
{
    return juce::ByteOrder::littleEndianShort(data);
}

uint32_t readLittleEndian32(const char* data)
{
    return juce::ByteOrder::littleEndianInt(data);
}

juce::String probeWavContainer(const juce::File& file)
{
    auto stream = std::unique_ptr<juce::InputStream>(file.createInputStream());
    if (stream == nullptr) {
        return {};
    }

    char header[128]{};
    const auto bytesRead = stream->read(header, static_cast<int>(sizeof(header)));
    if (bytesRead < 12) {
        return {};
    }

    const auto riffTag = decodeFourCC(header + 0);
    const auto waveTag = decodeFourCC(header + 8);
    juce::String detail = "container riff=" + riffTag + " wave=" + waveTag;

    if (riffTag != "RIFF" && riffTag != "RF64" && riffTag != "RIFX") {
        return detail;
    }

    if (waveTag != "WAVE") {
        return detail;
    }

    if (riffTag == "RIFX") {
        detail += " endian=big";
        return detail;
    }

    int offset = 12;
    while (offset + 8 <= bytesRead) {
        const auto chunkId = decodeFourCC(header + offset);
        const auto chunkSize = static_cast<int>(readLittleEndian32(header + offset + 4));
        offset += 8;

        if (chunkId == "fmt " && offset + 16 <= bytesRead) {
            const auto formatCode = readLittleEndian16(header + offset + 0);
            const auto numChannels = readLittleEndian16(header + offset + 2);
            const auto sampleRate = readLittleEndian32(header + offset + 4);
            const auto bitsPerSample = (offset + 16 <= bytesRead)
                ? readLittleEndian16(header + offset + 14)
                : 0;

            detail += " formatCode=0x" + juce::String::toHexString(static_cast<int>(formatCode)).paddedLeft('0', 4);
            detail += " channels=" + juce::String(static_cast<int>(numChannels));
            detail += " sampleRate=" + juce::String(static_cast<int>(sampleRate));
            detail += " bitsPerSample=" + juce::String(static_cast<int>(bitsPerSample));

            if (formatCode == 0xfffe && chunkSize >= 40 && offset + 26 <= bytesRead) {
                const auto validBits = readLittleEndian16(header + offset + 18);
                const auto subFormat = readLittleEndian16(header + offset + 24);
                detail += " extensibleValidBits=" + juce::String(static_cast<int>(validBits));
                detail += " extensibleSubFormat=0x" + juce::String::toHexString(static_cast<int>(subFormat)).paddedLeft('0', 4);
            }

            return detail;
        }

        if (chunkSize <= 0) {
            break;
        }

        offset += chunkSize + (chunkSize & 1);
    }

    return detail;
}

juce::String buildFormatDiagnostics(const juce::File& file, juce::AudioFormatManager& formatManager)
{
    juce::StringArray lines;

    for (int index = 0; index < formatManager.getNumKnownFormats(); ++index) {
        auto* format = formatManager.getKnownFormat(index);
        if (format == nullptr) {
            continue;
        }

        const bool canHandle = format->canHandleFile(file);
        bool streamOpened = false;
        bool readerCreated = false;

        if (canHandle) {
            auto stream = std::unique_ptr<juce::InputStream>(file.createInputStream());
            streamOpened = (stream != nullptr);

            if (streamOpened) {
                if (auto* reader = format->createReaderFor(stream.get(), false)) {
                    readerCreated = true;
                    delete reader;
                }
            }
        }

        lines.add(format->getFormatName()
            + " extMatch=" + juce::String(canHandle ? "yes" : "no")
            + " stream=" + juce::String(streamOpened ? "yes" : "no")
            + " reader=" + juce::String(readerCreated ? "yes" : "no"));
    }

    return lines.joinIntoString(" | ");
}

void appendOptionalFormats(juce::AudioFormatManager& formatManager)
{
#if JUCE_USE_FLAC
    formatManager.registerFormat(new juce::FlacAudioFormat(), false);
#endif

#if JUCE_USE_OGGVORBIS
    formatManager.registerFormat(new juce::OggVorbisAudioFormat(), false);
#endif

#if JUCE_MAC || JUCE_IOS
    formatManager.registerFormat(new juce::CoreAudioFormat(), false);
#endif

#if JUCE_USE_MP3AUDIOFORMAT
    formatManager.registerFormat(new juce::MP3AudioFormat(), false);
#endif

#if JUCE_USE_WINDOWS_MEDIA_FORMAT
    formatManager.registerFormat(new juce::WindowsMediaAudioFormat(), false);
#endif
}

juce::StringArray collectRegisteredFormatNames(const juce::AudioFormatManager& formatManager)
{
    juce::StringArray names;
    for (int index = 0; index < formatManager.getNumKnownFormats(); ++index) {
        if (const auto* format = formatManager.getKnownFormat(index)) {
            names.addIfNotAlreadyThere(format->getFormatName());
        }
    }

    return names;
}

} // namespace

void registerImportFormats(juce::AudioFormatManager& formatManager)
{
    formatManager.clearFormats();
    formatManager.registerFormat(new juce::WavAudioFormat(), true);
    formatManager.registerFormat(new juce::AiffAudioFormat(), false);
    appendOptionalFormats(formatManager);
}

juce::String getImportWildcardFilter()
{
    juce::AudioFormatManager formatManager;
    registerImportFormats(formatManager);
    return formatManager.getWildcardForAllFormats();
}

juce::String describeRegisteredImportFormats()
{
    juce::AudioFormatManager formatManager;
    registerImportFormats(formatManager);
    return collectRegisteredFormatNames(formatManager).joinIntoString(", ");
}

FileProbeResult probeFile(const juce::File& file)
{
    FileProbeResult result;
    result.fileExists = file.existsAsFile();
    result.fileSize = result.fileExists ? file.getSize() : -1;

    juce::AudioFormatManager formatManager;
    registerImportFormats(formatManager);

    result.wildcardFilter = formatManager.getWildcardForAllFormats();
    result.registeredFormats = collectRegisteredFormatNames(formatManager).joinIntoString(", ");
    result.formatDiagnostics = buildFormatDiagnostics(file, formatManager);
    result.containerDiagnostics = probeWavContainer(file);
    result.streamOpened = static_cast<bool>(file.createInputStream());
    return result;
}

std::unique_ptr<juce::AudioFormatReader> createReaderFor(const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    registerImportFormats(formatManager);
    return std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file));
}

} // namespace OpenTune::AudioFormatRegistry
