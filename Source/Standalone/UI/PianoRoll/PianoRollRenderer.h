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
#include "UI/ToolIds.h"
#include "UI/ViewMapper.h"
#include "Content/EditableContentSnapshot.h"
#include <algorithm>
#include <vector>
#include <array>
#include <unordered_map>
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

    // Modulation/Drift 拖拽期间的临时 pitch curve 快照（由组件注入，指向
    // InteractionState::tempPitchCurves；指针生命周期 = 组件生命周期，内容由
    // ToolHandler 拖拽期间维护）。非拖拽期间为 nullptr 或空 map，drawF0Curve 走
    // 已提交 PitchCurve 原路径。
    const std::unordered_map<size_t, std::vector<float>>* tempPitchCurves = nullptr;

    void setTempPitchCurves(const std::unordered_map<size_t, std::vector<float>>* curves) noexcept
    {
        tempPitchCurves = curves;
    }

    // OpenDyne blob 音量预览：拖拽期间由组件注入（指向 InteractionState::volumePreviewEnvelope）。
    // 非拖拽时为 nullptr，drawNotes 回退到 ownerSnapshot->volumeEnvelope。
    const AutomationLane* volumePreviewEnvelope_ = nullptr;
    void setVolumePreviewEnvelope(const AutomationLane* env) noexcept { volumePreviewEnvelope_ = env; }

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
        std::shared_ptr<const EditableContentSnapshot> ownerSnapshot;
        const std::vector<Note>* displayNotes = nullptr;
        bool active = false;
        juce::Colour displayColour{0xFF4A90D9}; // 从 placement 注入，Renderer 不查 owner
        bool notesPrimaryScheme = false;        // OpenDyne：波形 blob 模式

        bool isValid() const noexcept
        {
            return contentKey.isValid() && projection.isValid() && static_cast<bool>(timeGrid);
        }
    };

    struct ReferenceOverlay
    {
        std::vector<Note> ghostNotes;               // reference content 的 derived notes（content-local source time）
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
        juce::Rectangle<int> rasterBounds;  // Image-local rect to rasterize; full image bounds when empty
        int rulerHeight = 30;
        double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
        float pixelsPerSemitone = 15.0f;
        float minMidi = 24.0f;
        float maxMidi = 108.0f;
        std::vector<ContentRenderItem> contents;
        int scaleRootNote = 0;
        int scaleType = 1;
        NoteNameMode noteNameMode = NoteNameMode::COnly;
        bool showUnvoicedFrames = false;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;

        bool hasF0Selection = false;
        int f0SelectionStartFrame = -1;
        int f0SelectionEndFrameExclusive = -1;

        ViewMapper coords;

        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }
        uint64_t timeGridHoveredHandleId = 0;
        uint64_t timeGridSelectedHandleId = 0;
        std::vector<uint64_t> additionalSelectedHandleIds;
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
    void drawF0SelectionHighlight(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    void drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);
};

} // namespace OpenTune
