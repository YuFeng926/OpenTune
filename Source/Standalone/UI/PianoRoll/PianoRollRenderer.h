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
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace OpenTune {

struct TimelineMaterializationPlacement
{
    uint64_t materializationId = 0;
    MaterializationTimelineProjection projection;

    bool isValid() const noexcept
    {
        return materializationId != 0 && projection.isValid();
    }
};

/**
 * 钢琴卷帘渲染器
 * 负责绘制钢琴卷帘界面的所有元素，包括背景、琴键、音符、波形和音高曲线
 */
class PianoRollRenderer
{
public:
    PianoRollRenderer() = default;

    /**
     * 渲染上下文结构体
     * 包含渲染所需的所有参数和回调函数
     */
    struct MaterializationRenderItem
    {
        uint64_t materializationId = 0;
        MaterializationTimelineProjection projection;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        WaveformMipmap* waveformMipmap = nullptr;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
        std::vector<float> correctedF0;
        F0Timeline f0Timeline;
        std::vector<Note> notes;
        std::vector<double> chunkBoundaries;
        bool active = false;

        bool isValid() const noexcept
        {
            return materializationId != 0 && projection.isValid();
        }
    };

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
        std::vector<MaterializationRenderItem> materializations;
        int scaleRootNote = 0;
        int scaleType = 1;
        NoteNameMode noteNameMode = NoteNameMode::COnly;
        bool showLanes = true;
        bool showChunkBoundaries = false;
        bool showUnvoicedFrames = false;
        int pressedPianoKey = -1;

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
    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, const MaterializationRenderItem& item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, const MaterializationRenderItem& item);
    void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
    void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
    void drawChunkBoundaries(juce::Graphics& g, const RenderContext& ctx, const MaterializationRenderItem& item);
    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, const MaterializationRenderItem& item);

    void drawF0Curve(juce::Graphics& g,
                     const std::vector<float>& f0,
                     juce::Colour colour,
                     float alpha,
                     bool isThinLine,
                     const RenderContext& ctx,
                     const MaterializationRenderItem& item,
                     const std::vector<uint8_t>* visibleMask = nullptr);

private:
};

} // namespace OpenTune
