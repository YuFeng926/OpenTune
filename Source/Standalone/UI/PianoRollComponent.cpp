#include "PianoRollComponent.h"
#include "PianoRollNoteEntry.h"
#include "../../Content/EditableContentSnapshot.h"
#include "../../Utils/AppPreferences.h"
#include "../../Utils/PendingSeekPolicy.h"
#include "../../Utils/MagnifyZoom.h"
#include "../../Utils/WheelPan.h"
#include "../../Utils/PlayHeadState.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/VolumeEnvelopeRegionEdit.h"
#include "../../Utils/CursorTheme.h"
#include "../../Utils/TouchInput.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include "../Utils/ZoomSensitivityConfig.h"
#include "UiAssets.h"
#include "UiText.h"
#include "ToolbarIcons.h"
#include "../../Utils/AudioEditingScheme.h"
#include "../../Utils/AutomationLane.h"
#include "../../Utils/ScaleUiMapping.h"
#include "../../Utils/PitchUtils.h"
#include "Utils/PianoKeyAudition.h"
#include "Utils/NoteAudition.h"
#include "Utils/NoteSpeech.h"
#include "Utils/ScreenReaderAnnouncer.h"
#include "Utils/TimeCoordinate.h"
#include "Inference/RenderCache.h"
#include "TimelineViewportPolicy.h"
#include "TimelineLayerComposer.h"
#include "PianoRoll/PianoRollTimeMap.h"
#include "../../PluginProcessor.h"
namespace OpenTune {

namespace {

/// OpenDyne：双击滚动条 → 缩放到全部音符
class FitToAllNotesOnDoubleClick : public juce::MouseListener
{
public:
    explicit FitToAllNotesOnDoubleClick(PianoRollComponent& owner) : owner_(owner) {}
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (owner_.isOpenDyne())
            owner_.fitToAllNotes();
    }
private:
    PianoRollComponent& owner_;
};

} // namespace

// Per-note UIA child. It is a transparent component so that AccessibilityHandler's
// component hierarchy exposes each note as a list item when accessibility mode is on.
class PianoRollComponent::NoteAccessibilityItem final : public juce::Component
{
public:
    NoteAccessibilityItem(PianoRollComponent& owner, int noteIndex)
        : owner_(owner), noteIndex_(noteIndex)
    {
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(false);
        setMouseClickGrabsKeyboardFocus(false);
    }

    void setNoteIndex(int noteIndex) noexcept { noteIndex_ = noteIndex; }

    juce::String accessibilityTitle() const
    {
        const auto& notes = owner_.getCommittedNotes();
        return owner_.describeKeyboardNote(notes, noteIndex_, PianoRollComponent::NoteDetail::Full);
    }

    bool isSelected() const
    {
        const auto& selected = owner_.interactionState_.noteSelection.selectedIndices;
        return std::find(selected.begin(), selected.end(), noteIndex_) != selected.end();
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        class Handler final : public juce::AccessibilityHandler
        {
        public:
            explicit Handler(NoteAccessibilityItem& item)
                : juce::AccessibilityHandler(item, juce::AccessibilityRole::listItem), item_(item) {}

            juce::String getTitle() const override { return item_.accessibilityTitle(); }

            juce::AccessibleState getCurrentState() const override
            {
                auto state = juce::AccessibilityHandler::getCurrentState().withSelectable();
                return item_.isSelected() ? state.withSelected() : state;
            }

        private:
            NoteAccessibilityItem& item_;
        };

        return std::make_unique<Handler>(*this);
    }

private:
    PianoRollComponent& owner_;
    int noteIndex_ = -1;
};

void PianoRollComponent::initializeUIComponents() {
    setWantsKeyboardFocus(true);
    addAndMakeVisible(verticalScrollBar_);
    verticalScrollBar_.addListener(this);
    verticalScrollBar_.setAutoHide(false);
    // 滚动条像素由 owner paint 在原层位手动绘制（paintEntireComponent）；
    // 零 alpha 让 JUCE 跳过子组件自身的绘制，bounds/输入/listener 不变
    verticalScrollBar_.setAlpha(0.0f);

    // OpenDyne：双击滚动条 = 缩放到全部音符
    fitToAllNotesOnDoubleClick_ = std::make_unique<FitToAllNotesOnDoubleClick>(*this);
    verticalScrollBar_.addMouseListener(fitToAllNotesOnDoubleClick_.get(), false);

    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous ? LOC(kScrollContinuous) : LOC(kScrollPage));
    scrollModeToggleButton_.setFontHeight(11.0f);
    scrollModeToggleButton_.onClick = [this] {
        if (scrollMode_ == ScrollMode::Page) {
            setScrollMode(ScrollMode::Continuous);
            scrollModeToggleButton_.setButtonText(LOC(kScrollContinuous));
        } else {
            setScrollMode(ScrollMode::Page);
            scrollModeToggleButton_.setButtonText(LOC(kScrollPage));
        }
    };
    addAndMakeVisible(scrollModeToggleButton_);
    scrollModeToggleButton_.setTooltip(LOC(kTooltipScrollMode));

    // Time/Bars 切换按钮
    timeUnitToggleButton_.setFontHeight(11.0f);
    timeUnitToggleButton_.setButtonText(displayMode_ == TimelineDisplayMode::Time ? LOC(kTime) : "BPM");
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));
    timeUnitToggleButton_.onClick = [this] {
        const auto nextMode = (displayMode_ == TimelineDisplayMode::Time)
            ? TimelineDisplayMode::Bars
            : TimelineDisplayMode::Time;
        setTimelineDisplayMode(nextMode);
        listeners_.call([nextMode](Listener& l) { l.timelineDisplayModeChanged(nextMode); });
    };
    addAndMakeVisible(timeUnitToggleButton_);

    // 对数映射：等滑程 = 等倍率，与滚轮缩放手感一致。
    // 滑杆值直接使用实际缩放单位（水平 pps、垂直 pixelsPerSemitone），映射范围即钳制域。
    const juce::NormalisableRange<double> horizontalZoomRange(
        TimelineViewportPolicy::minPixelsPerSecond(TimelineViewportRequest::ViewKind::PianoRoll),
        TimelineViewportPolicy::maxPixelsPerSecond(TimelineViewportRequest::ViewKind::PianoRoll),
        [](double minPps, double maxPps, double pos) { return minPps * std::pow(maxPps / minPps, pos); },
        [](double minPps, double maxPps, double pps) { return std::log(pps / minPps) / std::log(maxPps / minPps); });
    horizontalZoomSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    horizontalZoomSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    horizontalZoomSlider_.setNormalisableRange(horizontalZoomRange);
    horizontalZoomSlider_.setScrollWheelEnabled(false);
    horizontalZoomSlider_.setMouseClickGrabsKeyboardFocus(false);
    horizontalZoomSlider_.setTitle(LOC(kHorizontalZoom));
    horizontalZoomSlider_.setTooltip(LOC(kHorizontalZoom));
    horizontalZoomSlider_.onValueChange = [this] { applyHorizontalZoomSlider(horizontalZoomSlider_.getValue()); };
    addAndMakeVisible(horizontalZoomSlider_);

    const juce::NormalisableRange<double> verticalZoomRange(
        kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
        [](double minPst, double maxPst, double pos) { return minPst * std::pow(maxPst / minPst, pos); },
        [](double minPst, double maxPst, double pst) { return std::log(pst / minPst) / std::log(maxPst / minPst); });
    verticalZoomSlider_.setSliderStyle(juce::Slider::LinearVertical);
    verticalZoomSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    verticalZoomSlider_.setNormalisableRange(verticalZoomRange);
    verticalZoomSlider_.setScrollWheelEnabled(false);
    verticalZoomSlider_.setMouseClickGrabsKeyboardFocus(false);
    verticalZoomSlider_.setTitle(LOC(kVerticalZoom));
    verticalZoomSlider_.setTooltip(LOC(kVerticalZoom));
    verticalZoomSlider_.onValueChange = [this] { applyVerticalZoomSlider(verticalZoomSlider_.getValue()); };
    addAndMakeVisible(verticalZoomSlider_);

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });

    scrollModeToggleButton_.toFront(false);
    timeUnitToggleButton_.toFront(false);
}



void PianoRollComponent::initializeRenderer() {
    renderer_ = std::make_unique<PianoRollRenderer>();
}

void PianoRollComponent::layoutZoomControls()
{
    constexpr int horizontalWidth = 120;
    constexpr int horizontalHeight = 24;
    constexpr int verticalWidth = 24;
    constexpr int verticalHeight = 56;
    constexpr int groupGap = 8;
    constexpr int cornerInset = 8;
    constexpr int totalHeight = verticalHeight + groupGap + horizontalHeight;

    const int left = juce::jmax(zoomControlsArea_.getX(), getTimelineContentStartX()) + cornerInset;
    const int right = juce::jmin(zoomControlsArea_.getRight(),
                                 getTimelineViewportBounds().getRight() - cornerInset);
    const int bottom = zoomControlsArea_.getBottom() - cornerInset;
    const int availableWidth = juce::jmax(0, right - left);
    const int availableHeight = bottom - zoomControlsArea_.getY();
    const bool visible = !zoomControlsArea_.isEmpty()
        && availableWidth >= horizontalWidth
        && availableHeight >= totalHeight;

    horizontalZoomSlider_.setVisible(visible);
    verticalZoomSlider_.setVisible(visible);
    if (!visible)
        return;

    const int horizontalY = bottom - horizontalHeight;
    const int verticalY = horizontalY - groupGap - verticalHeight;

    horizontalZoomSlider_.setBounds(left, horizontalY, horizontalWidth, horizontalHeight);
    verticalZoomSlider_.setBounds(left, verticalY, verticalWidth, verticalHeight);
}

void PianoRollComponent::applyHorizontalZoomSlider(double pps)
{
    beginZoomPreviewAt(getTimelineContentViewportWidth() / 2, pps / camera_.pixelsPerSecond);
}

void PianoRollComponent::applyVerticalZoomSlider(double pst)
{
    handleVerticalZoomAt(static_cast<float>(pst / pixelsPerSemitone_),
                         static_cast<float>(getTimelineContentViewportHeight()) * 0.5f);
}

void PianoRollComponent::syncZoomControls()
{
    horizontalZoomSlider_.setValue(camera_.pixelsPerSecond, juce::dontSendNotification);
    verticalZoomSlider_.setValue(pixelsPerSemitone_, juce::dontSendNotification);
}

void PianoRollComponent::handleVolumeEnvelopeRegionEditResult(
    const VolumeEnvelopeRegionEditResult& result)
{
    if (result.status == VolumeEnvelopeRegionEditStatus::StaleRevision) {
        closeEqPopup();
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
        }
        interactionState_.noteSelection.clear();
        parameterTargetDidChange();
    }

    const bool applied = result.status == VolumeEnvelopeRegionEditStatus::Applied
        && result.contentSnapshot != nullptr;
    if (applied) {
        cachedNotes_ = result.contentSnapshot->notes;
        interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
        lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
        parameterTargetDidChange();
    }

    // Drag preview was rasterized during mouseDrag. Rebuild after every result so
    // NoChange/Failed also remove it from the retained content surface.
    requestContentRedraw();
    if (applied) {
        listeners_.call([](Listener& listener) { listener.contentEdited(); });
    }
}

PianoRollToolHandler::Context PianoRollComponent::buildToolHandlerContext() {
    PianoRollToolHandler::Context toolCtx;
    toolCtx.getState = [this]() -> InteractionState& { return interactionState_; };

    toolCtx.getViewMapper = [this]() -> ViewMapper { return makeViewMapper(); };
    toolCtx.contentOriginY = rulerHeight_;

    toolCtx.getCommittedNotes = [this]() -> const std::vector<Note>& { return getCommittedNotes(); };
    toolCtx.getDisplayNotes = [this]() -> const std::vector<Note>& { return getDisplayedNotes(); };
    toolCtx.getNoteDraft = [this]() -> NoteInteractionDraft& { return getNoteDraft(); };
    toolCtx.beginNoteDraft = [this]() { beginNoteDraft(); };
    toolCtx.commitNoteDraft = [this]() { return commitNoteDraft(); };
    toolCtx.clearNoteDraft = [this]() { clearNoteDraft(); };
    toolCtx.commitDrawNote = [this](const DrawNoteEditInput& input,
                                    uint64_t expectedNotesRevision,
                                    juce::String description) {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return DrawNoteCommitResult{};
        return processor_->drawNote(
            editedContentKey_, input, expectedNotesRevision, std::move(description));
    };
    toolCtx.applyDrawNoteResult = [this](const DrawNoteCommitResult& result,
                                         const std::vector<Note>& beforeNotes,
                                         int selectedNoteIndex) {
        interactionState_.noteDraft.clear();
        pendingUndoDescription_ = {};

        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
            applyEditedContentCurve(result.contentSnapshot->pitchCurve);
        }

        if (result.status == DrawNoteCommitStatus::Applied) {
            interactionState_.noteSelection.clear();
            if (selectedNoteIndex >= 0) {
                interactionState_.noteSelection.setSingle(
                    selectedNoteIndex, static_cast<int>(cachedNotes_.size()));
            } else {
                interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
            }
            requestContentRedraw();
            if (result.pitchCurveEdited) {
                const int startFrame = result.affectedRange.startFrame;
                const int endFrame = result.affectedRange.endFrameExclusive - 1;
                listeners_.call([startFrame, endFrame](Listener& listener) {
                    listener.pitchCurveEdited(startFrame, endFrame);
                });
            } else {
                listeners_.call([](Listener& listener) { listener.contentEdited(); });
            }
            return;
        }

        if (result.status == DrawNoteCommitStatus::NoChange) {
            invalidateLiveNotes(beforeNotes, cachedNotes_);
            return;
        }

        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        interactionState_.noteSelection.clear();
        parameterTargetDidChange();
        if (result.status == DrawNoteCommitStatus::StaleRevision)
            closeEqPopup();
        requestContentRedraw();
    };
    toolCtx.commitNoteCorrectionDraft = [this](uint64_t expectedNotesRevision,
                                                const std::vector<Note>& notes,
                                                F0FrameRange editRange) {
        OpenTuneAudioProcessor::NoteDraftCommitResult result;
        if (processor_ != nullptr && editedContentKey_.isValid()) {
            NoteGeneratorParams params;
            params.retuneSpeed = currentRetuneSpeed_;
            params.vibratoDepth = currentVibratoDepth_;
            params.vibratoRate = currentVibratoRate_;
            result = processor_->commitNoteBasedCorrectionDraft(
                editedContentKey_, expectedNotesRevision, notes, editRange, params,
                pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : LOC(kEdit));
        }

        interactionState_.noteDraft.clear();
        interactionState_.noteDrag.previewSnapshot.reset();
        pendingUndoDescription_ = {};
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
            applyEditedContentCurve(result.contentSnapshot->pitchCurve);
        }

        using Status = OpenTuneAudioProcessor::NoteDraftCommitStatus;
        if (result.status == Status::StaleRevision) {
            parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
            invalidateEqPopup();
            interactionState_.noteSelection.clear();
            parameterTargetDidChange();
        } else if (result.status == Status::Applied || result.status == Status::NoChange) {
            interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
            parameterTargetDidChange();
        }

        requestContentRedraw();
        return result.status == Status::Applied || result.status == Status::NoChange;
    };
    toolCtx.commitNoteResize = [this](uint64_t expectedNotesRevision,
                                      int noteIndex,
                                      NoteResizeEdge edge,
                                      double clampedTime,
                                      double minDuration,
                                      const std::vector<PitchCorrectionSegment>& segments,
                                      F0FrameRange affectedRange,
                                      juce::String description) -> bool {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return false;

        const auto result = processor_->resizePianoRollNote(
            editedContentKey_, noteIndex, edge, clampedTime, minDuration,
            expectedNotesRevision, segments, affectedRange, std::move(description));
        pendingUndoDescription_ = {};
        if (result.status == OpenTuneAudioProcessor::NoteResizeCommitStatus::StaleRevision) {
            closeEqPopup();
            parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        }
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
            applyEditedContentCurve(result.contentSnapshot->pitchCurve);
        }

        if (result.status == OpenTuneAudioProcessor::NoteResizeCommitStatus::StaleRevision) {
            interactionState_.noteSelection.clear();
            parameterTargetDidChange();
        } else if (result.status == OpenTuneAudioProcessor::NoteResizeCommitStatus::Applied) {
            interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
            parameterTargetDidChange();
        }
        if (result.status != OpenTuneAudioProcessor::NoteResizeCommitStatus::Applied)
            interactionState_.noteDraft.clear();

        requestContentRedraw();
        return result.status == OpenTuneAudioProcessor::NoteResizeCommitStatus::Applied;
    };
    // ── OpenDyne 契约回调（Pitch/Scissors/Gain 域） ──
    // VolumeEnvelope 事务：Processor 消息线程读 snapshot → planner（revision 校验 +
    // baseline 区域 delta/reset）→ commitVolumeEnvelope + undo（固定 ContentKey）。
    // Applied 后按原 UI 行为刷新 notes 缓存 / dirty / rasterize / repaint / contentEdited；
    // NoChange 不提交不 undo；StaleRevision 刷新缓存/revision + 清选择。
    toolCtx.editVolumeEnvelopeRegions =
        [this](const std::vector<int>& selectedIndices, uint64_t expectedNotesRevision,
               float deltaGainDb, juce::String description) -> VolumeEnvelopeRegionEditResult {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return {};
        const auto result = processor_->editVolumeEnvelopeRegions(
            editedContentKey_, selectedIndices, expectedNotesRevision, deltaGainDb,
            std::move(description));
        handleVolumeEnvelopeRegionEditResult(result);
        return result;
    };
    toolCtx.resetVolumeEnvelopeRegion =
        [this](int noteIndex, uint64_t expectedNotesRevision,
               juce::String description) -> VolumeEnvelopeRegionEditResult {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return {};
        const auto result = processor_->resetVolumeEnvelopeRegion(
            editedContentKey_, noteIndex, expectedNotesRevision, std::move(description));
        handleVolumeEnvelopeRegionEditResult(result);
        return result;
    };
    // Scissors 事务：Processor 校验 revision + 全量替换 + republish + undo（固定
    // ContentKey）。失败/StaleRevision 时刷新 notes 缓存/revision，不推 undo。
    toolCtx.commitScissorsNotes = [this](const std::vector<Note>& afterNotes, juce::String undoDescription) {
        if (processor_ == nullptr || !editedContentKey_.isValid()) return false;
        const auto result = processor_->applyScissorsNotes(
            editedContentKey_, afterNotes, lastKnownNotesRevision_, std::move(undoDescription));
        if (result.status == ScissorsCommitStatus::Applied) {
            interactionState_.noteSelection.clear();
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            pendingUndoDescription_ = {};
            requestContentRedraw();
            listeners_.call([](Listener& listener) { listener.contentEdited(); });
            return true;
        }
        if (result.status == ScissorsCommitStatus::StaleRevision
            && result.contentSnapshot != nullptr) {
            // 旧 index 不再对应新 notes：关旧 EQ popup、清旧选区，避免旧 index 误编辑
            closeEqPopup();
            parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
            interactionState_.noteSelection.clear();
            interactionState_.noteDraft.clear();
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            parameterTargetDidChange();
            requestContentRedraw();
        }
        return false;
    };
    // Pitch 工具与 AUTO 同源的 scale snap：唯一映射入口 ScaleUiMapping
    toolCtx.getActiveScaleSnap = [this]() -> std::optional<ScaleSnapConfig> {
        return makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);
    };
    toolCtx.shouldSnapWholeNoteMoveToStandardPitch = [this]() {
        return appPreferences_ != nullptr
            && appPreferences_->getWholeNoteMoveSnapMode() == WholeNoteMoveSnapMode::StandardPitch;
    };
    toolCtx.getEditableContentSnapshot = [this]() { return readEditedSnapshot(); };
    toolCtx.planNoteValueEdit = [this](const std::vector<Note>& notes,
                                       const ContentNoteValueEditRequest& request) {
        return contentCommands_->planNoteValueEdit(notes, request);
    };
    toolCtx.planNoteCorrection = [this](const std::vector<Note>& notes, F0FrameRange editRange) {
        const auto snapshot = readEditedSnapshot();
        if (contentCommands_ == nullptr || snapshot == nullptr)
            return std::optional<ContentNoteCorrectionPlan>{};
        NoteGeneratorParams params;
        params.retuneSpeed = currentRetuneSpeed_;
        params.vibratoDepth = currentVibratoDepth_;
        params.vibratoRate = currentVibratoRate_;
        return contentCommands_->planNoteCorrection(*snapshot, notes, editRange, params);
    };
    toolCtx.applyAutoSnap = [this](const std::vector<std::size_t>& targets,
                                   const ScaleSnapConfig& scaleSnap,
                                   const juce::String& description) {
        if (contentCommands_ == nullptr || !editedContentKey_.isValid())
            return false;
        const auto snapshot = readEditedSnapshot();
        if (snapshot == nullptr)
            return false;
        const auto plan = contentCommands_->planAutoSnap(
            *snapshot, targets, scaleSnap, getCurrentAutoTuneParams());
        if (plan.status != ContentAutoSnapPlan::Status::Commit)
            return false;
        pendingUndoDescription_ = description;
        if (commitEditedContentNotesAndSegments(
                *snapshot, plan.correction.notes, plan.correction.segments,
                plan.correction.affectedRange))
            return true;
        pendingUndoDescription_ = {};
        return false;
    };
    toolCtx.getNotesRevision = [this]() { return lastKnownNotesRevision_; };
    toolCtx.editEraserStroke = [this](F0FrameRange brushRange,
                                      const std::vector<int>& noteIndices,
                                      uint64_t expectedNotesRevision,
                                      juce::String description) -> EraserStrokeEditResult {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return EraserStrokeEditResult{};

        auto result = processor_->editEraserStroke(
            editedContentKey_, brushRange, noteIndices, expectedNotesRevision,
            std::move(description));
        if (result.status == EraserStrokeEditStatus::StaleRevision) {
            closeEqPopup();
            parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
            interactionState_.noteSelection.clear();
            if (result.contentSnapshot != nullptr) {
                cachedNotes_ = result.contentSnapshot->notes;
                lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
                lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
                applyEditedContentCurve(result.contentSnapshot->pitchCurve);
            }
            parameterTargetDidChange();
            requestContentRedraw();
            return result;
        }
        if (result.status != EraserStrokeEditStatus::Applied)
            return result;

        cachedNotes_ = result.contentSnapshot->notes;
        interactionState_.noteSelection.clear();
        lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
        lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
        applyEditedContentCurve(result.contentSnapshot->pitchCurve);
        pendingUndoDescription_ = {};
        requestContentRedraw();
        return result;
    };
    toolCtx.deleteSelectedPianoRollNotes = [this](
        const std::vector<int>& selectedIndices, uint64_t expectedNotesRevision, juce::String description) {
        if (processor_ == nullptr)
            return SelectedNoteDeleteResult{};

        auto result = processor_->deleteSelectedPianoRollNotes(
            editedContentKey_, selectedIndices, expectedNotesRevision, std::move(description));
        if (result.status == SelectedNoteDeleteStatus::Applied
            || result.status == SelectedNoteDeleteStatus::StaleRevision) {
            if (result.status == SelectedNoteDeleteStatus::StaleRevision)
                closeEqPopup();
            if (result.status == SelectedNoteDeleteStatus::StaleRevision)
                parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
            interactionState_.noteSelection.clear();
            interactionState_.selection.isSelectingF0 = false;
            interactionState_.selection.f0SelectionAnchorFrame = -1;
            interactionState_.selection.f0SelectionCurrentFrame = -1;
            interactionState_.noteDraft.clear();
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
            if (result.status == SelectedNoteDeleteStatus::StaleRevision)
                parameterTargetDidChange();
            applyEditedContentCurve(result.contentSnapshot->pitchCurve);
            requestContentRedraw();
            if (result.status == SelectedNoteDeleteStatus::Applied) {
                pendingUndoDescription_ = {};
                if (!result.pitchCurveEdited)
                    listeners_.call([](Listener& listener) { listener.contentEdited(); });
            }
        }
        return result;
    };
    toolCtx.getOriginalF0 = [this]() -> std::vector<float> {
        if (!currentCurve_) return {};
        return currentCurve_->getOriginalF0();
    };
    toolCtx.getF0Timeline = [this]() -> F0Timeline {
        return currentF0Timeline();
    };
    toolCtx.getMinMidi = [this]() { return minMidi_; };
    toolCtx.getMaxMidi = [this]() { return maxMidi_; };
    toolCtx.getRetuneSpeed = [this]() { return currentRetuneSpeed_; };
    toolCtx.getVibratoDepth = [this]() { return currentVibratoDepth_; };
    toolCtx.getVibratoRate = [this]() { return currentVibratoRate_; };
    toolCtx.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& { return shortcutSettings_; };
    toolCtx.setCurrentTool = [this](ToolId tool) { setCurrentTool(tool); };
    // EQ 工具：主音符 index 由 ToolHandler 唯一判定并直通；EQ cursor 经主题机制应用
    toolCtx.openEqPreview = [this](int primaryIndex) { openEqPopupForSelection(primaryIndex); };
    toolCtx.openEqFromKeyboard = [this](int primaryIndex) { openEqPopupForSelection(primaryIndex, true); };
    toolCtx.getEditCursorSourceTime = [this]() { return stoppedEditCursorSourceTime(); };
    toolCtx.eqCursor = eqCursor_;
    toolCtx.showToolSelectionMenu = [this]() {
        // 在当前鼠标屏幕位置弹出纵向图标工具栏
        auto mousePos = juce::Desktop::getInstance().getMousePosition();
        showToolSelectionBar(mousePos);
    };
    toolCtx.notifyAutoTuneRequested = [this]() { listeners_.call([](Listener& l) { l.autoTuneRequested(); }); };
    toolCtx.notifyPlayPauseToggle = [this]() { listeners_.call([](Listener& l) { l.playPauseToggleRequested(); }); };
    toolCtx.notifyStopPlayback = [this]() { listeners_.call([](Listener& l) { l.stopPlaybackRequested(); }); };
    toolCtx.transportShortcutsEnabled = transportControlsEnabled_;
    toolCtx.notifyEscapeKey = [this]() {
        // Accessibility: an open EQ window closes first.
        if (eqPopup_ != nullptr) {
            closeEqPopup();
            announceForAccessibility(LOC_RAW("EQ closed"));
            return true;
        }
        // In the plugin Esc has no job of its own; the host gets it, so it closes the FX
        // window as in other plugins instead of the key being swallowed silently.
        if (escapePassesToHost_)
            return false;
        listeners_.call([](Listener& l) { l.escapeKeyPressed(); });
        return true;
    };
    toolCtx.notifyNoteOffsetChanged = [this](size_t noteIndex, float oldOffset, float newOffset) {
        listeners_.call([noteIndex, oldOffset, newOffset](Listener& l) { l.noteOffsetChanged(noteIndex, oldOffset, newOffset); });
    };
    toolCtx.notifyKeyboardNote = [this](PianoRollToolHandler::KeyboardNoteEvent event,
                                        const std::vector<Note>& notes, int index) {
        handleKeyboardNote(event, notes, index);
    };
    toolCtx.accessibilityModeEnabled = [this]() { return accessibilityModeEnabled(); };
    toolCtx.notifyToolChangedFromKeyboard = [this](ToolId tool) {
        auto name = keyboardToolName(tool);
        // The EQ tool does nothing by itself from the keyboard; say how to open the window.
        if (tool == ToolId::Eq && name.isNotEmpty())
            name << ", " << LOC_RAW("Enter opens it");
        if (name.isNotEmpty())
            announceForAccessibility(name);
    };
    toolCtx.auditionNotePitch = [this](double start, double end, float targetHz) {
        if (processor_ != nullptr) processor_->auditionNotePitch(editedContentKey_, start, end, targetHz);
    };
    toolCtx.stopNotePitchAudition = [this]() { stopNotePitchAudition(); };
    toolCtx.getPianoKeyWidth = [this]() { return getTimelineContentStartX(); };
    toolCtx.getContentProjection = [this]() { return activeContentProjection(); };
    toolCtx.getHandDrawPreviewBounds = [this]() { return getHandDrawPreviewBounds(); };
    toolCtx.getLineAnchorPreviewBounds = [this]() { return getLineAnchorPreviewBounds(); };

    toolCtx.getDirtyStartTime = [this]() { return interactionState_.drawing.dirtyStartTime; };
    toolCtx.setDirtyStartTime = [this](double v) { interactionState_.drawing.dirtyStartTime = v; };
    toolCtx.getDirtyEndTime = [this]() { return interactionState_.drawing.dirtyEndTime; };
    toolCtx.setDirtyEndTime = [this](double v) { interactionState_.drawing.dirtyEndTime = v; };

    toolCtx.getDrawingNoteStartTime = [this]() { return interactionState_.drawing.drawingNoteStartTime; };
    toolCtx.setDrawingNoteStartTime = [this](double v) { interactionState_.drawing.drawingNoteStartTime = v; };
    toolCtx.setDrawingNoteEndTime = [this](double v) { interactionState_.drawing.drawingNoteEndTime = v; };
    toolCtx.getDrawingNotePitch = [this]() { return interactionState_.drawing.drawingNotePitch; };
    toolCtx.setDrawingNotePitch = [this](float v) { interactionState_.drawing.drawingNotePitch = v; };

    toolCtx.getDrawNoteToolPendingDrag = [this]() { return interactionState_.drawNoteToolPendingDrag; };
    toolCtx.setDrawNoteToolPendingDrag = [this](bool v) { interactionState_.drawNoteToolPendingDrag = v; };
    toolCtx.getDrawNoteToolMouseDownPos = [this]() { return interactionState_.drawNoteToolMouseDownPos; };
    toolCtx.setDrawNoteToolMouseDownPos = [this](juce::Point<int> v) { interactionState_.drawNoteToolMouseDownPos = v; };
    toolCtx.getDragThreshold = [this]() { return dragThreshold_; };

    toolCtx.invalidateLiveNotes = [this](const std::vector<Note>& before, const std::vector<Note>& after) {
        invalidateLiveNotes(before, after);
    };
    toolCtx.invalidateSelectionFeedback = [this]() {
        invalidateSelectionFeedback();
    };
    toolCtx.parameterTargetWillChange = [this]() {
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
    };
    toolCtx.parameterTargetDidChange = [this]() { parameterTargetDidChange(); };
    toolCtx.invalidateInteractionPreview = [this](const juce::Rectangle<int>& bounds) {
        invalidateInteractionPreview(bounds);
    };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time, bool commit) {
        const uint64_t seekEpoch = playHeadState_.presentationEpoch.load(std::memory_order_acquire);
        const double hostTime = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
        bool hostAccepted = false;
        listeners_.call([&hostAccepted, time, commit](Listener& l) {
            const bool ok = commit ? l.playheadPositionChangeRequested(time)
                                   : l.playheadPositionPreviewRequested(time);
            if (ok)
                hostAccepted = true;
        });
        // The edit cursor always moves. Host transport is optional.
        pendingSeekTime_ = time;
        pendingSeekEpoch_ = seekEpoch;
        pendingSeekIssuedAtSeconds_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
        pendingSeekHostBound_ = hostAccepted;
        pendingSeekHostTimeAtGesture_ = hostTime;
        pendingSeekHostWasPlaying_ = playHeadState_.isPlaying.load(std::memory_order_acquire);
        playheadTimeForPaint_ = time;
        repaint();
        if (!hostAccepted)
            userScrollHold_ = true;
    };
    toolCtx.notifyPitchCurveEdited = [this](int s, int e) {
        listeners_.call([s, e](Listener& l) { l.pitchCurveEdited(s, e); });
    };

    toolCtx.editManualCorrection = [this](std::vector<ManualCorrectionOp> ops,
                                           int dirtyStartFrame,
                                           int dirtyEndFrame,
                                           juce::String description) {
        if (processor_ == nullptr || !editedContentKey_.isValid())
            return ManualCorrectionEditResult{};

        auto result = processor_->applyManualCorrection(
            editedContentKey_, ops, dirtyStartFrame, dirtyEndFrame, std::move(description));
        if (result.status == ManualCorrectionEditStatus::Applied) {
            cachedNotes_ = result.contentSnapshot->notes;
            interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
            applyEditedContentCurve(result.contentSnapshot->pitchCurve);
            pendingUndoDescription_ = {};
            requestContentRedraw();
        }
        return result;
    };
    toolCtx.selectNotesOverlappingFrames = [this](int startFrame, int endFrameExclusive) {
        return selectNotesOverlappingFrames(startFrame, endFrameExclusive);
    };
    toolCtx.findLineAnchorSegmentNear = [this](int x, int y) { return findLineAnchorSegmentNear(x, y); };
    toolCtx.selectLineAnchorSegment = [this](int idx) { selectLineAnchorSegment(idx); };
    toolCtx.toggleLineAnchorSegmentSelection = [this](int idx) { toggleLineAnchorSegmentSelection(idx); };
    toolCtx.clearLineAnchorSegmentSelection = [this]() { clearLineAnchorSegmentSelection(); };
    toolCtx.setUndoDescription = [this](juce::String desc) { pendingUndoDescription_ = std::move(desc); };

    // ============================================================
    // 閳库槄锟?vocal-time-stretch 锟?.7 锟?Time tool / TimeGrid wiring
    // ============================================================
    toolCtx.getActiveContentTimeGrid = [this]() -> std::shared_ptr<const TimeGridSnapshot> {
        auto placement = findEditedPlacement();
        if (!placement) return nullptr;
        auto snap = readSnapshotFor(placement->contentKey);
        return snap ? snap->timeGrid : nullptr;
    };
    toolCtx.commitTimeGrid = [this](std::shared_ptr<const TimeGridSnapshot> newSnap,
                                     std::shared_ptr<const TimeGridSnapshot> oldSnap,
                                     juce::String description) -> bool {
        if (processor_ == nullptr || contentCommands_ == nullptr || !editedContentKey_.isValid())
            return false;
        if (newSnap == nullptr || oldSnap == nullptr) return false;

        if (!processor_->applyTimeGridEdit(
                editedContentKey_, oldSnap, newSnap, std::move(description)))
            return false;

        {
            auto snap = readEditedSnapshot();
            if (snap) lastKnownTimeGridRevision_ = snap->timeGridRevision;
        }
        requestContentRedraw();
        return true;
    };
    toolCtx.repaintTimeGridHandles = [this]() {
        repaint();
    };

    return toolCtx;
}

// ============================================================================
// OpenDyne 右键横向工具选择弹出条
// ============================================================================

namespace {

struct ToolBarItem {
    ToolId id;
    juce::String name;
    const char* shortcut;
    std::function<juce::Path()> iconFactory;
};

juce::Path makeToolIcon(ToolId id) {
    switch (id) {
        case ToolId::Select:          return ToolbarIcons::getSelectIcon();
        case ToolId::Pitch:           return ToolbarIcons::getPitchToolIcon();
        case ToolId::PitchModulation: return ToolbarIcons::getPitchModulationToolIcon();
        case ToolId::PitchDrift:      return ToolbarIcons::getPitchDriftToolIcon();
        case ToolId::VolumeEnvelope:  return ToolbarIcons::getVolumeEnvelopeToolIcon();
        case ToolId::TimeTool:        return ToolbarIcons::getTimeToolIcon();
        case ToolId::Scissors:        return ToolbarIcons::getScissorsToolIcon();
        case ToolId::Eq:              return ToolbarIcons::getEqIcon();
        case ToolId::Eraser:          return ToolbarIcons::getEraseIcon();
        default:                      return {};
    }
}

} // namespace

bool PianoRollComponent::keyStateChanged(bool isKeyDown)
{
    // The Applications (menu) key never reaches keyPressed in JUCE.
    if (accessibilityModeEnabled() && applicationsKey_.pressedNow() && isKeyDown) {
        showToolMenuFromKeyboard();
        return true;
    }
    return juce::Component::keyStateChanged(isKeyDown);
}

// Keyboard twin of the right-click tool bar (showToolSelectionBar): a standard PopupMenu,
// which JUCE makes navigable with arrows and readable by screen readers. The tool list
// follows showToolSelectionBar; keep both in sync.
void PianoRollComponent::showToolMenuFromKeyboard()
{
    using Id = KeyShortcutConfig::ShortcutId;
    struct Entry { ToolId tool; juce::String name; Id shortcut; };
    std::vector<Entry> entries;
    if (isOpenDyne()) {
        entries = {
            { ToolId::Select, LOC(kToolODSelect), Id::ToolODSelect },
            { ToolId::Pitch, LOC(kToolODPitch), Id::ToolODPitch },
            { ToolId::PitchModulation, LOC(kToolODPitchModulation), Id::ToolODPitchModulation },
            { ToolId::PitchDrift, LOC(kToolODPitchDrift), Id::ToolODPitchDrift },
            { ToolId::HandDraw, LOC(kToolHandDraw), Id::ToolHandDraw },
            { ToolId::Eraser, LOC(kEraser), Id::Count },
            { ToolId::VolumeEnvelope, LOC(kToolODVolumeEnvelope), Id::ToolODVolumeEnvelope },
        };
        if (experimentalFeaturesEnabled_)
            entries.push_back({ ToolId::TimeTool, LOC(kTime), Id::ToolTimeTool });
        entries.push_back({ ToolId::Scissors, LOC(kToolODScissors), Id::ToolODScissors });
        entries.push_back({ ToolId::Eq, LOC(kToolEq), Id::Eq });
    } else {
        entries = {
            { ToolId::Select, LOC(kToolSelect), Id::ToolSelect },
            { ToolId::DrawNote, LOC(kToolDrawNote), Id::ToolDrawNote },
            { ToolId::LineAnchor, LOC(kToolLineAnchor), Id::ToolLineAnchor },
            { ToolId::HandDraw, LOC(kToolHandDraw), Id::ToolHandDraw },
            { ToolId::Eraser, LOC(kEraser), Id::Count },
        };
        if (experimentalFeaturesEnabled_)
            entries.push_back({ ToolId::TimeTool, LOC(kTime), Id::ToolTimeTool });
    }

    juce::PopupMenu menu;
    for (const auto& entry : entries) {
        juce::PopupMenu::Item item(entry.name);
        item.itemID = static_cast<int>(entry.tool) + 1;   // 0 means "cancelled"
        item.isTicked = currentTool_ == entry.tool;
        // The real binding, so a user's own shortcut is shown, not the default.
        if (entry.shortcut != Id::Count)
            item.shortcutKeyDescription = KeyShortcutConfig::getShortcutBinding(shortcutSettings_, entry.shortcut)
                                              .getDisplayNames();
        menu.addItem(std::move(item));
    }

    // Open on the current tool, highlighted, so Enter works at once (without a highlight JUCE
    // ignores the first Enter although NVDA already reads an item).
    auto options = juce::PopupMenu::Options().withTargetComponent(this);
    const bool currentListed = std::any_of(entries.begin(), entries.end(),
                                           [this](const Entry& entry) { return entry.tool == currentTool_; });
    if (currentListed)
        options = options.withInitiallySelectedItem(static_cast<int>(currentTool_) + 1);
    else if (const int firstId = a11y::firstSelectableItemId(menu); firstId != 0)
        options = options.withInitiallySelectedItem(firstId);

    juce::Component::SafePointer<PianoRollComponent> safeThis(this);
    menu.showMenuAsync(options,
                       [safeThis](int result) {
                           if (safeThis == nullptr)
                               return;
                           // Bring focus back to the notes whether a tool was chosen or Esc pressed.
                           safeThis->grabKeyboardFocus();
                           if (result > 0) {
                               const auto tool = static_cast<ToolId>(result - 1);
                               safeThis->setCurrentTool(tool);
                               safeThis->announceForAccessibility(safeThis->keyboardToolName(tool));
                           }
                       });
}

void PianoRollComponent::showToolSelectionBar(juce::Point<int> screenPos)
{
    dismissToolPopup();

    const bool isDyne = isOpenDyne();

    struct MainItem {
        ToolBarItem tool;
        bool hasDropdown = false;
    };

    std::vector<MainItem> mainItems;
    std::vector<ToolBarItem> subItems;

    if (isDyne) {
        mainItems = {
            { { ToolId::Select,    LOC(kToolODSelect),   "F1",  []{ return makeToolIcon(ToolId::Select); } } },
            { { ToolId::Pitch,     LOC(kToolODPitch),    "F2",  []{ return makeToolIcon(ToolId::Pitch); } }, true },
            { { ToolId::HandDraw,  LOC(kToolHandDraw), "5",   []{ return ToolbarIcons::getHandDrawIcon(); } } },
            { { ToolId::Eraser,    LOC(kEraser),   "",    []{ return ToolbarIcons::getEraseIcon(); } } },
            { { ToolId::VolumeEnvelope, LOC(kToolODVolumeEnvelope), "F4", []{ return makeToolIcon(ToolId::VolumeEnvelope); } } },
        };
        if (experimentalFeaturesEnabled_)
            mainItems.push_back({ { ToolId::TimeTool, LOC(kTime), "T", []{ return makeToolIcon(ToolId::TimeTool); } } });
        mainItems.push_back({ { ToolId::Scissors, LOC(kToolODScissors), "F6", []{ return makeToolIcon(ToolId::Scissors); } } });
        mainItems.push_back({ { ToolId::Eq, LOC(kToolEq), "E", []{ return makeToolIcon(ToolId::Eq); } } });
        subItems = {
            { ToolId::PitchModulation, LOC(kToolODPitchModulation), "", []{ return makeToolIcon(ToolId::PitchModulation); } },
            { ToolId::PitchDrift,      LOC(kToolODPitchDrift), "", []{ return makeToolIcon(ToolId::PitchDrift); } },
        };
    } else {
        mainItems = {
            { { ToolId::Select,     LOC(kToolSelect),      "3", []{ return makeToolIcon(ToolId::Select); } } },
            { { ToolId::DrawNote,   LOC(kToolDrawNote),   "2", []{ return ToolbarIcons::getDrawNoteIcon(); } } },
            { { ToolId::LineAnchor, LOC(kToolLineAnchor), "4", []{ return ToolbarIcons::getLineAnchorIcon(); } } },
            { { ToolId::HandDraw,   LOC(kToolHandDraw),   "5", []{ return ToolbarIcons::getHandDrawIcon(); } } },
            { { ToolId::Eraser,     LOC(kEraser),       "",  []{ return ToolbarIcons::getEraseIcon(); } } },
        };
        // Time 与侧栏一致：两种模式均仅在 experimental 开启时可选
        if (experimentalFeaturesEnabled_)
            mainItems.push_back({ { ToolId::TimeTool, LOC(kTime), "T", []{ return makeToolIcon(ToolId::TimeTool); } } });
    }

    // 布局常量
    const int btnSize = 32;
    const int gap = 4;
    const int pad = 6;
    const int pitchExtraW = 10;

    // ── 自绘图标按钮 ──────────────────────────────────────────────
    class ToolIconButton : public juce::Button {
    public:
        ToolIconButton(PianoRollComponent& owner, ToolId tid,
                       juce::Path iconPath, const juce::String& tooltip,
                       bool showArrow = false)
            : juce::Button(tooltip), owner_(owner), tid_(tid),
              iconPath_(std::move(iconPath)), showArrow_(showArrow) {
            setTooltip(tooltip);
            setClickingTogglesState(false);
            setToggleState(owner_.currentTool_ == tid_, juce::dontSendNotification);
            setColour(juce::ToggleButton::textColourId, juce::Colours::white);
            onClick = [this]() {
                owner_.setCurrentTool(tid_);
                owner_.dismissToolPopup();
            };
        }

        void paintButton(juce::Graphics& g, bool highlighted, bool) override {
            auto bounds = getLocalBounds().toFloat().reduced(1.0f);
            const bool active = getToggleState();
            const auto themeId = UIColors::currentThemeId();
            const auto radius = UIColors::currentThemeStyle().controlRadius;

            if (active) {
                if (themeId == ThemeId::Overdose) {
                    juce::ColourGradient fill(juce::Colour(0xFFFFFAFE), bounds.getX(), bounds.getY(),
                                               juce::Colour(0xFFD80050), bounds.getX(), bounds.getBottom(), false);
                    fill.addColour(0.35f, juce::Colour(0xFFFFA0E8));
                    g.setGradientFill(fill);
                } else {
                    g.setColour(UIColors::accent.withAlpha(0.85f));
                }
                g.fillRoundedRectangle(bounds, radius);
            } else if (highlighted) {
                g.setColour(juce::Colours::white.withAlpha(0.22f));
                g.fillRoundedRectangle(bounds, radius);
            }

            if (tid_ == ToolId::Eq) {
                // EQ tool uses bold text "EQ" instead of graphical icon
                auto textColor = active ? juce::Colours::white : UIColors::textPrimary;
                if (themeId == ThemeId::Overdose)
                    textColor = active ? juce::Colours::white
                                       : juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.85f);
                g.setColour(textColor);
                g.setFont(UIColors::getUIFontExact(bounds.getHeight() * 0.5f, juce::Font::bold));
                g.drawText("EQ", bounds, juce::Justification::centred);
            } else if (!iconPath_.isEmpty()) {
                const float iconSz = bounds.getWidth() * 0.55f;
                auto iconRect = bounds.withSizeKeepingCentre(iconSz, iconSz);
                auto iconColor = active ? juce::Colours::white : UIColors::textPrimary;
                if (themeId == ThemeId::Overdose)
                    iconColor = active ? juce::Colours::white
                                       : juce::Colour(Overdose::Colors::PrimaryPink).withAlpha(0.85f);
                ToolbarIcons::drawIcon(g, iconPath_, iconRect, iconColor, 2.0f, false);
            }

            // Pitch 下拉小三角
            if (showArrow_) {
                const float arrowSz = 5.0f;
                auto arrowBounds = juce::Rectangle<float>(
                    bounds.getRight() - arrowSz - 1.0f,
                    bounds.getBottom() - arrowSz - 1.0f,
                    arrowSz, arrowSz);
                juce::Path arrow;
                arrow.addTriangle(arrowBounds.getX(), arrowBounds.getY(),
                                  arrowBounds.getRight(), arrowBounds.getY(),
                                  arrowBounds.getCentreX(), arrowBounds.getBottom());
                auto arrowColor = active ? juce::Colours::white : UIColors::textPrimary;
                g.setColour(arrowColor.withAlpha(0.7f));
                g.fillPath(arrow);
            }
        }

    private:
        PianoRollComponent& owner_;
        ToolId tid_;
        juce::Path iconPath_;
        bool showArrow_;
    };

    // ── 弹出条容器 ────────────────────────────────────────────────
    class ToolBarPopup : public juce::Component {
    public:
        ToolBarPopup(PianoRollComponent& owner,
                     const std::vector<MainItem>& mainItems,
                     const std::vector<ToolBarItem>& subItems,
                     bool isDyne,
                     int btnSize, int gap, int pad,
                     int pitchExtraW)
            : owner_(owner), isDyne_(isDyne),
              btnSize_(btnSize), gap_(gap), pad_(pad), pitchExtraW_(pitchExtraW),
              subExpanded_(false)
        {
            int pitchIdx = -1;
            for (int i = 0; i < static_cast<int>(mainItems.size()); ++i) {
                const auto& mi = mainItems[i];
                int w = mi.hasDropdown ? btnSize + pitchExtraW : btnSize;
                auto btn = std::make_unique<ToolIconButton>(
                    owner_, mi.tool.id, mi.tool.iconFactory(),
                    juce::String(mi.tool.name) + "\n" + mi.tool.shortcut,
                    mi.hasDropdown);
                addAndMakeVisible(*btn);

                if (mi.hasDropdown) {
                    pitchIdx = i;
                    btn->onClick = [this, tid = mi.tool.id]() {
                        if (subExpanded_) {
                            // 已展开：选中 Pitch 工具并关闭
                            owner_.setCurrentTool(tid);
                            owner_.dismissToolPopup();
                        } else {
                            // 未展开：展开子行
                            subExpanded_ = true;
                            updateLayout();
                        }
                    };
                }

                mainButtons_.push_back(std::move(btn));
                mainWidths_.push_back(w);
            }
            pitchButtonIdx_ = pitchIdx;

            for (const auto& si : subItems) {
                auto btn = std::make_unique<ToolIconButton>(
                    owner_, si.id, si.iconFactory(),
                    juce::String(si.name) + "\n" + si.shortcut);
                addAndMakeVisible(*btn);
                btn->setVisible(false);
                subButtons_.push_back(std::move(btn));
            }

            updateLayout();
        }

        void updateLayout()
        {
            const bool showSub = isDyne_ && subExpanded_ && !subButtons_.empty();
            const int subH = showSub ? (btnSize_ * static_cast<int>(subButtons_.size()) + gap_ * (static_cast<int>(subButtons_.size()) - 1)) : 0;
            const int totalH = pad_ + btnSize_ + subH + pad_;

            // 一级菜单（横向工具列）始终完整显示；Pitch 子项在下方展开
            int x = pad_;
            for (int i = 0; i < static_cast<int>(mainButtons_.size()); ++i) {
                mainButtons_[i]->setVisible(true);
                mainButtons_[i]->setBounds(x, pad_, mainWidths_[i], btnSize_);
                x += mainWidths_[i] + gap_;
            }

            if (showSub) {
                int sx;
                if (pitchButtonIdx_ >= 0) {
                    const int pitchBtnW = mainWidths_[pitchButtonIdx_];
                    sx = mainButtons_[pitchButtonIdx_]->getX() + (pitchBtnW - btnSize_) / 2;
                } else {
                    sx = pad_;
                }
                int sy = pad_ + btnSize_ + gap_;
                for (auto& sb : subButtons_) {
                    sb->setBounds(sx, sy, btnSize_, btnSize_);
                    sb->setVisible(true);
                    sy += btnSize_ + gap_;
                }
            } else {
                for (auto& sb : subButtons_)
                    sb->setVisible(false);
            }

            // 弹窗宽度始终等于全部一级按钮总宽；高度随二级菜单展开而增加
            int totalW = pad_ * 2;
            for (int i = 0; i < static_cast<int>(mainButtons_.size()); ++i)
                totalW += mainWidths_[i] + gap_;
            if (!mainButtons_.empty()) totalW -= gap_;

            setSize(totalW, totalH);

            // 展开后弹窗尺寸增大，重新约束在父组件边界内
            if (auto* parent = getParentComponent()) {
                const int px = juce::jlimit(0, juce::jmax(0, parent->getWidth() - totalW), getX());
                const int py = juce::jlimit(0, juce::jmax(0, parent->getHeight() - totalH), getY());
                setTopLeftPosition(px, py);
            }
        }

        void paint(juce::Graphics& g) override {
            auto bounds = getLocalBounds().toFloat();
            const auto radius = UIColors::currentThemeStyle().controlRadius;
            const auto themeId = UIColors::currentThemeId();
            const bool showSub = isDyne_ && subExpanded_ && !subButtons_.empty();

            // 对指定矩形区域绘制 阴影 + 背景 + 顶部边缘线
            auto paintBg = [&](const juce::Rectangle<float>& r) {
                {
                    const auto& style = UIColors::currentThemeStyle();
                    g.setColour(juce::Colours::black.withAlpha(style.shadowAlpha * 0.5f));
                    g.fillRoundedRectangle(r.translated(0, 2).expanded(0, 2), radius + 2.0f);
                }
                if (themeId == ThemeId::Overdose) {
                    juce::ColourGradient bg(
                        UIColors::backgroundDark.brighter(0.06f), r.getX(), r.getY(),
                        UIColors::backgroundDark.darker(0.08f), r.getX(), r.getBottom(), false);
                    g.setGradientFill(bg);
                } else {
                    g.setColour(UIColors::backgroundDark);
                }
                g.fillRoundedRectangle(r, radius);
                g.setColour(UIColors::panelBorder.withAlpha(0.1f));
                g.drawLine(r.getX() + radius, r.getY() + 0.5f,
                           r.getRight() - radius, r.getY() + 0.5f, 1.0f);
            };

            if (showSub) {
                // 上方：全宽一级菜单背景
                const float topH = static_cast<float>(pad_ + btnSize_ + pad_);
                paintBg({ 0.0f, 0.0f, bounds.getWidth(), topH });

                // 下方：仅 Pitch 图标列宽的二级菜单背景
                auto* firstSub = subButtons_.front().get();
                const float subX = static_cast<float>(firstSub->getX() - pad_);
                const float subW = static_cast<float>(btnSize_) + static_cast<float>(pad_) * 2.0f;
                const float subY = static_cast<float>(pad_ + btnSize_);
                const float subH = bounds.getBottom() - subY;
                paintBg({ subX, subY, subW, subH });
            } else {
                paintBg(bounds);
            }
        }

        void mouseExit(const juce::MouseEvent&) override {
            juce::Timer::callAfterDelay(180, [weak = juce::Component::SafePointer<ToolBarPopup>(this)]() {
                if (weak != nullptr && !weak->getLocalBounds().contains(weak->getMouseXYRelative())) {
                    weak->owner_.dismissToolPopup();
                }
            });
        }

    private:
        PianoRollComponent& owner_;
        bool isDyne_;
        int btnSize_, gap_, pad_, pitchExtraW_;
        int pitchButtonIdx_ = -1;
        bool subExpanded_;

        std::vector<std::unique_ptr<ToolIconButton>> mainButtons_;
        std::vector<int> mainWidths_;
        std::vector<std::unique_ptr<ToolIconButton>> subButtons_;
    };

    auto popup = std::make_unique<ToolBarPopup>(
        *this, mainItems, subItems, isDyne, btnSize, gap, pad, pitchExtraW);

    // 定位到鼠标处，确保不超出组件边界
    auto localPos = getLocalPoint(nullptr, screenPos);
    int pw = popup->getWidth();
    int ph = popup->getHeight();
    int px = localPos.x;
    int py = localPos.y;
    px = juce::jlimit(0, getWidth() - pw, px);
    py = juce::jlimit(0, getHeight() - ph, py);
    popup->setBounds(px, py, pw, ph);

    addAndMakeVisible(popup.get());
    popup->setVisible(true);
    popup->toFront(true);
    toolSelectionBar_ = std::move(popup);
}

void PianoRollComponent::dismissToolPopup()
{
    if (toolSelectionBar_) {
        toolSelectionBar_->setVisible(false);
        toolSelectionBar_.reset();
    }
}

void PianoRollComponent::initializeToolHandler() {
    toolHandler_ = std::make_unique<PianoRollToolHandler>(buildToolHandlerContext());
}

PianoRollComponent::PianoRollComponent(const PlayHeadState& playHeadState,
                                       UndoManager& undoManager,
                                       SpectrumReader spectrumReader,
                                       bool transportControlsEnabled)
    : playHeadState_(playHeadState),
      undoManager_(undoManager),
      spectrumReader_(std::move(spectrumReader)),
      eqCursor_(CursorThemeManager::getInstance().resolveCursor(
          juce::MouseCursor(ToolbarIcons::createEqIconImage(), 12, 12))),
      transportControlsEnabled_(transportControlsEnabled) {
    initializeUIComponents();
    initializeRenderer();
    initializeToolHandler();
    // The vertical scroll bar is mouse-only; NVDA read it as "scroll bar 0".
    verticalScrollBar_.setAccessible(false);
}

PianoRollComponent::~PianoRollComponent() {
    if (processor_ != nullptr)
        processor_->setRenderTerminalCallback({});
    cancelPendingAudition();
    if (noteAudition_ != nullptr)
        noteAudition_->stop();
    stopNotePitchAudition();
    scrollVBlankAttachment_.reset();
    verticalScrollBar_.removeListener(this);
}

void PianoRollComponent::setProcessor(OpenTuneAudioProcessor* processor)
{
    if (processor_ != nullptr)
        processor_->setRenderTerminalCallback({});
    processor_ = processor;
    if (processor_ != nullptr)
        processor_->setRenderTerminalCallback([this](ContentKey key, bool completed) {
            handleRenderTerminal(key, completed);
        });
}

bool PianoRollComponent::accessibilityModeEnabled() const noexcept
{
    return appPreferences_ != nullptr && appPreferences_->getAccessibilityModeEnabled();
}

void PianoRollComponent::applyAccessibilityMode()
{
    const bool enabled = accessibilityModeEnabled();
    if (enabled == accessibilityModeApplied_)
        return;
    accessibilityModeApplied_ = enabled;

    setFocusContainerType(enabled ? juce::Component::FocusContainerType::focusContainer
                                  : juce::Component::FocusContainerType::none);
    syncNoteAccessibilityItems();

    if (!enabled) {
        cancelPendingAudition();
        if (noteAudition_ != nullptr)
            noteAudition_->stop();
    }
    invalidateAccessibilityHandler();
}

void PianoRollComponent::announceForAccessibility(const juce::String& text,
                                                  a11y::SpeechQueue queue)
{
    if (!accessibilityModeEnabled())
        return;
    a11y::announce(*this, text, queue);
}

void PianoRollComponent::stopNotePitchAudition() {
    if (processor_ != nullptr) processor_->stopNotePitchAudition();
}



void PianoRollComponent::setContentCommands(std::shared_ptr<ContentEditCommands> commands)
{
    contentCommands_ = std::move(commands);
}


std::shared_ptr<const EditableContentSnapshot> PianoRollComponent::refreshEditedContentNotes()
{
    cachedNotes_.clear();

    std::shared_ptr<const EditableContentSnapshot> snapshot;
    if (editedContentKey_.isValid()) {
        snapshot = readEditedSnapshot();
        if (snapshot) {
            if (lastKnownNotesRevision_ != snapshot->notesRevision) {
                invalidateEqPopup();
                interactionState_.noteSelection.clear();
            }
            cachedNotes_ = snapshot->notes;
            lastKnownNotesRevision_ = snapshot->notesRevision;
        }
    }
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncNoteAccessibilityItems();
    return snapshot;
}

const std::vector<Note>& PianoRollComponent::getCommittedNotes() const
{
    return cachedNotes_;
}

const std::vector<Note>& PianoRollComponent::getDisplayedNotes() const
{
    return interactionState_.noteDraft.active ? interactionState_.noteDraft.workingNotes : cachedNotes_;
}

NoteInteractionDraft& PianoRollComponent::getNoteDraft()
{
    return interactionState_.noteDraft;
}

const NoteInteractionDraft& PianoRollComponent::getNoteDraft() const
{
    return interactionState_.noteDraft;
}

void PianoRollComponent::beginNoteDraft()
{
    interactionState_.noteDraft.active = true;
    interactionState_.noteDraft.expectedNotesRevision = lastKnownNotesRevision_;
    interactionState_.noteDraft.baselineNotes = cachedNotes_;
    interactionState_.noteDraft.workingNotes = cachedNotes_;
}

bool PianoRollComponent::commitNoteDraft()
{
    if (!interactionState_.noteDraft.active) {
        clearNoteDraft();
        return true;
    }

    OpenTuneAudioProcessor::NoteDraftCommitResult result;
    if (processor_ != nullptr && editedContentKey_.isValid()) {
        result = processor_->commitNoteDraft(
            editedContentKey_, interactionState_.noteDraft.expectedNotesRevision,
            interactionState_.noteDraft.workingNotes,
            pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : LOC(kEdit));
    }
    clearNoteDraft();

    using Status = OpenTuneAudioProcessor::NoteDraftCommitStatus;
    if (result.status == Status::StaleRevision) {
        closeEqPopup();
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
    }
    if (result.contentSnapshot != nullptr) {
        cachedNotes_ = result.contentSnapshot->notes;
        lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
        lastKnownPitchRevision_ = result.contentSnapshot->pitchRevision;
        applyEditedContentCurve(result.contentSnapshot->pitchCurve);
    }
    if (result.status == Status::StaleRevision || result.status == Status::Failed)
        interactionState_.noteSelection.clear();
    else
        interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));

    requestContentRedraw();
    if (result.status == Status::Applied)
        listeners_.call([](Listener& listener) { listener.contentEdited(); });
    parameterTargetDidChange();
    return result.status == Status::Applied || result.status == Status::NoChange;
}

void PianoRollComponent::clearNoteDraft()
{
    interactionState_.noteDraft.clear();
    pendingUndoDescription_ = {};
}

// ============================================================================
// EQ 工具：预览弹窗（PianoRollComponent 唯一持有）与选中组原子提交
// ============================================================================

void PianoRollComponent::openEqPopupForSelection(int primaryIndex, bool fromKeyboard)
{
    closeEqPopup();

    const auto& notes = getCommittedNotes();
    if (primaryIndex < 0 || primaryIndex >= static_cast<int>(notes.size()))
        return;

    const Note& primary = notes[primaryIndex];

    // 原样快照目标索引；事务按单一 snapshot 完整解析目标集合
    eqEditTargetIndices_ = interactionState_.noteSelection.selectedIndices;
    eqEditRevision_ = lastKnownNotesRevision_;

    eqPopup_ = std::make_unique<EqPopupComponent>();
    addAndMakeVisible(*eqPopup_);
    // 主音符 eq 有值显示它，无值显示 EqSettings 默认；打开弹窗不提交
    eqPopup_->setEqSettings(primary.eq.value_or(EqSettings{}));
    eqPopup_->setPreviewMode(true);
    if (appPreferences_ != nullptr)
        eqPopup_->setRemoveConfirmationSuppressed(appPreferences_->getState().shared.suppressEqRemoveConfirmation);
    eqPopup_->onCommitSettings = [this](const EqSettings& settings) { applyEqSettingsToSelection(settings); };
    // Accessibility: closed from the keyboard, focus is back on the notes (closeEqPopup) and the result is said.
    // closeEqPopup deletes the popup and this lambda with it, so the captured pointer is
    // copied to the stack first and only the copy is used afterwards.
    eqPopup_->onRemoveEq = [this]() {
        auto* self = this;
        const bool fromKeyboard = eqPopup_ != nullptr && eqPopup_->hasKeyboardFocus(true);
        if (!self->removeEqFromSelection())
            return;
        self->closeEqPopup();
        if (fromKeyboard)
            self->announceForAccessibility(LOC_RAW("EQ removed"), a11y::SpeechQueue::keepAll);
    };
    eqPopup_->onClose = [this]() {
        auto* self = this;
        const bool fromKeyboard = eqPopup_ != nullptr && eqPopup_->hasKeyboardFocus(true);
        self->closeEqPopup();
        if (fromKeyboard)
            self->announceForAccessibility(LOC_RAW("EQ closed"), a11y::SpeechQueue::keepAll);
    };
    eqPopup_->onReadSpectrum = [this](SpectrumArray& spectrum,
                                      SpectrumArray& peaks) {
        spectrumReader_(spectrum, peaks);
    };
    eqPopup_->onRemoveConfirmationSuppressed = [this](bool suppress) {
        if (appPreferences_ != nullptr)
            appPreferences_->setSuppressEqRemoveConfirmation(suppress);
    };

    // 锚点：选中组音符 bounds 并集，退到主音符 bounds；弹窗在组件范围内靠近锚点
    std::vector<Note> selectedNotes;
    for (int idx : interactionState_.noteSelection.selectedIndices)
        if (idx >= 0 && idx < static_cast<int>(notes.size()))
            selectedNotes.push_back(notes[idx]);
    auto anchor = getNotesBounds(selectedNotes);
    if (anchor.isEmpty())
        anchor = getNoteBounds(primary);
    eqPopup_->setBounds(placeEqPopupBounds(anchor));
    eqPopup_->toFront(false);
    // Accessibility: from the keyboard the window opens full size with focus on it;
    // NVDA reads its title ("EQ, Band 1 of 3, Type, Low shelf").
    if (fromKeyboard)
        eqPopup_->startKeyboardEditing();
}

void PianoRollComponent::closeEqPopup()
{
    // A keyboard change still waiting to be committed must reach the notes first.
    if (eqPopup_ != nullptr)
        eqPopup_->flushPendingCommit();
    eqEditTargetIndices_.clear();
    eqEditRevision_ = 0;
    if (eqPopup_ == nullptr)
        return;
    // Focus inside the window would be lost with it; give it back to the notes.
    const bool hadFocus = eqPopup_->hasKeyboardFocus(true);
    removeChildComponent(eqPopup_.get());
    eqPopup_.reset();
    if (hadFocus && isShowing())
        grabKeyboardFocus();
}

void PianoRollComponent::invalidateEqPopup()
{
    eqEditTargetIndices_.clear();
    eqEditRevision_ = 0;
    if (eqPopup_ == nullptr)
        return;

    eqPopup_->onCommitSettings = {};
    eqPopup_->onRemoveEq = {};
    eqPopup_->onClose = {};
    eqPopup_->onRemoveConfirmationSuppressed = {};
    eqPopup_->onReadSpectrum = {};
    eqPopup_->stopTimer();
    // Accessibility: hiding the window would drop the keyboard focus into nowhere.
    const bool hadFocus = eqPopup_->hasKeyboardFocus(true);
    eqPopup_->setVisible(false);
    if (hadFocus && isShowing()) {
        grabKeyboardFocus();
        announceForAccessibility(LOC_RAW("EQ closed"), a11y::SpeechQueue::keepAll);
    }
    juce::Component::SafePointer<PianoRollComponent> safeThis(this);
    juce::Component::SafePointer<EqPopupComponent> safePopup(eqPopup_.get());
    juce::MessageManager::callAsync([safeThis, safePopup]() {
        if (safeThis != nullptr && safePopup != nullptr
            && safeThis->eqPopup_.get() == safePopup.getComponent())
            safeThis->closeEqPopup();
    });
}


juce::Rectangle<int> PianoRollComponent::placeEqPopupBounds(const juce::Rectangle<int>& anchor) const
{
    constexpr int kEqPreviewWidth = 180;   // 4:3 比例
    constexpr int kEqPreviewHeight = 135;  // 4:3 比例
    constexpr int kMargin = 8;
    const auto componentBounds = getLocalBounds();

    // 优先锚点右侧，越界翻到左侧；水平/垂直均夹紧在组件范围内
    int x = anchor.getRight() + kMargin;
    if (x + kEqPreviewWidth > componentBounds.getRight())
        x = anchor.getX() - kMargin - kEqPreviewWidth;
    x = juce::jlimit(componentBounds.getX(),
                     juce::jmax(componentBounds.getX(), componentBounds.getRight() - kEqPreviewWidth),
                     x);

    const int y = juce::jlimit(componentBounds.getY(),
                               juce::jmax(componentBounds.getY(), componentBounds.getBottom() - kEqPreviewHeight),
                               anchor.getY());

    return { x, y, kEqPreviewWidth, kEqPreviewHeight };
}

void PianoRollComponent::applyEqSettingsToSelection(const EqSettings& settings)
{
    if (!editedContentKey_.isValid() || processor_ == nullptr)
        return;
    const auto result = processor_->editSelectedNoteEq(
        editedContentKey_, eqEditTargetIndices_, eqEditRevision_,
        std::optional<EqSettings>{ settings }, LOC_RAW("Edit EQ"));
    if (result.status == SelectedNoteEqEditStatus::StaleRevision) {
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            interactionState_.noteSelection.clear();
            requestContentRedraw();
        }
        parameterTargetDidChange();
        invalidateEqPopup();
        return;
    }
    if (result.status != SelectedNoteEqEditStatus::Applied)
        return;

    cachedNotes_ = result.contentSnapshot->notes;
    lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
    eqEditRevision_ = result.contentSnapshot->notesRevision;
    requestContentRedraw();
    listeners_.call([](Listener& listener) { listener.contentEdited(); });
}

bool PianoRollComponent::removeEqFromSelection()
{
    if (!editedContentKey_.isValid() || processor_ == nullptr)
        return true;
    const auto result = processor_->editSelectedNoteEq(
        editedContentKey_, eqEditTargetIndices_, eqEditRevision_,
        std::nullopt, LOC_RAW("Remove EQ"));
    if (result.status == SelectedNoteEqEditStatus::StaleRevision) {
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            interactionState_.noteSelection.clear();
            requestContentRedraw();
        }
        parameterTargetDidChange();
        invalidateEqPopup();
        return false;
    }
    if (result.status != SelectedNoteEqEditStatus::Applied)
        return true;

    cachedNotes_ = result.contentSnapshot->notes;
    lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
    eqEditRevision_ = result.contentSnapshot->notesRevision;
    requestContentRedraw();
    listeners_.call([](Listener& listener) { listener.contentEdited(); });
    return true;
}

ContentCommitSnapshot PianoRollComponent::commitEditedContentNotesAndSegments(const EditableContentSnapshot& snapshot,
                                                               const std::vector<Note>& notes,
                                                               const std::vector<PitchCorrectionSegment>& segments,
                                                               F0FrameRange affectedRange)
{
    if (contentCommands_ == nullptr || !editedContentKey_.isValid() || snapshot.pitchCurve == nullptr)
        return {};

    auto edit = contentCommands_->commitNotesAndSegmentsUndoable(
        editedContentKey_, notes, segments,
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive},
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : LOC(kEdit));
    if (edit.snapshot == nullptr)
        return {};

    // Update local caches from committed state
    cachedNotes_ = edit.snapshot->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));

    if (edit.snapshot->pitchCurve) {
        applyEditedContentCurve(edit.snapshot->pitchCurve);
    }
    pendingUndoDescription_ = {};
    lastKnownNotesRevision_ = edit.snapshot->notesRevision;
    lastKnownPitchRevision_ = edit.snapshot->pitchRevision;
    {
        requestContentRedraw();
    }
    parameterTargetDidChange();
    return edit.snapshot;
}

std::vector<PitchCorrectionSegment> PianoRollComponent::getCurrentSegments() const
{
    if (!currentCurve_) return {};
    return currentCurve_->getCorrectionSegments();
}

bool PianoRollComponent::selectNotesOverlappingFrames(int startFrame, int endFrameExclusive)
{
    const auto& notes = getCommittedNotes();
    const auto f0tl = currentF0Timeline();
    if (notes.empty() || f0tl.isEmpty()) {
        const bool targetChanged = !interactionState_.noteSelection.selectedIndices.empty()
            || !interactionState_.frameSelection.ranges.empty();
        if (targetChanged)
            parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
        interactionState_.noteSelection.clear();
        interactionState_.frameSelection.clear();
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.isSelectingF0 = false;
        interactionState_.selection.f0SelectionAnchorFrame = -1;
        interactionState_.selection.f0SelectionCurrentFrame = -1;
        if (targetChanged)
            parameterTargetDidChange();
        repaint();
        return false;
    }

    const auto selectionRange = f0tl.rangeForFrames(startFrame, endFrameExclusive);

    bool anyOverlap = false;
    std::vector<int> selectedIndices;
    selectedIndices.reserve(notes.size());
    for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        const auto noteRange = f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime);
        const bool overlaps = std::min(selectionRange.endFrameExclusive, noteRange.endFrameExclusive)
            > std::max(selectionRange.startFrame, noteRange.startFrame);
        if (overlaps) {
            anyOverlap = true;
            selectedIndices.push_back(noteIndex);
        }
    }

    const int noteCount = static_cast<int>(notes.size());
    const bool targetChanged = selectedIndices != interactionState_.noteSelection.selectedIndices
        || !interactionState_.frameSelection.ranges.empty();
    if (targetChanged)
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
    interactionState_.noteSelection.setFromIndices(std::move(selectedIndices),
                                                   noteCount);
    interactionState_.frameSelection.clear();
    interactionState_.selection.isSelectingArea = false;
    // 帧选择由 collectSelectedFrameRanges 从 noteSelection 按需派生
    if (targetChanged)
        parameterTargetDidChange();
    repaint();

    return anyOverlap;
}

juce::Rectangle<int> PianoRollComponent::getNoteBounds(const Note& note) const
{
    const float adjustedPitch = note.getAdjustedPitch();
    if (adjustedPitch <= 0.0f) {
        return {};
    }

    const SourceEditRange sourceRange = sourceEditRange();
    if (note.endTime <= sourceRange.startSeconds
        || note.startTime >= sourceRange.endSeconds) {
        return {};
    }

    const int x1 = sourceTimeToX(note.startTime);
    const int x2 = sourceTimeToX(note.endTime);
    const int width = std::max(1, x2 - x1);
    const float midi = makeViewMapper().freqToMidi(adjustedPitch);
    const float y = makeViewMapper().midiToY(midi) - (pixelsPerSemitone_ * 0.5f);
    const int top = static_cast<int>(std::floor(y));
    const int height = std::max(1, static_cast<int>(std::ceil(pixelsPerSemitone_)));
    return juce::Rectangle<int>(x1, top, width, height)
        .expanded(4)
        .translated(0, rulerHeight_)
        .getIntersection(getTimelineViewportBounds());
}

juce::Rectangle<int> PianoRollComponent::getNotesBounds(const std::vector<Note>& notes) const
{
    juce::Rectangle<int> bounds;
    bool hasBounds = false;
    for (const auto& note : notes) {
        const auto noteBounds = getNoteBounds(note);
        if (noteBounds.isEmpty()) {
            continue;
        }

        bounds = hasBounds ? bounds.getUnion(noteBounds) : noteBounds;
        hasBounds = true;
    }

    return hasBounds ? bounds : juce::Rectangle<int>();
}

juce::Rectangle<int> PianoRollComponent::getHandDrawPreviewBounds() const
{
    if (!interactionState_.drawing.isDrawingF0
        || interactionState_.drawing.handDrawBuffer.empty()
        || interactionState_.drawing.dirtyStartTime < 0.0
        || interactionState_.drawing.dirtyEndTime < 0.0) {
        return {};
    }

    const int x1 = sourceTimeToX(std::min(interactionState_.drawing.dirtyStartTime,
                                          interactionState_.drawing.dirtyEndTime));
    const int x2 = sourceTimeToX(std::max(interactionState_.drawing.dirtyStartTime,
                                          interactionState_.drawing.dirtyEndTime));
    return juce::Rectangle<int>(std::min(x1, x2),
                                getTimelineViewportBounds().getY(),
                                std::max(1, std::abs(x2 - x1)),
                                getTimelineViewportBounds().getHeight())
        .expanded(4)
        .getIntersection(getTimelineViewportBounds());
}

juce::Rectangle<int> PianoRollComponent::getLineAnchorPreviewBounds() const
{
    if (!interactionState_.drawing.isPlacingAnchors || interactionState_.drawing.pendingAnchors.empty()) {
        return {};
    }

    juce::Rectangle<float> bounds;
    bool hasBounds = false;
    auto includePoint = [&](float x, float y) {
        const auto pointBounds = juce::Rectangle<float>(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        bounds = hasBounds ? bounds.getUnion(pointBounds) : pointBounds;
        hasBounds = true;
    };

    for (const auto& anchor : interactionState_.drawing.pendingAnchors) {
        includePoint(static_cast<float>(sourceTimeToX(anchor.time)),
                     makeViewMapper().freqToY(anchor.freq) + static_cast<float>(rulerHeight_));
    }
    includePoint(interactionState_.drawing.currentMousePos.x, interactionState_.drawing.currentMousePos.y);

    return hasBounds ? bounds.getSmallestIntegerContainer().expanded(4).getIntersection(getTimelineViewportBounds())
                     : juce::Rectangle<int>();
}

void PianoRollComponent::invalidateLiveNotes(const std::vector<Note>& beforeNotes, const std::vector<Note>& afterNotes)
{
    // 一次活动读取：旧/新音符矩形经各活动 placement 自己的映射投影（同 key 多
    // placement 逐一覆盖），端点裁剪与 F0 选区派生共用同一 committed 快照
    const double buildT0 = juce::Time::getMillisecondCounterHiRes();
    auto active = buildContentRenderItem(editedContentKey_);
    if (active.has_value())
        recordRenderProbe(RenderProbePoint::SnapshotBuild,
                          juce::Time::getMillisecondCounterHiRes() - buildT0, 1);

    juce::Rectangle<int> dirty;
    if (active.has_value())
    {
        const auto srcRange = SourceEditRange::fromTimeGrid(*active->committedSnapshot->timeGrid, 0.0);
        // 待补矩形命中表面陈旧像素：按来源几何 surfaceView_ 投影（相机增量
        // 尚未提交时，live 投影会错开陈旧像素位置）
        const auto mapper = makeViewMapperForView(surfaceView_);
        const float viewPpsSem = surfaceView_.pixelsPerSemitone;
        auto noteDamage = [&](const PianoRollTimeMap& map, const Note& note) -> juce::Rectangle<int> {
            const float adjustedPitch = note.getAdjustedPitch();
            if (adjustedPitch <= 0.0f) return {};
            if (note.endTime <= srcRange.startSeconds || note.startTime >= srcRange.endSeconds) return {};
            const int x1 = map.sourceToX(note.startTime);
            const int x2 = map.sourceToX(note.endTime);
            const int width = std::max(1, x2 - x1);
            const float midi = mapper.freqToMidi(adjustedPitch);
            const float y = mapper.midiToY(midi) - (viewPpsSem * 0.5f);
            const int top = static_cast<int>(std::floor(y));
            const int height = std::max(1, static_cast<int>(std::ceil(viewPpsSem)));
            return juce::Rectangle<int>(x1, top, width, height)
                .expanded(4)
                .translated(0, rulerHeight_);
        };

        // 拖拽预览曲线与 OpenDyne energy blob 的视觉范围可能远超 note 高度
        // （曲线斜率 / gainFactor 最大 2.5 倍）：拖拽中逐音符先赋内容区全高、
        // 裁视口（保住中心越界音符仍可见的 blob 边缘 X 区间），再合并。
        // 以拖拽状态驱动，不依赖 previewSnapshot 非空。
        const bool fullHeightPerNote = interactionState_.noteDrag.isDraggingNotes
            || interactionState_.isModDriftDragging
            || interactionState_.isVolumeDragging
            || (isOpenDyne() && interactionState_.noteResize.isResizing);
        const auto viewport = getTimelineViewportBounds();

        for (const auto& placement : timelineContentPlacements_)
        {
            if (!placement.isValid() || placement.contentKey != editedContentKey_)
                continue;
            const PianoRollTimeMap map(placement.projection, *active->committedSnapshot->timeGrid, mapper);
            for (const auto& note : beforeNotes) {
                const auto damage = noteDamage(map, note);
                dirty = dirty.getUnion(fullHeightPerNote
                    ? damage.withY(viewport.getY()).withHeight(viewport.getHeight())
                          .getIntersection(viewport)
                    : damage.getIntersection(viewport));
            }
            for (const auto& note : afterNotes) {
                const auto damage = noteDamage(map, note);
                dirty = dirty.getUnion(fullHeightPerNote
                    ? damage.withY(viewport.getY()).withHeight(viewport.getHeight())
                          .getIntersection(viewport)
                    : damage.getIntersection(viewport));
            }
        }
    }
    if (!dirty.isEmpty()) {
        // 局部重绘登记待补矩形（物理像素），paint 构建一次集合并消费；
        // 缩放事务的全量提交会在同一次 paint 中覆盖这些条带
        pendingContentRaster_.add(
            (dirty.toFloat() * renderScale_).getSmallestIntegerContainer());
        repaint(dirty.getX(), dirty.getY(), dirty.getWidth(), dirty.getHeight());
    }
}

void PianoRollComponent::invalidateSelectionFeedback()
{
    repaint();
}

void PianoRollComponent::invalidateInteractionPreview(const juce::Rectangle<int>& bounds)
{
    repaint(bounds);
}

// ============================================================================
// drawPlayheadOverlay — 直接绘制播放头（取代 FixedPlayheadComponent 子组件）
// ============================================================================

void PianoRollComponent::drawPlayheadOverlay(juce::Graphics& g)
{
    const auto viewportBounds = getTimelineViewportBounds();
    const auto mapper = makeViewMapper();

    const int contentViewportLeft = mapper.contentStartX;
    const int timeDerivedX = mapper.timeToX(playheadTimeForPaint_);
    const int contentViewportRight = viewportBounds.getRight();
    const int viewportCentreX = (contentViewportLeft + contentViewportRight) / 2;

    const bool playing = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    const bool continuousMode = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;

    const auto pres = TimelineViewportPolicy::computePlayheadPresentation(
        timeDerivedX, viewportCentreX, contentViewportRight, contentViewportLeft,
        playing, continuousMode);

    if (!pres.visible)
        return;

    const float anchorX = static_cast<float>(pres.anchorX);
    const float height = static_cast<float>(getHeight());
    juce::Graphics::ScopedSaveState playheadClip(g);
    g.reduceClipRegion(timeAxisRect());

    // Overdose: 粉色光晕（宽线打底）
    if (UIColors::isOverdoseTheme())
    {
        g.setColour(juce::Colour(Overdose::Colors::PlayheadGlow).withAlpha(0.55f));
        g.drawLine(anchorX, 0.0f, anchorX, height, 6.0f);
    }

    g.setColour(playheadColour_);
    g.drawLine(anchorX, 0.0f, anchorX, height, 2.0f);

    static const juce::Path kPlayheadTriangle = [] {
        juce::Path p;
        p.addTriangle(-6.0f, 0.0f, 6.0f, 0.0f, 0.0f, 6.0f);
        return p;
    }();
    g.fillPath(kPlayheadTriangle, juce::AffineTransform::translation(anchorX, 0.0f));
}

void PianoRollComponent::drawPlayheadNoteHighlight(juce::Graphics& g, OverlayPassItems& pass)
{
    const bool playing = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    if (!playing || playheadTimeForPaint_ <= 0.0) return;

    const auto placed = activeContent(pass);
    if (!placed.has_value() || placed->content.displayNotes == nullptr) return;
    const auto& item = placed->content;

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.contentStartX = getTimelineContentStartX();
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.coords = makeViewMapper();

    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid() || placement.contentKey != editedContentKey_) continue;
        // playheadTimeForPaint_ 是 timeline；Note start/end 是 source。
        // 必须按 placement 自己的 projection + committed timeGrid 逆映射后再比较。
        const PianoRollTimeMap map(placement.projection, *item.committedSnapshot->timeGrid, ctx.coords);
        const double sourcePlayhead = map.timelineToSource(playheadTimeForPaint_);
        std::vector<int> hitIndices;
        const auto* notes = item.displayNotes;
        for (int i = 0; i < static_cast<int>(notes->size()); ++i) {
            const auto& n = (*notes)[static_cast<size_t>(i)];
            if (sourcePlayhead >= n.startTime && sourcePlayhead < n.endTime)
                hitIndices.push_back(i);
        }
        if (!hitIndices.empty())
            renderer_->drawSelectedNoteHighlights(g, ctx, PianoRollRenderer::PlacedContent{placement, item},
                                                  hitIndices, true);
    }
}

// ============================================================================
// drawTransientOverlay 鈥?paint transient interaction previews
// ============================================================================

void PianoRollComponent::drawTransientOverlay(juce::Graphics& g, OverlayPassItems& pass)
{
    juce::Graphics::ScopedSaveState overlaySave(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    // 活动内容唯一时间映射：本函数内全部映射依赖预览共用（堵住各预览的嵌套快照读取）；
    // 谓词与各消费者既有绘制条件一致——橡皮圆、琴键、tooltip、无几何的 DrawNote 首帧零构建零复制
    const double drawNoteSpanEnd = std::max(interactionState_.drawing.drawingNoteStartTime,
                                            interactionState_.drawing.drawingNoteEndTime);
    const double drawNoteSpanStart = std::min(interactionState_.drawing.drawingNoteStartTime,
                                              interactionState_.drawing.drawingNoteEndTime);
    const bool needsContentMap =
        (!isOpenDyne()
         && ((interactionState_.drawing.isDrawingF0 && currentTool_ == ToolId::HandDraw
              && !interactionState_.drawing.handDrawBuffer.empty())
             || (interactionState_.drawing.isPlacingAnchors && currentTool_ == ToolId::LineAnchor
                 && !interactionState_.drawing.pendingAnchors.empty())))
        || (isOpenDyne() && currentTool_ == ToolId::Scissors
            && interactionState_.scissorsPreviewTime >= 0.0)
        || (interactionState_.drawing.isDrawingNote && currentTool_ == ToolId::DrawNote
            && interactionState_.drawing.drawingNotePitch > 0.0f
            && drawNoteSpanEnd > drawNoteSpanStart)
        || interactionState_.selection.isSelectingArea;

    std::optional<PianoRollTimeMap> timeMap;
    // PlacedContent 含引用成员：optional 仅可构造不可赋值，统一走初始化
    const std::optional<PianoRollRenderer::PlacedContent> placed =
        needsContentMap ? activeContent(pass) : std::nullopt;
    if (placed.has_value())
        timeMap.emplace(placed->placement.projection, *placed->content.committedSnapshot->timeGrid,
                        makeViewMapper());

    // note-drag Corrected F0 预览已收敛为 noteDrag.previewSnapshot（renderer 消费），
    // HandDraw 预览消费本轮 item 的 ownerSnapshot 曲线与 f0Timeline；LineAnchor 预览仅 OpenTune
    if (!isOpenDyne() && timeMap.has_value()) {
        drawHandDrawPreview(g, *timeMap, placed->content);
        drawLineAnchorPreview(g, *timeMap);
    }

    drawEraserPreview(g);

    // ── OpenDyne transient previews（Scissors 预览线、Mod/Drift tooltip、Volume dB tooltip） ──
    if (isOpenDyne()) {
        if (timeMap.has_value())
            drawScissorsPreview(g, *timeMap);
        drawModDriftDragPreview(g);
        drawVolumeDragPreview(g);
    }

    // ⚡️ Cursor preview (豁免路径): DrawNote tool 的绘制中 note preview
    // The committed/draft note body is drawn by the direct content pass.
    if (timeMap.has_value()
        && interactionState_.drawing.isDrawingNote
        && currentTool_ == ToolId::DrawNote) {
        juce::Graphics::ScopedSaveState previewSave(g);
        double startTime = std::min(interactionState_.drawing.drawingNoteStartTime,
                                    interactionState_.drawing.drawingNoteEndTime);
        double endTime = std::max(interactionState_.drawing.drawingNoteStartTime,
                                  interactionState_.drawing.drawingNoteEndTime);
        float pitch = interactionState_.drawing.drawingNotePitch;

        if (pitch > 0.0f && endTime > startTime) {
            // DrawNote 预览直接用 map.sourceToX
            int x1 = timeMap->sourceToX(startTime);
            int x2 = timeMap->sourceToX(endTime);
            const auto mapper = makeViewMapper();
            const float y = mapper.freqToY(pitch) - (pixelsPerSemitone_ * 0.5f);
            float noteHeight = pixelsPerSemitone_;

            juce::Rectangle<float> noteRect(static_cast<float>(std::min(x1, x2)),
                                            y,
                                            static_cast<float>(std::abs(x2 - x1)),
                                            noteHeight);

            g.setColour(UIColors::noteBlockSelected.withAlpha(0.5f));
            g.fillRoundedRectangle(noteRect, 3.0f);
            g.setColour(UIColors::noteBlockSelected.withAlpha(0.8f));
            g.drawRoundedRectangle(noteRect, 3.0f, 1.5f);
        }
    }

    if (timeMap.has_value())
        drawSelectionBox(g, *timeMap, UIColors::currentThemeId());
}

void PianoRollComponent::drawEraserPreview(juce::Graphics& g)
{
    if (currentTool_ != ToolId::Eraser || !interactionState_.eraser.hasCursor)
        return;

    juce::Graphics::ScopedSaveState state(g);
    const auto viewport = getTimelineViewportBounds().translated(0, -rulerHeight_);
    g.reduceClipRegion(viewport);

    const auto centre = interactionState_.eraser.cursorPosition;
    constexpr float radius = 18.0f;
    const float alpha = interactionState_.eraser.isErasing ? 0.58f : 0.36f;
    juce::ColourGradient fill(
        juce::Colours::white.withAlpha(alpha), centre.x, centre.y,
        juce::Colours::white.withAlpha(0.02f), centre.x + radius, centre.y, true);
    g.setGradientFill(fill);
    g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
    g.setColour(juce::Colours::white.withAlpha(interactionState_.eraser.isErasing ? 0.85f : 0.55f));
    g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.2f);
}

// ============================================================================
// drawScissorsPreview — OpenDyne Scissors Tool 切割预览线
// 只绘制在目标 blob 内，不贯穿背景波形（避免表达 PCM 被切开）
// ============================================================================
void PianoRollComponent::drawScissorsPreview(juce::Graphics& g, const PianoRollTimeMap& map)
{
    if (currentTool_ != ToolId::Scissors)
        return;

    const double previewTime = interactionState_.scissorsPreviewTime;
    if (previewTime < 0.0)
        return;

    const auto& notes = getCommittedNotes();
    const auto mapper = makeViewMapper();

    // 找到包含该时刻的 Note
    for (const auto& note : notes)
    {
        if (previewTime <= note.startTime || previewTime >= note.endTime)
            continue;
        const float adjustedPitch = note.getAdjustedPitch();
        if (adjustedPitch <= 0.0f) continue;

        const float midi = mapper.freqToMidi(adjustedPitch);
        const float centerY = mapper.midiToY(midi);
        const float halfH = pixelsPerSemitone_ * 0.5f;
        const int px = map.sourceToX(previewTime);
        const int x1 = map.sourceToX(note.startTime);
        const int x2 = map.sourceToX(note.endTime);
        if (px < x1 || px > x2)
            continue;

        // 只画在 blob 内
        static const float kDash[] = { 3.0f, 3.0f };
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.drawDashedLine(juce::Line<float>(static_cast<float>(px), centerY - halfH,
                                           static_cast<float>(px), centerY + halfH),
                         kDash, 2, 1.5f);
    }
}

// ============================================================================
// drawModDriftDragPreview — OpenDyne Modulation/Drift Tool 拖拽参数预览
// 参数视觉反馈由 F0 预览曲线表达，此处仅显示鼠标旁百分比 tooltip
// ============================================================================
void PianoRollComponent::drawModDriftDragPreview(juce::Graphics& g)
{
    if (!interactionState_.isModDriftDragging)
        return;
    if (currentTool_ != ToolId::PitchModulation && currentTool_ != ToolId::PitchDrift)
        return;

    const float value = interactionState_.modDriftPreviewValue;
    const bool isModulation = (interactionState_.modDriftTool == ToolId::PitchModulation);

    // 鼠标旁 tooltip
    const auto mousePos = getLocalPoint(nullptr, juce::Desktop::getInstance().getMousePosition())
        + juce::Point<int>(0, -rulerHeight_);
    const juce::String text = isModulation
        ? juce::String::formatted("%.0f%%", value * 100.0f)
        : juce::String::formatted("%+.0f%%", value * 100.0f);
    juce::Font font = UIColors::getUIFontExact(12.0f);
    const int textWidth = juce::roundToInt(juce::TextLayout::getStringWidth(font, text));
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle(static_cast<float>(mousePos.x + 12), static_cast<float>(mousePos.y + 12),
                           textWidth + 12.0f, 20.0f, 4.0f);
    g.setColour(juce::Colours::white);
    g.setFont(font);
    g.drawText(text, mousePos.getX() + 18, mousePos.getY() + 14, textWidth, 14,
               juce::Justification::centredLeft);
}

// ============================================================================
// drawVolumeDragPreview — OpenDyne VolumeEnvelope Tool 拖拽 dB 数值预览
// 与 drawModDriftDragPreview 同构范式
// ============================================================================
void PianoRollComponent::drawVolumeDragPreview(juce::Graphics& g)
{
    if (!interactionState_.isVolumeDragging)
        return;
    if (currentTool_ != ToolId::VolumeEnvelope)
        return;

    const float deltaDb = interactionState_.volumePreviewDeltaDb;
    if (std::abs(deltaDb) < 0.01f)
        return;

    // 鼠标旁 tooltip（与 drawModDriftDragPreview 同构）
    const auto mousePos = getLocalPoint(nullptr, juce::Desktop::getInstance().getMousePosition())
        + juce::Point<int>(0, -rulerHeight_);
    const juce::String text = juce::String::formatted("%+.1f dB", static_cast<double>(deltaDb));
    juce::Font font = UIColors::getUIFontExact(12.0f);
    const int textWidth = juce::roundToInt(juce::TextLayout::getStringWidth(font, text));
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle(static_cast<float>(mousePos.x + 12), static_cast<float>(mousePos.y + 12),
                           textWidth + 12.0f, 20.0f, 4.0f);
    g.setColour(juce::Colours::white);
    g.setFont(font);
    g.drawText(text, mousePos.getX() + 18, mousePos.getY() + 14, textWidth, 14,
               juce::Justification::centredLeft);
}

void PianoRollComponent::drawTimeGridHandles(juce::Graphics& g, OverlayPassItems& pass)
{
    if (!isTimeView()) return;
    if (interactionState_.timeTool.selectedHandleId == 0
        && interactionState_.timeTool.hoveredHandleId == 0
        && interactionState_.timeTool.additionalSelectedIds.empty()) return;

    const auto placed = activeContent(pass);
    if (!placed.has_value()) return;

    juce::Graphics::ScopedSaveState ss(g);
    const auto contentBounds = juce::Rectangle<int>(
        getTimelineContentStartX(), rulerHeight_,
        getTimelineContentViewportWidth(), getTimelineContentViewportHeight());
    g.reduceClipRegion(contentBounds);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.contentStartX = getTimelineContentStartX();
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.coords = makeViewMapper();
    ctx.timeGridHoveredHandleId = interactionState_.timeTool.hoveredHandleId;
    ctx.timeGridSelectedHandleId = interactionState_.timeTool.selectedHandleId;
    ctx.additionalSelectedHandleIds = interactionState_.timeTool.additionalSelectedIds;
    ctx.currentTool = currentTool_;

    const auto& item = placed->content;

    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid() || placement.contentKey != editedContentKey_) continue;
        // 拖拽中的 working TimeGrid snapshot 仅应用于 Overlay 把手绘制
        const auto& grid = (interactionState_.timeTool.isDraggingHandle
                            && interactionState_.timeTool.dragWorkingSnapshot != nullptr)
            ? *interactionState_.timeTool.dragWorkingSnapshot
            : *item.committedSnapshot->timeGrid;
        renderer_->drawTimeGridHandles(g, ctx, placement.projection, grid);
    }
}

void PianoRollComponent::drawSelectedNoteHighlights(juce::Graphics& g, OverlayPassItems& pass)
{
    if (interactionState_.noteSelection.selectedIndices.empty()) return;

    const auto placed = activeContent(pass);
    if (!placed.has_value()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.contentStartX = getTimelineContentStartX();
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.coords = makeViewMapper();

    const auto& item = placed->content;

    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid() || placement.contentKey != editedContentKey_) continue;
        renderer_->drawSelectedNoteHighlights(
            g, ctx, PianoRollRenderer::PlacedContent{placement, item},
            interactionState_.noteSelection.selectedIndices);
    }
}

void PianoRollComponent::drawF0SelectionHighlight(juce::Graphics& g, OverlayPassItems& pass)
{
    // 高亮 = 完整 F0 选择集合（音符派生 + 显式帧选择），多区间精确渲染；
    // 音符派生消费活动 item 的 committed 快照。零选区时只贡献显式帧选择（零构建），
    // 显式选区存在时才惰性取得 item 用于绘制
    std::shared_ptr<const EditableContentSnapshot> selectionCommitted;
    if (!interactionState_.noteSelection.selectedIndices.empty())
    {
        const auto placed = activeContent(pass);
        if (!placed.has_value()) return;
        selectionCommitted = placed->content.committedSnapshot;
    }
    const auto frameSel = toolHandler_->collectSelectedFrameRanges(selectionCommitted.get());
    if (frameSel.empty()) return;

    const auto placed = activeContent(pass);
    if (!placed.has_value()) return;
    const auto& item = placed->content;

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.contentStartX = getTimelineContentStartX();
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.showOriginalF0 = showOriginalF0_;
    ctx.originalF0Colour = originalF0Colour_;
    ctx.coords = makeViewMapper();
    ctx.hasF0Selection = true;
    ctx.f0SelectionRanges = frameSel.ranges;

    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid() || placement.contentKey != editedContentKey_) continue;
        renderer_->drawF0SelectionHighlight(g, ctx, PianoRollRenderer::PlacedContent{placement, item});
    }
}

void PianoRollComponent::drawPianoKeysPressed(juce::Graphics& g)
{
    if (pressedPianoKey_ < 0 || !shouldShowPianoKeys()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(0, rulerHeight_, pianoKeyWidth_, getTimelineContentViewportHeight());
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    const float vOrigin = std::floor(verticalScrollOffset_);
    const float vFrac = verticalScrollOffset_ - vOrigin;
    g.addTransform(juce::AffineTransform::translation(0.0f, -vFrac));

    const float noteY = (maxMidi_ - pressedPianoKey_) * pixelsPerSemitone_ - vOrigin;
    const float noteH = pixelsPerSemitone_;
    g.setColour(UIColors::noteBlockSelected.withAlpha(0.35f));
    g.fillRect(0.0f, noteY, static_cast<float>(pianoKeyWidth_), noteH);
}

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g, const PianoRollTimeMap& map,
                                             const PianoRollRenderer::ContentRenderItem& item) {
    if (!interactionState_.drawing.isDrawingF0 || currentTool_ != ToolId::HandDraw
        || interactionState_.drawing.handDrawBuffer.empty()
        || item.ownerSnapshot == nullptr || item.ownerSnapshot->pitchCurve == nullptr) return;

    // 唯一 F0 来源：本轮 item 的 ownerSnapshot 曲线与其 f0Timeline
    const auto& originalF0 = item.ownerSnapshot->pitchCurve->getOriginalF0();
    if (originalF0.empty() || interactionState_.drawing.handDrawBuffer.size() != originalF0.size()) return;

    const auto& f0tl = item.f0Timeline;
    if (f0tl.isEmpty()) return;
    juce::Colour previewColour = correctedF0Colour_;
    juce::Path previewPath;
    bool pathStarted = false;

    for (int i = 0; i < f0tl.endFrameExclusive(); ++i) {
        float f0 = interactionState_.drawing.handDrawBuffer[static_cast<size_t>(i)];
        if (f0 > 0.0f) {
            float y = makeViewMapper().freqToY(f0);
            double timePos = f0tl.timeAtFrame(i);
            float x = static_cast<float>(map.sourceToX(timePos));

            if (!pathStarted) {
                previewPath.startNewSubPath(x, y);
                pathStarted = true;
            } else {
                juce::Point<float> last = previewPath.getCurrentPosition();
                if (std::abs(x - last.x) > 30.0f) {
                    previewPath.startNewSubPath(x, y);
                } else {
                    previewPath.lineTo(x, y);
                }
            }
        } else if (pathStarted && f0 < -0.5f) {
            pathStarted = false;
        }
    }

    if (!previewPath.isEmpty()) {
        g.setColour(previewColour.withAlpha(0.85f));
        juce::PathStrokeType strokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.strokePath(previewPath, strokeType);
    }
}

void PianoRollComponent::drawLineAnchorPreview(juce::Graphics& g, const PianoRollTimeMap& map) {
    if (!interactionState_.drawing.isPlacingAnchors || currentTool_ != ToolId::LineAnchor || interactionState_.drawing.pendingAnchors.empty()) return;

    juce::Colour anchorColour = correctedF0Colour_;

    for (size_t i = 0; i < interactionState_.drawing.pendingAnchors.size(); ++i) {
        const auto& anchor = interactionState_.drawing.pendingAnchors[i];
        float x = static_cast<float>(map.sourceToX(anchor.time));
        float y = makeViewMapper().freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(map.sourceToX(prev.time));
            float prevY = makeViewMapper().freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(map.sourceToX(last.time));
        float lastY = makeViewMapper().freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY,
                   interactionState_.drawing.currentMousePos.x,
                   interactionState_.drawing.currentMousePos.y - static_cast<float>(rulerHeight_),
                   1.5f);
    }
}

void PianoRollComponent::drawSelectionBox(juce::Graphics& g, const PianoRollTimeMap& map, ThemeId themeId) {
    // 框选矩形仅在拖拽中绘制
    if (!interactionState_.selection.isSelectingArea) return;
    if (!toolHandler_) return;

    double startTime = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    double endTime = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    float minMidi = std::min(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);
    float maxMidi = std::max(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);

    int x1 = map.sourceToX(startTime);
    int x2 = map.sourceToX(endTime);
    float y1 = makeViewMapper().midiToY(maxMidi);
    float y2 = makeViewMapper().midiToY(minMidi);

    float left = static_cast<float>(std::min(x1, x2));
    float top = std::min(y1, y2);
    float width = static_cast<float>(std::abs(x2 - x1));
    float height = std::abs(y2 - y1);

    juce::Rectangle<float> rect(left, top, width, height);
    juce::Colour fill = UIColors::lightPurple;
    juce::Colour stroke = UIColors::lightPurple;
    float fillAlpha = 0.12f;
    float strokeAlpha = 0.5f;
    float strokeThickness = 1.0f;

    if (themeId == ThemeId::DarkBlueGrey) {
        fill = juce::Colours::white;
        stroke = juce::Colours::white;
        fillAlpha = 0.20f;
        strokeAlpha = 0.90f;
        strokeThickness = 2.0f;
    }
    else if (themeId == ThemeId::Overdose) {
        fill = UIColors::lightPurple;
        stroke = UIColors::accent;
        fillAlpha = 0.15f;
        strokeAlpha = 0.65f;
        strokeThickness = 1.2f;
    }

    g.setColour(fill.withAlpha(fillAlpha));
    g.fillRoundedRectangle(rect, 3.0f);
    g.setColour(stroke.withAlpha(strokeAlpha));
    g.drawRoundedRectangle(rect, 3.0f, strokeThickness);
}
void PianoRollComponent::paint(juce::Graphics& g)
{
    const double paintStartMs = juce::Time::getMillisecondCounterHiRes();
    const int vpW = getTimelineViewportBounds().getWidth();
    const int vpH = getTimelineViewportBounds().getHeight();
    if (vpW <= 0 || vpH <= 0) return;

    const auto damage = g.getClipBounds().getIntersection(getLocalBounds());

    // 有效倍率真值只在 paint 捕获；变化 → 旧表面失效，本帧按同一倍率重建
    const float effectiveScale = UiAssets::getEffectiveScaleFactor(g);
    if (! juce::approximatelyEqual(effectiveScale, renderScale_))
    {
        renderScale_ = effectiveScale;
        staticDirty_ = true;
        contentDirty_ = true;
    }

    // 同一次 paint 提交保留表面，并把快照集合交给 overlay。
    const bool zoomActive = zoomDeadlineTicks_ > 0;
    const bool zoomPending = std::exchange(zoomCameraPending_, false);
    std::optional<Items> paintItems;
    ensureSurfaceSizes();
    const auto waveformDamage = commitCameraDelta(zoomPending);
    if (staticDirty_) {
        rasterizeStatic();
        rasterizeLabels();
    }
    if (contentDirty_) {
        paintItems.emplace(buildContentRenderItems());
        rasterizeContentSurfaces(*paintItems);
        pendingContentRaster_.clear();
        if (zoomPending && !zoomActive)
            recordRenderProbe(RenderProbePoint::ZoomCommitRaster,
                              juce::Time::getMillisecondCounterHiRes() - paintStartMs);
    } else if (!pendingContentRaster_.isEmpty() || !waveformDamage.isEmpty()) {
        paintItems.emplace(buildContentRenderItems());
        const auto logicalBounds = [this](juce::Rectangle<int> physical) {
            return (physical.toFloat() / renderScale_).getSmallestIntegerContainer();
        };
        if (!waveformDamage.isEmpty())
            rasterizeWaveform(*paintItems, logicalBounds(waveformDamage), waveformDamage);
        pendingContentRaster_.consolidate();
        for (const auto& strip : pendingContentRaster_)
            rasterizeContent(*paintItems, logicalBounds(strip), strip);
        pendingContentRaster_.clear();
    }

    {
        juce::Graphics::ScopedSaveState baseSave(g);
        g.drawImage(staticSurface_, 0, 0, getWidth(), getHeight(),
                    0, 0, staticSurface_.getWidth(), staticSurface_.getHeight(), false);
        // 波形是时间参考，画在音高网格之上、音符之下，不参与垂直滚动。
        g.drawImage(waveformSurface_, 0, 0, vpW, vpH,
                    0, 0, waveformSurface_.getWidth(), waveformSurface_.getHeight(), false);
        g.drawImage(contentSurface_, 0, 0, vpW, vpH,
                    0, 0, contentSurface_.getWidth(), contentSurface_.getHeight(), false);

        if (trackCaptureTint_) {
            g.setColour(juce::Colour(0x55E07A2F));
            g.fillRect(getTimelineContentStartX(), rulerHeight_,
                       getTimelineContentViewportWidth(), getTimelineContentViewportHeight());
        }
        if (!shouldShowPianoKeys() && labelsSurface_.isValid()) {
            g.drawImage(labelsSurface_, 0, 0, pianoKeyWidth_, getHeight(),
                        0, 0, labelsSurface_.getWidth(), labelsSurface_.getHeight(), false);
        }
    }

    // 滚动条：组件 alpha=0 已从 JUCE 子组件绘制路径跳过，owner 在原层位（底图之后、overlay 之前）手动绘制
    {
        juce::Graphics::ScopedSaveState scrollbarSave(g);
        if (g.reduceClipRegion(verticalScrollBar_.getBounds()))
        {
            g.setOrigin(verticalScrollBar_.getPosition());
            verticalScrollBar_.paintEntireComponent(g, true);
        }
    }

    // overlay 七项：本轮确曾构建集合时共享之（含合法空集合）；
    // 干净 blit 路径 shared 为空，activeContent 惰性按需构建
    OverlayPassItems overlayPass;
    if (paintItems)
        overlayPass.shared = &*paintItems;
    drawOverlay(g, overlayPass);

    const double paintEndMs = juce::Time::getMillisecondCounterHiRes();
    recordRenderProbe(RenderProbePoint::RootPaint, paintEndMs - paintStartMs);
    // 缩放事务进行中的 paint 计入 ZoomPreviewPaint（预览帧成本）；
    // 截止后才呈现的提交栅格由 contentDirty_ 分支计入 ZoomCommitRaster
    if (zoomActive)
        recordRenderProbe(RenderProbePoint::ZoomPreviewPaint, paintEndMs - paintStartMs);
    if (lastVBlankMs_ > 0.0)
        recordRenderProbe(RenderProbePoint::VBlankToRootPaint, paintStartMs - lastVBlankMs_);
}

// ============================================================================
// drawOverlay — owner paint 收尾层（原 PianoRollOverlayComponent::paint 次序）
// ============================================================================

void PianoRollComponent::drawOverlay(juce::Graphics& g, OverlayPassItems& pass)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    juce::Graphics::ScopedSaveState overlaySave(g);

    // 1. 播放头
    drawPlayheadOverlay(g);

    // 2. 交互 Overlay（选中高亮、框选、绘制预览、ghost 把手等）
    drawTransientOverlay(g, pass);

    // 3. TimeGrid 把手
    drawTimeGridHandles(g, pass);

    // 4. 选中音符高亮
    drawSelectedNoteHighlights(g, pass);

    // 播放头音符高亮覆盖真实选中高亮，保持两种状态可区分。
    drawPlayheadNoteHighlight(g, pass);

    // 5. F0 曲线选中高亮
    drawF0SelectionHighlight(g, pass);

    // 6. Ghost content (已在内容表面绘制，此处跳过)

    // 7. 按下琴键高亮
    drawPianoKeysPressed(g);

    recordRenderProbe(RenderProbePoint::OverlayPresent,
                      juce::Time::getMillisecondCounterHiRes() - t0, 1);
}

std::optional<PianoRollRenderer::PlacedContent> PianoRollComponent::activeContent(OverlayPassItems& pass)
{
    if (!pass.activeBuilt)
    {
        pass.activeBuilt = true;
        pass.activePlacement = findEditedPlacement();
        if (pass.activePlacement != nullptr)
        {
            if (pass.shared != nullptr)
            {
                // 本轮集合已构建：命中即缓存引用；缺 key 视为本轮无该项（不重试）
                const auto it = pass.shared->find(pass.activePlacement->contentKey);
                if (it != pass.shared->end())
                    pass.sharedItem = &it->second;
            }
            else
            {
                // 干净 blit + overlay 路径：惰性构建一次，失败缓存空不重试
                const double buildT0 = juce::Time::getMillisecondCounterHiRes();
                pass.activeItem = buildContentRenderItem(pass.activePlacement->contentKey);
                if (pass.activeItem.has_value())
                    recordRenderProbe(RenderProbePoint::SnapshotBuild,
                                      juce::Time::getMillisecondCounterHiRes() - buildT0, 1);
            }
        }
    }
    if (pass.activePlacement == nullptr)
        return std::nullopt;
    if (pass.sharedItem != nullptr)
        return PianoRollRenderer::PlacedContent{*pass.activePlacement, *pass.sharedItem};
    if (pass.activeItem.has_value())
        return PianoRollRenderer::PlacedContent{*pass.activePlacement, *pass.activeItem};
    return std::nullopt;
}

void PianoRollComponent::invalidateTimeAxisStaticSurface()
{
    // 全局静态失效已在队列 → paint 全量栅格覆盖时间轴，不重复局部栅格；
    // 表面干净且来源相机与当前一致 → 局部即时栅格（原子提交）；
    // 相机增量尚未提交（来源为旧相机）或表面无效 → 交给 paint
    if (!staticDirty_ && surfaceView_ == viewportState() && staticSurface_.isValid()) {
        rasterizeStatic(timeAxisRect());
    } else {
        staticDirty_ = true;
    }
    repaint(timeAxisRect());
}

void PianoRollComponent::ensureSurfaceSizes()
{
    const int vpW = getTimelineViewportBounds().getWidth();
    const int vpH = getTimelineViewportBounds().getHeight();
    const int fullW = getWidth();
    const int fullH = getHeight();

    // 位图物理尺寸 = ceil(逻辑尺寸 × 有效倍率)
    const int physFullW = static_cast<int>(std::ceil(static_cast<float>(fullW) * renderScale_));
    const int physFullH = static_cast<int>(std::ceil(static_cast<float>(fullH) * renderScale_));
    const int physVpW = static_cast<int>(std::ceil(static_cast<float>(vpW) * renderScale_));
    const int physVpH = static_cast<int>(std::ceil(static_cast<float>(vpH) * renderScale_));

    // staticSurface_ 覆盖完整组件 chrome 区域
    if (!staticSurface_.isValid() || staticSurface_.getWidth() != physFullW || staticSurface_.getHeight() != physFullH) {
        staticSurface_ = juce::Image(juce::Image::ARGB, physFullW, physFullH, true);
        staticDirty_ = true;
    }
    // contentSurface_ 保持时间轴视口区
    if (!contentSurface_.isValid() || contentSurface_.getWidth() != physVpW || contentSurface_.getHeight() != physVpH) {
        contentSurface_ = juce::Image(juce::Image::ARGB, physVpW, physVpH, true);
        contentDirty_ = true;
    }
    if (!waveformSurface_.isValid() || waveformSurface_.getWidth() != physVpW || waveformSurface_.getHeight() != physVpH) {
        waveformSurface_ = juce::Image(juce::Image::ARGB, physVpW, physVpH, true);
        contentDirty_ = true;
    }
}

void PianoRollComponent::recordRenderProbe(RenderProbePoint point, double elapsedMs, int samples)
{
    RasterProbe* probe = nullptr;
    switch (point) {
        case RenderProbePoint::StaticRaster:  probe = &staticRasterProbe_;  break;
        case RenderProbePoint::ContentRaster: probe = &contentRasterProbe_; break;
        case RenderProbePoint::OverlayPresent: probe = &overlayPresentProbe_; break;
        case RenderProbePoint::RootPaint: probe = &rootPaintProbe_; break;
        case RenderProbePoint::VBlankToRootPaint: probe = &vblankToRootPaintProbe_; break;
        case RenderProbePoint::ZoomPreviewPaint: probe = &zoomPreviewPaintProbe_; break;
        case RenderProbePoint::ZoomCommitRaster: probe = &zoomCommitRasterProbe_; break;
        case RenderProbePoint::SnapshotBuild: probe = &snapshotBuildProbe_; break;
        case RenderProbePoint::MoveImageSection: probe = &moveImageSectionProbe_; break;
    }
    probe->count += samples;
    probe->totalMs += elapsedMs;

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (probeReportWindowStart_ == 0.0) probeReportWindowStart_ = now;
    if (now - probeReportWindowStart_ >= 2000.0) {
        auto avg = [](const RasterProbe& p) { return p.count > 0 ? p.totalMs / p.count : 0.0; };
        AppLogger::log(juce::String("[PR-Perf] static-raster:") + juce::String(avg(staticRasterProbe_), 1) + "ms x" + juce::String(staticRasterProbe_.count)
            + " content-raster:" + juce::String(avg(contentRasterProbe_), 1) + "ms x" + juce::String(contentRasterProbe_.count)
            + " overlay-present:" + juce::String(avg(overlayPresentProbe_), 1) + "ms x" + juce::String(overlayPresentProbe_.count)
            + " root-paint:" + juce::String(avg(rootPaintProbe_), 1) + "ms x" + juce::String(rootPaintProbe_.count)
            + " vblank-to-root-paint:" + juce::String(avg(vblankToRootPaintProbe_), 1) + "ms x" + juce::String(vblankToRootPaintProbe_.count)
            + " zoom-preview-paint:" + juce::String(avg(zoomPreviewPaintProbe_), 1) + "ms x" + juce::String(zoomPreviewPaintProbe_.count)
            + " zoom-commit-raster:" + juce::String(avg(zoomCommitRasterProbe_), 1) + "ms x" + juce::String(zoomCommitRasterProbe_.count)
            + " snapshot-build:" + juce::String(avg(snapshotBuildProbe_), 1) + "ms x" + juce::String(snapshotBuildProbe_.count)
            + " move-image-section:" + juce::String(avg(moveImageSectionProbe_), 1) + "ms x" + juce::String(moveImageSectionProbe_.count));
        staticRasterProbe_ = {};
        contentRasterProbe_ = {};
        overlayPresentProbe_ = {};
        rootPaintProbe_ = {};
        vblankToRootPaintProbe_ = {};
        zoomPreviewPaintProbe_ = {};
        zoomCommitRasterProbe_ = {};
        snapshotBuildProbe_ = {};
        moveImageSectionProbe_ = {};
        probeReportWindowStart_ = now;
    }
}

void PianoRollComponent::drawFixedChrome(juce::Graphics& g, juce::Rectangle<int> damage)
{
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(damage);

    const int imgW = getWidth();
    const int imgH = getHeight();

    g.setColour(UIColors::rollBackground);
    g.fillRect(0, 0, imgW, imgH);
    {
        juce::Path chromePath;
        chromePath.addRoundedRectangle(juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)), UIColors::cornerRadius);
        juce::Graphics::ScopedSaveState css(g);
        g.reduceClipRegion(chromePath);
        switch (UIColors::currentThemeId()) {
            case ThemeId::DarkBlueGrey: UIColors::fillSoothe2SpectrumBackground(g, juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)), UIColors::cornerRadius); break;
            case ThemeId::Aurora: g.setColour(UIColors::rollBackground); g.fillPath(chromePath); break;
            case ThemeId::BlueBreeze: UIColors::fillMistedTimelineField(g, juce::Rectangle<float>(0,0,static_cast<float>(imgW),static_cast<float>(imgH)), UIColors::cornerRadius); break;
            case ThemeId::Overdose: UIColors::fillOverdoseEditorBackground(g, juce::Rectangle<float>(0,0,static_cast<float>(imgW),static_cast<float>(imgH)), UIColors::cornerRadius); break;
            default: g.setColour(UIColors::rollBackground); g.fillPath(chromePath); break;
        }
    }
    // 背景亮度叠加：1.0=不改变，0.0=纯黑，2.0=高亮
    if (backgroundBrightness_ != 1.0f) {
        if (backgroundBrightness_ < 1.0f) {
            const float alpha = juce::jlimit(0.0f, 1.0f, 1.0f - backgroundBrightness_);
            g.setColour(juce::Colours::black.withAlpha(alpha));
            g.fillRect(0, 0, imgW, imgH);
        } else {
            const float alpha = juce::jlimit(0.0f, 0.5f, (backgroundBrightness_ - 1.0f) * 0.5f);
            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.fillRect(0, 0, imgW, imgH);
        }
    }
    UIColors::drawShadow(g, juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)));
}

void PianoRollComponent::drawRuler(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const int cw = getTimelineContentViewportWidth();
    const int contentStartX = getTimelineContentStartX();
    const juce::Rectangle<int> rulerDomain(contentStartX, 0, cw, rulerHeight_);
    const auto clipArea = damage.getIntersection(rulerDomain);
    if (clipArea.isEmpty()) return;

    juce::Graphics::ScopedSaveState rs(g);
    const double pps = view.camera.pixelsPerSecond;
    const double visibleStart = view.camera.visibleStartSeconds;
    const double visibleEnd = visibleStart + cw / pps;
    RenderParams rp;
    rp.visibleStartSeconds = visibleStart; rp.visibleEndSeconds = visibleEnd;
    rp.pixelsPerSecond = pps;
    rp.displayMode = displayMode_;
    rp.tempo = bpm_;
    rp.timeSigNumerator = timeSigNum_;
    rp.timeSigDenominator = timeSigDenom_;
    rp.themeId = static_cast<int>(UIColors::currentThemeId());
    rp.pixelsPerSemitone = 0.0f; rp.worldTopY = 0;
    rp.rulerHeight = rulerHeight_;
    rp.viewportWidth = cw; rp.viewportHeight = rulerHeight_;
    rp.viewKind = "pianoroll";
    g.reduceClipRegion(clipArea);
    g.addTransform(juce::AffineTransform::translation(static_cast<float>(contentStartX), 0.0f));
    g.reduceClipRegion(0, 0, cw, rulerHeight_);
    TimelineLayerComposer::drawTimeRuler(g, rp);
}

void PianoRollComponent::drawPitchBackground(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const int cw = getTimelineContentViewportWidth();
    const int ch = getTimelineContentViewportHeight();
    const int laneX = getTimelineContentStartX();
    const int laneW = cw;
    const juce::Rectangle<int> timelineDomain(laneX, rulerHeight_, laneW, ch);
    const auto clipArea = damage.getIntersection(timelineDomain);
    if (clipArea.isEmpty()) return;

    juce::Graphics::ScopedSaveState ls(g);
    const double pps = view.camera.pixelsPerSecond;
    const double visibleStart = view.camera.visibleStartSeconds;
    const double visibleEnd = visibleStart + cw / pps;
    const float vOrigin = std::floor(view.verticalScrollOffset);
    const float vFrac = view.verticalScrollOffset - vOrigin;

    RenderParams lp;
    lp.visibleStartSeconds = visibleStart; lp.visibleEndSeconds = visibleEnd;
    lp.pixelsPerSecond = pps; lp.displayMode = displayMode_;
    lp.tempo = bpm_;
    lp.timeSigNumerator = timeSigNum_;
    lp.timeSigDenominator = timeSigDenom_;
    lp.themeId = static_cast<int>(UIColors::currentThemeId());
    lp.pixelsPerSemitone = view.pixelsPerSemitone; lp.worldTopY = vOrigin;
    lp.rulerHeight = 0;
    lp.showPianoKeyboard = showPianoKeyboard_;
    lp.scaleAssistEnabled = scaleAssistEnabled_;
    lp.scaleRootNote = scaleRootNote_;
    lp.scaleType = scaleType_;
    lp.gridStyle = static_cast<int>(gridStyle_);
    lp.viewportWidth = laneW; lp.viewportHeight = ch; lp.viewKind = "pianoroll";

    g.reduceClipRegion(clipArea);

    // drawLaneStripRepeats 始终调用（不能关掉整层）
    {
        juce::Graphics::ScopedSaveState lss(g);
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(laneX), static_cast<float>(rulerHeight_) - vFrac));

        g.reduceClipRegion(0, 0, laneW, ch);
        TimelineLayerComposer::drawLaneStripRepeats(g, lp);
    }
    {
        juce::Graphics::ScopedSaveState gs(g);
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(getTimelineContentStartX()), static_cast<float>(rulerHeight_)));
        g.reduceClipRegion(0, 0, cw, ch);
        TimelineLayerComposer::drawGridLines(g, lp);
    }
}

void PianoRollComponent::drawPianoKeyboard(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const bool labelsOnly = !shouldShowPianoKeys();
    const int ch = getTimelineContentViewportHeight();
    const juce::Rectangle<int> pianoDomain(0, rulerHeight_, pianoKeyWidth_, ch);
    const auto clipArea = damage.getIntersection(pianoDomain);
    if (clipArea.isEmpty()) return;

    const int vpW = getTimelineViewportBounds().getWidth();
    const auto mapper = makeViewMapperForView(view);

    auto ctxForKeys = [&]() {
        PianoRollRenderer::RenderContext rctx;
        rctx.width = vpW;
        rctx.height = ch;
        rctx.pianoKeyWidth = pianoKeyWidth_;
        rctx.contentStartX = getTimelineContentStartX();
        rctx.rulerHeight = 0;
        rctx.pixelsPerSecond = view.camera.pixelsPerSecond;
        rctx.pixelsPerSemitone = view.pixelsPerSemitone;
        rctx.minMidi = minMidi_;
        rctx.maxMidi = maxMidi_;
        rctx.scaleRootNote = scaleRootNote_;
        rctx.scaleType = scaleType_;
        rctx.noteNameMode = noteNameMode_;
        rctx.labelsOnly = labelsOnly;
        rctx.coords = mapper;

        rctx.rasterBounds = clipArea;
        return rctx;
    };
    juce::Graphics::ScopedSaveState pks(g);
    g.reduceClipRegion(clipArea);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));
    g.reduceClipRegion(0, 0, pianoKeyWidth_, ch);
    renderer_->drawPianoKeys(g, ctxForKeys());
}

void PianoRollComponent::rasterizeStatic(
    std::optional<juce::Rectangle<int>> dirtyRect,
    std::optional<juce::Rectangle<int>> physicalDirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int imgW = staticSurface_.getWidth();
    const int imgH = staticSurface_.getHeight();
    if (!staticSurface_.isValid() || imgW <= 0 || imgH <= 0) return;

    // 逻辑损坏区；clear 区域转到物理像素
    const juce::Rectangle<int> rasterBounds = dirtyRect.value_or(
        juce::Rectangle<int>(0, 0, getWidth(), getHeight()));

    const auto physicalBounds = physicalDirtyRect.value_or(
        (rasterBounds.toFloat() * renderScale_).getSmallestIntegerContainer());
    staticSurface_.clear(physicalBounds);

    juce::Graphics g(staticSurface_);
    if (physicalDirtyRect.has_value())
        g.reduceClipRegion(physicalBounds);
    g.addTransform(juce::AffineTransform::scale(renderScale_));

    drawFixedChrome(g, rasterBounds);
    drawRuler(g, surfaceView_, rasterBounds);
    drawPitchBackground(g, surfaceView_, rasterBounds);
    if (shouldShowPianoKeys())
        drawPianoKeyboard(g, surfaceView_, rasterBounds);

    // 全局脏标志只由完整静态栅格清除：局部条带（标尺带/时间轴）不得吞掉
    // 先前的全局失效（音名模式→BPM 同帧序列下隐藏键盘音名依赖该标志存活）
    if (!dirtyRect.has_value())
        staticDirty_ = false;

    recordRenderProbe(RenderProbePoint::StaticRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::rasterizeLabels()
{
    if (shouldShowPianoKeys() || getHeight() <= 0)
        return;

    const int physWidth = static_cast<int>(std::ceil(static_cast<float>(pianoKeyWidth_) * renderScale_));
    const int physHeight = static_cast<int>(std::ceil(static_cast<float>(getHeight()) * renderScale_));

    if (!labelsSurface_.isValid()
        || labelsSurface_.getWidth() != physWidth
        || labelsSurface_.getHeight() != physHeight) {
        labelsSurface_ = juce::Image(juce::Image::ARGB, physWidth, physHeight, true);
    }

    labelsSurface_.clear(labelsSurface_.getBounds());
    juce::Graphics g(labelsSurface_);
    g.addTransform(juce::AffineTransform::scale(renderScale_));
    drawPianoKeyboard(g, surfaceView_, juce::Rectangle<int>(0, 0, pianoKeyWidth_, getHeight()));
}

void PianoRollComponent::drawContent(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage,
                                     ContentLayer layer, const Items& items)
{
    const int w = getTimelineViewportBounds().getWidth();
    const int cw = getTimelineContentViewportWidth();
    const int ch = getTimelineContentViewportHeight();
    const auto timelineZone = juce::Rectangle<int>(getTimelineContentStartX(), rulerHeight_, cw, ch);
    const auto clipArea = damage.getIntersection(timelineZone);
    if (clipArea.isEmpty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(clipArea);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    const auto mapper = makeViewMapperForView(view);

    PianoRollRenderer::RenderContext renderCtx;
    renderCtx.width = w;
    renderCtx.height = ch;
    renderCtx.pianoKeyWidth = pianoKeyWidth_;
    renderCtx.contentStartX = getTimelineContentStartX();
    renderCtx.rulerHeight = 0;
    renderCtx.pixelsPerSecond = view.camera.pixelsPerSecond;
    renderCtx.pixelsPerSemitone = view.pixelsPerSemitone;
    renderCtx.minMidi = minMidi_;
    renderCtx.maxMidi = maxMidi_;
    renderCtx.scaleRootNote = scaleRootNote_;
    renderCtx.scaleType = scaleType_;
    renderCtx.noteNameMode = noteNameMode_;
    renderCtx.showUnvoicedFrames = showUnvoicedFrames_;
    renderCtx.showOriginalF0 = showOriginalF0_;
    renderCtx.showCorrectedF0 = showCorrectedF0_;
    renderCtx.correctedF0Colour = correctedF0Colour_;
    renderCtx.originalF0Colour = originalF0Colour_;
    renderCtx.coords = mapper;
    renderCtx.rasterBounds = clipArea;

    const bool notesPrimary = isOpenDyne();

    // 背景波形：OpenDyne 模式下 blob 即波形表达，不叠加背景 PCM。
    // 垂直位置由视口高度决定，不读取 verticalScrollOffset。
    if (layer == ContentLayer::Waveform) {
        if (!showWaveform_ || notesPrimary) return;
        for (const auto& placement : timelineContentPlacements_) {
            if (!placement.isValid()) continue;
            const auto it = items.find(placement.contentKey);
            if (it == items.end()) continue;
            const auto* mipmap = waveformMipmapCache_.get(placement.contentKey);
            if (mipmap == nullptr || !mipmap->hasSource())
                continue;
            // 峰值查询走 queryExactPeak 真实 PCM；缓存层级只加速查询，不影响像素
            renderer_->drawWaveform(g, renderCtx, PianoRollRenderer::PlacedContent{placement, it->second},
                                    *mipmap);
        }
        return;
    }

    // Notes 层按 placements 原顺序绘制；同 key 多 placement 共享同一 item
    std::vector<PianoRollRenderer::PlacedContent> placed;
    placed.reserve(timelineContentPlacements_.size());
    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid()) continue;
        const auto it = items.find(placement.contentKey);
        if (it == items.end()) continue;
        placed.push_back({placement, it->second});
    }

    // unvoiced bands 与 F0 curve 由 RenderContext 显示开关
    // （showUnvoicedFrames_/showOriginalF0_/showCorrectedF0_）决定，与 scheme 无关
    for (const auto& placedContent : placed)
        renderer_->drawUnvoicedFrameBands(g, renderCtx, placedContent);

    for (const auto& placedContent : placed)
        renderer_->drawNotes(g, renderCtx, placedContent);

    for (const auto& placedContent : placed)
        renderer_->drawF0Curve(g, renderCtx, placedContent);

    for (const auto& placedContent : placed)
        renderer_->drawTimeGridAnchors(g, renderCtx, placedContent);
}

void PianoRollComponent::rasterizeContentSurfaces(const Items& items)
{
    rasterizeContent(items);
    rasterizeWaveform(items);
}

void PianoRollComponent::rasterizeContent(
    const Items& items,
    std::optional<juce::Rectangle<int>> dirtyRect,
    std::optional<juce::Rectangle<int>> physicalDirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int w = getTimelineViewportBounds().getWidth();
    const int h = getTimelineViewportBounds().getHeight();
    if (!contentSurface_.isValid() || w <= 0 || h <= 0) return;

    const bool fullRaster = !dirtyRect.has_value();
    const auto rasterBounds = dirtyRect.value_or(juce::Rectangle<int>(0, 0, w, h));

    const auto physicalBounds = physicalDirtyRect.value_or(
        (rasterBounds.toFloat() * renderScale_).getSmallestIntegerContainer());
    if (fullRaster)
        contentSurface_.clear(contentSurface_.getBounds());
    else
        contentSurface_.clear(physicalBounds);

    juce::Graphics g(contentSurface_);
    if (physicalDirtyRect.has_value())
        g.reduceClipRegion(physicalBounds);
    g.addTransform(juce::AffineTransform::scale(renderScale_));

    drawContent(g, surfaceView_, rasterBounds, ContentLayer::Notes, items);

    if (fullRaster)
        contentDirty_ = false;

    recordRenderProbe(RenderProbePoint::ContentRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::rasterizeWaveform(
    const Items& items,
    std::optional<juce::Rectangle<int>> dirtyRect,
    std::optional<juce::Rectangle<int>> physicalDirtyRect)
{
    const int w = getTimelineViewportBounds().getWidth();
    const int h = getTimelineViewportBounds().getHeight();
    if (!waveformSurface_.isValid() || w <= 0 || h <= 0) return;

    const bool fullRaster = !dirtyRect.has_value();
    const auto rasterBounds = dirtyRect.value_or(juce::Rectangle<int>(0, 0, w, h));
    const auto physicalBounds = physicalDirtyRect.value_or(
        (rasterBounds.toFloat() * renderScale_).getSmallestIntegerContainer());
    if (fullRaster)
        waveformSurface_.clear(waveformSurface_.getBounds());
    else
        waveformSurface_.clear(physicalBounds);

    juce::Graphics g(waveformSurface_);
    if (physicalDirtyRect.has_value())
        g.reduceClipRegion(physicalBounds);
    g.addTransform(juce::AffineTransform::scale(renderScale_));

    // 水平相机与音符相同；垂直方向固定在视口中线，不吃 verticalScrollOffset。
    drawContent(g, surfaceView_, rasterBounds, ContentLayer::Waveform, items);
}

juce::Rectangle<int> PianoRollComponent::commitCameraDelta(bool exactCamera)
{
    const bool panPending = cameraPanPending_;
    cameraPanPending_ = false;
    const auto view = viewportState();
    const auto oldPps = surfaceView_.camera.pixelsPerSecond;
    const auto dx = panPending && view.camera.pixelsPerSecond == oldPps
        ? std::llround((view.camera.visibleStartSeconds - surfaceView_.camera.visibleStartSeconds) * oldPps)
        : 0;
    const int dy = panPending ? juce::roundToInt(view.verticalScrollOffset - surfaceView_.verticalScrollOffset) : 0;
    const int contentW = getTimelineContentViewportWidth();
    const int contentH = getTimelineContentViewportHeight();

    // 决定是否全量提交，再移动任意表面。位图和来源几何在同一 paint 更新。
    // exactCamera（缩放事务/未呈现缩放输入）要求来源与当前相机精确一致：
    // 状态不同（含增量条件覆盖不到的无 panPending 位移）推进来源并全量重绘；
    // 状态相同不重复栅格，停轮后同相机像素一致。
    if ((exactCamera && view != surfaceView_)
        || (staticDirty_ && contentDirty_)
        || view.camera.pixelsPerSecond != oldPps
        || view.pixelsPerSemitone != surfaceView_.pixelsPerSemitone
        || (dx != 0 && std::abs(dx) >= contentW)
        || (dy != 0 && std::abs(dy) >= contentH)
        || ((dx != 0 || dy != 0) && (staticDirty_ || contentDirty_))) {
        surfaceView_ = view;
        staticDirty_ = true;
        contentDirty_ = true;
        return {};
    }

    if (dx == 0 && dy == 0)
        return {};

    const auto viewport = getTimelineViewportBounds();
    const auto pitchArea = (viewport.withTop(rulerHeight_).toFloat() * renderScale_).getSmallestIntegerContainer();
    const auto timeline = (juce::Rectangle<int>(getTimelineContentStartX(), rulerHeight_, contentW, contentH)
                            .toFloat() * renderScale_).getSmallestIntegerContainer();
    const auto physicalDelta = [this](double origin, double delta) {
        return static_cast<int>(std::llround((origin + delta) * renderScale_)
                                - std::llround(origin * renderScale_));
    };
    const auto rasterizeStrip = [this](juce::Rectangle<int> physical) {
        rasterizeStatic((physical.toFloat() / renderScale_).getSmallestIntegerContainer(), physical);
    };

    juce::Rectangle<int> waveformDamage;
    if (const int shift = physicalDelta(surfaceView_.camera.visibleStartSeconds * oldPps, static_cast<double>(dx)); shift != 0) {
        surfaceView_.camera.visibleStartSeconds += static_cast<double>(dx) / oldPps;
        const int src = timeline.getX() + std::max(0, shift);
        const int dst = timeline.getX() + std::max(0, -shift);
        const int width = timeline.getWidth() - std::abs(shift);
        if (width > 0) {
            const double started = juce::Time::getMillisecondCounterHiRes();
            staticSurface_.moveImageSection(dst, timeline.getY(), src, timeline.getY(), width, timeline.getHeight());
            waveformSurface_.moveImageSection(dst, timeline.getY(), src, timeline.getY(), width, timeline.getHeight());
            contentSurface_.moveImageSection(dst, timeline.getY(), src, timeline.getY(), width, timeline.getHeight());
            recordRenderProbe(RenderProbePoint::MoveImageSection,
                              juce::Time::getMillisecondCounterHiRes() - started, 3);
        }

        const int exposed = std::min(std::abs(shift), timeline.getWidth());
        waveformDamage = shift > 0 ? timeline.withLeft(timeline.getRight() - exposed)
                                   : timeline.withWidth(exposed);
        rasterizeStrip(waveformDamage);
        rasterizeStatic({juce::Rectangle<int>(getTimelineContentStartX(), 0, contentW, rulerHeight_)},
                        {juce::Rectangle<int>(timeline.getX(), 0, timeline.getWidth(), timeline.getY())});
        pendingContentRaster_.offsetAll(-shift, 0);
        pendingContentRaster_.clipTo(timeline);
        pendingContentRaster_.add(waveformDamage);
    }

    if (const int shift = physicalDelta(surfaceView_.verticalScrollOffset, dy); shift != 0) {
        surfaceView_.verticalScrollOffset += static_cast<float>(dy);
        const int src = pitchArea.getY() + std::max(0, shift);
        const int dst = pitchArea.getY() + std::max(0, -shift);
        const int height = pitchArea.getHeight() - std::abs(shift);
        if (height > 0) {
            const double started = juce::Time::getMillisecondCounterHiRes();
            staticSurface_.moveImageSection(0, dst, 0, src, pitchArea.getWidth(), height);
            contentSurface_.moveImageSection(0, dst, 0, src, pitchArea.getWidth(), height);
            // 波形以视口为参考，纵向保持原位。
            recordRenderProbe(RenderProbePoint::MoveImageSection,
                              juce::Time::getMillisecondCounterHiRes() - started, 2);
        }

        const int exposed = std::min(std::abs(shift), pitchArea.getHeight());
        const auto strip = shift > 0 ? pitchArea.withTop(pitchArea.getBottom() - exposed)
                                     : pitchArea.withHeight(exposed);
        rasterizeStrip(strip);
        rasterizeLabels();
        pendingContentRaster_.offsetAll(0, -shift);
        pendingContentRaster_.clipTo(pitchArea);
        pendingContentRaster_.add(strip);
    }
    return waveformDamage;
}

void PianoRollComponent::applyVerticalPan()
{
    cameraPanPending_ = true;
    // 纵向滚动事件收口：offset 已由调用方更新并钳位；
    // 表面提交（移动/条带/来源推进）由 paint 的 commitCameraDelta 原子完成
    updateScrollBars();
    repaint();
}

void PianoRollComponent::handleHorizontalScrollPixels(double pixelDelta)
{
    const double pps = camera_.pixelsPerSecond;
    if (!(pps > 0.0) || pixelDelta == 0.0)
        return;
    userScrollHold_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        camera_.visibleStartSeconds - pixelDelta / pps,
        0.0,
        pps));
}

void PianoRollComponent::handleVerticalScrollPixels(double pixelDelta)
{
    if (pixelDelta == 0.0)
        return;
    verticalScrollOffset_ -= static_cast<float>(pixelDelta);
    const float totalHeight = getTotalHeight();
    const float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    const float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f)
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    else
        verticalScrollOffset_ = 0.0f;
    applyVerticalPan();
}

PianoRollComponent::Items PianoRollComponent::buildContentRenderItems()
{
    const double buildT0 = juce::Time::getMillisecondCounterHiRes();
    Items items;
    // 先按有效 placement 占位唯一 key，再逐 key 填 item，读取为空的条目删除
    for (const auto& placement : timelineContentPlacements_)
        if (placement.isValid())
            items.try_emplace(placement.contentKey);
    for (auto it = items.begin(); it != items.end();) {
        if (auto item = buildContentRenderItem(it->first)) {
            it->second = std::move(*item);
            ++it;
        } else {
            it = items.erase(it);
        }
    }

    // 集合构建入口统一结算（集合内部对 buildContentRenderItem 的调用不单独记录）；
    // 空集零样本零耗时
    if (!items.empty())
        recordRenderProbe(RenderProbePoint::SnapshotBuild,
                          juce::Time::getMillisecondCounterHiRes() - buildT0,
                          static_cast<int>(items.size()));
    return items;
}

bool PianoRollComponent::shouldShowPianoKeys() const noexcept
{
    return ::OpenTune::shouldShowPianoKeys(showPianoKeyboard_, isTimeView());
}

int PianoRollComponent::getTimelineContentStartX() const noexcept
{
    return shouldShowPianoKeys() ? pianoKeyWidth_ : 0;
}

bool PianoRollComponent::getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const
{
    const auto notes = getEditedContentNotesCopy();
    if (interactionState_.noteSelection.selectedIndices.size() != 1) {
        return false;
    }

    const int noteIndex = interactionState_.noteSelection.selectedIndices.front();
    if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
        return false;
    }

    const auto* selectedNote = &notes[static_cast<size_t>(noteIndex)];
    const float resolvedRetuneSpeed = selectedNote->retuneSpeed;
    const float resolvedVibratoDepth = selectedNote->vibratoDepth;
    const float resolvedVibratoRate = selectedNote->vibratoRate;

    retuneSpeedPercent = juce::jlimit(0.0f, 100.0f, resolvedRetuneSpeed * 100.0f);
    vibratoDepth = juce::jlimit(0.0f, 100.0f, resolvedVibratoDepth);
    vibratoRate = juce::jlimit(3.0f, 12.0f, resolvedVibratoRate);
    return true;
}

juce::String PianoRollComponent::describeParameterEdit(AudioEditingScheme::ParameterId id,
                                                       const AudioEditingScheme::ParameterEditResult& result) const
{
    using Status = AudioEditingScheme::ParameterEditStatus;
    if (result.status == Status::NoTarget)
        return LOC_RAW("Default for new notes");
    if (result.status == Status::Failed) {
        if (!currentCurve_ || currentF0Timeline().isEmpty())
            return LOC_RAW("No pitch data, read the audio first");
        return LOC_RAW("Could not apply");
    }
    // Note Split cuts the notes again, so the new count is what changed.
    if (id == AudioEditingScheme::ParameterId::NoteSplit)
        return Loc::format(LOC_RAW("Notes now: {0}"), juce::String(static_cast<int>(getCommittedNotes().size())));
    const auto count = static_cast<int>(interactionState_.noteSelection.selectedIndices.size());
    if (count > 0)
        return Loc::format(LOC_RAW("Applied to notes: {0}"), juce::String(count));
    return LOC_RAW("Applied to the selection");
}

AudioEditingScheme::ParameterEditResult PianoRollComponent::editParameter(AudioEditingScheme::ParameterId id, float value) {
    AudioEditingScheme::ParameterEditResult result;

    switch (id) {
        case AudioEditingScheme::ParameterId::RetuneSpeed:
            value = juce::jlimit(0.0f, 1.0f, value);
            break;
        case AudioEditingScheme::ParameterId::VibratoDepth:
            value = juce::jlimit(0.0f, 100.0f, value);
            break;
        case AudioEditingScheme::ParameterId::VibratoRate:
            value = juce::jlimit(3.0f, 12.0f, value);
            break;
        case AudioEditingScheme::ParameterId::NoteSplit:
            value = juce::jlimit(PitchControlConfig::kMinNoteSplitCents,
                                 PitchControlConfig::kMaxNoteSplitCents, value);
            break;
    }

    const bool hasSelectedNotes = !interactionState_.noteSelection.empty();
    // SelectedNotes 目标短路优先（resolveParameterTarget）：有音符选区时无需帧选区
    // 参与目标解析，跳过快照读取与选区扫描；无选区时显式帧选区直接读现有归并状态
    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotes;
    context.hasFrameSelection = hasSelectedNotes
        ? false
        : !interactionState_.frameSelection.ranges.empty();

    const auto target = AudioEditingScheme::resolveParameterTarget(context);

    if (target == AudioEditingScheme::ParameterTarget::None) {
        // 无目标：契约内回落为创建默认值更新（编辑器无需再补偿）
        setCreationDefault(id, value);
        result.status = AudioEditingScheme::ParameterEditStatus::NoTarget;
        return result;
    }

    if (!currentCurve_) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }

    auto notes = getEditedContentNotesCopy();
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }

    // NoteSplit 是区段属性，不按音符存储：更新创建默认值（策略阈值）后，
    // 有选中范围时对范围并集重新分割（复用 AUTO 范围重生成路径，含修正曲线重建），
    // 无选中范围时仅更新默认值（由编辑器回落 setCreationDefault）。
    if (id == AudioEditingScheme::ParameterId::NoteSplit) {
        segmentationPolicy_.transitionThresholdCents = value;

        F0FrameRange requestedRange{0, 0};
        if (target == AudioEditingScheme::ParameterTarget::SelectedNotes) {
            interactionState_.noteSelection.trimToNoteCount(static_cast<int>(notes.size()));
            double selStartTime = 1e30;
            double selEndTime = -1e30;
            for (int noteIndex : interactionState_.noteSelection.selectedIndices) {
                const auto& n = notes[static_cast<size_t>(noteIndex)];
                selStartTime = std::min(selStartTime, n.startTime);
                selEndTime = std::max(selEndTime, n.endTime);
            }
            if (selEndTime > selStartTime)
                requestedRange = f0tl.rangeForTimes(selStartTime, selEndTime);
        } else if (target == AudioEditingScheme::ParameterTarget::FrameSelection
                   && !interactionState_.frameSelection.ranges.empty()) {
            requestedRange = { interactionState_.frameSelection.ranges.front().first,
                               interactionState_.frameSelection.ranges.back().second };
        }

        if (requestedRange.endFrameExclusive <= requestedRange.startFrame) {
            // 无可分割范围：仅更新创建默认值（与 NoTarget 同语义）
            result.status = AudioEditingScheme::ParameterEditStatus::NoTarget;
            return result;
        }

        const auto postSnapCfg = makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);
        auto edit = contentCommands_->commitAutoTuneContentRangeUndoable(
            editedContentKey_, requestedRange.startFrame, requestedRange.endFrameExclusive,
            getCurrentAutoTuneParams(), postSnapCfg, LOC_RAW("Split note"));
        if (edit.snapshot == nullptr) {
            result.status = AudioEditingScheme::ParameterEditStatus::Failed;
            return result;
        }

        lastKnownNotesRevision_ = edit.snapshot->notesRevision;
        lastKnownPitchRevision_ = edit.snapshot->pitchRevision;
        setEditedContent(editedContentKey_, edit.snapshot->pitchCurve, audioBuffer_, audioBufferSampleRate_);
        pendingUndoDescription_ = {};

        result.status = AudioEditingScheme::ParameterEditStatus::Applied;
        result.changed = true;
        return result;
    }

    if (target == AudioEditingScheme::ParameterTarget::SelectedNotes) {
        interactionState_.noteSelection.trimToNoteCount(static_cast<int>(notes.size()));
        if (processor_ == nullptr) {
            result.status = AudioEditingScheme::ParameterEditStatus::Failed;
            return result;
        }

        const auto edit = processor_->editSelectedNoteParameter(
            editedContentKey_,
            interactionState_.noteSelection.selectedIndices,
            lastKnownNotesRevision_,
            id,
            value,
            currentRetuneSpeed_,
            currentVibratoDepth_,
            currentVibratoRate_,
            LOC_RAW("Edit note"));
        result.status = edit.status;
        result.changed = edit.changed;
        if (edit.status != AudioEditingScheme::ParameterEditStatus::Applied || !edit.changed) {
            if (edit.contentSnapshot != nullptr) {
                parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
                cachedNotes_ = edit.contentSnapshot->notes;
                interactionState_.noteSelection.clear();
                if (edit.contentSnapshot->pitchCurve != nullptr)
                    applyEditedContentCurve(edit.contentSnapshot->pitchCurve);
                lastKnownNotesRevision_ = edit.contentSnapshot->notesRevision;
                lastKnownPitchRevision_ = edit.contentSnapshot->pitchRevision;
                requestContentRedraw();
                parameterTargetDidChange();
            }
            return result;
        }

        const auto affectedRange = edit.affectedRange;
        if (!affectedRange.isEmpty()) {
            const auto committedSnapshot = edit.contentSnapshot;
            if (committedSnapshot == nullptr) {
                result.status = AudioEditingScheme::ParameterEditStatus::Failed;
                return result;
            }
            cachedNotes_ = committedSnapshot->notes;
            interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
            if (committedSnapshot->pitchCurve != nullptr)
                applyEditedContentCurve(committedSnapshot->pitchCurve);
            pendingUndoDescription_ = {};
            lastKnownNotesRevision_ = committedSnapshot->notesRevision;
            lastKnownPitchRevision_ = committedSnapshot->pitchRevision;
            requestContentRedraw();
        }
        return result;
    }

    // FrameSelection 目标（SelectedNotes 已短路，此处必为无音符选区）：
    // 显式帧选区直接读现有归并状态，committed 在实际消费时读取一次
    const auto& frameSel = interactionState_.frameSelection;
    const auto& contentSnapshot = readEditedSnapshot();
    if (contentSnapshot == nullptr) {
        result.status = AudioEditingScheme::ParameterEditStatus::Failed;
        return result;
    }

    if (target == AudioEditingScheme::ParameterTarget::FrameSelection) {
        float effectiveRetuneSpeed = currentRetuneSpeed_;
        float effectiveVibratoDepth = currentVibratoDepth_;
        float effectiveVibratoRate = currentVibratoRate_;
        switch (id) {
            case AudioEditingScheme::ParameterId::RetuneSpeed: effectiveRetuneSpeed = value; break;
            case AudioEditingScheme::ParameterId::VibratoDepth: effectiveVibratoDepth = value; break;
            case AudioEditingScheme::ParameterId::VibratoRate: effectiveVibratoRate = value; break;
        }
        auto scratchCurve = PitchCurve::fromSnapshot(currentCurve_);
        auto snap = currentCurve_;
        bool anyBaked = false;
        for (const auto& [startFrame, endFrameExclusive] : frameSel.ranges) {
            if (endFrameExclusive <= startFrame) continue;
            if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) continue;
            snap = scratchCurve->applyCorrectionToRange(
                notes,
                startFrame,
                endFrameExclusive,
                static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
                effectiveRetuneSpeed,
                effectiveVibratoDepth,
                effectiveVibratoRate);
            anyBaked = true;
        }
        if (!anyBaked) {
            result.status = AudioEditingScheme::ParameterEditStatus::Failed;
            return result;
        }

        auto allSegments = snap->getCorrectionSegments();
        int unionStart = frameSel.ranges.front().first;
        int unionEnd = frameSel.ranges.front().second;
        for (const auto& [s, e] : frameSel.ranges) {
            unionStart = std::min(unionStart, s);
            unionEnd = std::max(unionEnd, e);
        }
        const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
            unionStart, unionEnd, f0tl.endFrameExclusive());

        pendingUndoDescription_ = LOC_RAW("Edit note");
        if (!commitEditedContentNotesAndSegments(*contentSnapshot, notes, allSegments, affectedRange)) {
            result.status = AudioEditingScheme::ParameterEditStatus::Failed;
            return result;
        }
        result.status = AudioEditingScheme::ParameterEditStatus::Applied;
        result.changed = true;
        return result;
    }

    result.status = AudioEditingScheme::ParameterEditStatus::NoTarget;
    return result;
}

void PianoRollComponent::setCreationDefault(AudioEditingScheme::ParameterId id, float value) {
    switch (id) {
        case AudioEditingScheme::ParameterId::RetuneSpeed:
            currentRetuneSpeed_ = juce::jlimit(0.0f, 1.0f, value);
            break;
        case AudioEditingScheme::ParameterId::VibratoDepth:
            currentVibratoDepth_ = juce::jlimit(0.0f, 100.0f, value);
            break;
        case AudioEditingScheme::ParameterId::VibratoRate:
            currentVibratoRate_ = juce::jlimit(3.0f, 12.0f, value);
            break;
        case AudioEditingScheme::ParameterId::NoteSplit:
            segmentationPolicy_.transitionThresholdCents = juce::jlimit(
                PitchControlConfig::kMinNoteSplitCents,
                PitchControlConfig::kMaxNoteSplitCents, value);
            break;
    }
}

int PianoRollComponent::findLineAnchorSegmentNear(int x, int y) const
{
    if (currentCurve_ == nullptr) return -1;

    const auto& allSegments = currentCurve_->getCorrectionSegments();

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1;
    const float tolerancePixels = 15.0f;

    int bestIdx = -1;
    float bestDist = tolerancePixels;

    for (int i = 0; i < static_cast<int>(allSegments.size()); ++i) {
        const auto& seg = allSegments[i];
        if (seg.source != PitchCorrectionSegment::Source::LineAnchor) continue;
        if (seg.f0Data.empty()) continue;

        const double startTime = f0tl.timeAtFrame(seg.startFrame);
        const double endTime   = f0tl.timeAtFrame(seg.endFrame);

        const int startX = sourceTimeToX(startTime);
        const int endX   = sourceTimeToX(endTime);

        if (x < startX - tolerancePixels || x > endX + tolerancePixels) continue;

        const double clickSource = xToSourceTime(x);
        const double relT = juce::jlimit(0.0, 1.0,
            (clickSource - startTime) / (endTime - startTime));
        const int f0Idx = juce::jlimit(0, static_cast<int>(seg.f0Data.size()) - 1,
                                       static_cast<int>(relT * (seg.f0Data.size() - 1)));

        const float segFreq = seg.f0Data[f0Idx];
        if (segFreq <= 0.0f) continue;

        const float segY = makeViewMapper().freqToY(segFreq);
        const float dist = std::abs(segY - static_cast<float>(y));

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }

    return bestIdx;
}

void PianoRollComponent::selectLineAnchorSegment(int idx)
{
    interactionState_.selectedLineAnchorSegmentIds.clear();
    if (idx >= 0) {
        interactionState_.selectedLineAnchorSegmentIds.push_back(idx);
    }
}

void PianoRollComponent::toggleLineAnchorSegmentSelection(int idx)
{
    auto it = std::find(interactionState_.selectedLineAnchorSegmentIds.begin(), interactionState_.selectedLineAnchorSegmentIds.end(), idx);
    if (it != interactionState_.selectedLineAnchorSegmentIds.end()) {
        interactionState_.selectedLineAnchorSegmentIds.erase(it);
    } else {
        interactionState_.selectedLineAnchorSegmentIds.push_back(idx);
    }
}

void PianoRollComponent::clearLineAnchorSegmentSelection()
{
    interactionState_.selectedLineAnchorSegmentIds.clear();
}

void PianoRollComponent::setZoomControlsArea(juce::Rectangle<int> area)
{
    zoomControlsArea_ = area;
    layoutZoomControls();
}

void PianoRollComponent::resized() {

    auto bounds = getLocalBounds();

    verticalScrollBar_.setBounds(bounds.removeFromRight(UIColors::scrollBarThickness));

    const float maxVerticalScroll = juce::jmax(0.0f, getTotalHeight() - static_cast<float>(getTimelineContentViewportHeight()));
    verticalScrollOffset_ = juce::jlimit(0.0f, maxVerticalScroll, verticalScrollOffset_);

    // Position toggle buttons in top right of ruler
    // 历史布局：timeUnit 在左，scrollMode 在右，间距 5，y=5，btnW=50，btnH=20
    int btnW = 50;
    int btnH = 20;
    int spacing = 5;
    int scrollModeX = getWidth() - spacing - btnW;
    int timeUnitX = scrollModeX - spacing - btnW;

    timeUnitToggleButton_.setBounds(timeUnitX, 5, btnW, btnH);
    scrollModeToggleButton_.setBounds(scrollModeX, 5, btnW, btnH);

    timeUnitToggleButton_.toFront(false);
    scrollModeToggleButton_.toFront(false);

    staticDirty_ = true;
    contentDirty_ = true;

    layoutZoomControls();

    // 初始 F0 定位更新视口，保留表面由下一次 paint 提交
    if (!tryConsumeInitialF0View(editedContentKey_)) {
        updateScrollBars();

        repaint();
    }

    horizontalZoomSlider_.toFront(false);
    verticalZoomSlider_.toFront(false);
}

void PianoRollComponent::applyEditedContentCurve(std::shared_ptr<const PitchCurveSnapshot> curve)
{
    currentCurve_ = std::move(curve);
}

void PianoRollComponent::applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                                       double sampleRate)
{
    audioBuffer_ = std::move(buffer);
    audioBufferSampleRate_ = sampleRate;

    if (editedContentKey_.isValid() && audioBuffer_ != nullptr && audioBuffer_->getNumSamples() > 0) {
        waveformMipmapCache_.setAudioSource(editedContentKey_, audioBuffer_);
    } else if (editedContentKey_.isValid() && waveformMipmapCache_.get(editedContentKey_) != nullptr) {
        waveformMipmapCache_.remove(editedContentKey_);
    }
}

double PianoRollComponent::getContentDurationSeconds() const
{
    return activeContentProjection().contentDurationSeconds;
}

const PianoRollComponent::TimelineContentPlacement* PianoRollComponent::findEditedPlacement() const noexcept
{
    if (!editedContentKey_.isValid())
        return nullptr;
    for (const auto& placement : timelineContentPlacements_)
    {
        if (placement.contentKey == editedContentKey_ && placement.projection.isValid())
            return &placement;
    }
    return nullptr;
}

ContentTimelineProjection PianoRollComponent::activeContentProjection() const noexcept
{
    if (const auto* placement = findEditedPlacement())
        return placement->projection;
    return {};
}

double PianoRollComponent::sourceTimeToTimelineTime(double sourceSeconds) const
{
    const auto projection = activeContentProjection();
    if (!projection.isValid())
        return 0.0;
    const auto snap = readEditedSnapshot();
    if (!snap)
        return 0.0;
    const PianoRollTimeMap map(projection, *snap->timeGrid, makeViewMapper());
    return map.sourceToTimeline(sourceSeconds);
}

int PianoRollComponent::sourceTimeToX(double sourceSeconds) const
{
    return makeViewMapper().timeToX(sourceTimeToTimelineTime(sourceSeconds));
}

double PianoRollComponent::xToSourceTime(int x) const
{
    const auto projection = activeContentProjection();
    if (!projection.isValid())
        return 0.0;

    const auto snap = readEditedSnapshot();
    if (!snap)
        return 0.0;

    const PianoRollTimeMap map(projection, *snap->timeGrid, makeViewMapper());
    return map.xToSource(x);
}

SourceEditRange PianoRollComponent::sourceEditRange() const
{
    const auto snap = readEditedSnapshot();
    if (!snap)
        return { 0.0, 0.0, 0.0 };
    return SourceEditRange::fromTimeGrid(*snap->timeGrid, 0.0);
}

bool PianoRollComponent::applyTimelineContentPlacements(std::vector<TimelineContentPlacement> placements,
                                                               bool explicitContract)
{
    placements.erase(std::remove_if(placements.begin(),
                                    placements.end(),
                                    [](const auto& placement) { return !placement.isValid(); }),
                     placements.end());

    const bool changed = placements.size() != timelineContentPlacements_.size()
        || !std::equal(placements.begin(), placements.end(), timelineContentPlacements_.begin(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.contentKey == rhs.contentKey
                    && lhs.displayColour == rhs.displayColour
                    && std::abs(lhs.projection.timelineStartSeconds - rhs.projection.timelineStartSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.timelineDurationSeconds - rhs.projection.timelineDurationSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.contentStartSeconds - rhs.projection.contentStartSeconds) <= 1.0e-9
                    && std::abs(lhs.projection.contentDurationSeconds - rhs.projection.contentDurationSeconds) <= 1.0e-9;
            });

    bool audioChanged = false;
    std::set<ContentKey> aliveContents;
    for (const auto& placement : placements) {
        if (!aliveContents.insert(placement.contentKey).second)
            continue;
        if (const auto snapshot = readSnapshotFor(placement.contentKey))
            audioChanged |= waveformMipmapCache_.setAudioSource(placement.contentKey, snapshot->audioBuffer);
    }
    waveformMipmapCache_.prune(aliveContents);

    explicitTimelineContentPlacements_ = explicitContract;
    if (!changed)
        return audioChanged;

    timelineContentPlacements_ = std::move(placements);

    userScrollHold_ = false;
    updateScrollBars();
    return true;
}

void PianoRollComponent::setTrackDisplayColour(juce::Colour colour)
{
    if (trackDisplayColour_ == colour)
        return;
    trackDisplayColour_ = colour;
    if (!explicitTimelineContentPlacements_)
    {
        deriveSingleTimelineContentPlacement();
        requestContentRedraw();
    }
}

void PianoRollComponent::deriveSingleTimelineContentPlacement()
{
    if (explicitTimelineContentPlacements_) {
        return;
    }

    std::vector<TimelineContentPlacement> placements;
    if (editedContentKey_.isValid() && pendingSingleContentProjection_.isValid()) {
        TimelineContentPlacement placement;
        placement.contentKey = editedContentKey_;
        placement.projection = pendingSingleContentProjection_;
        placement.displayColour = trackDisplayColour_;
        placements.push_back(std::move(placement));
    }

    applyTimelineContentPlacements(std::move(placements), false);
}

bool PianoRollComponent::setContentProjection(const ContentTimelineProjection& projection)
{
    const bool changed = std::abs(pendingSingleContentProjection_.timelineStartSeconds - projection.timelineStartSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.timelineDurationSeconds - projection.timelineDurationSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.contentStartSeconds - projection.contentStartSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.contentDurationSeconds - projection.contentDurationSeconds) > 1.0e-9;

    if (!changed) {
        return false;
    }

    pendingSingleContentProjection_ = projection;
    explicitTimelineContentPlacements_ = false;
    deriveSingleTimelineContentPlacement();
    userScrollHold_ = false;
    requestContentRedraw();
    return true;
}

void PianoRollComponent::setTimelineContentPlacements(std::vector<TimelineContentPlacement> placements)
{
    if (applyTimelineContentPlacements(std::move(placements), true)) {
        pendingSingleContentProjection_ = activeContentProjection();
        requestContentRedraw();
    }
}

void PianoRollComponent::setEditedContent(ContentKey contentKey,
                                           std::shared_ptr<const PitchCurveSnapshot> curve,
                                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                           double sampleRate,
                                           bool activePlacementChanged)
{
    applyEditedContent(contentKey, std::move(curve), std::move(buffer), sampleRate,
                       activePlacementChanged, false);
}

bool PianoRollComponent::setEditedContentAndProjection(ContentKey contentKey,
                                                        std::shared_ptr<const PitchCurveSnapshot> curve,
                                                        std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                                        double sampleRate,
                                                        const ContentTimelineProjection& projection,
                                                        bool activePlacementChanged)
{
    const bool projectionChanged = std::abs(pendingSingleContentProjection_.timelineStartSeconds - projection.timelineStartSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.timelineDurationSeconds - projection.timelineDurationSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.contentStartSeconds - projection.contentStartSeconds) > 1.0e-9
        || std::abs(pendingSingleContentProjection_.contentDurationSeconds - projection.contentDurationSeconds) > 1.0e-9;
    const bool contentChanged = editedContentKey_ != contentKey;
    const bool curveChanged = currentCurve_ != curve;
    const bool bufferChanged = audioBuffer_ != buffer || audioBufferSampleRate_ != sampleRate;

    if (!projectionChanged && !contentChanged && !activePlacementChanged && !curveChanged && !bufferChanged)
        return false;

    pendingSingleContentProjection_ = projection;
    explicitTimelineContentPlacements_ = false;
    applyEditedContent(contentKey, std::move(curve), std::move(buffer), sampleRate,
                       activePlacementChanged, true);

    if (!contentChanged && !bufferChanged)
    {
        deriveSingleTimelineContentPlacement();
        userScrollHold_ = false;
        updateScrollBars();
    }

    requestContentRedraw();

    return projectionChanged;
}

void PianoRollComponent::applyEditedContent(ContentKey contentKey,
                                             std::shared_ptr<const PitchCurveSnapshot> curve,
                                             std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                             double sampleRate,
                                             bool activePlacementChanged,
                                             bool deferRedraw)
{
    const bool contentChanged = editedContentKey_ != contentKey;
    const bool curveChanged = currentCurve_ != curve;
    const bool bufferChanged = audioBuffer_ != buffer || audioBufferSampleRate_ != sampleRate;

    if (!contentChanged && !activePlacementChanged && !curveChanged && !bufferChanged) {
        return;
    }

    const auto incomingSnapshot = contentChanged ? nullptr : readEditedSnapshot();
    const bool externalRevisionChanged = incomingSnapshot != nullptr
        && (incomingSnapshot->notesRevision != lastKnownNotesRevision_
            || incomingSnapshot->pitchRevision != lastKnownPitchRevision_);
    if (contentChanged || externalRevisionChanged)
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);

    if (activePlacementChanged) {
        // Pending seeks belong to the active placement, not just its content.
        pendingSeekTime_ = -1.0;
        pendingSeekEpoch_ = 0;
        pendingSeekHostWasPlaying_ = false;
    }

    if (contentChanged) {
        // 切换编辑目标：EQ 弹窗由选中组驱动，内容切换即失效
        closeEqPopup();
        stopNotePitchAudition();
        cancelPendingAudition();
        if (!contentKey.isValid())
            pendingInitialF0ViewContentKey_ = ContentKey{};
        editedContentKey_ = contentKey;
        // 切换编辑目标：清除全部拖拽/绘制瞬态与 note draft（noteDraft 是
        // interactionState_ 成员，resetTransient 的 noteDraft.clear() 已覆盖）
        interactionState_.resetTransient();
        pendingUndoDescription_ = {};
        lastKnownNotesRevision_ = 0;
        lastKnownPitchRevision_ = 0;
        lastKnownTimeGridRevision_ = 0;
        // 切换编辑目标时清空全部选择状态，避免旧 clip 选择残留
        interactionState_.noteSelection.clear();
        interactionState_.frameSelection.clear();
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.isSelectingF0 = false;
        interactionState_.selection.f0SelectionAnchorFrame = -1;
        interactionState_.selection.f0SelectionCurrentFrame = -1;
        interactionState_.selection.selectionStartTime = 0.0;
        interactionState_.selection.selectionEndTime = 0.0;
        interactionState_.selection.selectionStartMidi = 0.0f;
        interactionState_.selection.selectionEndMidi = 0.0f;
    }

    // notes 和 pitchCurve 通过 commitNotesAndPitchCurve 同步写入 store
    // 读侧也必须同步读：curveChanged 时需 refresh notes，否则 undo/redo 后不一致
    // 曲线回退后 notes 视觉残留的不对称（cachedNotes_ 滞后）。
    if (contentChanged || curveChanged) {
        const auto snapshot = refreshEditedContentNotes();
        if (contentChanged) {
            if (snapshot) {
                lastKnownNotesRevision_ = snapshot->notesRevision;
                lastKnownPitchRevision_ = snapshot->pitchRevision;
                lastKnownTimeGridRevision_ = snapshot->timeGridRevision;
            }
        }
    }

    if (contentChanged || curveChanged) {
        applyEditedContentCurve(std::move(curve));
    }

    if (contentChanged || bufferChanged) {
        applyEditedContentAudioBuffer(std::move(buffer), sampleRate);
    }

    if (contentChanged || bufferChanged) {
        deriveSingleTimelineContentPlacement();
    }

    userScrollHold_ = false;
    updateScrollBars();
    if (deferRedraw) {
        contentDirty_ = true;
    } else if (contentChanged || curveChanged)
        requestContentRedraw();
    else {
        contentDirty_ = true;

        repaint();
    }

    if (contentChanged || externalRevisionChanged)
        parameterTargetDidChange();
}


void PianoRollComponent::requestInitialF0View(ContentKey contentKey)
{
    if (!contentKey.isValid())
        return;

    pendingInitialF0ViewContentKey_ = contentKey;
    tryConsumeInitialF0View(contentKey);
}

namespace {

bool clipHasCenterableNote(const std::vector<Note>& notes)
{
    for (const auto& note : notes)
    {
        const CenterableNote sample { note.startTime, note.endTime, note.getAdjustedPitch() };
        if (isCenterableNote(sample))
            return true;
    }
    return false;
}

} // namespace

bool PianoRollComponent::prepareNoteCenterOnClipEntry()
{
    if (!clipHasCenterableNote(getCommittedNotes()))
        return false;

    // 进入后 F0 Ready 会沿同一条 pending 把镜头改到首帧。这里先取消，
    // 让随后的 centerNoteNearestPlayhead 成为这次进入的唯一镜头。
    pendingInitialF0ViewContentKey_ = ContentKey{};
    return true;
}

bool PianoRollComponent::centerNoteNearestPlayhead(double pixelsPerSecond)
{
    const auto& notes = getCommittedNotes();
    const int viewportW = getTimelineContentViewportWidth();
    const int viewportH = getTimelineContentViewportHeight();
    if (viewportW <= 0 || viewportH <= 0)
        return false;

    const double pps = pixelsPerSecond > 0.0 ? pixelsPerSecond : camera_.pixelsPerSecond;
    if (!(pps > 0.0) || !std::isfinite(pps))
        return false;

    const auto* placement = findEditedPlacement();
    if (placement == nullptr)
        return false;
    const auto snap = readSnapshotFor(placement->contentKey);
    if (snap == nullptr || snap->timeGrid == nullptr)
        return false;

    const PianoRollTimeMap map(placement->projection, *snap->timeGrid, makeViewMapper());
    double playhead = playHeadState_.getPresentedPositionSeconds();
    if (!std::isfinite(playhead))
        playhead = 0.0;

    std::vector<CenterableNote> onTimeline;
    onTimeline.reserve(notes.size());
    for (const auto& note : notes)
    {
        onTimeline.push_back({
            map.sourceToTimeline(note.startTime),
            map.sourceToTimeline(note.endTime),
            note.getAdjustedPitch()
        });
    }

    const int index = noteIndexNearestTime(onTimeline.data(), static_cast<int>(onTimeline.size()), playhead);
    if (index < 0)
        return false;

    const double timelineSeconds = noteCenterTimeNearest(playhead, onTimeline[static_cast<size_t>(index)]);
    if (!std::isfinite(timelineSeconds))
        return false;

    const float noteMidi = makeViewMapper().freqToMidi(notes[static_cast<size_t>(index)].getAdjustedPitch());
    if (!std::isfinite(noteMidi))
        return false;

    // 保持当前音高缩放，只把该音高车道滚到内容区中央。
    verticalScrollOffset_ = (maxMidi_ - noteMidi) * pixelsPerSemitone_
        - static_cast<float>(viewportH) * 0.5f;
    const float maxScroll = juce::jmax(0.0f, getTotalHeight() - static_cast<float>(viewportH));
    verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);

    // 挡住播放跟随，直到用户自己平移或缩放。Kind::Cont 用现有横向缩放把时间点放在视口中央。
    userScrollHold_ = true;
    staticDirty_ = true;
    contentDirty_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Cont,
        timelineSeconds,
        0.0,
        pps));
    updateScrollBars();
    repaint();
    return true;
}

void PianoRollComponent::onTimeGridRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->timeGridRevision : 0;
    if (rev == lastKnownTimeGridRevision_) return;
    lastKnownTimeGridRevision_ = rev;
    requestContentRedraw();
}

void PianoRollComponent::onNotesRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->notesRevision : 0;
    if (rev == lastKnownNotesRevision_) return;
    lastKnownNotesRevision_ = rev;
    closeEqPopup();
    parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
    interactionState_.noteSelection.clear();
    refreshEditedContentNotes();
    requestContentRedraw();
    parameterTargetDidChange();
}

void PianoRollComponent::onPitchRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->pitchRevision : 0;
    if (rev == lastKnownPitchRevision_) return;
    parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
    lastKnownPitchRevision_ = rev;
    requestContentRedraw();
    parameterTargetDidChange();
}

void PianoRollComponent::requestContentRedraw() {
    syncNoteAccessibilityItems();
    contentDirty_ = true;
    repaint();
}

void PianoRollComponent::requestThemeRedraw() {
    staticDirty_ = true;
    contentDirty_ = true;
    repaint();
}

juce::Rectangle<int> PianoRollComponent::getTimelineViewportBounds() const
{
    // 水平滚动条已由 TimelineOverview 永久替代；垂直滚动条在右侧占位。
    const int viewportWidth = juce::jmax(0, getWidth() - verticalScrollBar_.getWidth());
    return { 0, 0, viewportWidth, getHeight() };
}

juce::Rectangle<int> PianoRollComponent::timeAxisRect() const
{
    return juce::Rectangle<int>(getTimelineContentStartX(), 0,
        getTimelineContentViewportWidth(),
        rulerHeight_ + getTimelineContentViewportHeight());
}

TimelineViewportRequest PianoRollComponent::makeViewportRequest(
    TimelineViewportRequest::Kind kind,
    double targetTime,
    double anchorViewportX,
    double pps) const
{
    TimelineViewportRequest req;
    req.kind = kind;
    req.viewKind = TimelineViewportRequest::ViewKind::PianoRoll;
    req.targetTime = targetTime;
    req.currentVisibleStartSeconds = camera_.visibleStartSeconds;
    req.anchorViewportX = anchorViewportX;
    req.viewportWidth = getTimelineContentViewportWidth();
    req.pixelsPerSecond = pps;
    return req;
}

bool PianoRollComponent::tryConsumeInitialF0View(ContentKey contentKey)
{
    if (pendingInitialF0ViewContentKey_ != contentKey
        || contentKey != editedContentKey_) {
        return false;
    }

    const auto snapshot = readSnapshotFor(contentKey);
    if (snapshot == nullptr)
        return false;

    // 终态收束（在可见性 guard 之前）：Failed 直接清空 pending；Ready 但当前
    // projection 的 content 区间内没有有效 F0 也清空 pending。
    // NotRequested/Extracting、curve/TimeGrid/projection/几何未就绪则继续保留。
    if (snapshot->originalF0State != OriginalF0State::Ready) {
        if (snapshot->originalF0State == OriginalF0State::Failed)
            pendingInitialF0ViewContentKey_ = ContentKey{};
        return false;
    }

    if (currentCurve_ == nullptr)
        return false;

    const auto curveSnapshot = currentCurve_;
    const auto projection = activeContentProjection();
    if (snapshot->timeGrid->empty()
        || !projection.isValid()
        || curveSnapshot->getHopSize() <= 0
        || !std::isfinite(curveSnapshot->getSampleRate())
        || curveSnapshot->getSampleRate() <= 0.0) {
        return false;
    }

    const F0Timeline f0Timeline(curveSnapshot->getHopSize(),
                                curveSnapshot->getSampleRate(),
                                static_cast<int>(curveSnapshot->size()));
    if (f0Timeline.isEmpty()) {
        pendingInitialF0ViewContentKey_ = ContentKey{};
        return false;
    }

    // 仅在当前 projection 的 content 区间 [contentStartSeconds, contentStartSeconds
    // + contentDurationSeconds) 内寻找首个有效 F0：不得定位到 Trim 左侧已裁剪的 F0。
    const auto& originalF0 = curveSnapshot->getOriginalF0();
    const double contentEndSeconds = projection.contentStartSeconds + projection.contentDurationSeconds;
    int firstFrame = -1;
    float startFrequency = 0.0f;
    double firstContentSeconds = 0.0;
    for (int frame = 0; frame < static_cast<int>(originalF0.size()); ++frame) {
        const float f0 = originalF0[static_cast<size_t>(frame)];
        if (!(std::isfinite(f0) && f0 >= 20.0f && f0 <= 2000.0f))
            continue;
        const double contentSeconds = snapshot->timeGrid->tauForward(f0Timeline.timeAtFrame(frame));
        if (contentSeconds >= projection.contentStartSeconds && contentSeconds < contentEndSeconds) {
            firstFrame = frame;
            startFrequency = f0;
            firstContentSeconds = contentSeconds;
            break;
        }
    }

    // Ready 但当前 projection 区间内没有有效 F0 → 终态，清空 pending。
    if (firstFrame < 0) {
        pendingInitialF0ViewContentKey_ = ContentKey{};
        return false;
    }

    // 正常 Ready 定位仍需可见性与有效几何。
    const int contentWidth = getTimelineContentViewportWidth();
    const int contentHeight = getTimelineContentViewportHeight();
    if (!isShowing() || !isVisible()
        || contentWidth <= 0
        || contentHeight <= 0
        || camera_.pixelsPerSecond <= 0.0) {
        return false;
    }

    const double timelineSeconds = projection.projectContentTimeToTimeline(firstContentSeconds);
    if (!std::isfinite(timelineSeconds))
        return false;

    const auto request = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        timelineSeconds,
        0.0,
        camera_.pixelsPerSecond);
    const auto resolvedCamera = TimelineViewportPolicy::resolve(request);
    const double visibleEndSeconds = TimelineViewportPolicy::visibleEndSeconds(resolvedCamera,
                                                                                 contentWidth);
    const auto mapper = makeViewMapper();
    const PianoRollTimeMap timeMap(projection, *snapshot->timeGrid, mapper);
    float highestMidi = -std::numeric_limits<float>::infinity();
    float lowestMidi = std::numeric_limits<float>::infinity();
    for (const auto& note : getCommittedNotes()) {
        const float adjustedPitch = note.getAdjustedPitch();
        if (!(std::isfinite(adjustedPitch) && adjustedPitch > 0.0f))
            continue;

        const double noteTimelineStart = timeMap.sourceToTimeline(note.startTime);
        const double noteTimelineEnd = timeMap.sourceToTimeline(note.endTime);
        if (!std::isfinite(noteTimelineStart) || !std::isfinite(noteTimelineEnd)
            || noteTimelineEnd <= resolvedCamera.visibleStartSeconds
            || noteTimelineStart >= visibleEndSeconds) {
            continue;
        }

        const float noteMidi = mapper.freqToMidi(adjustedPitch);
        if (!std::isfinite(noteMidi))
            continue;
        highestMidi = std::max(highestMidi, noteMidi);
        lowestMidi = std::min(lowestMidi, noteMidi);
    }

    if (highestMidi > lowestMidi) {
        // +2 semitones padding: +1 for highest note's top half, +1 for lowest note's bottom half
        const float midiSpan = highestMidi - lowestMidi + 2.0f;
        pixelsPerSemitone_ = juce::jlimit(kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
                                          static_cast<float>(contentHeight) / midiSpan);
        verticalScrollOffset_ = (maxMidi_ - highestMidi - 1.0f) * pixelsPerSemitone_;
    } else {
        const float startMidi = mapper.freqToMidi(startFrequency);
        verticalScrollOffset_ = (maxMidi_ - startMidi) * pixelsPerSemitone_ - contentHeight * 0.5f;
    }
    verticalScrollOffset_ = std::clamp(verticalScrollOffset_, 0.0f, juce::jmax(0.0f, getTotalHeight() - static_cast<float>(contentHeight)));

    // 纵向变化 + 相机定位 → 标记两层全量失效
    staticDirty_ = true;
    contentDirty_ = true;
    activateTimelineCamera(resolvedCamera);
    pendingInitialF0ViewContentKey_ = ContentKey{};
    return true;
}

void PianoRollComponent::onHeartbeatTick()
{
#if !JucePlugin_Build_Standalone
    // 终态收束先于可见性 guard：隐藏 PianoRoll 也能在 viewToggled 计算 preserve 前
    // 结算 Failed/Ready-无目标 的 pending；正常 Ready 定位仍需 isShowing/isVisible。
    tryConsumeInitialF0View(editedContentKey_);
#endif

    // 波形缓存增量构建在可见性 guard 之前：导入音频后即使钢琴卷帘尚未显示也预先构建；
    // 层级完成只加速 queryExactPeak 查询，不改变像素，不放脏、不 repaint
    if (showWaveform_ || isOpenDyne())
        waveformMipmapCache_.buildIncremental(0.75);

    if (!isShowing())
        return;

    // 缩放事务截止：按既有心跳计数递减；到 0 仅结束事务，
    // 像素已与当前相机一致，不强制放脏 repaint
    if (zoomDeadlineTicks_ > 0)
        --zoomDeadlineTicks_;
}

void PianoRollComponent::onScrollVBlankCallback(double timestampSec)
{
    if (!isShowing()) {
        return;
    }

    lastVBlankMs_ = juce::Time::getMillisecondCounterHiRes();
    juce::ignoreUnused(timestampSec);

    const bool playingNow = playHeadState_.isPlaying.load(std::memory_order_relaxed);
    bool playStateChanged = (playingNow != lastObservedPlayHeadPlaying_);

    // ── Playhead hit bookkeeping ──
    // 命中判定在 source 域完成，但各活动 placement 有各自的 projection + committed
    // timeGrid，因此命中结果必须携带来源 placement 的映射，damage bounds 才能用
    // 同一映射还原。
    struct HitNote {
        int placementIndex = 0;
        int noteIndex = 0;
        Note note;
        ContentTimelineProjection projection;
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
    };

    // ── Helper: note highlight visual bounds using an arbitrary camera ──
    // Mirrors getNoteBounds' geometry but accepts a camera parameter so old/new
    // projection can differ.  For OpenDyne blobs the vertical extent covers the
    // worst-case blob envelope (kBlobHalfKeys × maxGain = 3.75 × pps).
    auto noteHighlightBounds = [this](const HitNote& hit,
                                      const TimelineViewportCamera& cam) -> juce::Rectangle<int> {
        const Note& n = hit.note;
        const float adjustedPitch = n.getAdjustedPitch();
        if (adjustedPitch <= 0.0f)
            return {};

        const SourceEditRange srcRange = SourceEditRange::fromTimeGrid(*hit.timeGrid, 0.0);
        if (n.endTime <= srcRange.startSeconds || n.startTime >= srcRange.endSeconds)
            return {};

        const ViewportState view{ cam, pixelsPerSemitone_, verticalScrollOffset_ };
        const auto mapper = makeViewMapperForView(view);
        const PianoRollTimeMap map(hit.projection, *hit.timeGrid, mapper);

        const int x1 = map.sourceToX(n.startTime);
        const int x2 = map.sourceToX(n.endTime);
        const int width = std::max(1, x2 - x1);
        const float midi = mapper.freqToMidi(adjustedPitch);
        const float cy = mapper.midiToY(midi);

        // Base: standard note rectangle + 4 px expansion
        const int top = static_cast<int>(std::floor(cy - pixelsPerSemitone_ * 0.5f));
        const int height = std::max(1, static_cast<int>(std::ceil(pixelsPerSemitone_)));
        auto rect = juce::Rectangle<int>(x1, top, width, height).expanded(4);

        // OpenDyne blob envelope: up to ±3.75 × pixelsPerSemitone from centre
        if (isOpenDyne()) {
            constexpr float kBlobHalfKeys = 1.5f;
            constexpr float kMaxGain = 2.5f;
            const float halfH = pixelsPerSemitone_ * kBlobHalfKeys * kMaxGain;
            const int blobTop = static_cast<int>(std::floor(cy - halfH));
            const int blobBottom = static_cast<int>(std::ceil(cy + halfH));
            rect = rect.getUnion(
                juce::Rectangle<int>(x1, blobTop, width,
                                     std::max(1, blobBottom - blobTop)).expanded(1));
        }

        return rect.translated(0, rulerHeight_)
                   .getIntersection(getTimelineViewportBounds());
    };

    // ── Helper: collect hit notes (same display items and condition as drawPlayheadNoteHighlight) ──
    // timelineTime 先按各活动 placement 自己的映射逆到 source，再比较
    // note.startTime ≤ sourceTime < note.endTime。消费调用方取得的活动 item。
    auto collectHitNotes = [this](const std::optional<Item>& item, double timelineTime, bool playing) -> std::vector<HitNote> {
        if (!playing || timelineTime <= 0.0 || !item.has_value() || item->displayNotes == nullptr)
            return {};
        std::vector<HitNote> hits;
        const auto& notes = *item->displayNotes;
        for (int placementIndex = 0;
             placementIndex < static_cast<int>(timelineContentPlacements_.size()); ++placementIndex) {
            const auto& placement = timelineContentPlacements_[static_cast<size_t>(placementIndex)];
            if (!placement.isValid() || placement.contentKey != editedContentKey_)
                continue;
            const PianoRollTimeMap map(placement.projection, *item->committedSnapshot->timeGrid, makeViewMapper());
            const double sourceTime = map.timelineToSource(timelineTime);
            for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
                if (notes[static_cast<size_t>(i)].startTime <= sourceTime
                    && sourceTime < notes[static_cast<size_t>(i)].endTime) {
                    hits.push_back({ placementIndex, i, notes[static_cast<size_t>(i)],
                                     placement.projection, item->committedSnapshot->timeGrid });
                }
            }
        }
        return hits;
    };

    // 旧播放头 bounds + fixedCentre（在 seek/camera 更新前用旧 state 计算）
    struct BoundsWithFixed { juce::Rectangle<int> bounds; bool fixedCentre = false; };
    auto playheadBoundsAt = [this](double t, bool isPlaying, bool continuousFollow) -> BoundsWithFixed {
        const auto mapper = makeViewMapper();
        const int timeX = mapper.timeToX(t);
        const int viewRight = getTimelineViewportBounds().getRight();
        const int centreX = (mapper.contentStartX + viewRight) / 2;
        const auto pres = TimelineViewportPolicy::computePlayheadPresentation(
            timeX, centreX, viewRight, mapper.contentStartX, isPlaying, continuousFollow);
        if (!pres.visible) return {};
        const int anchorX = pres.anchorX;
        const int playheadH = getHeight();
        return { juce::Rectangle<int>(anchorX - 6, 0, 13, playheadH).getIntersection(timeAxisRect()),
                 pres.fixedCentre };
    };

    const auto oldCamera = camera_;
    const double oldCameraStart = oldCamera.visibleStartSeconds;
    const bool oldIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const bool oldPlaying = lastObservedPlayHeadPlaying_;
    const double oldPlayheadTime = playheadTimeForPaint_;

    const auto oldPH = playheadBoundsAt(oldPlayheadTime, oldPlaying, oldIsContFollow);

    // 播放状态切换
    if (playStateChanged) {
        userScrollHold_ = false;
        lastObservedPlayHeadPlaying_ = playingNow;
    }
    trackReaperTransport();

    double playheadTime = playHeadState_.getPresentedPositionSeconds();
    if (pendingSeekTime_ >= 0.0) {
        PendingSeekPolicy::Input pendingSeekInput;
        pendingSeekInput.pendingTimeSeconds = pendingSeekTime_;
        pendingSeekInput.canonicalTimeSeconds = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
        pendingSeekInput.hostTimeAtGestureSeconds = pendingSeekHostTimeAtGesture_;
        pendingSeekInput.issuedAtSeconds = pendingSeekIssuedAtSeconds_;
        pendingSeekInput.nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        pendingSeekInput.pendingEpoch = pendingSeekEpoch_;
        pendingSeekInput.currentEpoch = playHeadState_.presentationEpoch.load(std::memory_order_acquire);
        pendingSeekInput.hostBound = pendingSeekHostBound_;
        pendingSeekInput.hostWasPlayingAtGesture = pendingSeekHostWasPlaying_;
        pendingSeekInput.hostIsPlayingNow = playHeadState_.isPlaying.load(std::memory_order_acquire);
        const PendingSeekPolicy::Decision decision = PendingSeekPolicy::evaluate(pendingSeekInput);
        if (decision.clearPending) {
            pendingSeekTime_ = -1.0;
            pendingSeekEpoch_ = 0;
            pendingSeekHostBound_ = false;
            pendingSeekHostWasPlaying_ = false;
            userScrollHold_ = false;
        } else {
            if (decision.detachFromHost) {
                pendingSeekHostBound_ = false;
                pendingSeekHostTimeAtGesture_ = playHeadState_.timeInSeconds.load(std::memory_order_relaxed);
            }
            if (decision.holdPending)
                playheadTime = pendingSeekTime_;
        }
    }

    // 自动跟随：仅 playing && !userScrollHold_
    if (playingNow && !userScrollHold_) {
        const double pps = camera_.pixelsPerSecond;
        const auto kind = scrollMode_ == ScrollMode::Continuous
            ? TimelineViewportRequest::Kind::Cont
            : TimelineViewportRequest::Kind::Page;
        const auto resolved = TimelineViewportPolicy::resolve(
            makeViewportRequest(kind, playheadTime, 0.0, pps));
        if (resolved != camera_)
            activateTimelineCamera(resolved);
    }

    // 唯一 playheadTimeForPaint_ 写入
    const bool timeChanged = (playheadTime != playheadTimeForPaint_);
    playheadTimeForPaint_ = playheadTime;

    const bool cameraChanged = (camera_.visibleStartSeconds != oldCameraStart);
    const bool newIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const auto newPH = playheadBoundsAt(playheadTime, playingNow, newIsContFollow);

    // ── 一次活动读取供 old/new 使用：至少一侧满足 playing && timelineTime > 0 ──
    std::optional<Item> activeItem;
    if (editedContentKey_.isValid()
        && ((oldPlaying && oldPlayheadTime > 0.0) || (playingNow && playheadTime > 0.0))) {
        const double buildT0 = juce::Time::getMillisecondCounterHiRes();
        activeItem = buildContentRenderItem(editedContentKey_);
        if (activeItem.has_value())
            recordRenderProbe(RenderProbePoint::SnapshotBuild,
                              juce::Time::getMillisecondCounterHiRes() - buildT0, 1);
    }
    const auto oldHits = collectHitNotes(activeItem, oldPlayheadTime, oldPlaying);
    const auto newHits = collectHitNotes(activeItem, playheadTime, playingNow);

    // ── Compute repaint damage ──
    juce::Rectangle<int> damage;

    // Strip repaint: playhead line/triangle/glow — gated by stableCont
    const bool stableCont = oldPH.fixedCentre && newPH.fixedCentre;
    const bool needsStripRepaint = (playStateChanged || timeChanged || cameraChanged) && !stableCont;
    if (needsStripRepaint)
        damage = oldPH.bounds.getUnion(newPH.bounds);

    // Highlight repaint: note hit set changed — NOT gated by stableCont
    auto sameHitSet = [](const std::vector<HitNote>& a, const std::vector<HitNote>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].placementIndex != b[i].placementIndex || a[i].noteIndex != b[i].noteIndex)
                return false;
        }
        return true;
    };
    if (!sameHitSet(oldHits, newHits)) {
        for (const auto& hit : oldHits)
            damage = damage.getUnion(noteHighlightBounds(hit, oldCamera));
        for (const auto& hit : newHits)
            damage = damage.getUnion(noteHighlightBounds(hit, camera_));
    }

    if (!damage.isEmpty())
        repaint(damage);
}

void PianoRollComponent::commitViewportRequest(TimelineViewportRequest req)
{
    activateTimelineCamera(TimelineViewportPolicy::resolve(req));
}

void PianoRollComponent::navigateFromOverview(TimelineViewportRequest req)
{
    userScrollHold_ = true;
    commitViewportRequest(req);
}

void PianoRollComponent::activateTimelineCamera(TimelineViewportCamera camera)
{
    // 外部相机交接（编排视图 pps 域可达 1000）统一收进 PianoRoll 钳制域；
    // 请求路径经 resolve() 已钳制，此处幂等。
    camera.pixelsPerSecond = TimelineViewportPolicy::normalisePixelsPerSecond(
        camera.pixelsPerSecond, TimelineViewportRequest::ViewKind::PianoRoll);
    cameraPanPending_ |= camera != camera_;
    camera_ = camera;
    updateScrollBars();
    repaint();
}

PianoRollComponent::ViewportState PianoRollComponent::viewportState() const noexcept
{
    return { camera_, pixelsPerSemitone_, verticalScrollOffset_ };
}

void PianoRollComponent::restoreViewportState(const ViewportState& state)
{
    // 恢复镜头代表用户明确的视图意图：标记手动缩放阻止初始自动定位，
    // 并清除该内容的 pending 初始定位，避免异步 F0 Ready 覆盖恢复镜头。
    userHasManuallyZoomed_ = true;
    if (pendingInitialF0ViewContentKey_ == editedContentKey_)
        pendingInitialF0ViewContentKey_ = ContentKey{};

    camera_ = state.camera;
    pixelsPerSemitone_ = state.pixelsPerSemitone;
    const float maxScroll = juce::jmax(0.0f, getTotalHeight() - static_cast<float>(getTimelineContentViewportHeight()));
    verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, state.verticalScrollOffset);

    staticDirty_ = true;
    contentDirty_ = true;

    updateScrollBars();
    repaint();
}

// ── OpenDyne scheme 切换原子重置（计划 §二.3） ──
void PianoRollComponent::applyAudioEditingScheme(AudioEditingScheme::Scheme scheme)
{
    if (audioEditingScheme_ == scheme)
        return;

    const bool wasOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_);
    audioEditingScheme_ = scheme;
    const bool nowOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_);

    stopNotePitchAudition();
    // 清除当前 transient drag/resize/drawing/scissors/volume 预览
    interactionState_.resetTransient();
    if (interactionState_.isPanning)
        interactionState_.isPanning = false;
    if (openDyneZoomPanActive_)
        endOpenDyneZoomPan();

    // 切入 OpenDyne：DrawNote→Pitch，LineAnchor→Select；HandDraw/Select/TimeTool 保持
    // 切回 OpenTune：Pitch→DrawNote，OpenDyne-only 工具（VolumeEnvelope/Scissors）→Select
    if (nowOpenDyne) {
        if (currentTool_ == ToolId::DrawNote)
            setCurrentTool(ToolId::Pitch);
        else if (currentTool_ == ToolId::LineAnchor)
            setCurrentTool(ToolId::Select);
    } else if (wasOpenDyne) {
        if (currentTool_ == ToolId::Pitch || currentTool_ == ToolId::PitchModulation || currentTool_ == ToolId::PitchDrift)
            setCurrentTool(ToolId::DrawNote);
        else if (currentTool_ == ToolId::VolumeEnvelope || currentTool_ == ToolId::Scissors)
            setCurrentTool(ToolId::Select);
    }

    staticDirty_ = true;
    contentDirty_ = true;


    // Scheme changed → relayout to show/hide the horizontal scrollbar.
    if (wasOpenDyne != nowOpenDyne)
        resized();
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    if (currentTool_ != tool)
        stopNotePitchAudition();

    // Time 仅在 experimental 开启时有效
    if (tool == ToolId::TimeTool && !experimentalFeaturesEnabled_) {
        tool = ToolId::Select;
    }

    if (tool == ToolId::TimeTool
        && currentTool_ != ToolId::TimeTool
        && contentCommands_ != nullptr
        && editedContentKey_.isValid()) {
        // TimeGrid anchor seed is a content command; the processor's async
        // preheat/F0 callback chain keeps driving the actual seeding.
        contentCommands_->ensureTimeToolAnchorSeed(editedContentKey_);
    }

    const bool toolChanged = currentTool_ != tool;
    const ToolId previousTool = currentTool_;
    const bool clearsFrameSelection = toolChanged && tool == ToolId::TimeTool
        && !interactionState_.frameSelection.ranges.empty();
    if (clearsFrameSelection)
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
    bool clearedAnchorPreview = false;
    if (interactionState_.drawing.isPlacingAnchors && tool != ToolId::LineAnchor) {
        interactionState_.drawing.isPlacingAnchors = false;
        interactionState_.drawing.pendingAnchors.clear();
        clearedAnchorPreview = true;
    }

    // 在清空任何拖拽预览瞬态之前捕获：TimeTool 分支会 noteDrag.clear()、
    // clearNoteDraft()，toolHandler_->setTool 也会 cancelActiveMouseGesture；
    // content surface 上残留的预览曲线必须立即重绘，否则旧 F0 预览曲线永久残留。
    const bool hadTransientPreview =
        interactionState_.noteDrag.previewSnapshot != nullptr
        || interactionState_.isModDriftDragging
        || interactionState_.isVolumeDragging;

    if (toolChanged) {
        // 离开 Eq 工具：关闭 EQ 预览弹窗（EQ 由选中组驱动，工具退出即失效）
        if (previousTool == ToolId::Eq)
            closeEqPopup();
        if (tool == ToolId::TimeTool) {
            if (pressedPianoKey_ >= 0) {
                if (pianoKeyAudition_ != nullptr)
                    pianoKeyAudition_->noteOff(pressedPianoKey_);
                pressedPianoKey_ = -1;
            }
            interactionState_.noteDrag.clear();
            interactionState_.noteResize.clear();
            clearNoteDraft();
            interactionState_.frameSelection.clear();
            interactionState_.selection.isSelectingF0 = false;
            interactionState_.selection.f0SelectionAnchorFrame = -1;
            interactionState_.selection.f0SelectionCurrentFrame = -1;
        } else if (currentTool_ == ToolId::TimeTool) {
            interactionState_.timeTool.clear();
        }
    }
    if (clearsFrameSelection)
        parameterTargetDidChange();

    currentTool_ = tool;
    if (toolHandler_) {
        toolHandler_->setTool(tool);
    }
    if (hadTransientPreview) {
        contentDirty_ = true;

    }

    switch (tool) {
        case ToolId::Select:
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
        case ToolId::DrawNote:
        case ToolId::LineAnchor:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::HandDraw:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::Eraser:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::AutoTune:
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            break;
        case ToolId::Pitch:
        case ToolId::PitchModulation:
        case ToolId::PitchDrift:
        case ToolId::VolumeEnvelope:
        case ToolId::Scissors:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            break;
        case ToolId::TimeTool:
            // 锟?.4: Time tool uses normal cursor + per-handle hover hand cursor
            // applied by handleTimeToolMouseMove (via ctx.setMouseCursor).
            setMouseCursor(juce::MouseCursor::NormalCursor);
            break;
        case ToolId::Eq:
            // EQ cursor：EQ 曲线图标经 CursorThemeManager::resolveCursor 应用主题，不是 Crosshair
            setMouseCursor(eqCursor_);
            break;
    }

    // 通知监听者工具已切换（参数面板需要同步按钮高度）
    if (toolChanged) {
        listeners_.call([tool](Listener& l) { l.currentToolChanged(tool); });
        }

    if (toolChanged || clearedAnchorPreview) {
        // 只有进出 TimeTool 才改变琴键可见性 → 影响 staticSurface_
        const bool wasTimeView = (previousTool == ToolId::TimeTool);
        const bool isNowTimeView = (currentTool_ == ToolId::TimeTool);
        if (wasTimeView != isNowTimeView && showPianoKeyboard_) {
            layoutZoomControls();
            staticDirty_ = true;
            contentDirty_ = true;

        }
        // 工具切换共同出口一次 repaint（overlay 已并入 owner paint）
        repaint();
    }
}

void PianoRollComponent::setExperimentalFeaturesEnabled(bool enabled)
{
    if (experimentalFeaturesEnabled_ == enabled) {
        return;
    }

    dismissToolPopup();
    experimentalFeaturesEnabled_ = enabled;
    if (!enabled && currentTool_ == ToolId::TimeTool) {
        setCurrentTool(ToolId::Select);
        return;
    }

    repaint(getLocalBounds());
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    if (showWaveform_ == shouldShow) return;
    showWaveform_ = shouldShow;
    contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setShowPianoKeyboard(bool shouldShow) {
    if (showPianoKeyboard_ == shouldShow) return;
    const int oldContentStartX = getTimelineContentStartX();
    showPianoKeyboard_ = shouldShow;
    layoutZoomControls();
    // 隐藏左侧键盘时立即结束正在试听的按键，避免 noteOff 因键盘不再显示而延迟
    if (!shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOff(pressedPianoKey_);
        pressedPianoKey_ = -1;
    }
    staticDirty_ = true;
    if (oldContentStartX != getTimelineContentStartX())
        contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setScaleAssistEnabled(bool enabled) {
    if (scaleAssistEnabled_ == enabled) return;
    scaleAssistEnabled_ = enabled;
    staticDirty_ = true;

    repaint();
}

void PianoRollComponent::setGridStyle(PianoGridStyle gridStyle)
{
    if (gridStyle_ == gridStyle) return;
    gridStyle_ = gridStyle;
    staticDirty_ = true;
    contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode) {
    if (noteNameMode_ == noteNameMode) return;
    noteNameMode_ = noteNameMode;
    staticDirty_ = true;

    repaint();
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow) {
    if (showUnvoicedFrames_ == shouldShow) return;
    showUnvoicedFrames_ = shouldShow;
    contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setBackgroundBrightness(float brightness)
{
    const auto clamped = juce::jlimit(0.0f, 2.0f, brightness);
    if (backgroundBrightness_ == clamped) return;
    backgroundBrightness_ = clamped;
    staticDirty_ = true;

    repaint();
}

void PianoRollComponent::setTrackCaptureTint(bool active)
{
    if (trackCaptureTint_ == active)
        return;
    trackCaptureTint_ = active;
    repaint();
}

void PianoRollComponent::setCorrectedF0Colour(juce::Colour colour)
{
    colour = colour.withAlpha(1.0f);
    if (correctedF0Colour_ == colour) return;
    correctedF0Colour_ = colour;
    contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setOriginalF0Colour(juce::Colour colour)
{
    colour = colour.withAlpha(1.0f);
    if (originalF0Colour_ == colour) return;
    originalF0Colour_ = colour;
    contentDirty_ = true;

    repaint();
}

void PianoRollComponent::setBpm(double bpm) {
    if (bpm_ == bpm) return;
    bpm_ = bpm;
    invalidateTimeAxisStaticSurface();
}

void PianoRollComponent::setTimeSignature(int numerator, int denominator) {
    if (timeSigNum_ == numerator && timeSigDenom_ == denominator) return;
    timeSigNum_ = numerator;
    timeSigDenom_ = denominator;
    invalidateTimeAxisStaticSurface();
}

void PianoRollComponent::setTimelineDisplayMode(TimelineDisplayMode mode) {
    if (displayMode_ == mode) return;
    displayMode_ = mode;
    timeUnitToggleButton_.setButtonText(displayMode_ == TimelineDisplayMode::Time ? LOC(kTime) : "BPM");
    invalidateTimeAxisStaticSurface();
}

void PianoRollComponent::refreshLocalizedText()
{
    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous
        ? LOC(kScrollContinuous)
        : LOC(kScrollPage));
    timeUnitToggleButton_.setButtonText(displayMode_ == TimelineDisplayMode::Time
        ? LOC(kTime)
        : "BPM");
    horizontalZoomSlider_.setTitle(LOC(kHorizontalZoom));
    horizontalZoomSlider_.setTooltip(LOC(kHorizontalZoom));
    verticalZoomSlider_.setTitle(LOC(kVerticalZoom));
    verticalZoomSlider_.setTooltip(LOC(kVerticalZoom));
}

void PianoRollComponent::addListener(Listener* listener) {
    listeners_.add(listener);
}

void PianoRollComponent::removeListener(Listener* listener) {
    listeners_.remove(listener);
}

void PianoRollComponent::parameterTargetWillChange(Listener::ParameterTargetChangePolicy policy)
{
    listeners_.call([policy](Listener& listener) { listener.parameterTargetWillChange(policy); });
}

void PianoRollComponent::parameterTargetDidChange()
{
    listeners_.call([](Listener& listener) { listener.parameterTargetDidChange(); });
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& e) {
    // 全局手势：Cmd+Alt 缩放 → 仅设 cursor，early return（不交 ToolHandler）
    if (isOpenDyne() && e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= getTimelineContentStartX()) {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }
    // 缩放/平移事务中，cursor 已由 mouseDown/mouseDrag 设置
    if (openDyneZoomPanActive_ || interactionState_.isPanning)
        return;
    // 普通情况：ToolHandler 是唯一 cursor 权威，一次事件最多一次 setMouseCursor
    toolHandler_->mouseMove(e);
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    if (std::exchange(fingerGesture_.suppressDoubleClick, false))
        return;
    // OpenDyne: Cmd+Alt+double-click → zoom to note / restore zoom
    if (isOpenDyne() && e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= getTimelineContentStartX()) {
        // Find note under cursor
        const auto& notes = getCommittedNotes();
        for (const auto& note : notes) {
            if (getNoteBounds(note).contains(e.getPosition())) {
                saveOpenDyneZoomState();
                fitToNote(note);
                return;
            }
        }
        // No note found → restore previous zoom
        restoreOpenDyneZoomState();
        return;
    }
    toolHandler_->mouseDoubleClick(e);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& e) {
    if (TouchInput::isFingerPress(e) && beginFingerPress(e))
        return;
    dispatchMouseDown(e);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
    if (continueFingerDrag(e))
        return;
    dispatchMouseDrag(e);
}

void PianoRollComponent::mouseUp(const juce::MouseEvent& e) {
    if (endFingerPress(e))
        return;
    dispatchMouseUp(e);
}

bool PianoRollComponent::fingerDragOnEmptySpacePans() const noexcept {
    if (currentTool_ == ToolId::HandDraw)
        return false;
    if (currentTool_ == ToolId::DrawNote && !isOpenDyne())
        return false;
    return true;
}

bool PianoRollComponent::beginFingerPress(const juce::MouseEvent& e) {
    auto& g = fingerGesture_;
    const int index = e.source.getIndex();
    g.fingers[index] = e.position;
    g.suppressDoubleClick = false;

    if (g.fingers.size() == 1) {
        g.primaryIndex = index;
        g.lastPrimaryEvent.emplace(e);
        dispatchMouseDown(e);
        g.mode = interactionState_.emptySpaceIntent.active && fingerDragOnEmptySpacePans()
            ? FingerGesture::Mode::EmptyTap
            : FingerGesture::Mode::Forwarded;
        return true;
    }

    if (g.mode == FingerGesture::Mode::Pinch || g.mode == FingerGesture::Mode::Ignore)
        return true;

    // 第二根手指落下：结束单指动作后转入捏合。空白单击意图直接丢弃，避免移动播放头。
    switch (g.mode) {
        case FingerGesture::Mode::EmptyTap:
            interactionState_.emptySpaceIntent.clear();
            break;
        case FingerGesture::Mode::Forwarded:
            if (g.lastPrimaryEvent.has_value())
                dispatchMouseUp(*g.lastPrimaryEvent);
            break;
        case FingerGesture::Mode::Idle:
        case FingerGesture::Mode::Panning:
        case FingerGesture::Mode::Pinch:
        case FingerGesture::Mode::Ignore:
            break;
    }

    g.pinchA = g.primaryIndex;
    g.pinchB = index;
    if (g.fingers.count(g.pinchA) == 0)
        g.pinchA = g.fingers.begin()->first;
    beginFingerPinch();
    return true;
}

bool PianoRollComponent::continueFingerDrag(const juce::MouseEvent& e) {
    auto& g = fingerGesture_;
    const int index = e.source.getIndex();
    const auto it = g.fingers.find(index);
    if (it == g.fingers.end())
        return false;
    it->second = e.position;

    switch (g.mode) {
        case FingerGesture::Mode::Pinch:
            if (index == g.pinchA || index == g.pinchB)
                updateFingerPinch();
            return true;
        case FingerGesture::Mode::EmptyTap:
            if (index != g.primaryIndex)
                return true;
            if (e.position.getDistanceFrom(e.mouseDownPosition) <= static_cast<float>(dragThreshold_))
                return true;
            interactionState_.emptySpaceIntent.clear();
            g.mode = FingerGesture::Mode::Panning;
            g.panStartPos = e.position;
            g.panStartVisibleSeconds = camera_.visibleStartSeconds;
            g.panStartScrollY = verticalScrollOffset_;
            return true;
        case FingerGesture::Mode::Panning:
            if (index == g.primaryIndex)
                updateFingerPan(e);
            return true;
        case FingerGesture::Mode::Forwarded:
            if (index != g.primaryIndex)
                return true;
            g.lastPrimaryEvent.emplace(e);
            dispatchMouseDrag(e);
            return true;
        case FingerGesture::Mode::Idle:
        case FingerGesture::Mode::Ignore:
            return true;
    }
    return true;
}

bool PianoRollComponent::endFingerPress(const juce::MouseEvent& e) {
    auto& g = fingerGesture_;
    const int index = e.source.getIndex();
    if (g.fingers.erase(index) == 0)
        return false;

    const bool wasPrimary = index == g.primaryIndex;
    const auto mode = g.mode;

    if (g.fingers.empty()) {
        g.mode = FingerGesture::Mode::Idle;
        g.primaryIndex = -1;
        g.lastPrimaryEvent.reset();
    } else if (mode == FingerGesture::Mode::Pinch) {
        g.mode = FingerGesture::Mode::Ignore;
    }

    if (!wasPrimary)
        return true;

    switch (mode) {
        case FingerGesture::Mode::EmptyTap:
            // 单击空白：交给工具处理（取消选择 + 移动播放头）；双击空白：再切换播放/暂停。
            dispatchMouseUp(e);
            if (e.getNumberOfClicks() == 2) {
                g.suppressDoubleClick = true;
                if (transportControlsEnabled_)
                    listeners_.call([](Listener& l) { l.playPauseToggleRequested(); });
            }
            break;
        case FingerGesture::Mode::Forwarded:
            dispatchMouseUp(e);
            break;
        case FingerGesture::Mode::Panning:
            grabKeyboardFocus();
            break;
        case FingerGesture::Mode::Idle:
        case FingerGesture::Mode::Pinch:
        case FingerGesture::Mode::Ignore:
            break;
    }
    return true;
}

void PianoRollComponent::updateFingerPan(const juce::MouseEvent& e) {
    const auto& g = fingerGesture_;
    const auto delta = e.position - g.panStartPos;
    applyViewportFromGesture(g.panStartVisibleSeconds - static_cast<double>(delta.x) / camera_.pixelsPerSecond,
                             camera_.pixelsPerSecond,
                             g.panStartScrollY - delta.y);
}

void PianoRollComponent::beginFingerPinch() {
    auto& g = fingerGesture_;
    const auto a = g.fingers[g.pinchA];
    const auto b = g.fingers[g.pinchB];
    const auto mid = (a + b) * 0.5f;

    g.mode = FingerGesture::Mode::Pinch;
    g.pinchStartSpan = { std::abs(a.x - b.x), std::abs(a.y - b.y) };

    // 按手指连线方向决定缩放轴：接近水平只缩放时间轴，接近竖直只缩放音高轴，斜向两轴同时缩放。
    constexpr float kTan30 = 0.577f;
    constexpr float kMinSpanPx = 24.0f;
    const bool nearlyHorizontal = g.pinchStartSpan.y <= g.pinchStartSpan.x * kTan30;
    const bool nearlyVertical = g.pinchStartSpan.x <= g.pinchStartSpan.y * kTan30;
    g.pinchZoomsTime = g.pinchStartSpan.x >= kMinSpanPx && !nearlyVertical;
    g.pinchZoomsPitch = g.pinchStartSpan.y >= kMinSpanPx && !nearlyHorizontal;

    g.pinchStartPps = camera_.pixelsPerSecond;
    g.pinchStartPixelsPerSemitone = pixelsPerSemitone_;
    g.pinchAnchorTime = camera_.visibleStartSeconds
        + static_cast<double>(mid.x - static_cast<float>(getTimelineContentStartX())) / camera_.pixelsPerSecond;
    g.pinchAnchorMidi = makeViewMapper().yToMidi(mid.y - static_cast<float>(rulerHeight_));
}

void PianoRollComponent::updateFingerPinch() {
    const auto& g = fingerGesture_;
    const auto a = g.fingers.at(g.pinchA);
    const auto b = g.fingers.at(g.pinchB);
    const auto mid = (a + b) * 0.5f;

    double pps = g.pinchStartPps;
    if (g.pinchZoomsTime) {
        const auto ratio = static_cast<double>(std::max(1.0f, std::abs(a.x - b.x)) / g.pinchStartSpan.x);
        pps = TimelineViewportPolicy::normalisePixelsPerSecond(
            g.pinchStartPps * ratio, TimelineViewportRequest::ViewKind::PianoRoll);
    }

    if (g.pinchZoomsPitch) {
        const auto ratio = std::max(1.0f, std::abs(a.y - b.y)) / g.pinchStartSpan.y;
        pixelsPerSemitone_ = juce::jlimit(kMinPixelsPerSemitone, kMaxPixelsPerSemitone, g.pinchStartPixelsPerSemitone * ratio);
        userHasManuallyZoomed_ = true;
    }

    // 恢复 restoreViewportState 的既有契约：手动缩放意图优先于异步初始定位
    if (g.pinchZoomsTime || g.pinchZoomsPitch) {
        if (pendingInitialF0ViewContentKey_ == editedContentKey_)
            pendingInitialF0ViewContentKey_ = ContentKey{};
    }

    // 锚点（捏合起点处的时间/音高）始终跟随两指中点，捏合时同时平移。
    const double visibleStart = g.pinchAnchorTime
        - static_cast<double>(mid.x - static_cast<float>(getTimelineContentStartX())) / pps;
    const float scrollY = (maxMidi_ - g.pinchAnchorMidi) * pixelsPerSemitone_
        - (mid.y - static_cast<float>(rulerHeight_));
    if (g.pinchZoomsTime || g.pinchZoomsPitch)
        extendZoomPreview();
    applyViewportFromGesture(visibleStart, pps, scrollY);
}

void PianoRollComponent::applyViewportFromGesture(double visibleStartSeconds, double pps, float scrollY) {
    const float maxScroll = getTotalHeight() - static_cast<float>(getTimelineContentViewportHeight());
    verticalScrollOffset_ = juce::jlimit(0.0f, std::max(0.0f, maxScroll), scrollY);

    cameraPanPending_ = true;
    userScrollHold_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        visibleStartSeconds,
        0.0,
        pps));
}

void PianoRollComponent::dispatchMouseDown(const juce::MouseEvent& e) {
    if (e.mods.isMiddleButtonDown()
        && handlePointerShortcut({ KeyShortcutConfig::BindingType::MiddleMouseButton, e.mods }))
        return;
    if (e.mods.isBackButtonDown()
        && handlePointerShortcut({ KeyShortcutConfig::BindingType::MouseBack, e.mods }))
        return;
    if (e.mods.isForwardButtonDown()
        && handlePointerShortcut({ KeyShortcutConfig::BindingType::MouseForward, e.mods }))
        return;

    // ── OpenDyne：Command+Alt+拖拽 = 同时缩放两轴；Command+Shift+拖拽 = 平移视区 ──
    if (isOpenDyne()) {
        if (e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= getTimelineContentStartX()) {
            openDyneZoomAxisLock_.reset();
            beginOpenDyneZoomPan(e);
            return;
        }
        if (e.mods.isCommandDown() && e.mods.isShiftDown() && e.x >= getTimelineContentStartX()) {
            interactionState_.panAxisLock.reset();
            interactionState_.isPanning = true;
            interactionState_.dragStartPos = e.getPosition();
            dragStartVerticalScrollOffset_ = verticalScrollOffset_;
            dragStartVisibleStartSeconds_ = camera_.visibleStartSeconds;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }

    // Ctrl+drag panning 锟?only on non-interactive area, so existing
    // Ctrl+click behaviors (note toggle selection, context menu) work.
    if (e.mods.isCtrlDown() && !e.mods.isPopupMenu() && e.x >= getTimelineContentStartX()) {
        bool onNote = false;
        for (const auto& note : getCommittedNotes()) {
            if (getNoteBounds(note).contains(e.getPosition())) {
                onNote = true;
                break;
            }
        }
        if (!onNote) {
            interactionState_.panAxisLock.reset();
            interactionState_.isPanning = true;
            interactionState_.dragStartPos = e.getPosition();
            dragStartVerticalScrollOffset_ = verticalScrollOffset_;
            dragStartVisibleStartSeconds_ = camera_.visibleStartSeconds;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }

    if (e.y < rulerHeight_ && e.x < getTimelineContentStartX()) {
        return;
    }

    // Piano key audition: click in piano key area triggers note preview.
    if (shouldShowPianoKeys() && e.y >= rulerHeight_ && e.x < pianoKeyWidth_) {
        int midiNote = static_cast<int>(std::ceil(
            makeViewMapper().yToMidi(static_cast<float>(e.y - rulerHeight_))));
        midiNote = juce::jlimit(0, 127, midiNote);
        pressedPianoKey_ = midiNote;
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOn(midiNote);
        repaint();
        return;
    }

    toolHandler_->mouseDown(e);
}

void PianoRollComponent::dispatchMouseDrag(const juce::MouseEvent& e) {
    if (openDyneZoomPanActive_) {
        updateOpenDyneZoomPan(e);
        return;
    }

    if (interactionState_.isPanning) {
        int deltaX = e.x - interactionState_.dragStartPos.x;
        int deltaY = e.y - interactionState_.dragStartPos.y;

        // 轴锁定：死区内不动，锁定后只应用一个轴
        constexpr int kAxisLockThresholdPx = 5;
        const auto axis = interactionState_.panAxisLock.resolve(deltaX, deltaY, kAxisLockThresholdPx);
        if (axis == AxisLockState::Axis::None)
            return;

        const double pps = camera_.pixelsPerSecond;

        if (axis == AxisLockState::Axis::Vertical) {
            float newScrollY = dragStartVerticalScrollOffset_ - (float)deltaY;
            float maxScroll = getTotalHeight() - getTimelineContentViewportHeight();
            newScrollY = juce::jlimit(0.0f, std::max(0.0f, maxScroll), newScrollY);
            if (newScrollY != verticalScrollOffset_) {
                verticalScrollOffset_ = newScrollY;
                applyVerticalPan();
            }
        } else {
            const double newVisibleStart = dragStartVisibleStartSeconds_ - deltaX / pps;
            const auto req = makeViewportRequest(
                TimelineViewportRequest::Kind::Manual,
                newVisibleStart,
                0.0,
                pps);
            userScrollHold_ = true;
            commitViewportRequest(req);
        }
        return;
    }

    // Piano key glissando: dragging across keys changes the note
    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        int midiNote = static_cast<int>(std::ceil(
            makeViewMapper().yToMidi(static_cast<float>(e.y - rulerHeight_))));
        midiNote = juce::jlimit(0, 127, midiNote);
        if (midiNote != pressedPianoKey_) {
            if (pianoKeyAudition_ != nullptr) {
                pianoKeyAudition_->noteOff(pressedPianoKey_);
                pianoKeyAudition_->noteOn(midiNote);
            }
            pressedPianoKey_ = midiNote;
            repaint();
        }
        return;
    }

    toolHandler_->mouseDrag(e);
    // Tool handlers invalidate either live note content or transient preview
    // feedback directly.
}

void PianoRollComponent::dispatchMouseUp(const juce::MouseEvent& e) {
    if (openDyneZoomPanActive_) {
        endOpenDyneZoomPan();
        grabKeyboardFocus();
        return;
    }

    if (interactionState_.isPanning) {
        interactionState_.isPanning = false;
        setCurrentTool(currentTool_);
        grabKeyboardFocus();
        return;
    }

    if (shouldShowPianoKeys() && pressedPianoKey_ >= 0) {
        if (pianoKeyAudition_ != nullptr)
            pianoKeyAudition_->noteOff(pressedPianoKey_);
        pressedPianoKey_ = -1;
        repaint();
        return;
    }

    toolHandler_->mouseUp(e);
    grabKeyboardFocus();
}

void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    handleVerticalZoomAt(1.0f + deltaY * zoomSensitivity_.verticalZoomFactor,
                         static_cast<float>(e.y - rulerHeight_));
}

void PianoRollComponent::handleVerticalZoomAt(float zoomFactor, float contentY) {
    float mouseMidi = makeViewMapper().yToMidi(contentY);

    pixelsPerSemitone_ *= zoomFactor;
    pixelsPerSemitone_ = juce::jlimit(kMinPixelsPerSemitone, kMaxPixelsPerSemitone, pixelsPerSemitone_);
    userHasManuallyZoomed_ = true;
    // 恢复 restoreViewportState 的既有契约：手动缩放意图优先于异步初始定位
    if (pendingInitialF0ViewContentKey_ == editedContentKey_)
        pendingInitialF0ViewContentKey_ = ContentKey{};

    float targetY = (maxMidi_ - mouseMidi) * pixelsPerSemitone_;
    verticalScrollOffset_ = targetY - contentY;

    float totalHeight = getTotalHeight();
    float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }

    // 进入缩放预览事务，冻结表面，到达截止时间后统一重建；
    // 冻结图 = 上次 paint 的完整提交，未提交的相机增量由预览状态对变换精确吸收
    extendZoomPreview();

    updateScrollBars();
    repaint();
}

void PianoRollComponent::handleHorizontalScrollWheel(float deltaX, float deltaY) {
    const auto& settings = zoomSensitivity_;
    float scrollDelta = (deltaX != 0 ? deltaX : deltaY);
    const double pixelDelta = static_cast<double>(scrollDelta) * static_cast<double>(settings.scrollSpeed);
    const double pps = camera_.pixelsPerSecond;
    const double newVisibleStart = camera_.visibleStartSeconds - pixelDelta / pps;
    userScrollHold_ = true;
    const auto req = makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        newVisibleStart,
        0.0,
        pps);
    commitViewportRequest(req);
}

void PianoRollComponent::handleVerticalScrollWheel(float deltaY) {
    handleVerticalScrollPixels(static_cast<double>(deltaY) * static_cast<double>(zoomSensitivity_.scrollSpeed));
}

void PianoRollComponent::handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const int anchorViewportX = e.x - getTimelineContentStartX();
    userScrollHold_ = true;
    beginZoomPreviewAt(anchorViewportX,
                       1.0 + static_cast<double>(deltaY) * zoomSensitivity_.horizontalZoomFactor);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    KeyShortcutConfig::BindingType wheelType;
    if (KeyShortcutConfig::getWheelBindingType(wheel, e.mods, wheelType)) {
        const KeyShortcutConfig::KeyBinding binding(wheelType, e.mods);

        // Resolve the wheel binding to a ShortcutId BEFORE generic handlePointerShortcut
        // so pointer-anchored horizontal zoom can use the matched shortcut for direction
        // and magnitude (custom wheel bindings may map up/down/left/right arbitrarily).
        const auto scope = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_)
            ? KeyShortcutConfig::kPianoRollOpenDyneScope
            : KeyShortcutConfig::kPianoRollOpenTuneScope;
        const auto id = KeyShortcutConfig::findShortcutForBinding(shortcutSettings_, binding, scope);

        if (id == KeyShortcutConfig::ShortcutId::ZoomHorizontalIn
            || id == KeyShortcutConfig::ShortcutId::ZoomHorizontalOut) {
            const bool zoomIn = id == KeyShortcutConfig::ShortcutId::ZoomHorizontalIn;
            const float magnitude = std::max(std::abs(wheel.deltaY), std::abs(wheel.deltaX));
            handleHorizontalZoomWheel(e, (zoomIn ? 1.0f : -1.0f) * magnitude);
            return;
        }

        if (handlePointerShortcut(binding))
            return;
    }

    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

#if JUCE_MAC || JUCE_IOS
    if (e.mods.isShiftDown() && deltaY == 0.0f && deltaX != 0.0f) {
        deltaY = deltaX;
        deltaX = 0.0f;
    }
#endif

    if (deltaY == 0.0f && deltaX == 0.0f) return;

    if (e.mods.isShiftDown() || e.mods.isCtrlDown()
        || e.mods.isCommandDown() || e.mods.isAltDown())
        return;

    const auto pan = wheelEventToPixels(deltaX, deltaY, wheel.isSmooth, zoomSensitivity_.scrollSpeed);
    if (pan.x != 0.0)
        handleHorizontalScrollPixels(pan.x);
    if (pan.y != 0.0)
        handleVerticalScrollPixels(pan.y);
}

void PianoRollComponent::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    const auto zoom = magnifyScaleToZoom(scaleFactor);
    if (!zoom.apply)
        return;

    beginZoomPreviewAt(e.x - getTimelineContentStartX(), zoom.factor);
    handleVerticalZoomAt(static_cast<float>(zoom.factor),
                         static_cast<float>(e.y - rulerHeight_));
}

void PianoRollComponent::beginOpenDyneZoomPan(const juce::MouseEvent& e) {
    saveOpenDyneZoomState();
    openDyneZoomPanActive_ = true;
    openDyneZoomPanStartPos_ = e.getPosition();
    openDyneZoomPanStartPps_ = camera_.pixelsPerSecond;
    openDyneZoomPanStartPixelsPerSemitone_ = pixelsPerSemitone_;
    const double mouseX = static_cast<double>(e.x - getTimelineContentStartX());
    openDyneZoomPanAnchorTime_ = camera_.visibleStartSeconds + mouseX / camera_.pixelsPerSecond;
    const float contentY = static_cast<float>(e.y - rulerHeight_);
    openDyneZoomPanAnchorMidi_ = makeViewMapper().yToMidi(contentY);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
}

void PianoRollComponent::updateOpenDyneZoomPan(const juce::MouseEvent& e) {
    const double dx = static_cast<double>(e.x - openDyneZoomPanStartPos_.x);
    const double dy = static_cast<double>(e.y - openDyneZoomPanStartPos_.y);

    constexpr int kAxisLockThresholdPx = 5;
    const auto axis = openDyneZoomAxisLock_.resolve(
        static_cast<int>(dx), static_cast<int>(dy), kAxisLockThresholdPx);
    if (axis == AxisLockState::Axis::None)
        return;

    extendZoomPreview();
    if (axis == AxisLockState::Axis::Horizontal) {
        // 时间轴：保持锚点时间在鼠标 X 下
        const double zoomFactorH = std::exp(dx * 0.005);
        const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
            openDyneZoomPanStartPps_ * zoomFactorH,
            TimelineViewportRequest::ViewKind::PianoRoll);
        const double mouseX = static_cast<double>(e.x - getTimelineContentStartX());
        const double newVisibleStart = openDyneZoomPanAnchorTime_ - mouseX / newPps;
        userScrollHold_ = true;
        commitViewportRequest(makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            newVisibleStart,
            0.0,
            newPps));
    } else {
        // 音高轴：保持锚点 midi 在鼠标 Y 下
        const float zoomFactorV = static_cast<float>(std::exp(dy * 0.005));
        const float newPixelsPerSemitone = juce::jlimit(
            kMinPixelsPerSemitone, kMaxPixelsPerSemitone, openDyneZoomPanStartPixelsPerSemitone_ * zoomFactorV);
        pixelsPerSemitone_ = newPixelsPerSemitone;
        const float contentY = static_cast<float>(e.y - rulerHeight_);
        const float targetY = (maxMidi_ - openDyneZoomPanAnchorMidi_) * pixelsPerSemitone_;
        verticalScrollOffset_ = targetY - contentY;
        const float totalHeight = getTotalHeight();
        const float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
        const float maxScroll = totalHeight - visibleHeight;
        if (maxScroll > 0.0f)
            verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
        else
            verticalScrollOffset_ = 0.0f;
        updateScrollBars();
        repaint();
    }
}

void PianoRollComponent::endOpenDyneZoomPan() {
    openDyneZoomPanActive_ = false;
    // 恢复当前工具 cursor，不固定 Normal（Cmd+Alt zoom 优先于 Ctrl pan，zoom 结束应恢复工具态）
    setCurrentTool(currentTool_);
}

void PianoRollComponent::saveOpenDyneZoomState() {
    if (!savedOpenDyneZoomState_.has_value())
        savedOpenDyneZoomState_ = ViewportState{camera_, pixelsPerSemitone_, verticalScrollOffset_};
}

void PianoRollComponent::restoreOpenDyneZoomState() {
    if (!savedOpenDyneZoomState_.has_value()) return;
    camera_ = savedOpenDyneZoomState_->camera;
    pixelsPerSemitone_ = savedOpenDyneZoomState_->pixelsPerSemitone;
    verticalScrollOffset_ = savedOpenDyneZoomState_->verticalScrollOffset;
    const float maxScroll = juce::jmax(0.0f, getTotalHeight() - static_cast<float>(getTimelineContentViewportHeight()));
    verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    savedOpenDyneZoomState_.reset();
    staticDirty_ = true;
    contentDirty_ = true;

    updateScrollBars();
    repaint();
}

void PianoRollComponent::fitToNote(const Note& note) {
    double minSource = note.startTime;
    double maxSource = note.endTime;
    float adjusted = note.getAdjustedPitch();
    if (adjusted <= 0.0f) return;
    float noteMidi = makeViewMapper().freqToMidi(adjusted);
    float minMidi = noteMidi - 1.0f;  // 1 semitone margin
    float maxMidi = noteMidi + 1.0f;

    // source-time → timeline projection (same as fitToAllNotes)
    double timelineStart = minSource;
    double timelineEnd = maxSource;
    if (const auto* placement = findEditedPlacement()) {
        if (auto snap = readSnapshotFor(placement->contentKey)) {
            const PianoRollTimeMap map(placement->projection, *snap->timeGrid, makeViewMapper());
            timelineStart = map.sourceToTimeline(minSource);
            timelineEnd = map.sourceToTimeline(maxSource);
        }
    }
    const double duration = std::max(1.0e-6, timelineEnd - timelineStart);
    const int viewportW = getTimelineContentViewportWidth();
    const int viewportH = getTimelineContentViewportHeight();
    if (viewportW <= 0 || viewportH <= 0) return;

    const double marginW = viewportW * 0.10;
    const double marginH = viewportH * 0.20;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        (viewportW - 2.0 * marginW) / duration,
        TimelineViewportRequest::ViewKind::PianoRoll);
    const float newPixelsPerSemitone = juce::jlimit(
        kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
        static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

    const double midTime = (timelineStart + timelineEnd) * 0.5;
    userScrollHold_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        midTime - viewportW * 0.5 / newPps,
        0.0,
        newPps));

    pixelsPerSemitone_ = newPixelsPerSemitone;
    const float midiCenter = (maxMidi + minMidi) * 0.5f;
    verticalScrollOffset_ = (maxMidi_ - midiCenter) * pixelsPerSemitone_ - viewportH * 0.5f;
    const float totalHeight = getTotalHeight();
    const float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    const float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f)
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    else
        verticalScrollOffset_ = 0.0f;

    staticDirty_ = true;
    contentDirty_ = true;

    updateScrollBars();
    repaint();
}

void PianoRollComponent::fitToAllNotes() {
    const auto& notes = getCommittedNotes();
    if (notes.empty()) return;

    double minSource = notes.front().startTime;
    double maxSource = notes.front().endTime;
    float minMidi = std::numeric_limits<float>::max();
    float maxMidi = std::numeric_limits<float>::lowest();
    for (const auto& n : notes) {
        minSource = std::min(minSource, n.startTime);
        maxSource = std::max(maxSource, n.endTime);
        const float adjusted = n.getAdjustedPitch();
        if (adjusted > 0.0f) {
            const float midi = makeViewMapper().freqToMidi(adjusted);
            minMidi = std::min(minMidi, midi);
            maxMidi = std::max(maxMidi, midi);
        }
    }
    if (maxMidi <= minMidi) return;

    // source-time → TimeGrid → timeline 投影
    double timelineStart = minSource;
    double timelineEnd = maxSource;
    if (const auto* placement = findEditedPlacement()) {
        if (auto snap = readSnapshotFor(placement->contentKey)) {
            const PianoRollTimeMap map(placement->projection, *snap->timeGrid, makeViewMapper());
            timelineStart = map.sourceToTimeline(minSource);
            timelineEnd = map.sourceToTimeline(maxSource);
        }
    }
    const double duration = std::max(1.0e-6, timelineEnd - timelineStart);

    const int viewportW = getTimelineContentViewportWidth();
    const int viewportH = getTimelineContentViewportHeight();
    if (viewportW <= 0 || viewportH <= 0) return;

    const double marginW = viewportW * 0.05;
    const double marginH = viewportH * 0.10;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        (viewportW - 2.0 * marginW) / duration,
        TimelineViewportRequest::ViewKind::PianoRoll);
    const float newPixelsPerSemitone = juce::jlimit(
        kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
        static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

    // 居中时间范围
    const double midTime = (timelineStart + timelineEnd) * 0.5;
    userScrollHold_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        midTime - viewportW * 0.5 / newPps,
        0.0,
        newPps));

    // 居中 midi 范围
    pixelsPerSemitone_ = newPixelsPerSemitone;
    const float midiCenter = (maxMidi + minMidi) * 0.5f;
    verticalScrollOffset_ = (maxMidi_ - midiCenter) * pixelsPerSemitone_ - viewportH * 0.5f;
    const float totalHeight = getTotalHeight();
    const float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    const float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f)
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    else
        verticalScrollOffset_ = 0.0f;

    staticDirty_ = true;
    contentDirty_ = true;

    updateScrollBars();
    repaint();
}

void PianoRollComponent::fitToSelectedNotes()
{
    const auto& notes = getCommittedNotes();
    const auto& sel = interactionState_.noteSelection;
    if (sel.empty() || notes.empty()) {
        fitToAllNotes();
        return;
    }

    double minSource = std::numeric_limits<double>::max();
    double maxSource = std::numeric_limits<double>::lowest();
    float minMidi = std::numeric_limits<float>::max();
    float maxMidi = std::numeric_limits<float>::lowest();
    for (int idx : sel.selectedIndices) {
        if (idx < 0 || idx >= static_cast<int>(notes.size())) continue;
        const auto& n = notes[static_cast<size_t>(idx)];
        minSource = std::min(minSource, n.startTime);
        maxSource = std::max(maxSource, n.endTime);
        const float adjusted = n.getAdjustedPitch();
        if (adjusted > 0.0f) {
            const float midi = makeViewMapper().freqToMidi(adjusted);
            minMidi = std::min(minMidi, midi);
            maxMidi = std::max(maxMidi, midi);
        }
    }
    if (maxSource <= minSource || maxMidi <= minMidi) return;

    // source-time → TimeGrid → timeline 投影
    double timelineStart = minSource;
    double timelineEnd = maxSource;
    if (const auto* placement = findEditedPlacement()) {
        if (auto snap = readSnapshotFor(placement->contentKey)) {
            const PianoRollTimeMap map(placement->projection, *snap->timeGrid, makeViewMapper());
            timelineStart = map.sourceToTimeline(minSource);
            timelineEnd = map.sourceToTimeline(maxSource);
        }
    }
    const double duration = std::max(1.0e-6, timelineEnd - timelineStart);

    const int viewportW = getTimelineContentViewportWidth();
    const int viewportH = getTimelineContentViewportHeight();
    if (viewportW <= 0 || viewportH <= 0) return;

    const double marginW = viewportW * 0.05;
    const double marginH = viewportH * 0.10;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        (viewportW - 2.0 * marginW) / duration,
        TimelineViewportRequest::ViewKind::PianoRoll);

    const float newPixelsPerSemitone = juce::jlimit(
        kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
        static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

    const double midTime = (timelineStart + timelineEnd) * 0.5;
    userScrollHold_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Manual,
        midTime - viewportW * 0.5 / newPps,
        0.0,
        newPps));

    pixelsPerSemitone_ = newPixelsPerSemitone;
    const float midiCenter = (maxMidi + minMidi) * 0.5f;
    verticalScrollOffset_ = (maxMidi_ - midiCenter) * pixelsPerSemitone_ - viewportH * 0.5f;
    const float totalHeight = getTotalHeight();
    const float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    const float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f)
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    else
        verticalScrollOffset_ = 0.0f;

    staticDirty_ = true;
    contentDirty_ = true;

    updateScrollBars();
    repaint();
}

void PianoRollComponent::beginZoomPreviewAt(int anchorViewportX, double zoomFactor) {
    const double oldPps = camera_.pixelsPerSecond;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        oldPps * zoomFactor,
        TimelineViewportRequest::ViewKind::PianoRoll);
    const double anchorTime = camera_.visibleStartSeconds + anchorViewportX / oldPps;

    extendZoomPreview();
    userHasManuallyZoomed_ = true;
    // 恢复 restoreViewportState 的既有契约：手动缩放意图优先于异步初始定位
    if (pendingInitialF0ViewContentKey_ == editedContentKey_)
        pendingInitialF0ViewContentKey_ = ContentKey{};
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        anchorTime,
        static_cast<double>(anchorViewportX),
        newPps));
}

void PianoRollComponent::extendZoomPreview()
{
    // 标记有未呈现的缩放输入并重置事务截止；
    // 下一次 paint 消费标记，按当前相机精确栅格（截止已过同样成立）
    zoomCameraPending_ = true;
    zoomDeadlineTicks_ = kZoomDeadlineTicks;
}

// ============================================================================
// Note Copy / Paste / Duplicate
// ============================================================================
void PianoRollComponent::copySelectedNotes()
{
    const auto& notes = getCommittedNotes();
    const auto& sel = interactionState_.noteSelection;
    if (sel.empty() || notes.empty()) return;

    notesClipboard_.clear();
    notesClipboard_.reserve(sel.selectedIndices.size());
    for (int idx : sel.selectedIndices) {
        if (idx >= 0 && idx < static_cast<int>(notes.size()))
            notesClipboard_.push_back(notes[static_cast<size_t>(idx)]);
    }
    announceForAccessibility(Loc::format(LOC_RAW("Notes copied: {0}"),
                                      juce::String(static_cast<int>(notesClipboard_.size()))));
}

// Accessibility: follow REAPER's own transport state, read on every frame, to know the
// edit cursor at the moment playback paused (so a later cursor move can be noticed).
void PianoRollComponent::trackReaperTransport()
{
    const auto transport = processor_ != nullptr ? processor_->getReaperTransport() : std::nullopt;
    if (!transport)
        return;

    if (transport->playState != lastReaperPlayState_) {
        const bool wasRunning = (lastReaperPlayState_ & (1 | 4)) != 0;
        if (wasRunning && !transport->isRunning())
            haltEditCursor_ = transport->editCursor;
        lastReaperPlayState_ = transport->playState;
    }
}

std::optional<PianoRollComponent::SongPosition> PianoRollComponent::currentSongPosition() const
{
    const auto transport = processor_ != nullptr ? processor_->getReaperTransport() : std::nullopt;
    if (!transport || transport->isRunning())
        return std::nullopt;

    // The same place REAPER announces. Paused: where playback paused, unless the edit cursor
    // was moved during the pause. Stopped: the edit cursor (REAPER itself decides whether a
    // stop moves it; the plugin's own guess was a quarter second off in a user test).
    if (transport->isPaused()) {
        const bool cursorMoved = haltEditCursor_ && std::abs(transport->editCursor - *haltEditCursor_) > 1.0e-6;
        return cursorMoved ? SongPosition{ transport->editCursor, SongPosition::Source::EditCursor }
                           : SongPosition{ transport->playPosition, SongPosition::Source::Paused };
    }
    return SongPosition{ transport->editCursor, SongPosition::Source::EditCursor };
}

std::optional<double> PianoRollComponent::stoppedEditCursorSourceTime() const
{
    const auto position = currentSongPosition();
    if (!position)
        return std::nullopt;
    const auto cursor = std::optional<double>(position->seconds);
    // Project (timeline) seconds to the clip's source seconds, as for the playhead in pasteNotes.
    const auto projection = activeContentProjection();
    const auto snap = readEditedSnapshot();
    if (!projection.isValid() || !snap)
        return std::nullopt;
    const PianoRollTimeMap map(projection, *snap->timeGrid, makeViewMapper());
    return map.timelineToSource(*cursor);
}

void PianoRollComponent::pasteNotes()
{
    if (notesClipboard_.empty()) {
        announceForAccessibility(LOC_RAW("Nothing to paste"));
        return;
    }
    if (contentCommands_ == nullptr || !editedContentKey_.isValid()) return;
    if (processor_ == nullptr) return;

    parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
    const auto& notes = getCommittedNotes();
    const auto& sel = interactionState_.noteSelection;

    // 粘贴位置：播放头时间经 PianoRollTimeMap 逆投影转为 source seconds
    //   presented timeline seconds
    //   → PianoRollTimeMap::timelineToSource()
    //   → source seconds
    double pasteStartTime = 0.0;
    // Accessibility: with REAPER stopped the plugin's playhead is where playback last
    // stopped, not where the user moved the edit cursor; ask REAPER for the cursor then.
    const auto songPosition = currentSongPosition();
    const double playheadTime = songPosition ? songPosition->seconds : playHeadState_.getPresentedPositionSeconds();
    if (playheadTime > 0.0) {
        const auto projection = activeContentProjection();
        const auto snap = readEditedSnapshot();
        if (projection.isValid() && snap) {
            const PianoRollTimeMap map(projection, *snap->timeGrid, makeViewMapper());
            pasteStartTime = map.timelineToSource(playheadTime);
        }
    } else if (!sel.empty() && !notes.empty()) {
        double maxEnd = 0.0;
        for (int idx : sel.selectedIndices) {
            if (idx >= 0 && idx < static_cast<int>(notes.size()))
                maxEnd = std::max(maxEnd, notes[static_cast<size_t>(idx)].endTime);
        }
        pasteStartTime = maxEnd;
    }

    // 拓扑编辑事务：Processor 读一次 snapshot，plan（平移 + 拓扑）在 Processor 内计算，
    // commitNoteTopologyPatch 处理 revision 推进 + republishPlaybackSource + snapshot 返回。
    const auto result = processor_->pasteNotesAt(
        editedContentKey_,
        notesClipboard_,
        pasteStartTime,
        lastKnownNotesRevision_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : LOC(kPaste));

    if (result.status == PasteNotesStatus::StaleRevision) {
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Cancel);
        if (result.contentSnapshot != nullptr) {
            cachedNotes_ = result.contentSnapshot->notes;
            lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;
            interactionState_.noteSelection.clear();
            requestContentRedraw();
        }
        parameterTargetDidChange();
        pendingUndoDescription_ = {};
        return;
    }
    if (result.status != PasteNotesStatus::Applied) return;

    cachedNotes_ = result.contentSnapshot->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    lastKnownNotesRevision_ = result.contentSnapshot->notesRevision;

    pendingUndoDescription_ = {};

    // 选中刚粘贴的 notes（按旧 exact start/end 匹配）
    interactionState_.noteSelection.clear();
    const int noteCount = static_cast<int>(cachedNotes_.size());
    for (int i = 0; i < noteCount; ++i) {
        for (const auto& pn : result.pastedNotes) {
            if (cachedNotes_[static_cast<size_t>(i)].startTime == pn.startTime
                && cachedNotes_[static_cast<size_t>(i)].endTime == pn.endTime) {
                interactionState_.noteSelection.add(i, noteCount);
                break;
            }
        }
    }

    parameterTargetDidChange();
    requestContentRedraw();
    listeners_.call([](Listener& listener) { listener.contentEdited(); });

    // Paste lands at the plugin's playhead, which goes stale while the host is stopped,
    // so say where the notes went (project time, as in the host).
    announceForAccessibility(Loc::format(LOC_RAW("Notes pasted: {0}, start {1} s"),
                                      juce::String(static_cast<int>(result.pastedNotes.size())),
                                      juce::String(sourceTimeToTimelineTime(pasteStartTime), 2)));
}

void PianoRollComponent::duplicateNotes()
{
    copySelectedNotes();
    pasteNotes();
}

bool PianoRollComponent::handleViewportShortcut(const juce::KeyPress& key)
{
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ScrollHorizontalLeft, key)) {
        handleHorizontalScrollWheel(0.0f, 1.0f);
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ScrollHorizontalRight, key)) {
        handleHorizontalScrollWheel(0.0f, -1.0f);
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ZoomHorizontalIn, key)) {
        zoomHorizontalIn();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ZoomHorizontalOut, key)) {
        zoomHorizontalOut();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ZoomVerticalIn, key)) {
        zoomVerticalIn();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::ZoomVerticalOut, key)) {
        zoomVerticalOut();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::FitSelectedNotes, key)) {
        fitToSelectedNotes();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::FitAllNotes, key)) {
        fitToAllNotes();
        return true;
    }
    return false;
}

void PianoRollComponent::zoomHorizontalIn()
{
    beginZoomPreviewAt(getTimelineContentViewportWidth() / 2,
                       1.0 + zoomSensitivity_.horizontalZoomFactor);
}

void PianoRollComponent::zoomHorizontalOut()
{
    beginZoomPreviewAt(getTimelineContentViewportWidth() / 2,
                       1.0 / (1.0 + zoomSensitivity_.horizontalZoomFactor));
}

void PianoRollComponent::zoomVerticalIn()
{
    handleVerticalZoomAt(1.0f + zoomSensitivity_.verticalZoomFactor,
                         static_cast<float>(getTimelineContentViewportHeight()) * 0.5f);
}

void PianoRollComponent::zoomVerticalOut()
{
    handleVerticalZoomAt(1.0f / (1.0f + zoomSensitivity_.verticalZoomFactor),
                         static_cast<float>(getTimelineContentViewportHeight()) * 0.5f);
}

bool PianoRollComponent::matchesShortcut(KeyShortcutConfig::ShortcutId id,
                                          const juce::KeyPress& key) const
{
    if (pointerShortcutBinding_ != nullptr)
        return KeyShortcutConfig::matchesShortcut(shortcutSettings_, id, *pointerShortcutBinding_);
    return KeyShortcutConfig::matchesShortcut(shortcutSettings_, id, key);
}

bool PianoRollComponent::handlePointerShortcut(const KeyShortcutConfig::KeyBinding& binding)
{
    const auto scope = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_)
        ? KeyShortcutConfig::kPianoRollOpenDyneScope
        : KeyShortcutConfig::kPianoRollOpenTuneScope;
    const auto id = KeyShortcutConfig::findShortcutForBinding(shortcutSettings_, binding, scope);
    if (id == KeyShortcutConfig::ShortcutId::Count)
        return false;

    pointerShortcutBinding_ = &binding;
    const bool handled = keyPressed(juce::KeyPress(0, binding.modifiers, 0));
    pointerShortcutBinding_ = nullptr;
    if (!handled)
        listeners_.call([&](Listener& listener) { listener.pointerShortcutRequested(id); });
    return true;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
    // Shift+F10 opens what the right-click opens: the tool menu.
    if (accessibilityModeEnabled() && a11y::isContextMenuKey(key)) {
        showToolMenuFromKeyboard();
        return true;
    }

    if (handleViewportShortcut(key))
        return true;

    const auto keyCode = key.getKeyCode();
    if (!key.getModifiers().isAnyModifierKeyDown()
        && (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey
            || keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey)) {
        return toolHandler_->keyPressed(key);
    }

    if (matchesShortcut(KeyShortcutConfig::ShortcutId::Undo, key)) {
        listeners_.call([](Listener& l) { l.undoRequested(); });
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::Redo, key)) {
        listeners_.call([](Listener& l) { l.redoRequested(); });
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::Copy, key)) {
        copySelectedNotes();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::Paste, key)) {
        pasteNotes();
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::Cut, key)) {
        parameterTargetWillChange(Listener::ParameterTargetChangePolicy::Commit);
        copySelectedNotes();
        toolHandler_->handleDeleteKey();  // 剪切语义：复制到剪贴板 + 删除选中
        return true;
    }
    if (matchesShortcut(KeyShortcutConfig::ShortcutId::DuplicateClip, key)) {
        duplicateNotes();
        return true;
    }
    return toolHandler_->keyPressed(key, pointerShortcutBinding_);
}

std::optional<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItem(
    ContentKey key) const
{
    // 每 key 唯一构建语句：active content 不再二次读取
    // （旧路径 placement snapshot + readEditedSnapshot 各建一次外层快照并复制 notes）
    const auto committed = readSnapshotFor(key);
    if (!committed)
        return std::nullopt;

    PianoRollRenderer::ContentRenderItem item;
    item.committedSnapshot = committed;

    if (key == editedContentKey_) {
        // 拖拽预览：ownerSnapshot 直接消费 noteDrag.previewSnapshot（已含 scratch 后
        // 经 applyCorrectionToRange 烘焙的 pitchCurve 与 working notes），
        // curve 与 displayNotes 均取自该 snapshot，renderer 无需任何覆盖注入。
        // 非预览：ownerSnapshot 为 committed snapshot，displayNotes 保留
        // getDisplayedNotes() 以支持 resize/draw 等 live draft。
        const auto& previewSnap = interactionState_.noteDrag.previewSnapshot;
        if (previewSnap != nullptr) {
            item.ownerSnapshot = previewSnap;
            item.displayNotes = &item.ownerSnapshot->notes;
        } else {
            item.ownerSnapshot = committed;
            item.displayNotes = &getDisplayedNotes();  // includes live note draft when active
        }
        // VolumeEnvelope 拖拽：blob 大小反馈使用预览包络
        item.volumeEnvelope = interactionState_.isVolumeDragging
            ? &interactionState_.volumePreviewEnvelope
            : &item.ownerSnapshot->volumeEnvelope;
    } else {
        item.ownerSnapshot = committed;
        item.displayNotes = &committed->notes;
        item.volumeEnvelope = &committed->volumeEnvelope;
    }

    // 唯一 F0 来源：ownerSnapshot 的 pitchCurve（预览 = 烘焙 scratch，否则 = committed）
    if (const auto& curve = item.ownerSnapshot->pitchCurve; curve != nullptr && curve->size() > 0)
        item.f0Timeline = { curve->getHopSize(), curve->getSampleRate(),
                            static_cast<int>(curve->size()) };

    // OpenDyne blob 主音符图形由 Renderer 从持久 originalEnergy 构建，
    // 不注入可选 PCM/mipmap；背景波形（非 OpenDyne）由 drawContent 直接读缓存。
    item.notesPrimaryScheme = isOpenDyne();

    return item;
}

void PianoRollComponent::visibilityChanged()
{
    // When component becomes visible, automatically grab keyboard focus
    // This ensures user can use shortcuts (e.g., Ctrl+A to select all) without
    // manual click.
    if (isShowing() && isVisible())
    {
        tryConsumeInitialF0View(editedContentKey_);
        // Use callAfterDelay to ensure focus grab after message loop processing
        // is complete. This is necessary because component may not be able to
        // receive focus immediately when it just became visible.
        juce::Component::SafePointer<PianoRollComponent> safeThis(this);
        juce::Timer::callAfterDelay(10, [safeThis]() {
            if (safeThis != nullptr && safeThis->isShowing())
            {
                safeThis->grabKeyboardFocus();
            }
        });
    }
    else
    {
        stopNotePitchAudition();
    }
}


int PianoRollComponent::getTimelineContentViewportWidth() const
{
    return juce::jmax(0, getTimelineViewportBounds().getWidth() - getTimelineContentStartX());
}

int PianoRollComponent::getTimelineContentViewportHeight() const
{
    return juce::jmax(0, getTimelineViewportBounds().getHeight() - rulerHeight_);
}

void PianoRollComponent::setScale(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    if (scaleRootNote_ == clampedRoot && scaleType_ == clampedType)
        return;
    scaleRootNote_ = clampedRoot;
    scaleType_ = clampedType;
    staticDirty_ = true;

    repaint();
}

void PianoRollComponent::fitToScreen() {
    // 如果用户已手动调整过缩放，不自动覆盖
    if (userHasManuallyZoomed_) {
        return;
    }

    // 1. Vertical Fit: Show C1 to C8 (minMidi_ to maxMidi_)
    // Total range: maxMidi_ - minMidi_
    // Available height: getTimelineContentViewportHeight()
    const auto timelineViewportBounds = getTimelineViewportBounds();
    const int contentViewportHeight = getTimelineContentViewportHeight();
    float range = maxMidi_ - minMidi_ + 1.0f;
    if (range > 0 && contentViewportHeight > 0) {
        pixelsPerSemitone_ = juce::jlimit(kMinPixelsPerSemitone, kMaxPixelsPerSemitone,
                                          static_cast<float>(contentViewportHeight) / range);

        // Reset scroll to show top
        verticalScrollOffset_ = 0; 
        staticDirty_ = true;
        contentDirty_ = true;
        // 不在此分支栅格；最终 commitViewportRequest 由下一次 paint 完整提交
    }

    // 2. Horizontal Fit: 精确覆盖 activeContentProjection 的
    //    [timelineStartSeconds, timelineEndSeconds]。
    //    缩放按完整 span 计算且 camera 起点即 timelineStartSeconds，
    //    不再"起点前移 10% 但缩放仍按原 duration"裁掉尾部。
    double fitStartSeconds = 0.0;
    double duration = 16.0;
    const auto activeProjection = activeContentProjection();
    const bool hasProjectedClipTimeline = activeProjection.isValid();
    if (hasProjectedClipTimeline) {
        fitStartSeconds = activeProjection.timelineStartSeconds;
        duration = activeProjection.timelineEndSeconds() - fitStartSeconds;
    }
    if (!hasProjectedClipTimeline && audioBuffer_ && audioBufferSampleRate_ > 0.0) {
        duration = static_cast<double>(audioBuffer_->getNumSamples()) / audioBufferSampleRate_;
    }

    // Available width: current timeline content viewport width
    int viewWidth = getTimelineContentViewportWidth();
    if (viewWidth > 0 && duration > 0) {
        double pixelsPerSecond = static_cast<double>(viewWidth) / duration;
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            fitStartSeconds,
            0.0,
            pixelsPerSecond);
        commitViewportRequest(req);
    } else {
        const auto req = makeViewportRequest(
            TimelineViewportRequest::Kind::Manual,
            0.0,
            0.0,
            TimelineViewportCamera::kDefaultPixelsPerSecond);
        commitViewportRequest(req);
    }
}

float PianoRollComponent::getTotalHeight() const {
    return (maxMidi_ - minMidi_ + 1.0f) * pixelsPerSemitone_;
}

juce::String PianoRollComponent::AutoTuneApplyResult::message() const
{
    switch (status) {
        case AutoTuneApplyStatus::Applied:
            return LOC(kAutoQueued);
        case AutoTuneApplyStatus::NoChange:
            return juce::String();   // 最终修正已达成：静默，不弹窗
        case AutoTuneApplyStatus::NoCurve:
            return LOC(kAutoNeedsPitchCurve);
        case AutoTuneApplyStatus::NoContentCommands:
            return LOC(kAutoNoContentCommands);
        case AutoTuneApplyStatus::NoContent:
            return LOC(kAutoNeedsEditableClip);
        case AutoTuneApplyStatus::MissingContentSnapshot:
            return LOC(kAutoCannotReadContentSnapshot);
        case AutoTuneApplyStatus::OriginalF0NotReady:
            return LOC(kAutoOriginalF0NotReady);
        case AutoTuneApplyStatus::MissingCurveSnapshot:
            return LOC(kAutoCannotReadCurveSnapshot);
        case AutoTuneApplyStatus::EmptyOriginalF0:
            return LOC(kAutoNeedsOriginalF0);
        case AutoTuneApplyStatus::EmptyTimeline:
            return LOC(kAutoCannotMapF0Timeline);
        case AutoTuneApplyStatus::NoTargetSelection:
            return LOC(kAutoNeedsSelection);
        case AutoTuneApplyStatus::EmptyTargetRange:
            return LOC(kAutoTargetRangeEmpty);
    }

    return LOC(kAutoCouldNotApply);
}

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::applyAutoTuneToSelection()
{
    if (!currentCurve_) {
        return { AutoTuneApplyStatus::NoCurve };
    }

    if (contentCommands_ == nullptr) {
        return { AutoTuneApplyStatus::NoContentCommands };
    }

    if (!editedContentKey_.isValid()) {
        return { AutoTuneApplyStatus::NoContent };
    }

    auto snap = readEditedSnapshot();
    if (snap == nullptr) {
        return { AutoTuneApplyStatus::MissingContentSnapshot };
    }

    const auto originalF0State = snap->originalF0State;
    if (originalF0State != OriginalF0State::Ready) {
        return { AutoTuneApplyStatus::OriginalF0NotReady };
    }

    if (snap->pitchCurve == nullptr) {
        return { AutoTuneApplyStatus::MissingCurveSnapshot };
    }

    // 单一快照读取：前置检查全部从 snap->pitchCurve 派生
    const auto curveSnap = snap->pitchCurve;
    if (curveSnap == nullptr) {
        return { AutoTuneApplyStatus::MissingCurveSnapshot };
    }

    const auto& originalF0 = curveSnap->getOriginalF0();
    if (originalF0.empty()) {
        return { AutoTuneApplyStatus::EmptyOriginalF0 };
    }
    const F0Timeline f0tl{ curveSnap->getHopSize(), curveSnap->getSampleRate(),
                           static_cast<int>(curveSnap->size()) };
    if (f0tl.isEmpty()) {
        return { AutoTuneApplyStatus::EmptyTimeline };
    }

    // ── OpenDyne：AUTO 按钮 = Auto Snap 一键吸附（Melodyne 语义）──
    // 有选中音符时只吸附选中音符；未选中任何音符时才对全量吸附。
    // 分流位于选区解析（collectSelectedFrameRanges）之前，目标集合由 applyAutoSnapToAllNotes 内部决定
    if (AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_)) {
        return applyAutoSnapToAllNotes(snap);
    }

    // ── OpenTune：选区解析 + autoTuneContentRange 逻辑 ──
    // auto-tune 是连续吸附操作：作用于选中范围的并集（包围区间）。
    // 无任何选择时保持既有行为：整段吸附（WholeClip）。
    const auto sel = toolHandler_->collectSelectedFrameRanges(snap.get());
    F0FrameRange requestedRange;
    if (!sel.empty()) {
        requestedRange = { sel.ranges.front().first, sel.ranges.back().second };
    } else {
        requestedRange = { 0, f0tl.endFrameExclusive() };
    }

    const auto targetRange = f0tl.rangeForFrames(requestedRange.startFrame,
                                                 requestedRange.endFrameExclusive);
    if (targetRange.isEmpty()) {
        return { AutoTuneApplyStatus::EmptyTargetRange };
    }

    const int startFrame = targetRange.startFrame;
    const int endFrame = targetRange.endFrameExclusive - 1;

    const std::optional<ScaleSnapConfig> postSnapCfg = makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);

    auto edit = contentCommands_->commitAutoTuneContentRangeUndoable(
        editedContentKey_, startFrame, endFrame + 1, getCurrentAutoTuneParams(), postSnapCfg,
        LOC_RAW("AUTO correction"));
    if (edit.snapshot == nullptr) {
        return { AutoTuneApplyStatus::NoContent };
    }

    lastKnownNotesRevision_ = edit.snapshot->notesRevision;
    lastKnownPitchRevision_ = edit.snapshot->pitchRevision;
    setEditedContent(editedContentKey_, edit.snapshot->pitchCurve, audioBuffer_, audioBufferSampleRate_);
    pendingUndoDescription_ = {};

    return { AutoTuneApplyStatus::Applied };
}

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::applyAutoSnapToAllNotes(
    const std::shared_ptr<const EditableContentSnapshot>& contentSnapshot)
{
    if (contentCommands_ == nullptr || contentSnapshot == nullptr)
        return { AutoTuneApplyStatus::NoContent };

    const auto scaleSnap = makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_).value_or(ScaleSnapConfig{});
    std::optional<std::vector<std::size_t>> targets;
    if (!interactionState_.noteSelection.empty()) {
        targets.emplace();
        for (const int index : interactionState_.noteSelection.selectedIndices)
            if (index >= 0)
                targets->push_back(static_cast<std::size_t>(index));
    }
    const auto plan = contentCommands_->planAutoSnap(
        *contentSnapshot, targets, scaleSnap, getCurrentAutoTuneParams());
    if (plan.status == ContentAutoSnapPlan::Status::NoChange)
        return { AutoTuneApplyStatus::NoChange };
    if (plan.status == ContentAutoSnapPlan::Status::EmptyRange)
        return { AutoTuneApplyStatus::EmptyTargetRange };

    pendingUndoDescription_ = LOC_RAW("Pitch snap");
    if (!commitEditedContentNotesAndSegments(
            *contentSnapshot, plan.correction.notes, plan.correction.segments,
            plan.correction.affectedRange)) {
        pendingUndoDescription_ = {};
        return { AutoTuneApplyStatus::NoContent };
    }
    return { AutoTuneApplyStatus::Applied };
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        // 与滚轮竖移共用增量平移路径（moveImageSection + 条带补绘），
        // 移除回调内专属的全量标脏与重建
        applyVerticalPan();
    }
}

std::vector<Note> PianoRollComponent::getEditedContentNotesCopy() const {
    return cachedNotes_;
}

void PianoRollComponent::updateScrollBars() {
    // 唯一滑杆同步挂点：所有相机 / pixelsPerSemitone_ 变更路径最终都经过这里
    syncZoomControls();
    syncNoteAccessibilityItems();
    // Vertical（水平滚动条已由 TimelineOverview 永久替代）
    float totalHeight = getTotalHeight();
    int visibleHeight = getTimelineContentViewportHeight();
    visibleHeight = juce::jmax(1, visibleHeight);

    verticalScrollBar_.setRangeLimits(0.0, totalHeight, juce::dontSendNotification);
    verticalScrollBar_.setCurrentRange(verticalScrollOffset_, visibleHeight, juce::dontSendNotification);
}

// ============================================================================
// v12 New: Camera-based viewport functions
// ============================================================================

ViewMapper PianoRollComponent::makeViewMapper() const noexcept {
    return ViewMapper{
        camera_.visibleStartSeconds,
        camera_.pixelsPerSecond,
        getTimelineContentStartX(),
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        pixelsPerSemitone_,
        verticalScrollOffset_,
        maxMidi_
    };
}

ViewMapper PianoRollComponent::makeViewMapperForView(const ViewportState& view) const noexcept {
    return ViewMapper{
        view.camera.visibleStartSeconds,
        view.camera.pixelsPerSecond,
        getTimelineContentStartX(),
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        view.pixelsPerSemitone,
        view.verticalScrollOffset,
        maxMidi_
    };
}

// ============================================================================
// Accessibility: piano roll for screen readers, note speech and keyboard audition
// ============================================================================

void PianoRollComponent::syncNoteAccessibilityItems()
{
    if (!accessibilityModeEnabled()) {
        clearNoteAccessibilityItems();
        return;
    }

    const auto& notes = getCommittedNotes();
    while (noteAccessibilityItems_.size() > notes.size())
    {
        removeChildComponent(noteAccessibilityItems_.back().get());
        noteAccessibilityItems_.pop_back();
    }

    for (size_t index = noteAccessibilityItems_.size(); index < notes.size(); ++index)
    {
        auto item = std::make_unique<NoteAccessibilityItem>(*this, static_cast<int>(index));
        addChildComponent(*item);
        noteAccessibilityItems_.push_back(std::move(item));
    }

    for (size_t index = 0; index < notes.size(); ++index)
    {
        auto& item = *noteAccessibilityItems_[index];
        item.setNoteIndex(static_cast<int>(index));
        item.setBounds(getNoteBounds(notes[index]));
        item.setVisible(true);
    }
}

void PianoRollComponent::clearNoteAccessibilityItems()
{
    for (auto& item : noteAccessibilityItems_)
        removeChildComponent(item.get());
    noteAccessibilityItems_.clear();
}

std::unique_ptr<juce::AccessibilityHandler> PianoRollComponent::createAccessibilityHandler()
{
    if (!accessibilityModeEnabled())
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::ignored);

    // The title is computed on every request so NVDA+Up Arrow reads the current note
    // without timers or extra events.
    class NotesAccessibilityHandler final : public juce::AccessibilityHandler {
    public:
        explicit NotesAccessibilityHandler(PianoRollComponent& owner)
            : juce::AccessibilityHandler(owner, juce::AccessibilityRole::list), owner_(owner) {}

        juce::String getTitle() const override
        {
            juce::String title = LOC_RAW("Notes");
            const auto& selected = owner_.interactionState_.noteSelection.selectedIndices;
            if (selected.size() == 1) {
                const auto line = owner_.describeKeyboardNote(owner_.getCommittedNotes(), selected.front(),
                                                              PianoRollComponent::NoteDetail::Navigation);
                if (line.isNotEmpty())
                    title << ", " << line;
            } else if (selected.size() > 1) {
                title << ", " << Loc::format(LOC_RAW("{0} selected"), juce::String(static_cast<int>(selected.size())));
            }
            return title;
        }

    private:
        PianoRollComponent& owner_;
    };

    return std::make_unique<NotesAccessibilityHandler>(*this);
}

juce::String PianoRollComponent::describeKeyboardNote(const std::vector<Note>& notes, int index,
                                                      NoteDetail detail) const
{
    if (index < 0 || index >= static_cast<int>(notes.size()))
        return {};

    const auto& note = notes[static_cast<size_t>(index)];
    // originalPitch is the sung pitch; old projects and drawn notes may not have it.
    const float sungHz = note.originalPitch > 0.0f ? note.originalPitch : note.pitch;
    if (sungHz <= 0.0f)
        return {};

    const float sungMidi = PitchUtils::freqToMidi(sungHz);
    const float targetMidi = note.pitch > 0.0f ? PitchUtils::freqToMidi(note.pitch) + note.pitchOffset
                                               : sungMidi;
    const std::string sharpWord = LOC_RAW("sharp").toStdString();
    const auto text = [&sharpWord](float midi) {
        return juce::String::fromUTF8(NoteSpeech::describeMidi(midi, sharpWord).c_str());
    };

    // When navigating and after a move, speak only how the note sounds now (the target).
    // The sung pitch and the rest are spoken by the I key (NoteDetail::Full).
    if (detail == NoteDetail::Target)
        return text(targetMidi);
    if (detail == NoteDetail::Navigation) {
        // With the modulation or drift tool add its value: that is what is being edited.
        juce::String line = text(targetMidi);
        if (currentTool_ == ToolId::PitchModulation)
            line << ", " << Loc::format(LOC_RAW("modulation {0}"), modulationPercentText(note));
        else if (currentTool_ == ToolId::PitchDrift)
            line << ", " << Loc::format(LOC_RAW("drift {0}"), driftPercentText(note));
        else if (currentTool_ == ToolId::VolumeEnvelope)
            line << ", " << Loc::format(LOC_RAW("volume {0}"), volumeText(note));
        return line;
    }

    // Times are on the project timeline, as in the host, not inside the audio file.
    const double timelineStart = sourceTimeToTimelineTime(note.startTime);
    const double timelineEnd = sourceTimeToTimelineTime(note.endTime);

    // Ordinal by start time, not by array position.
    int ordinal = 1;
    for (const auto& other : notes)
        if (other.startTime < note.startTime)
            ++ordinal;

    juce::String line = Loc::format(LOC_RAW("Note {0} of {1}"),
                                    juce::String(ordinal), juce::String(static_cast<int>(notes.size())));
    line << ", " << Loc::format(LOC_RAW("sung {0}"), text(sungMidi))
         << ", " << Loc::format(LOC_RAW("target {0}"), text(targetMidi))
         << ", " << Loc::format(LOC_RAW("start {0} s, length {1} s"),
                                juce::String(timelineStart, 2), juce::String(timelineEnd - timelineStart, 2));
    if (isOpenDyne()) {
        line << ", " << Loc::format(LOC_RAW("modulation {0}"), modulationPercentText(note))
             << ", " << Loc::format(LOC_RAW("drift {0}"), driftPercentText(note))
             << ", " << Loc::format(LOC_RAW("volume {0}"), volumeText(note));
    }
    return line;
}

// Note level from the volume envelope (the same value the Volume Envelope drag edits).
juce::String PianoRollComponent::volumeText(const Note& note) const
{
    const auto snap = readEditedSnapshot();
    const float db = snap != nullptr ? snap->volumeEnvelope.evalAt(note.startTime) : 0.0f;
    // Round to 0.1 dB so tiny ramps do not read as "-0.0".
    const float rounded = std::round(db * 10.0f) / 10.0f;
    juce::String value = std::abs(rounded) < 0.05f ? juce::String("0")
                                                   : (rounded > 0.0f ? "+" : "") + juce::String(rounded, 1);
    return Loc::format(LOC_RAW("{0} dB"), value);
}

// With several notes selected an edit changes all of them, so name them all, in time order:
// "Notes: 3, A4, B4, C5", or "Notes: 3, 50%" when the value is the same for each.
juce::String PianoRollComponent::describeEditedSelection(const std::vector<Note>& notes, int index,
                                                         const std::function<juce::String(int)>& valueAt) const
{
    const int count = static_cast<int>(notes.size());
    std::vector<int> selected;
    for (int i : interactionState_.noteSelection.selectedIndices)
        if (i >= 0 && i < count)
            selected.push_back(i);
    if (selected.size() < 2)
        return index >= 0 && index < count ? valueAt(index) : juce::String();

    std::sort(selected.begin(), selected.end(), [&notes](int a, int b) {
        return notes[static_cast<size_t>(a)].startTime < notes[static_cast<size_t>(b)].startTime;
    });

    juce::StringArray values;
    for (int i : selected)
        values.add(valueAt(i));
    juce::StringArray distinct(values);
    distinct.removeDuplicates(false);

    // A long list is not worth hearing: the first few are enough to tell what happened.
    constexpr int kMaxSpoken = 5;
    if (distinct.size() == 1)
        values = distinct;
    else if (values.size() > kMaxSpoken)
        values.removeRange(kMaxSpoken, values.size() - kMaxSpoken);

    return Loc::format(LOC_RAW("Notes: {0}, {1}"), juce::String(static_cast<int>(selected.size())),
                       values.joinIntoString(", "));
}

void PianoRollComponent::handleKeyboardNote(PianoRollToolHandler::KeyboardNoteEvent event,
                                            const std::vector<Note>& notes, int index)
{
    if (!accessibilityModeEnabled())
        return;

    using Event = PianoRollToolHandler::KeyboardNoteEvent;
    switch (event) {
        case Event::NoNotes:
            announceForAccessibility(LOC_RAW("No notes"));
            return;
        case Event::NoSelection:
            announceForAccessibility(LOC_RAW("No note selected"));
            return;
        case Event::Selected:
            cancelPendingAudition();
            announceForAccessibility(describeKeyboardNote(notes, index, NoteDetail::Navigation));
            if (index >= 0 && index < static_cast<int>(notes.size()))
                auditionNote(notes[static_cast<size_t>(index)]);
            return;
        case Event::PitchMoved:
            // After a move only the new pitch matters; audio plays once the correction is re-rendered.
            announceForAccessibility(describeEditedSelection(notes, index, [this, &notes](int i) {
                return describeKeyboardNote(notes, i, NoteDetail::Target);
            }));
            scheduleAuditionAfterRender(index);
            return;
        case Event::Info:
            announceForAccessibility(describeKeyboardNote(notes, index, NoteDetail::Full));
            return;
        case Event::Split:
        case Event::SplitInHalf: {
            // Name both parts so it is clear whether the cut separated two different notes.
            const auto parts = describeKeyboardNote(notes, index, NoteDetail::Target) + ", "
                             + describeKeyboardNote(notes, index + 1, NoteDetail::Target);
            announceForAccessibility(Loc::format(event == Event::Split ? LOC_RAW("Note split: {0}")
                                                                    : LOC_RAW("No clear pitch change, split in half: {0}"),
                                              parts));
            return;
        }
        case Event::NoteTooShort:
            announceForAccessibility(LOC_RAW("Note too short to split"));
            return;
        case Event::SplitAtCursor:
            announceForAccessibility(Loc::format(LOC_RAW("Note split at the cursor: {0}"),
                                              describeKeyboardNote(notes, index, NoteDetail::Target) + ", "
                                                  + describeKeyboardNote(notes, index + 1, NoteDetail::Target)));
            return;
        case Event::CursorUnknown:
            announceForAccessibility(LOC_RAW("Edit cursor unknown: works in REAPER with playback stopped"));
            return;
        case Event::CursorOutsideNote:
            announceForAccessibility(LOC_RAW("The edit cursor is not inside the note"));
            return;
        case Event::Merged:
            announceForAccessibility(LOC_RAW("Notes merged") + ", "
                                      + describeKeyboardNote(notes, index, NoteDetail::Navigation));
            return;
        case Event::NoAdjacentNote:
            announceForAccessibility(LOC_RAW("No adjacent next note"));
            return;
        case Event::ModulationChanged:
        case Event::DriftChanged:
            if (index >= 0 && index < static_cast<int>(notes.size())) {
                // Only the new value, like the drag tooltip: "40%", "+80%".
                announceForAccessibility(describeEditedSelection(notes, index, [this, &notes, event](int i) {
                    const auto& note = notes[static_cast<size_t>(i)];
                    return event == Event::ModulationChanged ? modulationPercentText(note)
                                                             : driftPercentText(note);
                }));
                scheduleAuditionAfterRender(index);
            }
            return;
        case Event::AllSelected:
            announceForAccessibility(Loc::format(LOC_RAW("All notes selected: {0}"),
                                              juce::String(static_cast<int>(notes.size()))));
            return;
        case Event::NoteResized:
            if (index >= 0 && index < static_cast<int>(notes.size())) {
                const auto& note = notes[static_cast<size_t>(index)];
                const double start = sourceTimeToTimelineTime(note.startTime);
                const double end = sourceTimeToTimelineTime(note.endTime);
                announceForAccessibility(Loc::format(LOC_RAW("start {0} s, length {1} s"),
                                                  juce::String(start, 2), juce::String(end - start, 2)));
                scheduleAuditionAfterRender(index);
            }
            return;
        case Event::SingleNoteNeeded:
            announceForAccessibility(LOC_RAW("Select a single note"));
            return;
        case Event::RangeSelected:
            // Count first, then the note at the moving end of the range.
            cancelPendingAudition();
            announceForAccessibility(Loc::format(LOC_RAW("Notes selected: {0}, {1}"),
                                              juce::String(static_cast<int>(interactionState_.noteSelection.selectedIndices.size())),
                                              describeKeyboardNote(notes, index, NoteDetail::Navigation)));
            if (index >= 0 && index < static_cast<int>(notes.size()))
                auditionNote(notes[static_cast<size_t>(index)]);
            return;
        case Event::PositionInfo:
        {
            // Shift+I: where the plugin thinks the user is, and where that comes from.
            if (processor_ == nullptr || !processor_->getReaperTransport()) {
                announceForAccessibility(LOC_RAW("Position unknown: works in REAPER"));
                return;
            }
            const auto position = currentSongPosition();
            if (!position) {
                announceForAccessibility(LOC_RAW("Playing: the position is known after a pause or stop"));
                return;
            }
            const auto seconds = juce::String(position->seconds, 2);
            const char* pattern = position->source == SongPosition::Source::Paused ? "Position {0} s, paused"
                                                                                   : "Position {0} s, edit cursor";
            announceForAccessibility(Loc::format(LOC_RAW(pattern), seconds));
            return;
        }
        case Event::ArrowHint:
        {
            // Says what Ctrl+Up/Down edits with the current tool.
            juce::String hint = LOC_RAW("Ctrl+Up/Down changes the pitch");
            if (currentTool_ == ToolId::PitchModulation)
                hint = LOC_RAW("Ctrl+Up/Down changes the modulation");
            else if (currentTool_ == ToolId::PitchDrift)
                hint = LOC_RAW("Ctrl+Up/Down changes the drift");
            else if (currentTool_ == ToolId::VolumeEnvelope)
                hint = LOC_RAW("Ctrl+Up/Down changes the volume");
            announceForAccessibility(hint);
            return;
        }
        case Event::VolumeChanged:
            if (index >= 0 && index < static_cast<int>(notes.size())) {
                // Only the new level, like the other value edits.
                announceForAccessibility(describeEditedSelection(notes, index, [this, &notes](int i) {
                    return volumeText(notes[static_cast<size_t>(i)]);
                }));
                scheduleAuditionAfterRender(index);
            }
            return;
        case Event::Removed:
        {
            // Then the note that took the place of the removed ones, if any.
            juce::String text = Loc::format(LOC_RAW("Notes removed: {0}"), juce::String(index));
            const auto& selected = interactionState_.noteSelection.selectedIndices;
            if (selected.size() == 1) {
                const auto line = describeKeyboardNote(notes, selected.front(), NoteDetail::Navigation);
                if (line.isNotEmpty())
                    text << ", " << line;
            }
            announceForAccessibility(text);
            return;
        }
        case Event::EditFailed:
            // Sighted users see the note stay put and, when the notes changed meanwhile,
            // the selection highlight disappear; say both.
            announceForAccessibility(interactionState_.noteSelection.selectedIndices.empty()
                                      ? LOC_RAW("Could not apply, selection cleared")
                                      : LOC_RAW("Could not apply"),
                           a11y::SpeechQueue::keepAll);
            return;
    }
}

void PianoRollComponent::auditionNote(const Note& note, const RenderCache* renderCacheToOverlay)
{
    if (noteAudition_ == nullptr || audioBuffer_ == nullptr || audioBufferSampleRate_ <= 0.0)
        return;

    // Note times are in source audio time, so the buffer index is computed directly.
    const double sampleRate = audioBufferSampleRate_;
    const int totalSamples = audioBuffer_->getNumSamples();
    const int startSample = juce::jlimit(0, totalSamples,
                                         static_cast<int>(std::llround(note.startTime * sampleRate)));
    int numSamples = juce::jlimit(0, totalSamples - startSample,
                                  static_cast<int>(std::llround(note.getDuration() * sampleRate)));
    numSamples = juce::jmin(numSamples, static_cast<int>(NoteAudition::kMaxDurationSeconds * sampleRate));
    if (numSamples <= 0)
        return;

    // As in export: the original vocal with the rendered correction overlaid where available.
    // Rendered audio is stored at the render rate, so overlay only when the rates match.
    juce::AudioBuffer<float> segment(audioBuffer_->getNumChannels(), numSamples);
    for (int channel = 0; channel < segment.getNumChannels(); ++channel)
        segment.copyFrom(channel, 0, *audioBuffer_, channel, startSample, numSamples);

    const RenderCache* renderCache = renderCacheToOverlay;
    std::shared_ptr<const RenderCache> loadedRenderCache;
    if (renderCache == nullptr && renderCacheProvider_ != nullptr && editedContentKey_.isValid()
        && std::abs(sampleRate - TimeCoordinate::kRenderSampleRate) < 1.0) {
        loadedRenderCache = renderCacheProvider_(editedContentKey_);
        renderCache = loadedRenderCache.get();
    }
    if (renderCache != nullptr)
        renderCache->overlayCanonicalAudio(segment, 0, numSamples, startSample);

    // Note volume (Volume Envelope tool) is not in the render: playback applies it
    // (applyAutomationGain). Apply it here too, or a volume change is not heard.
    // Segment samples are source time, so no time grid mapping is needed.
    if (const auto snapshot = readEditedSnapshot(); snapshot != nullptr && !snapshot->volumeEnvelope.empty()) {
        constexpr float kDbToLinear = 0.11512925465f; // ln(10) / 20
        for (int s = 0; s < numSamples; ++s) {
            const double sourceSeconds = static_cast<double>(startSample + s) / sampleRate;
            const float gain = std::exp(snapshot->volumeEnvelope.evalAt(sourceSeconds) * kDbToLinear);
            for (int channel = 0; channel < segment.getNumChannels(); ++channel)
                segment.getWritePointer(channel)[s] *= gain;
        }
    }

    noteAudition_->play(segment, sampleRate, 0, numSamples);
}

// Same names as the tool buttons in the parameter panel (ParameterPanel).
juce::String PianoRollComponent::keyboardToolName(ToolId tool) const
{
    switch (tool) {
        case ToolId::Select:          return LOC(kToolODSelect);
        case ToolId::Pitch:           return LOC(kToolODPitch);
        case ToolId::PitchModulation: return LOC(kToolODPitchModulation);
        case ToolId::PitchDrift:      return LOC(kToolODPitchDrift);
        case ToolId::VolumeEnvelope:  return LOC(kToolODVolumeEnvelope);
        case ToolId::Scissors:        return LOC(kToolODScissors);
        case ToolId::HandDraw:        return LOC(kTooltipHandDraw);
        case ToolId::DrawNote:        return LOC(kTooltipDrawNote);
        case ToolId::LineAnchor:      return LOC(kTooltipLineAnchor);
        case ToolId::Eraser:          return LOC(kEraser);
        case ToolId::TimeTool:        return LOC_RAW("Time tool");
        case ToolId::Eq:              return "EQ";
        default:                      return {};
    }
}

// Same format as the drag tooltip (drawModDriftDragPreview).
juce::String PianoRollComponent::modulationPercentText(const Note& note) const
{
    // A negative value means the note follows the global Retune Speed knob.
    const float value = note.retuneSpeed >= 0.0f ? note.retuneSpeed : currentRetuneSpeed_;
    return juce::String::formatted("%.0f%%", value * 100.0f);
}

juce::String PianoRollComponent::driftPercentText(const Note& note) const
{
    return juce::String::formatted("%+.0f%%", note.pitchDriftScale * 100.0f);
}

void PianoRollComponent::scheduleAuditionAfterRender(int noteIndex)
{
    if (!accessibilityModeEnabled())
        return;
    // The user can turn this off in Preferences > Editing.
    if (appPreferences_ != nullptr && !appPreferences_->getNoteTransposePreviewEnabled()) {
        cancelPendingAudition();
        return;
    }

    pendingAuditionNoteIndex_ = noteIndex;
    pendingAuditionContentKey_ = editedContentKey_;

    // Edited notes are auditioned from the rendered correction. Without a usable
    // canonical cache there is no corrected output to play.
    const bool canonicalAuditionAvailable = renderCacheProvider_ != nullptr
        && editedContentKey_.isValid()
        && audioBufferSampleRate_ > 0.0
        && std::abs(audioBufferSampleRate_ - TimeCoordinate::kRenderSampleRate) < 1.0;
    if (!canonicalAuditionAvailable) {
        playPendingAudition(true);
        return;
    }

    const auto cache = renderCacheProvider_(editedContentKey_);
    if (cache == nullptr) {
        playPendingAudition(true);
        return;
    }
    // Volume-only edits do not invalidate the render. Pitch edits enqueue a render
    // synchronously before returning, so a settled cache here means there is
    // nothing to wait for.
    if (!cache->isCanonicalSettled())
        return;
    playPendingAudition(false);
}

void PianoRollComponent::playPendingAudition(bool renderFailed)
{
    if (renderFailed) {
        cancelPendingAudition();
        announceForAccessibility(LOC_RAW("Not rendered yet"));
        return;
    }

    const int noteIndex = pendingAuditionNoteIndex_;
    if (noteIndex < 0)
        return;

    const auto& notes = getCommittedNotes();
    if (noteIndex >= static_cast<int>(notes.size())) {
        cancelPendingAudition();
        return;
    }

    std::shared_ptr<const RenderCache> cache;
    if (renderCacheProvider_ != nullptr && editedContentKey_.isValid()
        && audioBufferSampleRate_ > 0.0
        && std::abs(audioBufferSampleRate_ - TimeCoordinate::kRenderSampleRate) < 1.0)
        cache = renderCacheProvider_(editedContentKey_);

    if (cache == nullptr) {
        cancelPendingAudition();
        announceForAccessibility(LOC_RAW("Not rendered yet"));
        return;
    }
    // A completed terminal event can still belong to an older render while the
    // cache is rendering a newer revision: keep waiting for the next terminal.
    if (!cache->isCanonicalSettled())
        return;

    cancelPendingAudition();
    auditionNote(notes[static_cast<size_t>(noteIndex)], cache.get());
}

void PianoRollComponent::cancelPendingAudition()
{
    pendingAuditionNoteIndex_ = -1;
    pendingAuditionContentKey_ = {};
}

void PianoRollComponent::handleRenderTerminal(ContentKey key, bool completed)
{
    if (!accessibilityModeEnabled()) {
        cancelPendingAudition();
        return;
    }
    if (pendingAuditionNoteIndex_ < 0 || key != pendingAuditionContentKey_)
        return;
    playPendingAudition(!completed);
}

} // namespace OpenTune
