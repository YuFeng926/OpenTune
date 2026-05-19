/**
 * TimeGridEditAction — Undo/Redo 用的 TimeGrid 编辑事务。
 *
 * 与 PianoRollEditAction 严格平级实现，遵循 cross-cutting/undo-affected-range-invariant.md
 * 中描述的同一不变量: affected source range 由 ToolHandler 编辑时计算并显式传入,
 * **不得**从 before/after snapshot diff 反推.
 *
 * v2.0 a122bca 重构曾在 PianoRollEditAction 上无声丢失 affected-range 优化;
 * 此处需严格沿 ToolHandler 计算 → commit 透传 → action 构造的链路。
 */
#pragma once

#include "UndoManager.h"
#include "TimeGrid.h"
#include <cstdint>
#include <memory>

namespace OpenTune {

class OpenTuneAudioProcessor;

class TimeGridEditAction : public UndoAction {
public:
    /**
     * @param processor                 Processor 引用; undo/redo 通过它切换 TimeGridSnapshot
     * @param materializationId         目标 materialization
     * @param description               用户可见的 undo 描述 ("Drag handle"/"Insert handle"/...)
     * @param oldSnapshot               编辑前 snapshot (undo 时切回这个)
     * @param newSnapshot               编辑后 snapshot (redo 时切回这个)
     * @param affectedSrcStartFrame     受影响源帧范围起 (UI 计算, 不得反推)
     * @param affectedSrcEndFrame       受影响源帧范围终 (inclusive)
     */
    TimeGridEditAction(OpenTuneAudioProcessor& processor,
                       uint64_t materializationId,
                       juce::String description,
                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                       std::shared_ptr<const TimeGridSnapshot> newSnapshot,
                       int64_t affectedSrcStartFrame,
                       int64_t affectedSrcEndFrame);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    uint64_t getMaterializationId() const noexcept { return materializationId_; }
    int64_t  getAffectedSrcStartFrame() const noexcept { return affectedSrcStartFrame_; }
    int64_t  getAffectedSrcEndFrame() const noexcept { return affectedSrcEndFrame_; }

private:
    OpenTuneAudioProcessor& processor_;
    uint64_t                materializationId_;
    juce::String            description_;
    std::shared_ptr<const TimeGridSnapshot> oldSnapshot_;
    std::shared_ptr<const TimeGridSnapshot> newSnapshot_;
    int64_t                 affectedSrcStartFrame_{0};
    int64_t                 affectedSrcEndFrame_{0};
};

} // namespace OpenTune
