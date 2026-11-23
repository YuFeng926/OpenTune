#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <map>
#include "UI/UIColors.h"
#include "UI/WaveformMipmap.h"
#include "Utils/F0Timeline.h"
#include "Utils/ContentTimelineProjection.h"
#include "Content/ContentKey.h"
#include "Utils/PitchCurve.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/Note.h"
#include "Utils/TimeGrid.h"   // ⚡️ vocal-time-stretch §8.5 — TimeGrid handles
#include "UI/ToolIds.h"
#include "UI/ViewMapper.h"
#include "Content/EditableContentSnapshot.h"
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace OpenTune {

struct TimelineContentPlacement
{
    ContentKey contentKey;
    ContentTimelineProjection projection;
    juce::Colour displayColour{0xFF4A90D9}; // 轨道主题色/音符主色

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
     * 每个 contentKey 一次 owner render pass 构建一份；owner/committed 快照
     * 身份由构建方决定，Renderer 只读消费。
     */
    struct ContentRenderItem
    {
        std::shared_ptr<const EditableContentSnapshot> committedSnapshot;
        std::shared_ptr<const EditableContentSnapshot> ownerSnapshot;
        const std::vector<Note>* displayNotes = nullptr;
        F0Timeline f0Timeline;
        const AutomationLane* volumeEnvelope = nullptr; // OpenDyne blob 音量包络（含拖拽预览），构建时由 owner 决定
        bool notesPrimaryScheme = false;                // OpenDyne：波形 blob 模式
    };

    /// placement（投影 + 显示色，多个 placement 可指向同一 content）与共享只读 content 的配对。
    struct PlacedContent
    {
        const TimelineContentPlacement& placement;
        const ContentRenderItem& content;
    };

    using ContentRenderItems = std::map<ContentKey, ContentRenderItem>;

    /// Render context — includes stable content state and transient UI fields.
    struct RenderContext
    {
        int width = 0;
        int height = 0;
        int pianoKeyWidth = 60;
        int contentStartX = 60;
        juce::Rectangle<int> rasterBounds;  // Image-local rect to rasterize; full image bounds when empty
        int rulerHeight = 30;
        double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
        float pixelsPerSemitone = 15.0f;
        float minMidi = 24.0f;
        float maxMidi = 108.0f;
        int scaleRootNote = 0;
        int scaleType = 1;
        NoteNameMode noteNameMode = NoteNameMode::COnly;
        bool labelsOnly = false;
        bool showUnvoicedFrames = false;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;
        juce::Colour correctedF0Colour{0xFF1D6FC0};
        juce::Colour originalF0Colour{0xFFC91B1E};

        bool hasF0Selection = false;
        std::vector<std::pair<int, int>> f0SelectionRanges;  // {startFrame, endFrameExclusive}，排序归并

        ViewMapper coords;

        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }
        uint64_t timeGridHoveredHandleId = 0;
        uint64_t timeGridSelectedHandleId = 0;
        std::vector<uint64_t> additionalSelectedHandleIds;
    };

    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, PlacedContent item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, PlacedContent item,
                      const WaveformMipmap::Level& wfLevel, int wfLevelIndex);

    /// Draw published TimeGrid anchors as cache-friendly neutral lines.
    /// No hover/selected/drag affordances — those are painted by drawTimeGridHandles in overlay.
    void drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx, PlacedContent item);

    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, PlacedContent item);
    void drawSelectedNoteHighlights(juce::Graphics& g,
                                    const RenderContext& ctx,
                                    PlacedContent item,
                                    const std::vector<int>& selectedNoteIndices,
                                    bool asPlayheadHighlight = false);
    void drawF0Curve(juce::Graphics& g, const RenderContext& ctx, PlacedContent item);
    void drawF0SelectionHighlight(juce::Graphics& g, const RenderContext& ctx, PlacedContent item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    // TimeGrid 拖拽的 working grid 由调用方传入，共享 item 保持只读。
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx,
                             const ContentTimelineProjection& projection,
                             const TimeGridSnapshot& grid);


};

} // namespace OpenTune
