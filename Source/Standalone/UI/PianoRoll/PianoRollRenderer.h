#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include "UI/UIColors.h"
#include "UI/WaveformMipmap.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include <vector>
#include <array>
#include <cstdint>

namespace OpenTune {

/**
 * 钢琴卷帘渲染器
 * 负责绘制钢琴卷帘界面的所有元素，包括背景、琴键、音符、波形和音高曲线
 */
class PianoRollRenderer
{
public:
    PianoRollRenderer() = default;

    void setWaveformMipmap(WaveformMipmap* mipmap) { waveformMipmap_ = mipmap; }
    WaveformMipmap* getWaveformMipmap() const { return waveformMipmap_; }

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
        int noteNameMode = 1; // 0=ShowAll, 1=COnly, 2=Hide
        bool showWaveform = true;
        bool showLanes = true;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;
        bool isRendering = false;
        float renderingProgress = 0.0f;
        bool hasUserAudio = false;

        bool hasF0Selection = false;
        int f0SelectionStartFrame = -1;
        int f0SelectionEndFrame = -1;

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

private:
    WaveformMipmap* waveformMipmap_ = nullptr;
    std::vector<float> correctedF0Cache_;
    std::shared_ptr<const PitchCurveSnapshot> cachedSnapshot_;
};

} // namespace OpenTune
