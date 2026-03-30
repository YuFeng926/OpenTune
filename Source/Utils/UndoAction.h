#pragma once

/**
 * 撤销/重做动作定义
 * 
 * 定义 OpenTune 的 Undo/Redo 框架，包括：
 * - UndoAction: 撤销动作基类
 * - CompoundUndoAction: 复合动作（多个子动作打包为原子操作）
 * - NotesChangeAction: 音符编辑动作
 * - CorrectedSegmentsChangeAction: CorrectedF0 编辑动作
 * - ParameterChangeAction: 参数旋钮变化动作
 * - ClipMoveAction/ClipDeleteAction/ClipSplitAction: Clip 操作动作
 * - ScaleKeyChangeAction: 音阶/调式选择动作
 * - TrackMuteAction/TrackSoloAction/TrackVolumeAction: 轨道状态动作
 * 
 * 全局 UndoManager 位于 PluginProcessor::globalUndoManager_，容量 500 步。
 * 所有视图共享同一个 Undo 历史。
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <functional>
#include "Note.h"
#include "../Utils/PitchCurve.h"

namespace OpenTune {

// 前向声明，避免循环依赖
class OpenTuneAudioProcessor;
class RenderCache;
enum class Key;
enum class Scale;

/**
 * UndoAction - 撤销动作基类
 * 
 * 所有可撤销的操作都必须继承此类，实现 undo() 和 redo() 方法。
 */
class UndoAction
{
public:
    virtual ~UndoAction() = default;

    /** 执行撤销操作 */
    virtual void undo() = 0;
    
    /** 执行重做操作 */
    virtual void redo() = 0;
    
    /** 获取动作描述（用于 UI 显示） */
    virtual juce::String getDescription() const = 0;
};

/**
 * CompoundUndoAction - 复合撤销动作
 * 
 * 将多个子 Action 打包为一个原子操作。
 * 一次 Undo 全部回退（逆序），一次 Redo 全部重做（正序）。
 */
class CompoundUndoAction : public UndoAction
{
public:
    CompoundUndoAction(const juce::String& description = "Compound Action")
        : description_(description)
    {
    }

    /** 添加子动作到复合动作中 */
    void addAction(std::unique_ptr<UndoAction> action)
    {
        actions_.push_back(std::move(action));
    }

    void undo() override
    {
        // 逆序撤销所有子动作
        for (int i = static_cast<int>(actions_.size()) - 1; i >= 0; --i) {
            actions_[static_cast<size_t>(i)]->undo();
        }
    }

    void redo() override
    {
        // 正序重做所有子动作
        for (auto& action : actions_) {
            action->redo();
        }
    }

    juce::String getDescription() const override
    {
        return description_;
    }

    /** 检查是否为空（无子动作） */
    bool isEmpty() const { return actions_.empty(); }

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    juce::String description_;
};

/**
 * NotesChangeAction - 音符编辑撤销动作
 * 
 * 记录音符编辑前后的状态，支持创建/删除/移动/调整大小等操作。
 * 使用回调函数方式恢复音符，避免引用失效问题。
 */
class NotesChangeAction : public UndoAction
{
public:
    NotesChangeAction(const std::vector<Note>& oldNotes, 
                      const std::vector<Note>& newNotes,
                      std::function<void(const std::vector<Note>&)> applier,
                      const juce::String& description = "Edit Notes")
        : oldNotes_(oldNotes), newNotes_(newNotes),
          applier_(std::move(applier)),
          description_(description.isEmpty() ? "Edit Notes" : description)
    {
    }

    // 兼容旧版引用方式构造函数（已废弃，仅用于向后兼容）
    NotesChangeAction(std::vector<Note>& targetNotes, const std::vector<Note>& oldNotes, 
                      const std::vector<Note>& newNotes, const juce::String& description = "Edit Notes")
        : targetNotes_(&targetNotes), oldNotes_(oldNotes), newNotes_(newNotes), 
          description_(description.isEmpty() ? "Edit Notes" : description)
    {
    }

    void undo() override
    {
        if (applier_) {
            applier_(oldNotes_);
        } else if (targetNotes_) {
            *targetNotes_ = oldNotes_;
        }
    }

    void redo() override
    {
        if (applier_) {
            applier_(newNotes_);
        } else if (targetNotes_) {
            *targetNotes_ = newNotes_;
        }
    }

    juce::String getDescription() const override
    {
        return description_;
    }

private:
    std::vector<Note>* targetNotes_ = nullptr;  // 指针替代引用，可为空
    std::vector<Note> oldNotes_;
    std::vector<Note> newNotes_;
    std::function<void(const std::vector<Note>&)> applier_;
    juce::String description_;
};

// CorrectedF0 编辑（稀疏区间）撤销动作
// 保存编辑前后的 correctedSegments 完整快照，支持任意 F0 编辑操作的撤销/重做
// 使用回调方式恢复状态，避免引用失效导致崩溃
class CorrectedSegmentsChangeAction : public UndoAction
{
public:
    // 存储单个片段的快照
    struct SegmentSnapshot {
        int startFrame;
        int endFrame;
        std::vector<float> f0Data;
        CorrectedSegment::Source source;
        float retuneSpeed;
        float vibratoDepth;
        float vibratoRate;

        SegmentSnapshot(const CorrectedSegment& seg)
            : startFrame(seg.startFrame),
              endFrame(seg.endFrame),
              f0Data(seg.f0Data),
              source(seg.source),
              retuneSpeed(seg.retuneSpeed),
              vibratoDepth(seg.vibratoDepth),
              vibratoRate(seg.vibratoRate) {}
    };

    // 描述本次变更的元信息
    struct ChangeInfo {
        int affectedStartFrame;
        int affectedEndFrame;
        juce::String description;
    };

    // 回调方式构造函数
    CorrectedSegmentsChangeAction(
        const std::vector<SegmentSnapshot>& oldSegments,
        const std::vector<SegmentSnapshot>& newSegments,
        std::function<void(const std::vector<SegmentSnapshot>&)> applier,
        const ChangeInfo& info)
        : oldSegments_(oldSegments),
          newSegments_(newSegments),
          applier_(std::move(applier)),
          info_(info)
    {
    }

    // 旧版引用方式构造函数（已废弃，仅用于向后兼容）
    CorrectedSegmentsChangeAction(
        std::vector<CorrectedSegment>& targetSegments,
        const std::vector<SegmentSnapshot>& oldSegments,
        const std::vector<SegmentSnapshot>& newSegments,
        const ChangeInfo& info)
        : targetSegments_(&targetSegments),
          oldSegments_(oldSegments),
          newSegments_(newSegments),
          info_(info)
    {
    }

    void undo() override
    {
        applySegments(oldSegments_);
    }

    void redo() override
    {
        applySegments(newSegments_);
    }

    juce::String getDescription() const override
    {
        return info_.description.isEmpty() ? "Edit F0 Curve" : info_.description;
    }

private:
    void applySegments(const std::vector<SegmentSnapshot>& snapshots)
    {
        if (applier_) {
            // 使用回调方式（安全）
            applier_(snapshots);
        } else if (targetSegments_) {
            // 使用引用方式
            targetSegments_->clear();
            targetSegments_->reserve(snapshots.size());
            for (const auto& snap : snapshots) {
                CorrectedSegment seg;
                seg.startFrame = snap.startFrame;
                seg.endFrame = snap.endFrame;
                seg.f0Data = snap.f0Data;
                seg.source = snap.source;
                seg.retuneSpeed = snap.retuneSpeed;
                seg.vibratoDepth = snap.vibratoDepth;
                seg.vibratoRate = snap.vibratoRate;
                targetSegments_->push_back(std::move(seg));
            }
        }
    }

    std::vector<CorrectedSegment>* targetSegments_ = nullptr;  // 指针替代引用，可为空
    std::vector<SegmentSnapshot> oldSegments_;
    std::vector<SegmentSnapshot> newSegments_;
    std::function<void(const std::vector<SegmentSnapshot>&)> applier_;  // 回调方式恢复
    ChangeInfo info_;
};

// Clip 分割撤销动作
class ClipSplitAction : public UndoAction
{
public:
    struct SplitResult {
        uint64_t newClipId;
        double splitSeconds;
        double originalClipDuration;
    };

    ClipSplitAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t originalClipId,
                    const SplitResult& result, int originalClipIndex, int newClipIndex)
        : processor_(processor),
          trackId_(trackId),
          originalClipId_(originalClipId),
          result_(result),
          originalClipIndex_(originalClipIndex),
          newClipIndex_(newClipIndex)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Split Clip";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t originalClipId_;
    SplitResult result_;
    int originalClipIndex_;
    int newClipIndex_;
};

// Clip 增益调整撤销动作
class ClipGainChangeAction : public UndoAction
{
public:
    ClipGainChangeAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId,
                         float oldGain, float newGain)
        : processor_(processor),
          trackId_(trackId),
          clipId_(clipId),
          oldGain_(oldGain),
          newGain_(newGain)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Change Clip Gain";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    float oldGain_;
    float newGain_;
};

// Clip 移动撤销动作
class ClipMoveAction : public UndoAction
{
public:
    ClipMoveAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId,
                   double oldStartSeconds, double newStartSeconds)
        : processor_(processor),
          trackId_(trackId),
          clipId_(clipId),
          oldStartSeconds_(oldStartSeconds),
          newStartSeconds_(newStartSeconds)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Move Clip";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_{0};
    uint64_t clipId_{0};
    double oldStartSeconds_{0.0};
    double newStartSeconds_{0.0};
};

// Clip 删除撤销动作
// 注意：ClipSnapshot 定义在 PluginProcessor.h 中，此处使用前向声明
// 实现放在 .cpp 文件中以避免循环依赖
class ClipDeleteAction : public UndoAction
{
public:
    struct ClipSnapshotData {
        juce::AudioBuffer<float> audioBuffer;
        double startSeconds{0.0};
        float gain{1.0f};
        double fadeInDuration{0.0};
        double fadeOutDuration{0.0};
        juce::String name;
        juce::Colour colour;
        std::shared_ptr<PitchCurve> pitchCurve;
        int originalF0State{0};
        int detectedRootNote{0};
        int detectedScaleType{0};
        std::shared_ptr<RenderCache> renderCache;
    };

    ClipDeleteAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId,
                     int deletedIndex, ClipSnapshotData snapshot)
        : processor_(processor),
          trackId_(trackId),
          clipId_(clipId),
          deletedIndex_(deletedIndex),
          snapshot_(std::move(snapshot))
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Delete Clip";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_{0};
    uint64_t clipId_{0};
    int deletedIndex_{0};
    ClipSnapshotData snapshot_;
};

// 参数变化撤销动作（Retune Speed / Vibrato Depth / Vibrato Rate / Note Split）
class ParameterChangeAction : public UndoAction
{
public:
    // 参数类型枚举
    enum class ParamType {
        RetuneSpeed,
        VibratoDepth,
        VibratoRate,
        NoteSplit
    };

    ParameterChangeAction(ParamType type, float oldValue, float newValue,
                          std::function<void(float)> applier)
        : type_(type),
          oldValue_(oldValue),
          newValue_(newValue),
          applier_(std::move(applier))
    {
    }

    void undo() override
    {
        if (applier_) applier_(oldValue_);
    }

    void redo() override
    {
        if (applier_) applier_(newValue_);
    }

    juce::String getDescription() const override
    {
        switch (type_) {
            case ParamType::RetuneSpeed:  return "Change Retune Speed";
            case ParamType::VibratoDepth: return "Change Vibrato Depth";
            case ParamType::VibratoRate:  return "Change Vibrato Rate";
            case ParamType::NoteSplit:    return "Change Note Split";
        }
        return "Change Parameter";
    }

private:
    ParamType type_;
    float oldValue_;
    float newValue_;
    std::function<void(float)> applier_;
};

// Clip 创建撤销动作（音频导入）
class ClipCreateAction : public UndoAction
{
public:
    ClipCreateAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId)
        : processor_(processor),
          trackId_(trackId),
          clipId_(clipId)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Import Audio";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    // undo 时保存被移除的 clip 快照，用于 redo 恢复
    ClipDeleteAction::ClipSnapshotData snapshot_;
    bool hasSnapshot_{false};
};

// Scale/Key 选择撤销动作
class ScaleKeyChangeAction : public UndoAction
{
public:
    ScaleKeyChangeAction(int oldRootNote, int oldScaleType,
                         int newRootNote, int newScaleType,
                         std::function<void(int, int)> applier)
        : oldRootNote_(oldRootNote), oldScaleType_(oldScaleType),
          newRootNote_(newRootNote), newScaleType_(newScaleType),
          applier_(std::move(applier))
    {
    }

    void undo() override
    {
        if (applier_) applier_(oldRootNote_, oldScaleType_);
    }

    void redo() override
    {
        if (applier_) applier_(newRootNote_, newScaleType_);
    }

    juce::String getDescription() const override
    {
        return "Change Scale/Key";
    }

private:
    int oldRootNote_;
    int oldScaleType_;
    int newRootNote_;
    int newScaleType_;
    std::function<void(int, int)> applier_;
};

// Clip 感知的 Scale/Key 选择撤销动作
class ClipScaleKeyChangeAction : public UndoAction
{
public:
    ClipScaleKeyChangeAction(int trackId,
                             uint64_t clipId,
                             int oldRootNote,
                             int oldScaleType,
                             int newRootNote,
                             int newScaleType,
                             std::function<void(int, uint64_t, int, int)> applier)
        : trackId_(trackId),
          clipId_(clipId),
          oldRootNote_(oldRootNote),
          oldScaleType_(oldScaleType),
          newRootNote_(newRootNote),
          newScaleType_(newScaleType),
          applier_(std::move(applier))
    {
    }

    void undo() override
    {
        if (applier_) applier_(trackId_, clipId_, oldRootNote_, oldScaleType_);
    }

    void redo() override
    {
        if (applier_) applier_(trackId_, clipId_, newRootNote_, newScaleType_);
    }

    juce::String getDescription() const override
    {
        return "Change Clip Scale/Key";
    }

private:
    int trackId_;
    uint64_t clipId_;
    int oldRootNote_;
    int oldScaleType_;
    int newRootNote_;
    int newScaleType_;
    std::function<void(int, uint64_t, int, int)> applier_;
};

// Track 静音撤销动作
class TrackMuteAction : public UndoAction
{
public:
    TrackMuteAction(OpenTuneAudioProcessor& processor, int trackId,
                    bool oldMuted, bool newMuted,
                    std::function<void(int, bool)> uiUpdater = nullptr)
        : processor_(processor), trackId_(trackId),
          oldMuted_(oldMuted), newMuted_(newMuted),
          uiUpdater_(std::move(uiUpdater))
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Toggle Track Mute";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    bool oldMuted_;
    bool newMuted_;
    std::function<void(int, bool)> uiUpdater_;
};

// Track Solo 撤销动作
class TrackSoloAction : public UndoAction
{
public:
    TrackSoloAction(OpenTuneAudioProcessor& processor, int trackId,
                    bool oldSolo, bool newSolo,
                    std::function<void(int, bool)> uiUpdater = nullptr)
        : processor_(processor), trackId_(trackId),
          oldSolo_(oldSolo), newSolo_(newSolo),
          uiUpdater_(std::move(uiUpdater))
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Toggle Track Solo";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    bool oldSolo_;
    bool newSolo_;
    std::function<void(int, bool)> uiUpdater_;
};

// Track 音量撤销动作
class TrackVolumeAction : public UndoAction
{
public:
    TrackVolumeAction(OpenTuneAudioProcessor& processor, int trackId,
                      float oldVolume, float newVolume,
                      std::function<void(int, float)> uiUpdater = nullptr)
        : processor_(processor), trackId_(trackId),
          oldVolume_(oldVolume), newVolume_(newVolume),
          uiUpdater_(std::move(uiUpdater))
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Change Track Volume";
    }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    float oldVolume_;
    float newVolume_;
    std::function<void(int, float)> uiUpdater_;
};

// Clip 跨轨道移动撤销动作
class ClipCrossTrackMoveAction : public UndoAction
{
public:
    ClipCrossTrackMoveAction(OpenTuneAudioProcessor& processor,
                              int sourceTrackId, int targetTrackId,
                              uint64_t clipId,
                              double oldStartSeconds, double newStartSeconds)
        : processor_(processor),
          sourceTrackId_(sourceTrackId),
          targetTrackId_(targetTrackId),
          clipId_(clipId),
          oldStartSeconds_(oldStartSeconds),
          newStartSeconds_(newStartSeconds)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return "Move Clip to Track " + juce::String(targetTrackId_ + 1);
    }

private:
    OpenTuneAudioProcessor& processor_;
    int sourceTrackId_;
    int targetTrackId_;
    uint64_t clipId_;
    double oldStartSeconds_;
    double newStartSeconds_;
};

class UndoManager
{
public:
    UndoManager(int maxHistorySize = 100)
        : maxHistorySize_(maxHistorySize), currentIndex_(-1)
    {
    }

    void addAction(std::unique_ptr<UndoAction> action)
    {
        if (currentIndex_ < static_cast<int>(undoStack_.size()) - 1)
        {
            undoStack_.erase(undoStack_.begin() + currentIndex_ + 1, undoStack_.end());
        }

        // 组合事务优化：若连续出现“改调式 -> Auto Tune”，自动合并为一个原子操作。
        // 这样一次 Undo/Redo 可同时回放 Scale + Notes/CorrectedSegments。
        if (!undoStack_.empty() && currentIndex_ == static_cast<int>(undoStack_.size()) - 1 && action)
        {
            const auto* last = undoStack_.back().get();
            const bool lastIsScaleChange = (dynamic_cast<const ClipScaleKeyChangeAction*>(last) != nullptr)
                || (dynamic_cast<const ScaleKeyChangeAction*>(last) != nullptr);

            const juce::String newDesc = action->getDescription();
            const bool isAutoTuneAction = newDesc.containsIgnoreCase("Auto Tune");

            if (lastIsScaleChange && isAutoTuneAction)
            {
                auto compound = std::make_unique<CompoundUndoAction>("Scale + Auto Tune");
                compound->addAction(std::move(undoStack_.back()));
                undoStack_.pop_back();
                --currentIndex_;
                compound->addAction(std::move(action));
                action = std::move(compound);
            }
        }

        undoStack_.push_back(std::move(action));
        currentIndex_ = static_cast<int>(undoStack_.size()) - 1;

        while (undoStack_.size() > static_cast<size_t>(maxHistorySize_))
        {
            undoStack_.erase(undoStack_.begin());
            --currentIndex_;
        }
    }

    bool canUndo() const
    {
        return currentIndex_ >= 0;
    }

    bool canRedo() const
    {
        return currentIndex_ < static_cast<int>(undoStack_.size()) - 1;
    }

    void undo()
    {
        if (canUndo())
        {
            undoStack_[currentIndex_]->undo();
            --currentIndex_;
        }
    }

    void redo()
    {
        if (canRedo())
        {
            ++currentIndex_;
            undoStack_[currentIndex_]->redo();
        }
    }

    void clear()
    {
        undoStack_.clear();
        currentIndex_ = -1;
    }

    juce::String getUndoDescription() const
    {
        if (canUndo())
        {
            return undoStack_[currentIndex_]->getDescription();
        }
        return juce::String();
    }

    juce::String getRedoDescription() const
    {
        if (canRedo())
        {
            return undoStack_[currentIndex_ + 1]->getDescription();
        }
        return juce::String();
    }

    int getHistorySize() const
    {
        return static_cast<int>(undoStack_.size());
    }

private:
    std::vector<std::unique_ptr<UndoAction>> undoStack_;
    int currentIndex_;
    int maxHistorySize_;
};

} // namespace OpenTune
