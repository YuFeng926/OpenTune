#pragma once

#include "UndoManager.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include "OutputGainEnvelope.h"
#include <memory>

namespace OpenTune {

// A 层增益 undo action：保存受影响 notes 的 before/after 完整 patch，
// undo/redo 直接应用（不反算 dB），只走 republish 语义，零 render enqueue。
class OutputGainEditAction : public UndoAction {
public:
    OutputGainEditAction(std::shared_ptr<ContentEditCommands> commands,
                         ContentKey key,
                         juce::String description,
                         ContentNoteRangePatch beforePatch,
                         ContentNoteRangePatch afterPatch);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    ContentNoteRangePatch beforePatch_, afterPatch_;
};

// B 层 envelope undo action：保存 B 的 before/after 完整状态，
// undo/redo 一次替换 B 层，只走 republish 语义，零 render enqueue。
class SibilantEnvelopeEditAction : public UndoAction {
public:
    SibilantEnvelopeEditAction(std::shared_ptr<ContentEditCommands> commands,
                               ContentKey key,
                               juce::String description,
                               SibilantGainEnvelope beforeEnvelope,
                               SibilantGainEnvelope afterEnvelope);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    SibilantGainEnvelope beforeEnvelope_, afterEnvelope_;
};

} // namespace OpenTune
