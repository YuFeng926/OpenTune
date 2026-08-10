#pragma once

#include "TimelineViewportCamera.h"
#include <cmath>

namespace OpenTune {

// ── 共享播放头表现判定 ────────────────────────────────────────
// 给定 resolved camera 与 view 几何，推导 playhead anchor/visible/fixedCentre。
// 核心原则：playing + Continuous 只是意图，不是结果。
// 播放头是否固定居中，必须由 resolved camera 是否真的让
// timeToX(playheadTime) 落在 content viewport center 来决定。

struct TimelinePlayheadPresentation
{
    int anchorX = 0;
    bool visible = false;
    bool fixedCentre = false;
};

// ─────────────────────────────────────────────────────────────────

struct TimelineViewportRequest
{
    enum class Kind
    {
        Cont,    // Continuous scroll — center targetTime
        Page,    // Page navigation — show the page containing targetTime
        Manual,  // Manual drag — targetTime is desired visibleStartSeconds
        Click,   // Click seeking — position targetTime at anchorViewportX
        Zoom     // Zoom at mouse
    };

    enum class ViewKind
    {
        Arrangement,
        PianoRoll
    };

    Kind kind = Kind::Manual;
    ViewKind viewKind = ViewKind::Arrangement;
    double targetTime = 0.0;          // 目标时间点（absolute timeline seconds）
    double currentVisibleStartSeconds = 0.0;
    double anchorViewportX = 0.0;     // anchor 在 viewport 中的 X 像素位置
    int viewportWidth = 0;            // viewport 宽度（像素）
    double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
};

class TimelineViewportPolicy
{
public:
    static constexpr double kMinPixelsPerSecond = 10.0;
    static constexpr double kMaxPixelsPerSecond = 1000.0;

    static double normalisePixelsPerSecond(double pps, TimelineViewportRequest::ViewKind viewKind) noexcept;

    // 唯一入口：根据 request 计算 camera（含 Zoom）
    static TimelineViewportCamera resolve(const TimelineViewportRequest& request);

    // 钳制 visibleStartSeconds >= 0
    static double clampStartSeconds(double startSeconds);

    // 便捷：从 camera + viewportWidth 计算 visibleEndSeconds
    static double visibleEndSeconds(const TimelineViewportCamera& camera, int viewportWidth);

    // 共享播放头表现判定：只有 resolved camera 真的让 playhead 居中时才固定居中
    // timeDerivedX: ViewMapper::timeToX(playheadTime)
    // viewportCentreX: content viewport 中心（组件坐标系）
    // viewportRight: content viewport 右边界
    // viewLeftGuardX: 播放头最小可见 x（PianoRoll: mapper.contentStartX; Arrangement: viewport left）
    // playing / continuousMode: 状态和意图（PianoRoll continuousMode 应含 !userScrollHold_）
    static TimelinePlayheadPresentation computePlayheadPresentation(
        int timeDerivedX,
        int viewportCentreX,
        int viewportRight,
        int viewLeftGuardX,
        bool playing,
        bool continuousMode) noexcept;
};

} // namespace OpenTune
