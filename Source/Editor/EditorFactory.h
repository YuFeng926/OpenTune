#pragma once

/**
 * OpenTune 编辑器工厂
 * 
 * 负责创建 OpenTune 的 JUCE 音频处理器编辑器实例。
 */

#include <juce_audio_processors/juce_audio_processors.h>

namespace OpenTune {

class OpenTuneAudioProcessor;

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor);

} // namespace OpenTune

