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
#include <functional>
#include <optional>

namespace OpenTune {

enum class PianoRollTimeUnit { Seconds, Bars };

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
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
        F0Timeline f0Timeline;
        std::vector<Note> displayNotes;
        bool active = false;

        bool isValid() const noexcept
        {
            return contentKey.isValid() && projection.isValid() && static_cast<bool>(timeGrid);
        }
    };

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

    struct F0VisualBuildOptions
    {
        int startFrame = 0;
        int endFrameExclusive = 0;
        int viewportStartX = 0;
        int viewportEndX = 0;
        double pixelsPerSecond = 100.0;
        double secondsPerFrame = 0.01;
    };

    using F0FrameToX = std::function<float(int)>;
    using F0FrameToY = std::function<float(int, float)>;

    static std::vector<F0VisualSegment> buildF0VisualSegments(const std::vector<float>& f0,
                                                              const std::vector<float>* originalEnergy,
                                                              const F0VisualBuildOptions& options,
                                                              const F0FrameToX& frameToX,
                                                              const F0FrameToY& frameToY);

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
        // reference content 的 TimeGrid（source↔output 转换）
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
    };

    /// Render context — includes stable content state and transient UI fields.
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
        bool showUnvoicedFrames = false;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;

        PianoRollTimeUnit timeUnit = PianoRollTimeUnit::Seconds;
        ViewMapper coords;

        int pressedPianoKey = -1;
        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }
        bool hasF0Selection = false;
        int f0SelectionStartFrame = -1;
        int f0SelectionEndFrameExclusive = -1;
        std::optional<ReferenceOverlay> referenceOverlay;
        uint64_t timeGridHoveredHandleId = 0;
        uint64_t timeGridSelectedHandleId = 0;
        std::vector<uint64_t> additionalSelectedHandleIds;
        std::vector<int> selectedLineAnchorSegmentIds;
    };

    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item,
                      const WaveformMipmap::Level& wfLevel, int wfLevelIndex);

    /// Draw published TimeGrid anchors as cache-friendly neutral lines.
    /// No hover/selected/drag affordances — those are painted by drawTimeGridHandles in overlay.
    void drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawSelectedNoteHighlights(juce::Graphics& g,
                                    const RenderContext& ctx,
                                    const std::vector<Note>& notes,
                                    const std::vector<int>& selectedNoteIndices,
                                    const ContentRenderItem& item);
    void drawF0Curve(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    void drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
    void drawGhostAnchors(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
};

} // namespace OpenTune
