#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>

namespace OpenTune {

/**
 * 钢琴琴键试听 - 为 PianoRoll 组件提供按键预览声音。
 *
 * MIDI 21-108（88 键）每音对应一个 MP3 采样，从 BinaryData 加载，
 * 直接按索引播放，无需变调。音频混入 processBlock 的输出缓冲区。
 *
 * 线程安全：noteOn/noteOff 由消息线程调用，mixIntoBuffer 由音频线程调用。
 * 通过无锁 FIFO 传递事件，单生产者单消费者。
 */
class PianoKeyAudition {
public:
    PianoKeyAudition();

    /// 从 BinaryData 加载 88 个采样。构造后由消息线程调用一次。
    void loadSamples();

    /// 由消息线程调用
    void noteOn(int midiNote);
    void noteOff(int midiNote);

    /// 由音频线程在 processBlock 中调用。将试听音频混入缓冲区。
    void mixIntoBuffer(juce::AudioBuffer<float>& output, int numSamples, double sampleRate);

    /// 当前按下的 MIDI 音符（-1 表示无）
    int getPressedNote() const noexcept { return pressedNote_.load(std::memory_order_relaxed); }

private:
    static constexpr int kMaxVoices = 8;
    static constexpr double kSampleRate = 44100.0;
    static constexpr float kReleaseTimeSeconds = 0.05f;
    static constexpr int kEventBufferSize = 64;
    static constexpr int kMidiMin = 21;
    static constexpr int kMidiMax = 108;
    static constexpr int kSampleCount = kMidiMax - kMidiMin + 1;

    struct Sample {
        juce::AudioBuffer<float> pcm;
        int onsetSample = 0;  // 首个有效振幅位置，跳过静音前导
    };

    struct Voice {
        bool active = false;
        int midiNote = -1;
        int sampleIndex = -1;
        double position = 0.0;
        double playbackRate = 1.0;
        float gain = 0.7f;
        bool releasing = false;
        float releaseGain = 1.0f;
        float releaseDecrement = 0.0f;
    };

    struct NoteEvent {
        int midiNote = 0;
        bool isOn = false;
    };

    // 无锁 SPSC 事件环形缓冲区
    std::array<NoteEvent, kEventBufferSize> eventBuffer_{};
    std::atomic<int> eventWritePos_{0};
    std::atomic<int> eventReadPos_{0};

    // samples_[midiNote - kMidiMin] 直接索引
    std::array<Sample, kSampleCount> samples_{};
    std::array<Voice, kMaxVoices> voices_{};
    std::atomic<int> pressedNote_{-1};
    bool samplesLoaded_ = false;

    void processEvents(double sampleRate);
    void startVoice(int midiNote, double sampleRate);
    void stopVoice(int midiNote, double sampleRate);
};

} // namespace OpenTune
