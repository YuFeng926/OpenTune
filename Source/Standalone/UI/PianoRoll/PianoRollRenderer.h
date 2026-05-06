#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <functional>
#include "UI/UIColors.h"
#include "UI/WaveformMipmap.h"
#include "Utils/F0Timeline.h"
#include "Utils/MaterializationTimelineProjection.h"
#include "Utils/PitchCurve.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/Note.h"
#include "Inference/GameTypes.h"
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
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
        double pixelsPerSecond = 100.0;
        float pixelsPerSemitone = 15.0f;
        float minMidi = 24.0f;
        float maxMidi = 108.0f;
        double bpm = 120.0;
        MaterializationTimelineProjection materializationProjection;
        F0Timeline f0Timeline;
        int scaleRootNote = 0;
        int scaleType = 1;
        NoteNameMode noteNameMode = NoteNameMode::COnly;
        bool showLanes = true;
        bool showChunkBoundaries = false;
        bool showUnvoicedFrames = false;
        int pressedPianoKey = -1;
        bool hasUserAudio = false;
        std::vector<double> chunkBoundaries;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;

        bool hasF0Selection = false;
        int f0SelectionStartFrame = -1;
        int f0SelectionEndFrameExclusive = -1;

        enum class TimeUnit { Seconds, Bars } timeUnit = TimeUnit::Seconds;

        std::function<float(float)> midiToY;
        std::function<float(float)> freqToY;
        std::function<float(float)> freqToMidi;
        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;
    };

    void drawLanes(juce::Graphics& g, const RenderContext& ctx);
    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx);
    void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
    void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
    void drawChunkBoundaries(juce::Graphics& g, const RenderContext& ctx);
    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx,
                   const std::vector<Note>& notes);

    void drawReferenceNotes(juce::Graphics& g, const RenderContext& ctx,
                            const std::vector<ReferenceNote>& referenceNotes);

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
