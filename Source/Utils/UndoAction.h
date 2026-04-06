#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <functional>
#include "Note.h"
#include "ClipSnapshot.h"
#include "../Utils/PitchCurve.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
class RenderCache;

class UndoAction
{
public:
    virtual ~UndoAction() = default;

    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
    virtual uint64_t getClipId() const = 0;
};

class CompoundUndoAction : public UndoAction
{
public:
    explicit CompoundUndoAction(const juce::String& description = "Compound Action")
        : description_(description)
    {
    }

    void addAction(std::unique_ptr<UndoAction> action)
    {
        actions_.push_back(std::move(action));
    }

    void undo() override
    {
        for (int i = static_cast<int>(actions_.size()) - 1; i >= 0; --i) {
            actions_[static_cast<size_t>(i)]->undo();
        }
    }

    void redo() override
    {
        for (auto& action : actions_) {
            action->redo();
        }
    }

    juce::String getDescription() const override
    {
        return description_;
    }

    uint64_t getClipId() const override
    {
        if (actions_.empty()) return 0;
        return actions_.front()->getClipId();
    }

    bool isEmpty() const { return actions_.empty(); }

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    juce::String description_;
};

class NotesChangeAction : public UndoAction
{
public:
    NotesChangeAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId,
                      const std::vector<Note>& oldNotes, 
                      const std::vector<Note>& newNotes,
                      const juce::String& description = "Edit Notes")
        : processor_(processor),
          trackId_(trackId),
          clipId_(clipId),
          oldNotes_(oldNotes), 
          newNotes_(newNotes),
          description_(description.isEmpty() ? "Edit Notes" : description)
    {
    }

    void undo() override;
    void redo() override;

    juce::String getDescription() const override
    {
        return description_;
    }

    uint64_t getClipId() const override { return clipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    std::vector<Note> oldNotes_;
    std::vector<Note> newNotes_;
    juce::String description_;
};

class CorrectedSegmentsChangeAction : public UndoAction
{
public:
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

    struct ChangeInfo {
        int affectedStartFrame;
        int affectedEndFrame;
        juce::String description;
    };

    CorrectedSegmentsChangeAction(
        uint64_t clipId,
        const std::vector<SegmentSnapshot>& oldSegments,
        const std::vector<SegmentSnapshot>& newSegments,
        std::function<void(const std::vector<SegmentSnapshot>&)> applier,
        const ChangeInfo& info)
        : clipId_(clipId),
          oldSegments_(oldSegments),
          newSegments_(newSegments),
          applier_(std::move(applier)),
          info_(info)
    {
    }

    static std::vector<SegmentSnapshot> captureSegments(const std::shared_ptr<PitchCurve>& curve);
    static bool snapshotsEquivalent(const std::vector<SegmentSnapshot>& a,
                                    const std::vector<SegmentSnapshot>& b,
                                    float epsilon = 1e-6f);
    static std::function<void(const std::vector<SegmentSnapshot>&)> makeCurveApplier(
        const std::shared_ptr<PitchCurve>& curve);
    static std::unique_ptr<CorrectedSegmentsChangeAction> createForCurve(
        uint64_t clipId,
        const std::vector<SegmentSnapshot>& oldSegments,
        const std::vector<SegmentSnapshot>& newSegments,
        const std::shared_ptr<PitchCurve>& curve,
        int affectedStartFrame,
        int affectedEndFrame,
        const juce::String& description);

    void undo() override
    {
        if (applier_) {
            applier_(oldSegments_);
        }
        lastAffectedStartFrame_ = info_.affectedStartFrame;
        lastAffectedEndFrame_ = info_.affectedEndFrame;
    }

    void redo() override
    {
        if (applier_) {
            applier_(newSegments_);
        }
        lastAffectedStartFrame_ = info_.affectedStartFrame;
        lastAffectedEndFrame_ = info_.affectedEndFrame;
    }

    juce::String getDescription() const override
    {
        return info_.description.isEmpty() ? "Edit F0 Curve" : info_.description;
    }

    uint64_t getClipId() const override { return clipId_; }

    /** After undo/redo, retrieve the affected frame range of the last executed action. */
    static int getLastAffectedStartFrame() { return lastAffectedStartFrame_; }
    static int getLastAffectedEndFrame() { return lastAffectedEndFrame_; }
    static void resetLastAffectedRange() { lastAffectedStartFrame_ = -1; lastAffectedEndFrame_ = -1; }

private:
    uint64_t clipId_;
    std::vector<SegmentSnapshot> oldSegments_;
    std::vector<SegmentSnapshot> newSegments_;
    std::function<void(const std::vector<SegmentSnapshot>&)> applier_;
    ChangeInfo info_;

    static inline int lastAffectedStartFrame_ = -1;
    static inline int lastAffectedEndFrame_ = -1;
};

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

    uint64_t getClipId() const override { return originalClipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t originalClipId_;
    SplitResult result_;
    int originalClipIndex_;
    int newClipIndex_;
};

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

    uint64_t getClipId() const override { return clipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    float oldGain_;
    float newGain_;
};

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

    uint64_t getClipId() const override { return clipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    double oldStartSeconds_;
    double newStartSeconds_;
};

class ClipDeleteAction : public UndoAction
{
public:
    ClipDeleteAction(OpenTuneAudioProcessor& processor, int trackId, uint64_t clipId,
                     int deletedIndex, ClipSnapshot snapshot)
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

    uint64_t getClipId() const override { return clipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    int deletedIndex_;
    ClipSnapshot snapshot_;
};

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

    uint64_t getClipId() const override { return clipId_; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t clipId_;
    ClipSnapshot snapshot_;
    bool hasSnapshot_{false};
};

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

    uint64_t getClipId() const override { return 0; }

private:
    int oldRootNote_;
    int oldScaleType_;
    int newRootNote_;
    int newScaleType_;
    std::function<void(int, int)> applier_;
};

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

    uint64_t getClipId() const override { return clipId_; }

private:
    int trackId_;
    uint64_t clipId_;
    int oldRootNote_;
    int oldScaleType_;
    int newRootNote_;
    int newScaleType_;
    std::function<void(int, uint64_t, int, int)> applier_;
};

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

    uint64_t getClipId() const override { return 0; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    bool oldMuted_;
    bool newMuted_;
    std::function<void(int, bool)> uiUpdater_;
};

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

    uint64_t getClipId() const override { return 0; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    bool oldSolo_;
    bool newSolo_;
    std::function<void(int, bool)> uiUpdater_;
};

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

    uint64_t getClipId() const override { return 0; }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    float oldVolume_;
    float newVolume_;
    std::function<void(int, float)> uiUpdater_;
};

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

    uint64_t getClipId() const override { return clipId_; }

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
    explicit UndoManager(int maxHistorySize = 100)
        : maxHistorySize_(maxHistorySize)
    {
    }

    void addAction(std::unique_ptr<UndoAction> action)
    {
        if (!actions_.empty() && currentIndex_ >= 0)
        {
            const auto* last = actions_[static_cast<size_t>(currentIndex_)].get();
            const bool lastIsScaleChange = (dynamic_cast<const ClipScaleKeyChangeAction*>(last) != nullptr)
                || (dynamic_cast<const ScaleKeyChangeAction*>(last) != nullptr);

            const juce::String newDesc = action->getDescription();
            const bool isAutoTuneAction = newDesc.containsIgnoreCase("Auto Tune");

            if (lastIsScaleChange && isAutoTuneAction)
            {
                auto compound = std::make_unique<CompoundUndoAction>("Scale + Auto Tune");
                compound->addAction(std::move(actions_[static_cast<size_t>(currentIndex_)]));
                actions_.resize(static_cast<size_t>(currentIndex_));
                --currentIndex_;
                compound->addAction(std::move(action));
                action = std::move(compound);
            }
        }

        actions_.resize(static_cast<size_t>(currentIndex_ + 1));
        actions_.push_back(std::move(action));
        ++currentIndex_;

        while (actions_.size() > static_cast<size_t>(maxHistorySize_))
        {
            actions_.erase(actions_.begin());
            --currentIndex_;
        }
    }

    bool canUndo() const
    {
        return currentIndex_ >= 0;
    }

    bool canRedo() const
    {
        return currentIndex_ < static_cast<int>(actions_.size()) - 1;
    }

    void undo()
    {
        if (currentIndex_ >= 0)
        {
            actions_[static_cast<size_t>(currentIndex_)]->undo();
            --currentIndex_;
        }
    }

    void redo()
    {
        if (currentIndex_ < static_cast<int>(actions_.size()) - 1)
        {
            ++currentIndex_;
            actions_[static_cast<size_t>(currentIndex_)]->redo();
        }
    }

    void clear()
    {
        actions_.clear();
        currentIndex_ = -1;
    }

    juce::String getUndoDescription() const
    {
        if (currentIndex_ >= 0)
        {
            return actions_[static_cast<size_t>(currentIndex_)]->getDescription();
        }
        return juce::String();
    }

    juce::String getRedoDescription() const
    {
        if (currentIndex_ < static_cast<int>(actions_.size()) - 1)
        {
            return actions_[static_cast<size_t>(currentIndex_ + 1)]->getDescription();
        }
        return juce::String();
    }

    int getHistorySize() const
    {
        return static_cast<int>(actions_.size());
    }

private:
    std::vector<std::unique_ptr<UndoAction>> actions_;
    int currentIndex_{-1};
    int maxHistorySize_;
};

} // namespace OpenTune
