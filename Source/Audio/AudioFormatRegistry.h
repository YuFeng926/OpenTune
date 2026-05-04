#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace OpenTune::AudioFormatRegistry {

struct FileProbeResult {
    bool fileExists = false;
    bool streamOpened = false;
    juce::int64 fileSize = -1;
    juce::String wildcardFilter;
    juce::String registeredFormats;
    juce::String formatDiagnostics;
    juce::String containerDiagnostics;
};

void registerImportFormats(juce::AudioFormatManager& formatManager);
juce::String getImportWildcardFilter();
juce::String describeRegisteredImportFormats();
FileProbeResult probeFile(const juce::File& file);
std::unique_ptr<juce::AudioFormatReader> createReaderFor(const juce::File& file);

} // namespace OpenTune::AudioFormatRegistry
