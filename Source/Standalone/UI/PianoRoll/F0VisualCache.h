#pragma once

#include "../../../Utils/ContentTimelineProjection.h"
#include "../../../Content/EditableContentSnapshot.h"
#include "../../../Utils/TimeGrid.h"
#include <memory>
#include <vector>

namespace OpenTune {

/// F0 绘制的设备无关缓存条目。缓存键为时间投影的实际决定因素：
/// 帧轴取值（帧数/hop/采样率）+ committed TimeGridSnapshot 身份 + placement
/// 投影 4 个 double。gapRatio 与 scheme 只影响发射/着色（留在直算原语内），
/// 拖拽预览烘焙新频率但保留帧轴——两种 scheme 与预览共用同一时间数组。
///
/// 缓存数据只有全曲线帧 → timeline 秒投影（sourceToTimeline(timeAtFrame(f))）。
/// 段发射/合并/长度判定语义不在缓存内：每次绘制以直算原语
/// （forEachCorrectionF0Span / 原始 F0 单段发射）作用于当前窗口，
/// 仅 X 源换用本数组——修正段守卫（声明跨度 > 数据长度的发射判定）依赖
/// 请求窗口端点，任何「先全量构建再裁剪」都会造成发射差异。
struct F0VisualCacheEntry
{
    // ── 缓存键（帧轴取值 + committed 网格身份 + 投影逐位）────────────
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    ContentTimelineProjection projection;
    int frameCount = 0;
    int hopSize = 0;
    double sampleRate = 0.0;

    // ── 设备无关数据（全曲线帧域）───────────────────────────
    /// [帧] sourceToTimeline(timeAtFrame(f))——绘制期 X 重映射的源
    /// （coords.timeToXExact 是其纯函数，逐位等于直算的 sourceToXExact 复合链）。
    std::vector<double> timelineSeconds;
};

/// 组件侧小容量缓存仓库：按键线性查找（placement 个数为个位数），
/// 每个绘制 pass 末尾 prune 掉未命中条目——帧轴/网格/投影变更后旧条目
/// 自然淘汰，无定时器、无过期策略。
class F0VisualCacheStore
{
public:
    /// 命中返回缓存条目；键未命中则按 owner 快照的帧轴构建并入库。
    /// 单一入口：空安全判定在此一次完成（曲线为空/无帧/网格为空 → nullptr，
    /// 调用方走直算路径，直算侧自身的空判断保证不绘制）。
    const F0VisualCacheEntry* resolve(
        std::shared_ptr<const EditableContentSnapshot> ownerSnapshot,
        std::shared_ptr<const EditableContentSnapshot> committedSnapshot,
        const ContentTimelineProjection& projection);

    /// 清除本轮未命中的条目。
    void prune();

private:
    struct Stored
    {
        F0VisualCacheEntry entry;
        bool used = false;
    };
    std::vector<Stored> entries_;
};

} // namespace OpenTune
