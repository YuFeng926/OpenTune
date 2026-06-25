#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
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
#include "UI/ViewMapper.h"
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

    /**
     * 渲染上下文结构体
     * 包含渲染所需的所有参数
     */
    struct ContentRenderItem
    {
        ContentKey contentKey;
        ContentTimelineProjection projection;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        WaveformMipmap* waveformMipmap = nullptr;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
        std::vector<float> correctedF0;
        F0Timeline f0Timeline;
        std::vector<Note> displayNotes;
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

        // source content → timeline 的投影值
        ContentTimelineProjection sourceProjection;
    };

    struct RenderContext
    {
        int width = 0;
        int height = 0;
        int pianoKeyWidth = 60;
        int rulerHeight = 30;
        double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
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
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;
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

        // Active content → timeline projection.
        // drawTimeGridHandles and ghost-note rendering use this to convert
        // content-local time to timeline time for screen coordinate mapping.
        ContentTimelineProjection activeProjection;

        // ⚡️ vocal-time-stretch §8.5 (Phase J) — current tool drives view
        // mode: TimeTool → Time view (no piano keys, no notes/F0, full-height
        // handles); else → Pitch view (existing).
        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }

        enum class TimeUnit { Seconds, Bars } timeUnit = TimeUnit::Seconds;

        // Coordinate mapper — replaces midiToY/freqToY/freqToMidi/xToTime/timeToX lambdas.
        // All coordinate conversions use ctx.coords.xxx directly.
        ViewMapper coords;
    };

    void drawLanes(juce::Graphics& g, const RenderContext& ctx);
    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
    void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
    void drawChunkBoundaries(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawSelectedNoteHighlights(juce::Graphics& g,
                                    const RenderContext& ctx,
                                    const std::vector<Note>& notes,
                                    const std::vector<int>& selectedNoteIndices,
                                    const ContentRenderItem& item);
    void drawF0Curve(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx);

    void drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
    void drawGhostAnchors(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
};

} // namespace OpenTune
