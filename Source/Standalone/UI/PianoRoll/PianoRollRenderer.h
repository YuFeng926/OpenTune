#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <functional>
#include "UI/UIColors.h"
#include "UI/WaveformMipmap.h"
#include "Utils/F0Timeline.h"
#include "Utils/ContentTimelineProjection.h"
#include "Content/ContentKey.h"
#include "Utils/PitchCurve.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/Note.h"
#include "Utils/TimeGrid.h"   // ⚡️ vocal-time-stretch §8.5 — TimeGrid handles
#include "UI/ToolIds.h"       // ⚡️ vocal-time-stretch §8.5 (Phase J) — currentTool
#include "PianoRollCoordinateMapper.h"
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace OpenTune {

struct TimelineContentPlacement
{
    ContentKey contentKey;
    ContentTimelineProjection projection;

    bool isValid() const noexcept
    {
        return contentKey.isValid() && projection.isValid();
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

    struct F0VisualPoint
    {
        int frame = 0;
        float x = 0.0f;
        float y = 0.0f;
        float energyAlpha = 1.0f;
        float levelHotMix = 0.0f;
    };

    struct F0VisualSegment
    {
        std::vector<F0VisualPoint> points;
    };

    /**
     * 渲染上下文结构体
     * 包含渲染所需的所有参数和回调函数
     */
    struct ContentRenderItem
    {
        ContentKey contentKey;
        ContentTimelineProjection projection;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        WaveformMipmap* waveformMipmap = nullptr;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
        std::vector<float> correctedF0;
        std::vector<F0VisualSegment> originalF0VisualSegments;
        std::vector<F0VisualSegment> correctedF0VisualSegments;
        F0Timeline f0Timeline;
        std::vector<Note> displayNotes;
        std::vector<int> selectedNoteIndices;
        std::vector<double> chunkBoundaries;
        bool active = false;

        bool isValid() const noexcept
        {
            return contentKey.isValid() && projection.isValid();
        }
    };

    struct ReferenceOverlay
    {
        std::vector<Note> ghostNotes;               // reference content 的 derived notes（content-local source time）
        struct GhostAnchor {
            double sourceSeconds{0.0};
            float strength{0.0f};
        };
        std::vector<GhostAnchor> ghostAnchors;
        float ghostOpacity{0.20f};                   // 透明度
        juce::Colour ghostColour;                    // ghost 颜色（不同于当前轨）
        bool enabled{false};                         // 是否启用 overlay

        // sourceSeconds → timeline time 的投影函数
        std::function<double(double)> projectSourceTime;
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
        std::vector<ContentRenderItem> contents;
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

        std::optional<ReferenceOverlay> referenceOverlay; // 可选参考投射

        // ⚡️ vocal-time-stretch §8.5 — TimeGrid handles overlay.
        // When non-null, renderer paints vertical guide lines at each handle's
        // output_seconds (because piano-roll x-axis is OUTPUT/display time).
        // Endpoint handles are visually distinguished (locked = solid, dimmer);
        // user-draggable handles use kind-specific colors.
        std::shared_ptr<const TimeGridSnapshot> timeGridSnapshot;
        uint64_t timeGridHoveredHandleId = 0;
        uint64_t timeGridSelectedHandleId = 0;
        std::vector<uint64_t> additionalSelectedHandleIds;
        std::vector<int> selectedLineAnchorSegmentIds;

        // ⚡️ vocal-time-stretch §8.5 (Phase I) — 将 content-local 的
        // handle output_seconds 转换为 timeline time，供 drawTimeGridHandles
        // 通过 timeToX 正确映射到屏幕坐标。在 Standalone 模式下此投影为恒等。
        std::function<double(double)> contentTimeToTimeline;

        // ⚡️ vocal-time-stretch §8.5 (Phase J) — current tool drives view
        // mode: TimeTool → Time view (no piano keys, no notes/F0, full-height
        // handles); else → Pitch view (existing).
        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }

        enum class TimeUnit { Seconds, Bars } timeUnit = TimeUnit::Seconds;

        std::function<float(float)> midiToY;
        std::function<float(float)> freqToY;
        std::function<float(float)> freqToMidi;
        std::function<double(int)> xToTime;
        std::function<int(double)> timeToX;

        // Direct coordinate mapper — avoids lambda overhead in tile worker
        PianoRollCoordinateMapper coords;
    };

    struct F0VisualBuildOptions
    {
        int startFrame = 0;
        int endFrameExclusive = 0;
        int viewportStartX = 0;
        int viewportEndX = 0;
    };

    using F0FrameToX = std::function<float(int)>;
    using F0FrameToY = std::function<float(int, float)>;

    static std::vector<F0VisualSegment> buildF0VisualSegments(const std::vector<float>& f0,
                                                              const std::vector<float>* originalEnergy,
                                                              const std::vector<uint8_t>* visibleMask,
                                                              const F0VisualBuildOptions& options,
                                                              const F0FrameToX& frameToX,
                                                              const F0FrameToY& frameToY);

    void drawLanes(juce::Graphics& g, const RenderContext& ctx);
    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
    void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
    void drawChunkBoundaries(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx);

    void drawPreparedF0Curve(juce::Graphics& g,
                             const std::vector<F0VisualSegment>& visualSegments,
                             juce::Colour colour,
                             float alpha,
                             bool isThinLine,
                             const RenderContext& ctx);

    void drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
    void drawGhostAnchors(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
};

} // namespace OpenTune
