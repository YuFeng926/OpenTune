#pragma once

/**
 * 宿主集成接口
 * 
 * 定义与宿主应用程序（DAW或独立模式）集成的抽象接口。
 * 支持插件模式和独立模式的不同行为。
 */

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

namespace juce {
class AudioProcessorEditor;
}

namespace OpenTune {

class OpenTuneAudioProcessor;

class HostIntegration {
public:
    virtual ~HostIntegration() = default;

    virtual void configureInitialState(OpenTuneAudioProcessor& processor) = 0;

    virtual bool processIfApplicable(OpenTuneAudioProcessor& processor,
                                     juce::AudioBuffer<float>& buffer,
                                     int totalNumInputChannels,
                                     int totalNumOutputChannels,
                                     int numSamples) = 0;

    virtual void audioSettingsRequested(juce::AudioProcessorEditor& editor) = 0;
};

std::unique_ptr<HostIntegration> createHostIntegration();

} // namespace OpenTune

