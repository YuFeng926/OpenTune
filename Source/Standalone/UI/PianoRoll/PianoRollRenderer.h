#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "UI/UIColors.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include <vector>
#include <array>
#include <cstdint>

namespace OpenTune {

/**
 * 波形缩略图采样点结构体
 * 存储单个缩略采样点的最小和最大振幅值
 */
struct MinMaxValue
{
    int8_t values[2];

    MinMaxValue() noexcept : values{0, 0} {}

    void setFloat(juce::Range<float> r) noexcept
    {
        values[0] = static_cast<int8_t>(juce::jlimit(-128, 127, juce::roundToInt(r.getStart() * 127.0f)));
        values[1] = static_cast<int8_t>(juce::jlimit(-128, 127, juce::roundToInt(r.getEnd() * 127.0f)));
    }

    juce::Range<float> getRange() const noexcept
    {
        return juce::Range<float>(values[0] / 127.0f, values[1] / 127.0f);
    }

    bool isNonZero() const noexcept
    {
        return values[0] != 0 || values[1] != 0;
    }
};

/**
 * 波形缩略图缓存结构体
 * 缓存音频波形的缩略图数据以实现高效渲染
 */
struct WaveformThumbCache
{
    const juce::AudioBuffer<float>* buffer{nullptr};
    int numSamples{0};
    int sampleRate{44100};

    static constexpr int samplesPerThumbSample = 512;
    int numThumbSamples{0};
    std::vector<MinMaxValue> thumbData;
    int numThumbSamplesFinished{0};
    bool thumbComplete{false};
};

/**
 * 钢琴卷帘渲染器
 * 负责绘制钢琴卷帘界面的所有元素，包括背景、琴键、音符、波形和音高曲线
 */
class PianoRollRenderer
{
public:
    PianoRollRenderer() = default;

    void prepareWaveformThumbCache(const juce::AudioBuffer<float>* buffer, int sampleRate);
    bool buildWaveformThumbSlice(double timeBudgetMs);
    void invalidateWaveformCache() { waveformThumbCache_ = WaveformThumbCache{}; }

    /**
     * 渲染上下文结构体
     * 包含渲染所需的所有参数和回调函数
     */
    struct RenderContext
    {
        int width = 0;
        int height = 0;
        int pianoKeyWidth = 60;
        int rulerHeight = 30;
        double zoomLevel = 1.0;
        int scrollOffset = 0;
        float pixelsPerSemitone = 15.0f;
        float minMidi = 24.0f;
        float maxMidi = 108.0f;
        double bpm = 120.0;
        int timeSigNum = 4;
        int timeSigDenom = 4;
        double trackOffsetSeconds = 0.0;
        double audioSampleRate = 44100.0;
        int hopSize = 512;
        double f0SampleRate = 16000.0;
        int scaleRootNote = 0;
        int scaleType = 1;
        bool showWaveform = true;
        bool showLanes = true;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;
        bool isRendering = false;
        float renderingProgress = 0.0f;
        bool hasUserAudio = false;

        enum class TimeUnit { Seconds, Bars } timeUnit = TimeUnit::Seconds;

        std::function<float(float)> midiToY;
        std::function<float(float)> freqToY;
        std::function<float(float)> freqToMidi;
        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;
        std::function<double(double)> clipSecondsToFrameIndex;
        std::function<double(int)> frameIndexToClipSeconds;
    };

    void drawBackground(juce::Graphics& g, const RenderContext& ctx);
    void drawLanes(juce::Graphics& g, const RenderContext& ctx);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx);
    void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
    void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx,
                   const std::vector<Note>& notes,
                   double trackOffsetSeconds);

    void drawF0Curve(juce::Graphics& g,
                     const std::vector<float>& f0,
                     juce::Colour colour,
                     float alpha,
                     bool isThinLine,
                     const RenderContext& ctx,
                     std::shared_ptr<PitchCurve> currentCurve,
                     const std::vector<uint8_t>* visibleMask = nullptr);

    void updateCorrectedF0Cache(std::shared_ptr<const PitchCurveSnapshot> snapshot);
    void clearCorrectedF0Cache() { correctedF0Cache_.clear(); cachedSnapshot_.reset(); }
    const std::vector<float>& getCorrectedF0Cache() const { return correctedF0Cache_; }

    const WaveformThumbCache& getWaveformThumbCache() const { return waveformThumbCache_; }

private:
    WaveformThumbCache waveformThumbCache_;
    std::vector<float> correctedF0Cache_;
    std::shared_ptr<const PitchCurveSnapshot> cachedSnapshot_;
};

} // namespace OpenTune
