#include "PianoRollComponent.h"
#include "../../PluginProcessor.h"
#include "../../Utils/LocalizationManager.h"
#include "../Utils/AppLogger.h"
#include "../../Utils/PianoRollEditAction.h"
#include "../../Utils/PianoRollNotePatchAction.h"
#include "../../Utils/VolumeEnvelopeEditAction.h"
#include "../../Utils/TimeGridEditAction.h"   // 閳库槄?vocal-time-stretch ?.7
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
#include "TimelineViewportPolicy.h"
#include "TimelineLayerComposer.h"
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

// 检查 [startFrame, endFrameExclusive) 内每帧都被 correction 覆盖（无空洞）。
// 与播放路径一致：基于 forEachCorrectionF0Span 的 span 遍历，
// f0Data 长度不足的 segment 视为无覆盖。合法范围：0 <= startFrame <= endFrameExclusive <= size()。
bool isFullyCorrectedInRange(const PitchCurveSnapshot& curve, int startFrame, int endFrameExclusive)
{
    if (startFrame < 0 || endFrameExclusive > static_cast<int>(curve.size())
        || endFrameExclusive <= startFrame)
        return false;
    bool fullyCorrected = true;
    curve.forEachCorrectionF0Span(startFrame, endFrameExclusive,
        [&](int, const float* values, int) {
            if (values == nullptr)
                fullyCorrected = false;
        });
    return fullyCorrected;
}

} // namespace

void PianoRollComponent::initializeUIComponents() {
    setWantsKeyboardFocus(true);
    addAndMakeVisible(verticalScrollBar_);
    verticalScrollBar_.addListener(this);
    verticalScrollBar_.setAutoHide(false);

    // OpenDyne：双击滚动条 = 缩放到全部音符
    fitToAllNotesOnDoubleClick_ = std::make_unique<FitToAllNotesOnDoubleClick>(*this);
    verticalScrollBar_.addMouseListener(fitToAllNotesOnDoubleClick_.get(), false);

    scrollModeToggleButton_.setButtonText(scrollMode_ == ScrollMode::Continuous ? "Cont" : "Page");
    scrollModeToggleButton_.setFontHeight(11.0f);
    scrollModeToggleButton_.onClick = [this] {
        if (scrollMode_ == ScrollMode::Page) {
            setScrollMode(ScrollMode::Continuous);
            scrollModeToggleButton_.setButtonText("Cont");
        } else {
            setScrollMode(ScrollMode::Page);
            scrollModeToggleButton_.setButtonText("Page");
        }
    };
    addAndMakeVisible(scrollModeToggleButton_);
    scrollModeToggleButton_.setTooltip(LOC(kTooltipScrollMode));

    // Time/Bars 切换按钮
    timeUnitToggleButton_.setFontHeight(11.0f);
    timeUnitToggleButton_.setButtonText(displayMode_ == TimelineDisplayMode::Time ? "Time" : "BPM");
    timeUnitToggleButton_.setTooltip(LOC(kTooltipTimeUnit));
    timeUnitToggleButton_.onClick = [this] {
        const auto nextMode = (displayMode_ == TimelineDisplayMode::Time)
            ? TimelineDisplayMode::Bars
            : TimelineDisplayMode::Time;
        setTimelineDisplayMode(nextMode);
        listeners_.call([nextMode](Listener& l) { l.timelineDisplayModeChanged(nextMode); });
    };
    addAndMakeVisible(timeUnitToggleButton_);

    scrollVBlankAttachment_ = std::make_unique<juce::VBlankAttachment>(
        this, [this](double timestampSec) { onScrollVBlankCallback(timestampSec); });

    scrollModeToggleButton_.toFront(false);
    timeUnitToggleButton_.toFront(false);
}



void PianoRollComponent::initializeRenderer() {
    renderer_ = std::make_unique<PianoRollRenderer>();
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
    toolCtx.commitNotesAndSegments = [this](const std::vector<Note>& notes,
                                            const std::vector<PitchCorrectionSegment>& segments,
                                            F0FrameRange affectedRange) {
        const auto snap = readEditedSnapshot();
        if (snap == nullptr) return ContentCommitSnapshot{};
        return commitEditedContentNotesAndSegments(*snap, notes, segments, affectedRange);
    };
    // ── OpenDyne 契约回调（Pitch/Scissors/Gain 域） ──
    toolCtx.commitVolumeEnvelope = [this](AutomationLane before, AutomationLane after) -> ContentCommitSnapshot {
        if (contentCommands_ == nullptr || !editedContentKey_.isValid()) return nullptr;
        const auto committedSnap = contentCommands_->commitVolumeEnvelope(editedContentKey_, after);
        if (committedSnap != nullptr && processor_ != nullptr) {
            processor_->getUndoManager().addAction(std::make_unique<VolumeEnvelopeEditAction>(
                contentCommands_, editedContentKey_, juce::String::fromUTF8(u8"音量包络"),
                std::move(before), std::move(after)));
        }
        refreshEditedContentNotes();
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        overlay_->repaint();
        if (committedSnap != nullptr)
            listeners_.call([](Listener& listener) { listener.contentEdited(); });
        return committedSnap;
    };
    toolCtx.replaceContentNotesForFullMutation = [this](const std::vector<Note>& notes) {
        return contentCommands_->replaceContentNotesForFullMutation(editedContentKey_, notes);
    };
    toolCtx.republishPlaybackSource = [this]() {
        if (contentCommands_ == nullptr || !editedContentKey_.isValid()) return;
        contentCommands_->republishPlaybackSource(editedContentKey_);
    };
    toolCtx.pushUndoAction = [this](std::unique_ptr<UndoAction> action) {
        if (processor_ != nullptr && action != nullptr)
            processor_->getUndoManager().addAction(std::move(action));
    };
    // Pitch 工具与 AUTO 同源的 scale snap：唯一映射入口 ScaleUiMapping
    toolCtx.getActiveScaleSnap = [this]() -> std::optional<ScaleSnapConfig> {
        return makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);
    };
    toolCtx.getPitchCurve = [this]() { return currentCurve_; };
    toolCtx.getEditableContentSnapshot = [this]() { return readEditedSnapshot(); };
    toolCtx.getOriginalF0 = [this]() -> std::vector<float> {
        if (!currentCurve_) return {};
        auto snap = currentCurve_->getSnapshot();
        if (!snap) return {};
        return snap->getOriginalF0();
    };
    toolCtx.getF0Timeline = [this]() -> F0Timeline {
        return currentF0Timeline();
    };
    toolCtx.getMinMidi = [this]() { return minMidi_; };
    toolCtx.getMaxMidi = [this]() { return maxMidi_; };
    toolCtx.getRetuneSpeed = [this]() { return currentRetuneSpeed_; };
    toolCtx.getVibratoDepth = [this]() { return currentVibratoDepth_; };
    toolCtx.getVibratoRate = [this]() { return currentVibratoRate_; };
    toolCtx.calculateEffectivePIP = [this](Note& note) -> float { return calculateEffectivePIP(note); };
    toolCtx.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& { return shortcutSettings_; };
    toolCtx.setCurrentTool = [this](ToolId tool) { setCurrentTool(tool); };
    toolCtx.showToolSelectionMenu = [this]() {
        // 在当前鼠标屏幕位置弹出纵向图标工具栏
        auto mousePos = juce::Desktop::getInstance().getMousePosition();
        showToolSelectionBar(mousePos);
    };
    toolCtx.notifyAutoTuneRequested = [this]() { listeners_.call([](Listener& l) { l.autoTuneRequested(); }); };
    toolCtx.notifyPlayPauseToggle = [this]() { listeners_.call([](Listener& l) { l.playPauseToggleRequested(); }); };
    toolCtx.notifyStopPlayback = [this]() { listeners_.call([](Listener& l) { l.stopPlaybackRequested(); }); };
    toolCtx.notifyEscapeKey = [this]() { listeners_.call([](Listener& l) { l.escapeKeyPressed(); }); };
    toolCtx.notifyNoteOffsetChanged = [this](size_t noteIndex, float oldOffset, float newOffset) {
        listeners_.call([noteIndex, oldOffset, newOffset](Listener& l) { l.noteOffsetChanged(noteIndex, oldOffset, newOffset); });
    };
    toolCtx.getPianoKeyWidth = [this]() { return pianoKeyWidth_; };
    toolCtx.getContentProjection = [this]() { return activeContentProjection(); };
    toolCtx.getHandDrawPreviewBounds = [this]() { return getHandDrawPreviewBounds(); };
    toolCtx.getLineAnchorPreviewBounds = [this]() { return getLineAnchorPreviewBounds(); };

    toolCtx.getDirtyStartTime = [this]() { return interactionState_.drawing.dirtyStartTime; };
    toolCtx.setDirtyStartTime = [this](double v) { interactionState_.drawing.dirtyStartTime = v; };
    toolCtx.getDirtyEndTime = [this]() { return interactionState_.drawing.dirtyEndTime; };
    toolCtx.setDirtyEndTime = [this](double v) { interactionState_.drawing.dirtyEndTime = v; };

    toolCtx.getDrawingNoteStartTime = [this]() { return interactionState_.drawing.drawingNoteStartTime; };
    toolCtx.setDrawingNoteStartTime = [this](double v) { interactionState_.drawing.drawingNoteStartTime = v; };
    toolCtx.getDrawingNoteEndTime = [this]() { return interactionState_.drawing.drawingNoteEndTime; };
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
    toolCtx.invalidateInteractionPreview = [this](const juce::Rectangle<int>& bounds) {
        invalidateInteractionPreview(bounds);
    };
    toolCtx.setMouseCursor = [this](const juce::MouseCursor& c) { setMouseCursor(c); };
    toolCtx.grabKeyboardFocus = [this]() { grabKeyboardFocus(); };
    toolCtx.getAudioEditingScheme = [this]() { return audioEditingScheme_; };
    toolCtx.notifyPlayheadChange = [this](double time) {
        const auto seekRevision = playHeadState_.hostPositionRevision.load(std::memory_order_acquire);
        bool requestDispatched = false;
        listeners_.call([&](Listener& l) {
            if (l.playheadPositionChangeRequested(time))
                requestDispatched = true;
        });
        userScrollHold_ = false;
        if (requestDispatched) {
            seekSentRevision_ = seekRevision;
            pendingSeekTime_ = time;
        } else {
            pendingSeekTime_ = -1.0;
            seekSentRevision_ = 0;
        }
    };
    toolCtx.notifyPitchCurveEdited = [this](int s, int e) {
        listeners_.call([s, e](Listener& l) { l.pitchCurveEdited(s, e); });
    };

    toolCtx.applyManualCorrection = [this](std::vector<PianoRollToolHandler::ManualCorrectionOp> ops, int s, int e, bool render) {
        return applyManualCorrectionPatch(ops, s, e, render);
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
        if (processor_ == nullptr || !editedContentKey_.isValid()) return false;
        if (newSnap == nullptr || oldSnap == nullptr) return false;

        auto action = std::make_unique<TimeGridEditAction>(
            contentCommands_,
            editedContentKey_,
            description.isNotEmpty() ? description : juce::String("编辑时间网格"),
            std::move(oldSnap),
            newSnap);
        const bool published = contentCommands_->setTimeGrid(editedContentKey_, newSnap);
        if (!published) return false;
        processor_->getUndoManager().addAction(std::move(action));

        {
            auto snap = readEditedSnapshot();
            if (snap) lastKnownTimeGridRevision_ = snap->timeGridRevision;
        }
        requestContentRedraw();
        return true;
    };
    toolCtx.repaintTimeGridHandles = [this]() {
        overlay_->repaint();
    };

    return toolCtx;
}

// ============================================================================
// OpenDyne 右键横向工具选择弹出条
// ============================================================================

namespace {

struct ToolBarItem {
    ToolId id;
    const char* name;
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
        default:                      return {};
    }
}

} // namespace

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
            { { ToolId::Select,    "Select",   "F1",  []{ return makeToolIcon(ToolId::Select); } } },
            { { ToolId::Pitch,     "Pitch",    "F2",  []{ return makeToolIcon(ToolId::Pitch); } }, true },
            { { ToolId::HandDraw,  "Hand Draw","5",   []{ return ToolbarIcons::getHandDrawIcon(); } } },
            { { ToolId::VolumeEnvelope, "Volume", "F4", []{ return makeToolIcon(ToolId::VolumeEnvelope); } } },
            { { ToolId::TimeTool,  "Time",     "T",   []{ return makeToolIcon(ToolId::TimeTool); } } },
            { { ToolId::Scissors,  "Scissors", "F6",  []{ return makeToolIcon(ToolId::Scissors); } } },
        };
        subItems = {
            { ToolId::PitchModulation, "Modulation", "F2x2", []{ return makeToolIcon(ToolId::PitchModulation); } },
            { ToolId::PitchDrift,      "Drift",      "F2x3", []{ return makeToolIcon(ToolId::PitchDrift); } },
        };
    } else {
        mainItems = {
            { { ToolId::Select,     "Select",      "3", []{ return makeToolIcon(ToolId::Select); } } },
            { { ToolId::DrawNote,   "Draw Note",   "2", []{ return ToolbarIcons::getDrawNoteIcon(); } } },
            { { ToolId::LineAnchor, "Line Anchor", "4", []{ return ToolbarIcons::getLineAnchorIcon(); } } },
            { { ToolId::HandDraw,   "Hand Draw",   "5", []{ return ToolbarIcons::getHandDrawIcon(); } } },
        };
        // Time 与侧栏一致：OpenTune 仅 experimental 开启时可选（OpenDyne 分支恒含）
        if (experimentalFeaturesEnabled_)
            mainItems.push_back({ { ToolId::TimeTool, "Time", "T", []{ return makeToolIcon(ToolId::TimeTool); } } });
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

            if (!iconPath_.isEmpty()) {
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

            // 展开子按钮时只保留 Pitch 主按钮，隐藏其余；收起时全部显示
            int x = pad_;
            for (int i = 0; i < static_cast<int>(mainButtons_.size()); ++i) {
                if (showSub && i != pitchButtonIdx_) {
                    mainButtons_[i]->setVisible(false);
                } else {
                    mainButtons_[i]->setVisible(true);
                    mainButtons_[i]->setBounds(x, pad_, mainWidths_[i], btnSize_);
                    x += mainWidths_[i] + gap_;
                }
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

            // 弹窗宽度：展开时仅占 Pitch 一格；收起时等于全部一级按钮总宽
            int totalW;
            if (showSub && pitchButtonIdx_ >= 0) {
                totalW = pad_ * 2 + mainWidths_[pitchButtonIdx_];
            } else {
                totalW = pad_ * 2;
                for (int i = 0; i < static_cast<int>(mainButtons_.size()); ++i)
                    totalW += mainWidths_[i] + gap_;
                if (!mainButtons_.empty()) totalW -= gap_;
            }

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

            // 深色背景 + 圆角 + 阴影
            {
                const auto& style = UIColors::currentThemeStyle();
                g.setColour(juce::Colours::black.withAlpha(style.shadowAlpha * 0.5f));
                g.fillRoundedRectangle(bounds.translated(0, 2).expanded(0, 2), radius + 2.0f);
            }

            if (themeId == ThemeId::Overdose) {
                juce::ColourGradient bg(
                    UIColors::backgroundDark.brighter(0.06f), bounds.getX(), bounds.getY(),
                    UIColors::backgroundDark.darker(0.08f), bounds.getX(), bounds.getBottom(), false);
                g.setGradientFill(bg);
            } else {
                g.setColour(UIColors::backgroundDark);
            }
            g.fillRoundedRectangle(bounds, radius);

            // 顶部内边缘线
            g.setColour(UIColors::panelBorder.withAlpha(0.1f));
            g.drawLine(bounds.getX() + radius, bounds.getY() + 0.5f,
                       bounds.getRight() - radius, bounds.getY() + 0.5f, 1.0f);
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

PianoRollComponent::PianoRollComponent(const PlayHeadState& playHeadState)
    : playHeadState_(playHeadState) {
    initializeUIComponents();
    initializeRenderer();
    initializeToolHandler();
    overlay_ = std::make_unique<PianoRollOverlayComponent>(*this);
    addAndMakeVisible(*overlay_);
    overlay_->setInterceptsMouseClicks(false, false);
}

PianoRollComponent::~PianoRollComponent() {
    scrollVBlankAttachment_.reset();
    verticalScrollBar_.removeListener(this);
}

bool PianoRollComponent::applyCorrectionToEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate)
{
    if (!currentCurve_) {
        return false;
    }
    const auto contentSnapshot = readEditedSnapshot();
    if (contentSnapshot == nullptr)
        return false;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) {
        return false;
    }

    auto notes = getCommittedNotes();
    // 全局参数调节（无选中）语义：应用到整条 clip 的每个音符，
    // 同步写入音符字段（否则 PitchCurve 渲染时音符级旧值覆盖新全局参数）。
    for (auto& note : notes) {
        note.retuneSpeed = retuneSpeed;
        note.vibratoDepth = vibratoDepth;
        note.vibratoRate = vibratoRate;
        note.dirty = true;
    }
    auto editedCurve = currentCurve_->clone();
    editedCurve->applyCorrectionToRange(notes, 0, f0tl.endFrameExclusive(),
                                        static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
                                        retuneSpeed, vibratoDepth, vibratoRate);

    const auto snap = editedCurve->getSnapshot();
    const auto allSegments = snap->getCorrectionSegments();
    const F0FrameRange affectedRange{0, f0tl.endFrameExclusive()};

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    if (!commitEditedContentNotesAndSegments(*contentSnapshot, notes, allSegments, affectedRange)) {
        return false;
    }
    return true;
}

void PianoRollComponent::setProcessor(OpenTuneAudioProcessor* processor)
{
    processor_ = processor;
    refreshEditedContentNotes();
}

void PianoRollComponent::setContentCommands(std::shared_ptr<ContentEditCommands> commands)
{
    contentCommands_ = std::move(commands);
}


void PianoRollComponent::refreshEditedContentNotes()
{
    cachedNotes_.clear();

    if (processor_ != nullptr && editedContentKey_.isValid()) {
        if (auto snap = readEditedSnapshot()) {
            cachedNotes_ = snap->notes;
        }
    }
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();
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
    interactionState_.noteDraft.contentDirty = false;
    interactionState_.noteDraft.baselineNotes = cachedNotes_;
    interactionState_.noteDraft.workingNotes = cachedNotes_;
}

bool PianoRollComponent::commitNoteDraft()
{
    if (!interactionState_.noteDraft.active) {
        return true;
    }

    if (!interactionState_.noteDraft.contentDirty) {
        clearNoteDraft();
        pendingUndoDescription_ = {};
        undoSnapshotCaptured_ = false;
        return true;
    }

    if (processor_ == nullptr || !editedContentKey_.isValid()) {
        return false;
    }

    // Build ContentNoteRangePatch via merge-based diff (content-based, not index-based)
    const auto& baseline = interactionState_.noteDraft.baselineNotes;
    const auto& working = interactionState_.noteDraft.workingNotes;

    ContentNoteRangePatch patch;
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;

    // Notes are sorted by startTime. Walk both arrays simultaneously.
    // When startTime matches 锟?same note, compare content.
    // When startTime differs 锟?deletion or insertion.
    auto notesContentEqual = [](const Note& a, const Note& b) {
        return a.endTime == b.endTime
            && a.pitch == b.pitch
            && a.pitchOffset == b.pitchOffset
            && a.retuneSpeed == b.retuneSpeed
            && a.pitchDriftScale == b.pitchDriftScale
            && a.vibratoDepth == b.vibratoDepth
            && a.vibratoRate == b.vibratoRate;
    };

    size_t i = 0, j = 0;
    while (i < baseline.size() || j < working.size()) {
        const bool bHas = i < baseline.size();
        const bool wHas = j < working.size();

        if (bHas && wHas && baseline[i].startTime == working[j].startTime) {
            // Same position 锟?compare content for modification
            if (!notesContentEqual(baseline[i], working[j])) {
                dirtyStartTime = std::min(dirtyStartTime, baseline[i].startTime);
                dirtyEndTime = std::max({dirtyEndTime, baseline[i].endTime, working[j].endTime});
            }
            i++; j++;
        } else if (!wHas || (bHas && baseline[i].startTime < working[j].startTime)) {
            // Baseline note at earlier position was deleted
            dirtyStartTime = std::min(dirtyStartTime, baseline[i].startTime);
            dirtyEndTime = std::max(dirtyEndTime, baseline[i].endTime);
            i++;
        } else {
            // Working note at earlier position was inserted
            dirtyStartTime = std::min(dirtyStartTime, working[j].startTime);
            dirtyEndTime = std::max(dirtyEndTime, working[j].endTime);
            j++;
        }
    }

    // No actual changes 锟?skip commit, not a failure
    if (dirtyEndTime <= dirtyStartTime) {
        clearNoteDraft();
        return true;
    }

    patch.affectedRange.startSeconds = dirtyStartTime;
    patch.affectedRange.endSeconds = dirtyEndTime;

    // Extract after notes overlapping the dirty time range
    auto overlapsRange = [dirtyStartTime, dirtyEndTime](const Note& n) {
        return n.endTime > dirtyStartTime && n.startTime < dirtyEndTime;
    };
    for (const auto& n : working) {
        if (overlapsRange(n)) patch.afterNotesInRange.push_back(n);
    }

    const auto committedSnap = contentCommands_->commitNoteTopologyPatch(editedContentKey_, patch);
    if (!committedSnap) {
        return false;
    }

    cachedNotes_ = committedSnap->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();

    // Build the before-patch from baseline notes in the same seconds range.
    // Note-only undo uses seconds-based PianoRollNotePatchAction — no frame
    // conversion, no segment involvement, same coordinate system as commitNoteTopologyPatch().
    ContentNoteRangePatch beforePatch;
    beforePatch.affectedRange = patch.affectedRange;
    for (const auto& n : baseline) {
        if (overlapsRange(n)) beforePatch.afterNotesInRange.push_back(n);
    }

    auto action = std::make_unique<PianoRollNotePatchAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("缂栬緫"),
        std::move(beforePatch),
        std::move(patch));

    if (processor_ != nullptr)
        processor_->getUndoManager().addAction(std::move(action));

    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    clearNoteDraft();
    lastKnownNotesRevision_ = committedSnap->notesRevision;
    requestContentRedraw();
    return true;
}

void PianoRollComponent::clearNoteDraft()
{
    interactionState_.noteDraft.clear();
}

ContentCommitSnapshot PianoRollComponent::commitEditedContentPitchCorrectionSegments(const std::vector<PitchCorrectionSegment>& segments,
                                                                         F0FrameRange affectedRange)
{
    // Delegate to the range-scoped merge path.  setPitchCorrectionSegments does
    // full replacement which would discard segments outside affectedRange.
    // commitEditedContentNotesAndSegments 锟?commitContentNotesAndSegments
    // performs range-scoped merge (keptBefore + incoming + keptAfter).
    const auto snap = readEditedSnapshot();
    if (snap == nullptr) return {};
    return commitEditedContentNotesAndSegments(*snap, cachedNotes_, segments, affectedRange);
}

ContentCommitSnapshot PianoRollComponent::commitEditedContentNotesAndSegments(const EditableContentSnapshot& snapshot,
                                                               const std::vector<Note>& notes,
                                                               const std::vector<PitchCorrectionSegment>& segments,
                                                               F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid() || snapshot.pitchCurve == nullptr) {
        return {};
    }

    // Capture range-scoped before data directly from the single snapshot (no full re-read).
    const auto curveSnapshot = snapshot.pitchCurve->getSnapshot();
    if (curveSnapshot == nullptr) {
        return {};
    }
    const F0Timeline f0tl{ curveSnapshot->getHopSize(), curveSnapshot->getSampleRate(),
                           static_cast<int>(curveSnapshot->size()) };
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.endFrameExclusive);

    auto extractNotesInRange = [](const std::vector<Note>& notes, double startSec, double endSec) {
        std::vector<Note> result;
        for (const auto& note : notes) {
            if (note.startTime < endSec && note.endTime > startSec)
                result.push_back(note);
        }
        return result;
    };

    auto extractSegmentsInRange = [](const std::vector<PitchCorrectionSegment>& segs, int startFrame, int endFrame) {
        std::vector<PitchCorrectionSegment> result;
        for (const auto& seg : segs) {
            if (seg.endFrame <= startFrame || seg.startFrame >= endFrame)
                continue;  // Outside range
            
            // Clip to range boundaries (split-preserve for boundary-crossing segments)
            const int clipStart = std::max(seg.startFrame, startFrame);
            const int clipEnd = std::min(seg.endFrame, endFrame);
            if (clipEnd <= clipStart)
                continue;  // Empty after clip
            
            PitchCorrectionSegment clipped = seg;
            const int startOffset = clipStart - seg.startFrame;
            const int clipLen = clipEnd - clipStart;
            if (startOffset >= 0 && clipLen > 0 && startOffset + clipLen <= static_cast<int>(seg.f0Data.size())) {
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(seg.f0Data.begin() + startOffset, seg.f0Data.begin() + startOffset + clipLen);
                result.push_back(std::move(clipped));
            }
        }
        return result;
    };

    auto beforeNotes = extractNotesInRange(snapshot.notes, rangeStartSec, rangeEndSec);
    auto beforeSegments = extractSegmentsInRange(curveSnapshot->getCorrectionSegments(), affectedRange.startFrame, affectedRange.endFrameExclusive);

    // Enforce range-scoped contract: filter incoming data so sink never
    // receives notes/segments outside the affected range.
    auto scopedNotes = extractNotesInRange(notes, rangeStartSec, rangeEndSec);
    auto scopedSegments = extractSegmentsInRange(segments, affectedRange.startFrame, affectedRange.endFrameExclusive);

    ContentEditRangeFrames editRange;
    editRange.startFrame = affectedRange.startFrame;
    editRange.endFrameExclusive = affectedRange.endFrameExclusive;

    const auto committedSnap =
        contentCommands_->commitNotesAndSegments(editedContentKey_,
                                                 std::move(scopedNotes),
                                                 std::move(scopedSegments),
                                                 editRange);
    if (!committedSnap) {
        return {};
    }

    // Update local caches from committed state
    cachedNotes_ = committedSnap->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));
    syncF0SelectionToSelectedNotes();

    if (committedSnap->pitchCurve) {
        applyEditedContentCurve(committedSnap->pitchCurve);
    }

    // Capture range-scoped after data from committed snapshot
    auto afterNotes = extractNotesInRange(committedSnap->notes, rangeStartSec, rangeEndSec);
    const auto committedPitchSnapshot = committedSnap->pitchCurve->getSnapshot();
    auto afterSegments = extractSegmentsInRange(
        committedPitchSnapshot->getCorrectionSegments(),
        affectedRange.startFrame,
        affectedRange.endFrameExclusive);

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("缂栬緫"),
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeSegments),
        std::move(afterSegments),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});

    processor_->getUndoManager().addAction(std::move(action));
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    lastKnownNotesRevision_ = committedSnap->notesRevision;
    lastKnownPitchRevision_ = committedSnap->pitchRevision;
    {
        requestContentRedraw();
    }
    return committedSnap;
}

std::vector<PitchCorrectionSegment> PianoRollComponent::getCurrentSegments() const
{
    if (!currentCurve_) return {};
    auto snap = currentCurve_->getSnapshot();
    if (!snap) return {};
    return snap->getCorrectionSegments();
}

void PianoRollComponent::captureBeforeUndoSnapshot()
{
    beforeUndoNotes_ = cachedNotes_;
    beforeUndoSegments_ = getCurrentSegments();
    undoSnapshotCaptured_ = true;
}

void PianoRollComponent::recordUndoAction(const juce::String& description, F0FrameRange affectedRange)
{
    if (processor_ == nullptr || !editedContentKey_.isValid() || !undoSnapshotCaptured_)
        return;

    AppLogger::log("AutoTune: recordUndoAction entry beforeNotes=" + juce::String(static_cast<int>(beforeUndoNotes_.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeUndoSegments_.size()))
        + " cachedNotes=" + juce::String(static_cast<int>(cachedNotes_.size()))
        + " affectedRange=[" + juce::String(affectedRange.startFrame)
        + "," + juce::String(affectedRange.endFrameExclusive) + ")");

    // Extract range-scoped notes and segments for memory-efficient undo.
    // Notes use seconds; segments use frames. Convert frame range to seconds.
    const auto f0tl = currentF0Timeline();
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.endFrameExclusive);

    auto notesInRange = [](const std::vector<Note>& notes, double startSec, double endSec) {
        std::vector<Note> result;
        for (const auto& note : notes) {
            if (note.startTime < endSec && note.endTime > startSec)
                result.push_back(note);
        }
        return result;
    };

    auto segmentsInRange = [](const std::vector<PitchCorrectionSegment>& segments, int startFrame, int endFrameExclusive) {
        std::vector<PitchCorrectionSegment> result;
        for (const auto& seg : segments) {
            if (seg.endFrame <= startFrame || seg.startFrame >= endFrameExclusive)
                continue;  // Outside range
            
            // Clip to range boundaries (split-preserve for boundary-crossing segments)
            const int clipStart = std::max(seg.startFrame, startFrame);
            const int clipEnd = std::min(seg.endFrame, endFrameExclusive);
            if (clipEnd <= clipStart)
                continue;  // Empty after clip
            
            PitchCorrectionSegment clipped = seg;
            const int startOffset = clipStart - seg.startFrame;
            const int clipLen = clipEnd - clipStart;
            if (startOffset >= 0 && clipLen > 0 && startOffset + clipLen <= static_cast<int>(seg.f0Data.size())) {
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(seg.f0Data.begin() + startOffset, seg.f0Data.begin() + startOffset + clipLen);
                result.push_back(std::move(clipped));
            }
        }
        return result;
    };

    auto beforeNotes = notesInRange(beforeUndoNotes_, rangeStartSec, rangeEndSec);
    auto afterNotes = notesInRange(cachedNotes_, rangeStartSec, rangeEndSec);
    auto beforeSegments = segmentsInRange(beforeUndoSegments_, affectedRange.startFrame, affectedRange.endFrameExclusive);
    auto afterSegments = segmentsInRange(getCurrentSegments(), affectedRange.startFrame, affectedRange.endFrameExclusive);

    AppLogger::log("AutoTune: recordUndoAction range-scoped beforeNotes=" + juce::String(static_cast<int>(beforeNotes.size()))
        + " afterNotes=" + juce::String(static_cast<int>(afterNotes.size()))
        + " beforeSegments=" + juce::String(static_cast<int>(beforeSegments.size()))
        + " afterSegments=" + juce::String(static_cast<int>(afterSegments.size())));

    auto action = std::make_unique<PianoRollEditAction>(
        contentCommands_,
        editedContentKey_,
        description.isNotEmpty() ? description : TRANS("缂栬緫"),
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeSegments),
        std::move(afterSegments),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});

    AppLogger::log("AutoTune: recordUndoAction before addAction");
    processor_->getUndoManager().addAction(std::move(action));
    AppLogger::log("AutoTune: recordUndoAction after addAction");
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    beforeUndoNotes_.clear();
    beforeUndoSegments_.clear();
}

bool PianoRollComponent::selectNotesOverlappingFrames(int startFrame, int endFrameExclusive)
{
    const auto& notes = getCommittedNotes();
    const auto f0tl = currentF0Timeline();
    if (notes.empty() || f0tl.isEmpty()) {
        interactionState_.noteSelection.clear();
        interactionState_.selection.hasSelectionArea = false;
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.clearF0Selection();
        overlay_->repaint();
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

    interactionState_.noteSelection.setFromIndices(std::move(selectedIndices),
                                                   static_cast<int>(notes.size()));
    interactionState_.selection.hasSelectionArea = false;
    interactionState_.selection.isSelectingArea = false;
    if (anyOverlap) {
        interactionState_.selection.setF0Range(selectionRange.startFrame, selectionRange.endFrameExclusive);
    } else {
        interactionState_.selection.clearF0Selection();
    }
    overlay_->repaint();

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

juce::Rectangle<int> PianoRollComponent::getSelectionBounds() const
{
    if (!interactionState_.selection.hasSelectionArea) {
        return {};
    }

    const double startTime = std::min(interactionState_.selection.selectionStartTime,
                                      interactionState_.selection.selectionEndTime);
    const double endTime = std::max(interactionState_.selection.selectionStartTime,
                                    interactionState_.selection.selectionEndTime);
    const float minMidi = std::min(interactionState_.selection.selectionStartMidi,
                                   interactionState_.selection.selectionEndMidi);
    const float maxMidi = std::max(interactionState_.selection.selectionStartMidi,
                                   interactionState_.selection.selectionEndMidi);

    const int x1 = sourceTimeToX(startTime);
    const int x2 = sourceTimeToX(endTime);
    const int y1 = static_cast<int>(std::floor(makeViewMapper().midiToY(maxMidi)));
    const int y2 = static_cast<int>(std::ceil(makeViewMapper().midiToY(minMidi)));
    return juce::Rectangle<int>(std::min(x1, x2),
                                std::min(y1, y2),
                                std::max(1, std::abs(x2 - x1)),
                                std::max(1, std::abs(y2 - y1)))
        .expanded(4)
        .getIntersection(getTimelineViewportBounds());
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
    auto beforeBounds = getNotesBounds(beforeNotes);
    auto afterBounds = getNotesBounds(afterNotes);
    auto dirty = beforeBounds.getUnion(afterBounds);
    // 拖拽预览曲线与 OpenDyne energy blob 的视觉范围可能远超 note 高度
    // （曲线斜率 / gainFactor 最大 2.5 倍），拖拽进行中（含 mouseUp 时刻
    // previewSnapshot 已 reset 但必须全高抹除上一帧曲线的场景）dirty 必须
    // 扩展为全视口高度，两种 scheme 共用，否则旧预览曲线像素残留在 content
    // 缓存中。以拖拽状态驱动，不依赖 previewSnapshot 非空。
    if (interactionState_.noteDrag.isDraggingNotes
        || interactionState_.isModDriftDragging
        || interactionState_.isVolumeDragging) {
        const auto viewport = getTimelineViewportBounds();
        dirty = dirty.withY(viewport.getY()).withHeight(viewport.getHeight());
    }
    if (!dirty.isEmpty()) {
        if (zoomPreviewActive_) {
            // 缩放事务期间冻结 Image，仅标记脏，由 endZoomPreview 最终重建
            contentDirty_ = true;
        } else {
            rasterizeContent(dirty);
            repaint(dirty.getX(), dirty.getY(), dirty.getWidth(), dirty.getHeight());
        }
        overlay_->repaint(dirty.getX(), dirty.getY(), dirty.getWidth(), dirty.getHeight());
    }
}

void PianoRollComponent::invalidateSelectionFeedback()
{
    overlay_->repaint();
}

void PianoRollComponent::invalidateInteractionPreview(const juce::Rectangle<int>& bounds)
{
    overlay_->repaint(bounds);
}

bool PianoRollComponent::applyManualCorrectionPatch(const std::vector<PianoRollToolHandler::ManualCorrectionOp>& ops,
                                                     int dirtyStartFrame,
                                                     int dirtyEndFrame,
                                                     bool triggerRenderEvent)
{
    if (!currentCurve_ || ops.empty()) {
        return false;
    }

    auto editedCurve = currentCurve_->clone();
    for (const auto& op : ops) {
        if (op.endFrameExclusive <= op.startFrame) {
            continue;
        }

        editedCurve->setManualCorrectionRange(
            op.startFrame,
            op.endFrameExclusive,
            op.f0Data,
            op.source);
    }

    // dirtyStartFrame/dirtyEndFrame 是所有 manual ops 的 dirty 帧并集（含端点）。
    const F0FrameRange affectedRange{dirtyStartFrame,
                                      dirtyEndFrame >= dirtyStartFrame ? dirtyEndFrame + 1 : dirtyStartFrame};
    if (!commitEditedContentPitchCorrectionSegments(editedCurve->copyCorrectionSegments(), affectedRange)) {
        return false;
    }

    if (triggerRenderEvent && dirtyEndFrame >= dirtyStartFrame) {
        listeners_.call([dirtyStartFrame, dirtyEndFrame](Listener& l) {
            l.pitchCurveEdited(dirtyStartFrame, dirtyEndFrame);
        });
    }

    return true;
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

// ============================================================================
// drawTransientOverlay 鈥?paint transient interaction previews
// ============================================================================

void PianoRollComponent::drawTransientOverlay(juce::Graphics& g)
{
    juce::Graphics::ScopedSaveState overlaySave(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    // note-drag Corrected F0 预览已收敛为 noteDrag.previewSnapshot（renderer 消费），
    // HandDraw/LineAnchor 预览仅 OpenTune
    if (!isOpenDyne()) {
        drawHandDrawPreview(g);
        drawLineAnchorPreview(g);
    }

    // ── OpenDyne transient previews（Scissors 预览线、Mod/Drift tooltip、Volume dB tooltip） ──
    if (isOpenDyne()) {
        drawScissorsPreview(g);
        drawModDriftDragPreview(g);
        drawVolumeDragPreview(g);
    }

    // ⚡️ Cursor preview (豁免路径): DrawNote tool 的绘制中 note preview
    // 杩欐槸浜や簰 cursor preview锛屼笉锟?committed/draft note body
    // The committed/draft note body is drawn by the direct content pass.
    if (interactionState_.drawing.isDrawingNote
        && currentTool_ == ToolId::DrawNote) {
        juce::Graphics::ScopedSaveState previewSave(g);
        double startTime = std::min(interactionState_.drawing.drawingNoteStartTime,
                                    interactionState_.drawing.drawingNoteEndTime);
        double endTime = std::max(interactionState_.drawing.drawingNoteStartTime,
                                  interactionState_.drawing.drawingNoteEndTime);
        float pitch = interactionState_.drawing.drawingNotePitch;

        if (pitch > 0.0f && endTime > startTime) {
            int x1 = sourceTimeToX(startTime);
            int x2 = sourceTimeToX(endTime);
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

    drawSelectionBox(g, UIColors::currentThemeId());
}

// ============================================================================
// drawScissorsPreview — OpenDyne Scissors Tool 切割预览线
// 只绘制在目标 blob 内，不贯穿背景波形（避免表达 PCM 被切开）
// ============================================================================
void PianoRollComponent::drawScissorsPreview(juce::Graphics& g)
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
        const int px = sourceTimeToX(previewTime);
        const int x1 = sourceTimeToX(note.startTime);
        const int x2 = sourceTimeToX(note.endTime);
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
    const auto mousePos = juce::Desktop::getInstance().getMousePosition() - getScreenPosition()
        + juce::Point<int>(0, -rulerHeight_);
    const juce::String text = isModulation
        ? juce::String::formatted("%.0f%%", value * 100.0f)
        : juce::String::formatted("%+.0f%%", value * 100.0f);
    juce::Font font(12.0f);
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle(static_cast<float>(mousePos.x + 12), static_cast<float>(mousePos.y + 12),
                           font.getStringWidth(text) + 12.0f, 20.0f, 4.0f);
    g.setColour(juce::Colours::white);
    g.setFont(font);
    g.drawText(text, mousePos.getX() + 18, mousePos.getY() + 14, font.getStringWidth(text), 14,
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
    const auto mousePos = juce::Desktop::getInstance().getMousePosition() - getScreenPosition()
        + juce::Point<int>(0, -rulerHeight_);
    const juce::String text = juce::String::formatted("%+.1f dB", static_cast<double>(deltaDb));
    juce::Font font(12.0f);
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle(static_cast<float>(mousePos.x + 12), static_cast<float>(mousePos.y + 12),
                           font.getStringWidth(text) + 12.0f, 20.0f, 4.0f);
    g.setColour(juce::Colours::white);
    g.setFont(font);
    g.drawText(text, mousePos.getX() + 18, mousePos.getY() + 14, font.getStringWidth(text), 14,
               juce::Justification::centredLeft);
}

void PianoRollComponent::drawTimeGridHandles(juce::Graphics& g)
{
    if (!isTimeView()) return;
    if (interactionState_.timeTool.selectedHandleId == 0
        && interactionState_.timeTool.hoveredHandleId == 0
        && interactionState_.timeTool.additionalSelectedIds.empty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    const auto contentBounds = juce::Rectangle<int>(
        pianoKeyWidth_, rulerHeight_,
        getTimelineContentViewportWidth(), getTimelineContentViewportHeight());
    g.reduceClipRegion(contentBounds);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
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
    ctx.contents = buildContentRenderItems();
    // 拖拽中的 working TimeGrid snapshot 仅应用于 Overlay 把手绘制
    if (interactionState_.timeTool.isDraggingHandle
        && interactionState_.timeTool.dragWorkingSnapshot != nullptr) {
        for (auto& item : ctx.contents) {
            if (item.active)
                item.timeGrid = interactionState_.timeTool.dragWorkingSnapshot;
        }
    }

    for (const auto& item : ctx.contents) {
        if (item.active)
            renderer_->drawTimeGridHandles(g, ctx, item);
    }
}

void PianoRollComponent::drawSelectedNoteHighlights(juce::Graphics& g)
{
    if (interactionState_.noteSelection.selectedIndices.empty()) return;

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.coords = makeViewMapper();
    ctx.contents = buildContentRenderItems();

    for (const auto& item : ctx.contents) {
        if (!item.active) continue;
        renderer_->drawSelectedNoteHighlights(
            g, ctx, *item.displayNotes,
            interactionState_.noteSelection.selectedIndices, item);
    }
}

void PianoRollComponent::drawF0SelectionHighlight(juce::Graphics& g)
{
    // F0 高亮范围 = 当前有效选择范围：音符派生 F0 选择优先，无音符时回退到框选/全选选区。
    int selStartFrame = 0;
    int selEndFrameExclusive = 0;
    if (!getF0SelectionFrameRange(selStartFrame, selEndFrameExclusive)) {
        if (!getSelectionAreaFrameRange(selStartFrame, selEndFrameExclusive)) {
            return;
        }
    }

    juce::Graphics::ScopedSaveState ss(g);
    g.addTransform(juce::AffineTransform::translation(0.0f, static_cast<float>(rulerHeight_)));

    PianoRollRenderer::RenderContext ctx;
    ctx.width = getTimelineViewportBounds().getWidth();
    ctx.height = getTimelineContentViewportHeight();
    ctx.pianoKeyWidth = pianoKeyWidth_;
    ctx.rulerHeight = 0;
    ctx.pixelsPerSecond = camera_.pixelsPerSecond;
    ctx.pixelsPerSemitone = pixelsPerSemitone_;
    ctx.minMidi = minMidi_;
    ctx.maxMidi = maxMidi_;
    ctx.showOriginalF0 = showOriginalF0_;
    ctx.coords = makeViewMapper();
    ctx.hasF0Selection = true;
    ctx.f0SelectionStartFrame = selStartFrame;
    ctx.f0SelectionEndFrameExclusive = selEndFrameExclusive;
    ctx.contents = buildContentRenderItems();

    for (const auto& item : ctx.contents) {
        if (!item.active) continue;
        renderer_->drawF0SelectionHighlight(g, ctx, item);
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

void PianoRollComponent::drawHandDrawPreview(juce::Graphics& g) {
    if (!interactionState_.drawing.isDrawingF0 || currentTool_ != ToolId::HandDraw || interactionState_.drawing.handDrawBuffer.empty() || !currentCurve_) return;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();
    if (originalF0.empty() || interactionState_.drawing.handDrawBuffer.size() != originalF0.size()) return;

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return;
    juce::Colour previewColour = UIColors::correctedF0;
    juce::Path previewPath;
    bool pathStarted = false;

    for (int i = 0; i < f0tl.endFrameExclusive(); ++i) {
        float f0 = interactionState_.drawing.handDrawBuffer[static_cast<size_t>(i)];
        if (f0 > 0.0f) {
            float y = makeViewMapper().freqToY(f0);
            double timePos = f0tl.timeAtFrame(i);
            float x = static_cast<float>(sourceTimeToX(timePos));

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

void PianoRollComponent::drawLineAnchorPreview(juce::Graphics& g) {
    if (!interactionState_.drawing.isPlacingAnchors || currentTool_ != ToolId::LineAnchor || interactionState_.drawing.pendingAnchors.empty()) return;

    juce::Colour anchorColour = UIColors::correctedF0;

    for (size_t i = 0; i < interactionState_.drawing.pendingAnchors.size(); ++i) {
        const auto& anchor = interactionState_.drawing.pendingAnchors[i];
        float x = static_cast<float>(sourceTimeToX(anchor.time));
        float y = makeViewMapper().freqToY(anchor.freq);

        g.setColour(anchorColour);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);

        if (i > 0) {
            const auto& prev = interactionState_.drawing.pendingAnchors[i - 1];
            float prevX = static_cast<float>(sourceTimeToX(prev.time));
            float prevY = makeViewMapper().freqToY(prev.freq);
            g.setColour(anchorColour.withAlpha(0.7f));
            g.drawLine(prevX, prevY, x, y, 2.0f);
        }
    }

    if (!interactionState_.drawing.pendingAnchors.empty()) {
        const auto& last = interactionState_.drawing.pendingAnchors.back();
        float lastX = static_cast<float>(sourceTimeToX(last.time));
        float lastY = makeViewMapper().freqToY(last.freq);
        g.setColour(anchorColour.withAlpha(0.4f));
        g.drawLine(lastX, lastY,
                   interactionState_.drawing.currentMousePos.x,
                   interactionState_.drawing.currentMousePos.y - static_cast<float>(rulerHeight_),
                   1.5f);
    }
}

void PianoRollComponent::drawSelectionBox(juce::Graphics& g, ThemeId themeId) {
    if (!interactionState_.selection.hasSelectionArea) return;
    if (!toolHandler_ || !interactionState_.selection.isSelectingArea) return;

    double startTime = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    double endTime = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    float minMidi = std::min(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);
    float maxMidi = std::max(interactionState_.selection.selectionStartMidi, interactionState_.selection.selectionEndMidi);

    int x1 = sourceTimeToX(startTime);
    int x2 = sourceTimeToX(endTime);
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

    if (zoomPreviewActive_) {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto fullBounds = getLocalBounds();
        const ViewportState liveView{camera_, pixelsPerSemitone_, verticalScrollOffset_};
        drawFixedChrome(g, fullBounds);
        drawRuler(g, liveView, fullBounds);
        drawPitchBackground(g, liveView, fullBounds);
        drawPianoKeyboard(g, liveView, fullBounds);
        drawContent(g, liveView, fullBounds);
        recordRenderProbe(RenderProbePoint::ZoomPreviewPaint, juce::Time::getMillisecondCounterHiRes() - t0);
    } else {
        if (staticSurface_.isValid()) {
            g.drawImageAt(staticSurface_, 0, 0, false);
        }
        if (contentSurface_.isValid()) {
            g.drawImageAt(contentSurface_, 0, 0, false);
        }
    }

    const double paintEndMs = juce::Time::getMillisecondCounterHiRes();
    recordRenderProbe(RenderProbePoint::RootPaint, paintEndMs - paintStartMs);
    if (lastVBlankMs_ > 0.0)
        recordRenderProbe(RenderProbePoint::VBlankToRootPaint, paintStartMs - lastVBlankMs_);
}

void PianoRollComponent::invalidateTimeAxisStaticSurface()
{
    // 非缩放预览状态：标脏 → 同步栅格化 → repaint，构成原子展示提交
    if (!zoomPreviewActive_ && staticSurface_.isValid()) {
        staticDirty_ = true;
        rasterizeStatic(timeAxisRect());
    } else {
        staticDirty_ = true;
    }
    repaint(timeAxisRect());
}

void PianoRollComponent::rasterizeDirtySurfaces()
{
    if (zoomPreviewActive_) return;  // 缩放预览期间保留 dirty，不栅格

    const int vpW = getTimelineViewportBounds().getWidth();
    const int vpH = getTimelineViewportBounds().getHeight();
    if (vpW <= 0 || vpH <= 0) return;

    const int fullW = getWidth();
    const int fullH = getHeight();

    // staticSurface_ 覆盖完整组件 chrome 区域
    if (!staticSurface_.isValid() || staticSurface_.getWidth() != fullW || staticSurface_.getHeight() != fullH) {
        staticSurface_ = juce::Image(juce::Image::ARGB, fullW, fullH, true);
        staticDirty_ = true;
    }
    // contentSurface_ 保持时间轴视口区
    if (!contentSurface_.isValid() || contentSurface_.getWidth() != vpW || contentSurface_.getHeight() != vpH) {
        contentSurface_ = juce::Image(juce::Image::ARGB, vpW, vpH, true);
        contentDirty_ = true;
    }

    // 全量双表面重建时一次性同步 surfaceView_ 为 live 状态
    if (staticDirty_ && contentDirty_) {
        surfaceView_.camera = camera_;
        surfaceView_.pixelsPerSemitone = pixelsPerSemitone_;
        surfaceView_.verticalScrollOffset = verticalScrollOffset_;
    }

    // 仅栅格脏表面
    if (staticDirty_) rasterizeStatic();
    if (contentDirty_) rasterizeContent();
}

void PianoRollComponent::recordRenderProbe(RenderProbePoint point, double elapsedMs)
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
    }
    probe->count++;
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
            + " zoom-commit-raster:" + juce::String(avg(zoomCommitRasterProbe_), 1) + "ms x" + juce::String(zoomCommitRasterProbe_.count));
        staticRasterProbe_ = {};
        contentRasterProbe_ = {};
        overlayPresentProbe_ = {};
        rootPaintProbe_ = {};
        vblankToRootPaintProbe_ = {};
        zoomPreviewPaintProbe_ = {};
        zoomCommitRasterProbe_ = {};
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
    UIColors::drawShadow(g, juce::Rectangle<float>(0, 0, static_cast<float>(imgW), static_cast<float>(imgH)));
}

void PianoRollComponent::drawRuler(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const int cw = getTimelineContentViewportWidth();
    const juce::Rectangle<int> rulerDomain(pianoKeyWidth_, 0, cw, rulerHeight_);
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
    rp.rulerHeight = rulerHeight_; rp.laneStyle = 0;
    rp.viewportWidth = cw; rp.viewportHeight = rulerHeight_;
    rp.viewKind = "pianoroll";
    g.reduceClipRegion(clipArea);
    g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), 0.0f));
    g.reduceClipRegion(0, 0, cw, rulerHeight_);
    TimelineLayerComposer::drawTimeRuler(g, rp);
}

void PianoRollComponent::drawPitchBackground(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const int cw = getTimelineContentViewportWidth();
    const int ch = getTimelineContentViewportHeight();
    const juce::Rectangle<int> timelineDomain(pianoKeyWidth_, rulerHeight_, cw, ch);
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
    lp.rulerHeight = 0; lp.laneStyle = encodeLaneStyle(showLanes_, scaleRootNote_, scaleType_);
    lp.gridStyle = static_cast<int>(gridStyle_);
    lp.viewportWidth = cw; lp.viewportHeight = ch; lp.viewKind = "pianoroll";

    g.reduceClipRegion(clipArea);

    if (showLanes_) {
        juce::Graphics::ScopedSaveState lss(g);
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), static_cast<float>(rulerHeight_) - vFrac));
        g.reduceClipRegion(0, 0, cw, ch);
        TimelineLayerComposer::drawLaneStripRepeats(g, lp);
    }
    {
        juce::Graphics::ScopedSaveState gs(g);
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(pianoKeyWidth_), static_cast<float>(rulerHeight_)));
        g.reduceClipRegion(0, 0, cw, ch);
        TimelineLayerComposer::drawGridLines(g, lp);
    }
}

void PianoRollComponent::drawPianoKeyboard(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    if (!shouldShowPianoKeys()) return;

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
        rctx.rulerHeight = 0;
        rctx.pixelsPerSecond = view.camera.pixelsPerSecond;
        rctx.pixelsPerSemitone = view.pixelsPerSemitone;
        rctx.minMidi = minMidi_;
        rctx.maxMidi = maxMidi_;
        rctx.scaleRootNote = scaleRootNote_;
        rctx.scaleType = scaleType_;
        rctx.noteNameMode = noteNameMode_;
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

void PianoRollComponent::rasterizeStatic(std::optional<juce::Rectangle<int>> dirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int imgW = staticSurface_.getWidth();
    const int imgH = staticSurface_.getHeight();
    if (!staticSurface_.isValid() || imgW <= 0 || imgH <= 0) return;

    const juce::Rectangle<int> rasterBounds = dirtyRect.value_or(staticSurface_.getBounds());

    staticSurface_.clear(rasterBounds);

    juce::Graphics g(staticSurface_);

    drawFixedChrome(g, rasterBounds);
    drawRuler(g, surfaceView_, rasterBounds);
    drawPitchBackground(g, surfaceView_, rasterBounds);
    drawPianoKeyboard(g, surfaceView_, rasterBounds);

    staticDirty_ = false;

    recordRenderProbe(RenderProbePoint::StaticRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::drawContent(juce::Graphics& g, const ViewportState& view, juce::Rectangle<int> damage)
{
    const int w = getTimelineViewportBounds().getWidth();
    const int cw = getTimelineContentViewportWidth();
    const int ch = getTimelineContentViewportHeight();
    const auto timelineZone = juce::Rectangle<int>(pianoKeyWidth_, rulerHeight_, cw, ch);
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
    renderCtx.coords = mapper;
    renderCtx.rasterBounds = clipArea;

    // VolumeEnvelope 拖拽：blob 大小反馈使用预览包络
    renderer_->setVolumePreviewEnvelope(interactionState_.isVolumeDragging
        ? &interactionState_.volumePreviewEnvelope : nullptr);

    renderCtx.contents = buildContentRenderItems();

    const double pps = view.camera.pixelsPerSecond;

    const bool notesPrimary = isOpenDyne();

    // 背景波形：OpenDyne 模式下 blob 即波形表达，不叠加背景 PCM
    if (showWaveform_ && !notesPrimary) {
        for (const auto& item : renderCtx.contents) {
            const auto* mipmap = waveformMipmapCache_.get(item.contentKey);
            if (mipmap == nullptr || !mipmap->hasSource())
                continue;
            const int bestLevel = mipmap->selectBestLevelIndex(pps);
            if (bestLevel < 0)
                continue;
            const auto& level = mipmap->getLevel(bestLevel);
            renderer_->drawWaveform(g, renderCtx, item, level, bestLevel);
        }
    }

    // unvoiced bands 与 F0 curve 由 RenderContext 显示开关
    // （showUnvoicedFrames_/showOriginalF0_/showCorrectedF0_）决定，与 scheme 无关
    for (const auto& item : renderCtx.contents)
        renderer_->drawUnvoicedFrameBands(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawNotes(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawF0Curve(g, renderCtx, item);

    for (const auto& item : renderCtx.contents)
        renderer_->drawTimeGridAnchors(g, renderCtx, item);

    if (referenceOverlay_.has_value() && referenceOverlay_->enabled) {
        renderer_->drawGhostNotes(g, renderCtx, *referenceOverlay_);
    }
}

void PianoRollComponent::rasterizeContent(std::optional<juce::Rectangle<int>> dirtyRect)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    const int w = getTimelineViewportBounds().getWidth();
    const int h = getTimelineViewportBounds().getHeight();
    if (!contentSurface_.isValid() || w <= 0 || h <= 0) return;

    const bool fullRaster = !dirtyRect.has_value();
    const auto rasterBounds = dirtyRect.value_or(juce::Rectangle<int>(0, 0, w, h));

    if (fullRaster)
        contentSurface_.clear(contentSurface_.getBounds());
    else
        contentSurface_.clear(*dirtyRect);

    juce::Graphics g(contentSurface_);

    drawContent(g, surfaceView_, rasterBounds);

    if (fullRaster)
        contentDirty_ = false;

    recordRenderProbe(RenderProbePoint::ContentRaster, juce::Time::getMillisecondCounterHiRes() - t0);
}

void PianoRollComponent::applyRasterCamera(const TimelineViewportCamera& newCamera)
{
    // 表面无效或脏 → 全量重建
    if (!staticSurface_.isValid() || staticDirty_) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // PPS 变化 → 全量重建
    if (newCamera.pixelsPerSecond != surfaceView_.camera.pixelsPerSecond) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // dPixels 由连续 surfaceView_.camera 投影，不含任何量化时间
    const int dPixels = static_cast<int>(std::llround(
        (newCamera.visibleStartSeconds - surfaceView_.camera.visibleStartSeconds) * surfaceView_.camera.pixelsPerSecond));

    // 大幅跳转（超过视口宽度）→ 全量重建
    if (std::abs(dPixels) >= getTimelineContentViewportWidth()) {
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
        return;
    }

    // 无整数像素差 → 不移动、不栅格、不写 surfaceView_.camera
    if (dPixels == 0) {
        return;
    }

    // ── 同 PPS 横向滚动：标尺带完整重栅格 + 静态/内容带moveImageSection ──
    const int viewportW = getTimelineViewportBounds().getWidth();
    const int viewportH = getTimelineViewportBounds().getHeight();
    const int timelineLeft = pianoKeyWidth_;
    const int timelineW = viewportW - timelineLeft;

    // surfaceView_.camera 描述 retained pixels 的实际来源；
    // 仅推进 moveImageSection 已执行的整数像素位移；连续语义继续由 camera_ 保持
    surfaceView_.camera.visibleStartSeconds += static_cast<double>(dPixels) / surfaceView_.camera.pixelsPerSecond;

    // 标尺带：完整重栅格（文本像素不能安全平移）
    const juce::Rectangle<int> rulerRect(timelineLeft, 0, timelineW, rulerHeight_);
    rasterizeStatic(rulerRect);

    // 内容带：staticSurface_ + contentSurface_ 同步平移
    const int srcX = timelineLeft + (dPixels > 0 ? dPixels : 0);
    const int dstX = timelineLeft + (dPixels > 0 ? 0 : -dPixels);
    const int moveW = timelineW - std::abs(dPixels);
    const int contentH = viewportH - rulerHeight_;

    if (moveW > 0) {
        staticSurface_.moveImageSection(dstX, rulerHeight_, srcX, rulerHeight_, moveW, contentH);
        contentSurface_.moveImageSection(dstX, rulerHeight_, srcX, rulerHeight_, moveW, contentH);
    }

    // 补绘露出条带
    int stripX, stripW;
    if (dPixels > 0) {
        stripX = timelineLeft + timelineW - dPixels;
        stripW = dPixels;
    } else {
        stripX = timelineLeft;
        stripW = -dPixels;
    }
    stripX = juce::jmax(timelineLeft, stripX);
    stripW = juce::jmin(stripW, timelineW);

    if (stripW > 0) {
        juce::Rectangle<int> strip(stripX, rulerHeight_, stripW, contentH);
        rasterizeStatic(strip);
        rasterizeContent(strip);
    }

    // Normal scroll: only repaint timeAxisRect, not full component
    repaint(timeAxisRect());
}

std::vector<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItems() const
{
    std::vector<PianoRollRenderer::ContentRenderItem> items;
    items.reserve(timelineContentPlacements_.size());
    for (const auto& placement : timelineContentPlacements_) {
        if (!placement.isValid()) continue;
        if (auto item = buildContentRenderItem(placement)) {
            // 内容表面始终使用已提交的 TimeGrid；拖拽中的 working snapshot 仅由 Overlay 应用
            items.push_back(std::move(*item));
        }
    }
    return items;
}

bool PianoRollComponent::shouldShowPianoKeys() const noexcept
{
    return currentTool_ != ToolId::TimeTool;
}

void PianoRollComponent::setInferenceActive(bool active)
{
    inferenceActive_ = active;
    waveformBuildTickCounter_ = 0;
}

bool PianoRollComponent::applyNoteParameterToSelectedNotes(float retuneSpeed, float vibratoDepth, float vibratoRate) {
    auto notes = getEditedContentNotesCopy();
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;
    const auto contentSnapshot = readEditedSnapshot();
    if (contentSnapshot == nullptr) return false;

    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    bool anySelected = false;

    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(notes.size()));
    for (int noteIndex : interactionState_.noteSelection.selectedIndices) {
        auto& n = notes[static_cast<size_t>(noteIndex)];
        anySelected = true;
        n.retuneSpeed = retuneSpeed;
        n.vibratoDepth = vibratoDepth;
        n.vibratoRate = vibratoRate;
        n.dirty = true;
        dirtyStartTime = std::min(dirtyStartTime, n.startTime);
        dirtyEndTime = std::max(dirtyEndTime, n.endTime);
    }
    if (!anySelected) return false;

    if (dirtyEndTime > dirtyStartTime && currentCurve_) {
        const auto editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
        if (!editRange.isEmpty()) {
            auto clonedCurve = currentCurve_->clone();
            clonedCurve->applyCorrectionToRange(
                notes, editRange.startFrame, editRange.endFrameExclusive,
                static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
                retuneSpeed, vibratoDepth, vibratoRate);
            auto snap = clonedCurve->getSnapshot();

            const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
                editRange.startFrame,
                editRange.endFrameExclusive,
                f0tl.endFrameExclusive());

            // Extract notes overlapping the expanded affected range (not just dirty range)
            const double affectedStartSec = f0tl.timeAtFrame(affectedRange.startFrame);
            const double affectedEndSec = f0tl.timeAtFrame(affectedRange.endFrameExclusive);
            auto overlapsRange = [affectedStartSec, affectedEndSec](const Note& n) {
                return n.endTime > affectedStartSec && n.startTime < affectedEndSec;
            };
            std::vector<Note> notesInRange;
            for (const auto& n : notes) {
                if (overlapsRange(n)) notesInRange.push_back(n);
            }

            // Extract segments overlapping the affected range (range-scoped, not full)
            auto allSegments = snap->getCorrectionSegments();
            std::vector<PitchCorrectionSegment> segmentsInRange;
            for (const auto& seg : allSegments) {
                if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                    segmentsInRange.push_back(seg);
            }

            if (!commitEditedContentNotesAndSegments(*contentSnapshot, notesInRange, segmentsInRange, affectedRange)) {
                return false;
            }

            listeners_.call([affectedRange](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, affectedRange.endFrameExclusive - 1); });
    return true;
        }
    }

    return false;
}

bool PianoRollComponent::applyParameterToFrameRange(float retuneSpeed, float vibratoDepth, float vibratoRate, int startFrame, int endFrameExclusive) {
    if (!currentCurve_ || endFrameExclusive <= startFrame) return false;
    if (!currentCurve_->hasCorrectionInRange(startFrame, endFrameExclusive)) return false;
    const auto contentSnapshot = readEditedSnapshot();
    if (contentSnapshot == nullptr) return false;

    auto notes = getEditedContentNotesCopy();
    // 帧范围参数调节 = 把当前全局参数应用到该范围音符，同步写入音符字段
    // （否则 PitchCurve 渲染时音符级旧值覆盖新全局参数）。
    const auto f0tl = currentF0Timeline();
    const double rangeStartSec = f0tl.timeAtFrame(startFrame);
    const double rangeEndSec = f0tl.timeAtFrame(endFrameExclusive);
    for (auto& note : notes) {
        if (note.endTime > rangeStartSec && note.startTime < rangeEndSec) {
            note.retuneSpeed = retuneSpeed;
            note.vibratoDepth = vibratoDepth;
            note.vibratoRate = vibratoRate;
            note.dirty = true;
        }
    }
    auto editedCurve = currentCurve_->clone();
    editedCurve->applyCorrectionToRange(notes, startFrame, endFrameExclusive,
                                        static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
                                        retuneSpeed, vibratoDepth, vibratoRate);

    const auto snap = editedCurve->getSnapshot();
    auto allSegments = snap->getCorrectionSegments();
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(startFrame,
                                                                          endFrameExclusive,
                                                                          currentF0Timeline().endFrameExclusive());

    if (!commitEditedContentNotesAndSegments(*contentSnapshot, notes, allSegments, affectedRange)) {
        return false;
    }

    const int notifyEndFrame = std::max(affectedRange.startFrame, affectedRange.endFrameExclusive - 1);
    listeners_.call([affectedRange, notifyEndFrame](Listener& l) { l.pitchCurveEdited(affectedRange.startFrame, notifyEndFrame); });
    return true;
}

bool PianoRollComponent::getFrameRangeForTimeSpan(double startTime, double endTime, int& startFrame, int& endFrameExclusive) const
{
    startFrame = 0;
    endFrameExclusive = 0;
    if (currentCurve_ == nullptr || endTime <= startTime) return false;
    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;
    const auto range = f0tl.nonEmptyRangeForTimes(startTime, endTime);
    startFrame = range.startFrame;
    endFrameExclusive = range.endFrameExclusive;
    return endFrameExclusive > startFrame;
}

bool PianoRollComponent::getSelectedNotesFrameRange(int& startFrame, int& endFrameExclusive) const
{
    const auto notes = getEditedContentNotesCopy();
    double minStart = std::numeric_limits<double>::max();
    double maxEnd = -1.0;
    for (int noteIndex : interactionState_.noteSelection.selectedIndices) {
        if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
            continue;
        }
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        minStart = std::min(minStart, note.startTime);
        maxEnd = std::max(maxEnd, note.endTime);
    }
    return maxEnd > minStart && getFrameRangeForTimeSpan(minStart, maxEnd, startFrame, endFrameExclusive);
}

void PianoRollComponent::syncF0SelectionToSelectedNotes()
{
    int startFrame = 0;
    int endFrameExclusive = 0;
    if (getSelectedNotesFrameRange(startFrame, endFrameExclusive)) {
        interactionState_.selection.setF0Range(startFrame, endFrameExclusive);
        return;
    }

    interactionState_.selection.clearF0Selection();
}

bool PianoRollComponent::getSelectionAreaFrameRange(int& startFrame, int& endFrameExclusive) const
{
    if (!interactionState_.selection.hasSelectionArea) { startFrame = 0; endFrameExclusive = 0; return false; }
    const double s = std::min(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    const double e = std::max(interactionState_.selection.selectionStartTime, interactionState_.selection.selectionEndTime);
    return getFrameRangeForTimeSpan(s, e, startFrame, endFrameExclusive);
}

bool PianoRollComponent::getF0SelectionFrameRange(int& startFrame, int& endFrameExclusive) const
{
    startFrame = 0;
    endFrameExclusive = 0;

    if (currentCurve_ == nullptr || !interactionState_.selection.hasF0Selection) {
        return false;
    }

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return false;

    const auto range = f0tl.rangeForFrames(interactionState_.selection.selectedF0StartFrame,
                                           interactionState_.selection.selectedF0EndFrameExclusive);
    startFrame = range.startFrame;
    endFrameExclusive = range.endFrameExclusive;
    return endFrameExclusive > startFrame;
}

bool PianoRollComponent::applyRetuneSpeedToSelection(float speed) {
    speed = juce::jlimit(0.0f, 1.0f, speed);
    pendingUndoDescription_ = TRANS("Edit retune speed");
    if (!currentCurve_) return false;

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int frameSelectionStartFrame = 0;
    int frameSelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(frameSelectionStartFrame,
                                                              frameSelectionEndFrameExclusive);
    const bool hasSelectionAreaRange = hasF0SelectionRange
        || getSelectionAreaFrameRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive);

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotesRange;
    context.hasFrameSelection = hasSelectionAreaRange;

    switch (AudioEditingScheme::resolveParameterTarget(context)) {
        case AudioEditingScheme::ParameterTarget::SelectedNotes:
            return applyNoteParameterToSelectedNotes(speed, currentVibratoDepth_, currentVibratoRate_);
        case AudioEditingScheme::ParameterTarget::FrameSelection:
            return applyParameterToFrameRange(speed,
                                              currentVibratoDepth_,
                                              currentVibratoRate_,
                                              frameSelectionStartFrame,
                                              frameSelectionEndFrameExclusive);
        default:
            break;
    }

    return false;
}

bool PianoRollComponent::applyVibratoDepthToSelection(float depth) {
    pendingUndoDescription_ = TRANS("修改颤音深度");
    return applyVibratoParameterToSelection(VibratoParam::Depth, depth);
}

bool PianoRollComponent::applyVibratoRateToSelection(float rate) {
    pendingUndoDescription_ = TRANS("修改颤音速率");
    return applyVibratoParameterToSelection(VibratoParam::Rate, rate);
}

bool PianoRollComponent::applyVibratoParameterToSelection(VibratoParam param, float value) {
    auto clampValue = [&]() -> float {
        return (param == VibratoParam::Depth) ? juce::jlimit(0.0f, 100.0f, value)
                                              : juce::jlimit(0.1f, 30.0f, value);
    };
    
    value = clampValue();
    if (!currentCurve_) return false;

    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                  selectedNotesEndFrameExclusive);

    int frameSelectionStartFrame = 0;
    int frameSelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(frameSelectionStartFrame,
                                                              frameSelectionEndFrameExclusive);
    const bool hasSelectionAreaRange = hasF0SelectionRange
        || getSelectionAreaFrameRange(frameSelectionStartFrame, frameSelectionEndFrameExclusive);

    AudioEditingScheme::ParameterTargetContext context;
    context.hasSelectedNotes = hasSelectedNotesRange;
    context.hasFrameSelection = hasSelectionAreaRange;

    switch (AudioEditingScheme::resolveParameterTarget(context)) {
        case AudioEditingScheme::ParameterTarget::SelectedNotes:
        {
            float effectiveDepth = (param == VibratoParam::Depth) ? value : currentVibratoDepth_;
            float effectiveRate = (param == VibratoParam::Rate) ? value : currentVibratoRate_;
            return applyNoteParameterToSelectedNotes(currentRetuneSpeed_, effectiveDepth, effectiveRate);
        }
        case AudioEditingScheme::ParameterTarget::FrameSelection:
            return applyParameterToFrameRange(
                currentRetuneSpeed_,
                (param == VibratoParam::Depth) ? value : currentVibratoDepth_,
                (param == VibratoParam::Rate) ? value : currentVibratoRate_,
                frameSelectionStartFrame, frameSelectionEndFrameExclusive);
        default:
            return false;
    }
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
    const float resolvedRetuneSpeed = selectedNote->retuneSpeed >= 0.0f ? selectedNote->retuneSpeed : currentRetuneSpeed_;
    const float resolvedVibratoDepth = selectedNote->vibratoDepth >= 0.0f ? selectedNote->vibratoDepth : currentVibratoDepth_;
    const float resolvedVibratoRate = selectedNote->vibratoRate >= 0.0f ? selectedNote->vibratoRate : currentVibratoRate_;

    retuneSpeedPercent = juce::jlimit(0.0f, 100.0f, resolvedRetuneSpeed * 100.0f);
    vibratoDepth = juce::jlimit(0.0f, 100.0f, resolvedVibratoDepth);
    vibratoRate = juce::jlimit(3.0f, 12.0f, resolvedVibratoRate);
    return true;
}

int PianoRollComponent::findLineAnchorSegmentNear(int x, int y) const
{
    if (currentCurve_ == nullptr) return -1;

    auto snapshot = currentCurve_->getSnapshot();
    const auto& allSegments = snapshot->getCorrectionSegments();

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

void PianoRollComponent::setNoteSplit(float value) {
    // Note Split 控制音高分割阈值（cents）
    segmentationPolicy_.transitionThresholdCents = juce::jlimit(
        OpenTune::PitchControlConfig::kMinNoteSplitCents,
        OpenTune::PitchControlConfig::kMaxNoteSplitCents,
        value);

    // Note Split 仅更新分段策略参数，不触发 AUTO 重新生成
    // AUTO 操作由用户主动触发，使用当前策略执行分割
    repaint();
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

    overlay_->setBounds(getLocalBounds());

    // tryConsumeInitialF0View 成功时 activateTimelineCamera 完成一次最终 full raster（含 updateScrollBars）
    if (tryConsumeInitialF0View(editedContentKey_))
        return;

    updateScrollBars();
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::applyEditedContentCurve(std::shared_ptr<PitchCurve> curve)
{
    currentCurve_ = std::move(curve);
}

void PianoRollComponent::applyEditedContentAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                                       int sampleRate)
{
    audioBuffer_ = std::move(buffer);
    audioBufferSampleRate_ = sampleRate > 0 ? static_cast<double>(sampleRate)
                                            : static_cast<double>(PianoRollComponent::kAudioSampleRate);

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
    if (!snap || !snap->timeGrid)
        return 0.0;
    const double outputSeconds = snap->timeGrid->tauForward(sourceSeconds);
    return projection.projectContentTimeToTimeline(outputSeconds);
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
    if (!snap || !snap->timeGrid)
        return 0.0;

    const double timeline = makeViewMapper().xToTime(x);
    const double output   = projection.projectTimelineTimeToContent(timeline);
    return snap->timeGrid->tauInverse(output);
}

SourceEditRange PianoRollComponent::sourceEditRange() const
{
    const auto snap = readEditedSnapshot();
    if (!snap || !snap->timeGrid)
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

    // Register waveform mipmap sources (before early-return so late-binding audio works)
    std::set<ContentKey> aliveContents;
    for (const auto& placement : placements) {
        aliveContents.insert(placement.contentKey);
        if (const auto snapshot = readSnapshotFor(placement.contentKey);
            snapshot != nullptr && snapshot->audioBuffer != nullptr) {
            waveformMipmapCache_.setAudioSource(placement.contentKey, snapshot->audioBuffer);
        }
    }
    waveformMipmapCache_.prune(aliveContents);

    explicitTimelineContentPlacements_ = explicitContract;
    if (!changed) {
        return false;
    }

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
                                           std::shared_ptr<PitchCurve> curve,
                                           std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                           int sampleRate)
{
    const double normalizedSampleRate = sampleRate > 0 ? static_cast<double>(sampleRate)
                                                        : static_cast<double>(PianoRollComponent::kAudioSampleRate);
    const bool contentChanged = editedContentKey_ != contentKey;
    const bool curveChanged = currentCurve_ != curve;
    const bool bufferChanged = audioBuffer_ != buffer || audioBufferSampleRate_ != normalizedSampleRate;

    if (!contentChanged && !curveChanged && !bufferChanged) {
        return;
    }

    if (contentChanged) {
        if (!contentKey.isValid())
            pendingInitialF0ViewContentKey_ = ContentKey{};
        editedContentKey_ = contentKey;
        // 切换编辑目标：清除全部拖拽/绘制瞬态与 note draft（noteDraft 是
        // interactionState_ 成员，resetTransient 的 noteDraft.clear() 已覆盖）
        interactionState_.resetTransient();
        pendingUndoDescription_ = {};
        beforeUndoNotes_.clear();
        beforeUndoSegments_.clear();
        undoSnapshotCaptured_ = false;
        lastKnownNotesRevision_ = 0;
        lastKnownPitchRevision_ = 0;
        lastKnownTimeGridRevision_ = 0;
        // 切换编辑目标时清空全部选择状态，避免旧 clip 选择残留
        interactionState_.noteSelection.clear();
        interactionState_.selection.clearF0Selection();
        interactionState_.selection.hasSelectionArea = false;
        interactionState_.selection.isSelectingArea = false;
        interactionState_.selection.selectionStartTime = 0.0;
        interactionState_.selection.selectionEndTime = 0.0;
        interactionState_.selection.selectionStartMidi = 0.0f;
        interactionState_.selection.selectionEndMidi = 0.0f;
    }

    // notes 和 pitchCurve 通过 commitNotesAndPitchCurve 同步写入 store
    // 读侧也必须同步读：curveChanged 时需 refresh notes，否则 undo/redo 后不一致
    // 曲线回退后 notes 视觉残留的不对称（cachedNotes_ 滞后）。
    if (contentChanged || curveChanged) {
        refreshEditedContentNotes();
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
    if (contentChanged || curveChanged)
        requestContentRedraw();
    else {
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        repaint();
    }

    ensureOpenDyneNotesIfNeeded();
}


void PianoRollComponent::requestInitialF0View(ContentKey contentKey)
{
    if (!contentKey.isValid())
        return;

    pendingInitialF0ViewContentKey_ = contentKey;
    tryConsumeInitialF0View(contentKey);
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
    refreshEditedContentNotes();
    requestContentRedraw();
}

void PianoRollComponent::onPitchRevisionChanged()
{
    auto snap = readEditedSnapshot();
    uint64_t rev = snap ? snap->pitchRevision : 0;
    if (rev == lastKnownPitchRevision_) return;
    lastKnownPitchRevision_ = rev;
    requestContentRedraw();
}

void PianoRollComponent::requestContentRedraw() {
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::requestThemeRedraw() {
    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
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
    return juce::Rectangle<int>(pianoKeyWidth_, 0,
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

    const auto curveSnapshot = currentCurve_->getSnapshot();
    const auto projection = activeContentProjection();
    if (snapshot->timeGrid == nullptr
        || snapshot->timeGrid->empty()
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
    const auto mapper = makeViewMapper();
    const float startMidi = mapper.freqToMidi(startFrequency);
    verticalScrollOffset_ = (maxMidi_ - startMidi) * pixelsPerSemitone_ - contentHeight * 0.5f;
    verticalScrollOffset_ = std::clamp(verticalScrollOffset_, 0.0f, juce::jmax(0.0f, getTotalHeight() - static_cast<float>(contentHeight)));

    // 纵向变化 + 相机定位 → 标记脏让 applyRasterCamera 全量重建
    staticDirty_ = true;
    contentDirty_ = true;
    activateTimelineCamera(TimelineViewportPolicy::resolve(request));
    pendingInitialF0ViewContentKey_ = ContentKey{};
    return true;
}

void PianoRollComponent::onHeartbeatTick()
{
    // 终态收束先于可见性 guard：隐藏 PianoRoll 也能在 viewToggled 计算 preserve 前
    // 结算 Failed/Ready-无目标 的 pending；正常 Ready 定位仍需 isShowing/isVisible。
    tryConsumeInitialF0View(editedContentKey_);

    if (!isShowing())
        return;

    if (zoomPreviewActive_ && zoomDeadlineTicks_ > 0) {
        --zoomDeadlineTicks_;
        if (zoomDeadlineTicks_ == 0) {
            endZoomPreview();
        }
    }

    // DPI 变化检测 → 重建静态表面（标尺/琴键/网格 尺寸变化）
    {
        const int64_t currentDpiMilli = static_cast<int64_t>(
            std::llround(getDesktopScaleFactor() * 1000.0));
        if (currentDpiMilli != lastDpiMilli_) {
            lastDpiMilli_ = currentDpiMilli;
            staticDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
    }

    if (showWaveform_ || isOpenDyne()) {
        bool progressed = false;
        if (inferenceActive_) {
            waveformBuildTickCounter_ = (waveformBuildTickCounter_ + 1) % 8;
            if (waveformBuildTickCounter_ == 0)
                progressed = waveformMipmapCache_.buildIncremental(0.15);
        } else {
            waveformBuildTickCounter_ = 0;
            progressed = waveformMipmapCache_.buildIncremental(0.75);
        }

        // 每次产生构建进度即重栅格 content surface：任一 complete 非空 level
        // 出现即可显示，不等待全量 6 级完成
        if (progressed) {
            contentDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
    } else {
        waveformBuildTickCounter_ = 0;
    }

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

    const double oldCameraStart = camera_.visibleStartSeconds;
    const bool oldIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const bool oldPlaying = lastObservedPlayHeadPlaying_;
    const auto oldPH = playheadBoundsAt(playheadTimeForPaint_, oldPlaying, oldIsContFollow);

    // 播放状态切换
    if (playStateChanged) {
        userScrollHold_ = false;
        lastObservedPlayHeadPlaying_ = playingNow;
    }

    // 解 pending seek
    const double currentPlayheadTime = playHeadState_.getPresentedPositionSeconds();
    double playheadTime = currentPlayheadTime;
    if (pendingSeekTime_ >= 0.0) {
        const auto hostRevision = playHeadState_.hostPositionRevision.load(std::memory_order_acquire);
        if (hostRevision != seekSentRevision_) {
            pendingSeekTime_ = -1.0;
            seekSentRevision_ = 0;
        } else {
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
        activateTimelineCamera(resolved);
    }

    // 唯一 playheadTimeForPaint_ 写入
    const bool timeChanged = (playheadTime != playheadTimeForPaint_);
    playheadTimeForPaint_ = playheadTime;

    const bool cameraChanged = (camera_.visibleStartSeconds != oldCameraStart);
    const bool newIsContFollow = scrollMode_ == ScrollMode::Continuous && !userScrollHold_;
    const auto newPH = playheadBoundsAt(playheadTime, playingNow, newIsContFollow);

    // 稳定 CONT 居中且无状态变化 → 不重绘
    const bool stableCont = oldPH.fixedCentre && newPH.fixedCentre;
    const bool needsRepaint = (playStateChanged || timeChanged || cameraChanged) && !stableCont;
    if (needsRepaint) {
        juce::Rectangle<int> repaintBounds = oldPH.bounds.getUnion(newPH.bounds);
        if (!repaintBounds.isEmpty())
            overlay_->repaint(repaintBounds);
    }
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
    camera_ = camera;
    updateScrollBars();
    if (!zoomPreviewActive_)
        applyRasterCamera(camera);
    else {
        repaint();
        overlay_->repaint();
    }
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
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
}

// ── OpenDyne scheme 切换原子重置（计划 §二.3） ──
void PianoRollComponent::applyAudioEditingScheme(AudioEditingScheme::Scheme scheme)
{
    if (audioEditingScheme_ == scheme)
        return;

    const bool wasOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_);
    audioEditingScheme_ = scheme;
    const bool nowOpenDyne = AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_);

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
    rasterizeDirtySurfaces();
    overlay_->repaint();

    // Scheme changed → relayout to show/hide the horizontal scrollbar.
    if (wasOpenDyne != nowOpenDyne)
        resized();

    ensureOpenDyneNotesIfNeeded();
}

void PianoRollComponent::ensureOpenDyneNotesIfNeeded()
{
    // OpenDyne 语义：模式呈现需要音符可见。切入 OpenDyne 或加载/切换内容时，
    // 若内容已有 OriginalF0 数据但尚未初始化音符拓扑，则生成音符（复用导入同一路径：
    // 初始状态不量化，保持 originalPitch，SNAP 负责吸附；不重新提取 F0）。
    if (!isOpenDyne() || !editedContentKey_.isValid() || processor_ == nullptr)
        return;
    auto snap = readEditedSnapshot();
    if (snap == nullptr || snap->noteTopologyInitialized)
        return;  // 内容已经历过至少一次音符拓扑提交（导入生成 / 用户绘制 / 粘贴 / 合法空结果）
    if (snap->pitchCurve == nullptr)
        return;
    const auto curveSnap = snap->pitchCurve->getSnapshot();
    if (curveSnap == nullptr || curveSnap->getOriginalF0().empty())
        return;  // F0 未就绪（提取中或失败）
    if (processor_->generateNotesOnlyByContentKey(editedContentKey_, getCurrentAutoTuneParams()))
        listeners_.call([](Listener& listener) { listener.contentEdited(); });
    // 提交置位 noteTopologyInitialized 并推进 notesRevision → onNotesRevisionChanged 自动刷新缓存并重绘
}

void PianoRollComponent::setCurrentTool(ToolId tool) {
    // OpenDyne：Time 始终有效；OpenTune：Time 受 experimental 门控
    if (tool == ToolId::TimeTool && !experimentalFeaturesEnabled_ && !isOpenDyne()) {
        tool = ToolId::Select;
    }

    if (tool == ToolId::TimeTool
        && currentTool_ != ToolId::TimeTool
        && processor_ != nullptr
        && editedContentKey_.isValid()) {
        // This is a processor-specific operation, not content
        processor_->ensureTimeToolAnchorSeed(editedContentKey_);
    }

    const bool toolChanged = currentTool_ != tool;
    const ToolId previousTool = currentTool_;
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
        if (tool == ToolId::TimeTool) {
            if (pressedPianoKey_ >= 0) {
                if (pianoKeyAudition_ != nullptr)
                    pianoKeyAudition_->noteOff(pressedPianoKey_);
                pressedPianoKey_ = -1;
            }
            interactionState_.noteDrag.clear();
            interactionState_.noteResize.clear();
            clearNoteDraft();
            interactionState_.selection.clearF0Selection();
        } else if (currentTool_ == ToolId::TimeTool) {
            interactionState_.timeTool.clear();
        }
    }

    currentTool_ = tool;
    if (toolHandler_) {
        toolHandler_->setTool(tool);
    }
    if (hadTransientPreview) {
        contentDirty_ = true;
        rasterizeDirtySurfaces();
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
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
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
        if (wasTimeView != isNowTimeView) {
            staticDirty_ = true;
            rasterizeDirtySurfaces();
            repaint();
        }
        overlay_->repaint();
    }
}

void PianoRollComponent::setExperimentalFeaturesEnabled(bool enabled)
{
    if (experimentalFeaturesEnabled_ == enabled) {
        return;
    }

    experimentalFeaturesEnabled_ = enabled;
    // 关闭 experimental 仅令 OpenTune 的 Time 回落 Select；OpenDyne 的 Time 保持
    if (!enabled && !isOpenDyne() && currentTool_ == ToolId::TimeTool) {
        setCurrentTool(ToolId::Select);
        return;
    }

    repaint(getLocalBounds());
}

void PianoRollComponent::setShowWaveform(bool shouldShow) {
    if (showWaveform_ == shouldShow) return;
    showWaveform_ = shouldShow;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowLanes(bool shouldShow) {
    if (showLanes_ == shouldShow) return;
    showLanes_ = shouldShow;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setGridStyle(PianoGridStyle gridStyle)
{
    if (gridStyle_ == gridStyle) return;
    gridStyle_ = gridStyle;
    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setNoteNameMode(NoteNameMode noteNameMode) {
    if (noteNameMode_ == noteNameMode) return;
    noteNameMode_ = noteNameMode;
    staticDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

void PianoRollComponent::setShowUnvoicedFrames(bool shouldShow) {
    if (showUnvoicedFrames_ == shouldShow) return;
    showUnvoicedFrames_ = shouldShow;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
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
    timeUnitToggleButton_.setButtonText(displayMode_ == TimelineDisplayMode::Time ? "Time" : "BPM");
    invalidateTimeAxisStaticSurface();
}

void PianoRollComponent::addListener(Listener* listener) {
    listeners_.add(listener);
}

void PianoRollComponent::removeListener(Listener* listener) {
    listeners_.remove(listener);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& e) {
    if (isOpenDyne() && e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= pianoKeyWidth_) {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    } else if (!openDyneZoomPanActive_ && !interactionState_.isPanning) {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
    toolHandler_->mouseMove(e);
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    // OpenDyne: Cmd+Alt+double-click → zoom to note / restore zoom
    if (isOpenDyne() && e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= pianoKeyWidth_) {
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
    // ── OpenDyne：Command+Alt+拖拽 = 同时缩放两轴；Command+Shift+拖拽 = 平移视区 ──
    if (isOpenDyne()) {
        if (e.mods.isCommandDown() && e.mods.isAltDown() && e.x >= pianoKeyWidth_) {
            openDyneZoomAxisLock_.reset();
            beginOpenDyneZoomPan(e);
            return;
        }
        if (e.mods.isCommandDown() && e.mods.isShiftDown() && e.x >= pianoKeyWidth_) {
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
    if (e.mods.isCtrlDown() && !e.mods.isPopupMenu() && e.x >= pianoKeyWidth_) {
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

    if (e.y < rulerHeight_ && e.x < pianoKeyWidth_) {
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
        overlay_->repaint();
        return;
    }

    toolHandler_->mouseDown(e);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& e) {
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
                staticDirty_ = true;
                contentDirty_ = true;
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
            overlay_->repaint();
        }
        return;
    }

    toolHandler_->mouseDrag(e);
    // Tool handlers invalidate either live note content or transient preview
    // feedback directly.
}

void PianoRollComponent::mouseUp(const juce::MouseEvent& e) {
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
        overlay_->repaint();
        return;
    }

    toolHandler_->mouseUp(e);
    grabKeyboardFocus();
}

void PianoRollComponent::handleVerticalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
    float zoomFactor = 1.0f + (deltaY * settings.verticalZoomFactor);
    const float contentY = static_cast<float>(e.y - rulerHeight_);
    float mouseMidi = makeViewMapper().yToMidi(contentY);

    pixelsPerSemitone_ *= zoomFactor;
    pixelsPerSemitone_ = juce::jlimit(5.0f, 60.0f, pixelsPerSemitone_);
    userHasManuallyZoomed_ = true;

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

    // 进入缩放预览事务，冻结表面，到达截止时间后统一重建
    zoomPreviewActive_ = true;
    zoomDeadlineTicks_ = kZoomDeadlineTicks;

    updateScrollBars();
    repaint();
    overlay_->repaint();
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
    const auto& settings = zoomSensitivity_;
    float scrollDelta = deltaY * settings.scrollSpeed;
    verticalScrollOffset_ -= scrollDelta;
    float totalHeight = getTotalHeight();
    float visibleHeight = static_cast<float>(getTimelineContentViewportHeight());
    float maxScroll = totalHeight - visibleHeight;
    if (maxScroll > 0.0f) {
        verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
    } else {
        verticalScrollOffset_ = 0.0f;
    }
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        updateScrollBars();
        repaint();
    }

void PianoRollComponent::handleHorizontalZoomWheel(const juce::MouseEvent& e, float deltaY) {
    if (!zoomPreviewActive_ || zoomAnchorTime_ < 0.0) {
        beginZoomPreview(e, deltaY);
    } else {
        updateZoomPreview(deltaY);
    }
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

#if JUCE_MAC
    if (e.mods.isShiftDown() && deltaY == 0.0f && deltaX != 0.0f) {
        deltaY = deltaX;
        deltaX = 0.0f;
    }
#endif

    if (deltaY == 0.0f && deltaX == 0.0f) return;

    // ── OpenDyne（NotesPrimary）Melodyne 式导航 ──
    if (isOpenDyne()) {
        if (e.mods.isCommandDown()) {
            handleOpenDyneZoomAtMouse(e, deltaY); // Windows Ctrl / macOS Command：横纵向同步缩放
        } else if (e.mods.isAltDown()) {
            handleVerticalZoomWheel(e, deltaY); // 纵向缩放音高轴
        } else if (e.mods.isShiftDown()) {
            handleOpenDyneHorizontalScrollWheel(deltaX, deltaY); // 横向滚动
        } else {
            handleVerticalScrollWheel(deltaY); // 纵向滚动
        }
        return;
    }

    if (e.mods.isShiftDown()) {
        handleVerticalZoomWheel(e, deltaY);
    } else if (e.mods.isCtrlDown()) {
        handleHorizontalZoomWheel(e, deltaY);
    } else if (e.mods.isAltDown()) {
        handleHorizontalScrollWheel(deltaX, deltaY);
    } else {
        handleVerticalScrollWheel(deltaY);
    }
}

void PianoRollComponent::handleOpenDyneHorizontalScrollWheel(float deltaX, float deltaY) {
    handleHorizontalScrollWheel(deltaX != 0.0f ? deltaX : deltaY, 0.0f);
}

void PianoRollComponent::handleOpenDyneZoomAtMouse(const juce::MouseEvent& e, float deltaY) {
    saveOpenDyneZoomState();
    // 时间轴与音高轴同时缩放，保持鼠标下的时间与音高不动
    handleHorizontalZoomWheel(e, deltaY);
    handleVerticalZoomWheel(e, deltaY);
}

void PianoRollComponent::beginOpenDyneZoomPan(const juce::MouseEvent& e) {
    saveOpenDyneZoomState();
    openDyneZoomPanActive_ = true;
    openDyneZoomPanStartPos_ = e.getPosition();
    openDyneZoomPanStartPps_ = camera_.pixelsPerSecond;
    openDyneZoomPanStartPixelsPerSemitone_ = pixelsPerSemitone_;
    const double mouseX = static_cast<double>(e.x - pianoKeyWidth_);
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

    if (axis == AxisLockState::Axis::Horizontal) {
        // 时间轴：保持锚点时间在鼠标 X 下
        const double zoomFactorH = std::exp(dx * 0.005);
        const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
            openDyneZoomPanStartPps_ * zoomFactorH,
            TimelineViewportRequest::ViewKind::PianoRoll);
        const double mouseX = static_cast<double>(e.x - pianoKeyWidth_);
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
            5.0f, 60.0f, openDyneZoomPanStartPixelsPerSemitone_ * zoomFactorV);
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
    }

    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
}

void PianoRollComponent::endOpenDyneZoomPan() {
    openDyneZoomPanActive_ = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
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
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
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
            if (snap->timeGrid) {
                timelineStart = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(minSource));
                timelineEnd = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(maxSource));
            }
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
        5.0f, 60.0f, static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

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
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
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
            if (snap->timeGrid) {
                timelineStart = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(minSource));
                timelineEnd = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(maxSource));
            }
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
        5.0f, 60.0f, static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

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
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
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
            if (snap->timeGrid) {
                timelineStart = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(minSource));
                timelineEnd = placement->projection.projectContentTimeToTimeline(
                    snap->timeGrid->tauForward(maxSource));
            }
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
        5.0f, 60.0f, static_cast<float>((viewportH - 2.0 * marginH) / (maxMidi - minMidi + 1.0f)));

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
    rasterizeDirtySurfaces();
    updateScrollBars();
    repaint();
    overlay_->repaint();
}

void PianoRollComponent::beginZoomPreview(const juce::MouseEvent& e, float deltaY) {
    const auto& settings = zoomSensitivity_;
    const double zoomFactor = 1.0 + static_cast<double>(deltaY) * settings.horizontalZoomFactor;
    const double oldPps = camera_.pixelsPerSecond;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        oldPps * zoomFactor,
        TimelineViewportRequest::ViewKind::PianoRoll);
    const int mouseX = e.x - pianoKeyWidth_;
    const double mouseTime = camera_.visibleStartSeconds + mouseX / oldPps;

    zoomPreviewActive_ = true;
    zoomAnchorTime_ = mouseTime;
    zoomAnchorViewportX_ = mouseX;
    zoomDeadlineTicks_ = kZoomDeadlineTicks;

    userHasManuallyZoomed_ = true;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        zoomAnchorTime_,
        static_cast<double>(zoomAnchorViewportX_),
        newPps));
}

void PianoRollComponent::updateZoomPreview(float deltaY) {
    const auto& settings = zoomSensitivity_;
    const double zoomFactor = 1.0 + static_cast<double>(deltaY) * settings.horizontalZoomFactor;
    const double newPps = TimelineViewportPolicy::normalisePixelsPerSecond(
        camera_.pixelsPerSecond * zoomFactor,
        TimelineViewportRequest::ViewKind::PianoRoll);

    zoomDeadlineTicks_ = kZoomDeadlineTicks;
    commitViewportRequest(makeViewportRequest(
        TimelineViewportRequest::Kind::Zoom,
        zoomAnchorTime_,
        static_cast<double>(zoomAnchorViewportX_),
        newPps));
}

void PianoRollComponent::endZoomPreview() {
    zoomPreviewActive_ = false;
    zoomDeadlineTicks_ = 0;
    zoomAnchorTime_ = -1.0;

    const double t0 = juce::Time::getMillisecondCounterHiRes();

    // 从当前 live 状态一次性赋值 surfaceView_，保证 preview 最后一帧与恢复 blit 首帧使用同一状态
    surfaceView_.camera = camera_;
    surfaceView_.pixelsPerSemitone = pixelsPerSemitone_;
    surfaceView_.verticalScrollOffset = verticalScrollOffset_;

    // 恰好一次重建两张表面
    staticDirty_ = true;
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();

    recordRenderProbe(RenderProbePoint::ZoomCommitRaster, juce::Time::getMillisecondCounterHiRes() - t0);
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
}

void PianoRollComponent::pasteNotes()
{
    if (notesClipboard_.empty()) return;
    if (processor_ == nullptr || !editedContentKey_.isValid()) return;

    const auto& notes = getCommittedNotes();
    const auto& sel = interactionState_.noteSelection;

    // 粘贴位置：播放头时间经唯一逆投影转为 source seconds
    //   presented timeline seconds
    //   → projection.projectTimelineTimeToContent()
    //   → timeGrid.tauInverse()
    //   → source seconds
    double pasteStartTime = 0.0;
    const double playheadTime = playHeadState_.getPresentedPositionSeconds();
    if (playheadTime > 0.0) {
        const auto projection = activeContentProjection();
        const auto snap = readEditedSnapshot();
        if (projection.isValid() && snap && snap->timeGrid) {
            const double output = projection.projectTimelineTimeToContent(playheadTime);
            pasteStartTime = snap->timeGrid->tauInverse(output);
        }
    } else if (!sel.empty() && !notes.empty()) {
        double maxEnd = 0.0;
        for (int idx : sel.selectedIndices) {
            if (idx >= 0 && idx < static_cast<int>(notes.size()))
                maxEnd = std::max(maxEnd, notes[static_cast<size_t>(idx)].endTime);
        }
        pasteStartTime = maxEnd;
    }

    // 剪贴板始终保存 source 时间；粘贴音符之间的 source 相对间距保持不变
    double clipMinTime = notesClipboard_.front().startTime;
    for (const auto& n : notesClipboard_) {
        clipMinTime = std::min(clipMinTime, n.startTime);
    }

    std::vector<Note> pastedNotes;
    pastedNotes.reserve(notesClipboard_.size());
    for (auto n : notesClipboard_) {
        const double origStart = n.startTime;
        const double origDuration = n.endTime - n.startTime;
        n.startTime = pasteStartTime + (origStart - clipMinTime);
        n.endTime = n.startTime + origDuration;
        n.dirty = true;
        pastedNotes.push_back(n);
    }

    // 拓扑计划：归一化合并 + affected range（粘贴包络 ∪ 严格相交原音符完整边界）
    // + before/after patch 内容。range 覆盖被截短原音符的完整区间，Undo/Redo 对称。
    const auto plan = planPasteTopology(notes, pastedNotes);

    // 拓扑编辑事务：commitNoteTopologyPatch 内部处理
    // revision 推进 + republishPlaybackSource + snapshot 返回
    ContentNoteRangePatch afterPatch;
    afterPatch.affectedRange.startSeconds = plan.affectedRange.startSeconds;
    afterPatch.affectedRange.endSeconds = plan.affectedRange.endSeconds;
    afterPatch.afterNotesInRange = plan.afterNotesInRange;

    const auto committedSnap = contentCommands_->commitNoteTopologyPatch(editedContentKey_, afterPatch);
    if (!committedSnap) return;

    cachedNotes_ = committedSnap->notes;
    interactionState_.noteSelection.trimToNoteCount(static_cast<int>(cachedNotes_.size()));

    // 构建 before-patch：仅范围内的原有 notes，用于 Undo
    ContentNoteRangePatch beforePatch;
    beforePatch.affectedRange = afterPatch.affectedRange;
    beforePatch.afterNotesInRange = plan.beforeNotesInRange;

    auto action = std::make_unique<PianoRollNotePatchAction>(
        contentCommands_,
        editedContentKey_,
        pendingUndoDescription_.isNotEmpty() ? pendingUndoDescription_ : TRANS("粘贴"),
        std::move(beforePatch),
        std::move(afterPatch));

    processor_->getUndoManager().addAction(std::move(action));
    pendingUndoDescription_ = {};
    undoSnapshotCaptured_ = false;
    lastKnownNotesRevision_ = committedSnap->notesRevision;

    // 选中刚粘贴的 notes（按 source 时间匹配）
    interactionState_.noteSelection.clear();
    const int noteCount = static_cast<int>(cachedNotes_.size());
    for (int i = 0; i < noteCount; ++i) {
        for (const auto& pn : pastedNotes) {
            if (cachedNotes_[static_cast<size_t>(i)].startTime == pn.startTime
                && cachedNotes_[static_cast<size_t>(i)].endTime == pn.endTime) {
                interactionState_.noteSelection.add(i, noteCount);
                break;
            }
        }
    }
    syncF0SelectionToSelectedNotes();

    requestContentRedraw();
    overlay_->repaint();
}

void PianoRollComponent::duplicateNotes()
{
    copySelectedNotes();
    pasteNotes();
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key) {
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Undo, key)) {
        listeners_.call([](Listener& l) { l.undoRequested(); });
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Redo, key)) {
        listeners_.call([](Listener& l) { l.redoRequested(); });
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Copy, key)) {
        copySelectedNotes();
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Paste, key)) {
        pasteNotes();
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Cut, key)) {
        copySelectedNotes();
        toolHandler_->handleDeleteKey();  // 剪切语义：复制到剪贴板 + 删除选中
        return true;
    }
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::DuplicateClip, key)) {
        duplicateNotes();
        return true;
    }
    // OpenDyne: Cmd+Alt+Arrow = precise keyboard zoom
    if (isOpenDyne() && key.getModifiers().isCommandDown() && key.getModifiers().isAltDown()) {
        float zoomFactor = 1.15f; // ~15% per keypress
        const double visibleWidthSeconds = getTimelineContentViewportWidth() / camera_.pixelsPerSecond;
        const int keyCode = key.getKeyCode();
        if (keyCode == juce::KeyPress::leftKey) {
            commitViewportRequest(makeViewportRequest(
                TimelineViewportRequest::Kind::Manual,
                camera_.visibleStartSeconds + visibleWidthSeconds * 0.5 * (1.0 - 1.0/zoomFactor),
                0.0, camera_.pixelsPerSecond / zoomFactor));
            return true;
        }
        if (keyCode == juce::KeyPress::rightKey) {
            commitViewportRequest(makeViewportRequest(
                TimelineViewportRequest::Kind::Manual,
                camera_.visibleStartSeconds + visibleWidthSeconds * 0.5 * (1.0 - zoomFactor),
                0.0, camera_.pixelsPerSecond * zoomFactor));
            return true;
        }
        if (keyCode == juce::KeyPress::upKey) {
            const float contentViewportHeight = static_cast<float>(getTimelineContentViewportHeight());
            float centerMidi = maxMidi_ - (verticalScrollOffset_ + contentViewportHeight * 0.5f) / pixelsPerSemitone_;
            pixelsPerSemitone_ = juce::jlimit(5.0f, 60.0f, pixelsPerSemitone_ * zoomFactor);
            verticalScrollOffset_ = (maxMidi_ - centerMidi) * pixelsPerSemitone_ - contentViewportHeight * 0.5f;
            const float maxScroll = juce::jmax(0.0f, getTotalHeight() - contentViewportHeight);
            verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
            staticDirty_ = true; contentDirty_ = true;
            rasterizeDirtySurfaces(); updateScrollBars(); repaint(); overlay_->repaint();
            return true;
        }
        if (keyCode == juce::KeyPress::downKey) {
            const float contentViewportHeight = static_cast<float>(getTimelineContentViewportHeight());
            float centerMidi = maxMidi_ - (verticalScrollOffset_ + contentViewportHeight * 0.5f) / pixelsPerSemitone_;
            pixelsPerSemitone_ = juce::jlimit(5.0f, 60.0f, pixelsPerSemitone_ / zoomFactor);
            verticalScrollOffset_ = (maxMidi_ - centerMidi) * pixelsPerSemitone_ - contentViewportHeight * 0.5f;
            const float maxScroll = juce::jmax(0.0f, getTotalHeight() - contentViewportHeight);
            verticalScrollOffset_ = juce::jlimit(0.0f, maxScroll, verticalScrollOffset_);
            staticDirty_ = true; contentDirty_ = true;
            rasterizeDirtySurfaces(); updateScrollBars(); repaint(); overlay_->repaint();
            return true;
        }
    }
    // Cmd+0: Zoom to selected notes (or all if none selected)
    if (key.getKeyCode() == '0' && (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())) {
        if (key.getModifiers().isShiftDown()) {
            fitToAllNotes();
        } else {
            fitToSelectedNotes();
        }
        return true;
    }
    return toolHandler_->keyPressed(key);
}

std::optional<PianoRollRenderer::ContentRenderItem> PianoRollComponent::buildContentRenderItem(
    const TimelineContentPlacement& placement) const
{
    auto snap = readSnapshotFor(placement.contentKey);
    if (!snap || !snap->timeGrid)
        return std::nullopt;

    PianoRollRenderer::ContentRenderItem item;
    item.contentKey = placement.contentKey;
    item.projection = placement.projection;
    item.timeGrid = snap->timeGrid;
    item.active = placement.contentKey == editedContentKey_;
    item.displayColour = placement.displayColour;

    std::shared_ptr<PitchCurve> curve;
    if (item.active) {
        // 拖拽预览：active item 直接消费 noteDrag.previewSnapshot（已含 clone 后
        // 经 applyCorrectionToRange 烘焙的 pitchCurve 与 working notes），
        // curve 与 displayNotes 均取自该 snapshot，renderer 无需任何覆盖注入。
        // 非预览：ownerSnapshot 为 committed snapshot，displayNotes 保留
        // getDisplayedNotes() 以支持 resize/draw 等 live draft。
        const auto& previewSnap = interactionState_.noteDrag.previewSnapshot;
        if (previewSnap != nullptr) {
            item.ownerSnapshot = previewSnap;
            item.displayNotes = &item.ownerSnapshot->notes;
        } else {
            item.ownerSnapshot = readEditedSnapshot();
            item.displayNotes = &getDisplayedNotes();  // includes live note draft when active
        }
        // 唯一 F0 来源：ownerSnapshot 的 pitchCurve（预览 = 烘焙 clone，否则 = committed）
        curve = item.ownerSnapshot != nullptr ? item.ownerSnapshot->pitchCurve : nullptr;
        item.audioBuffer = audioBuffer_;
    } else {
        curve = snap->pitchCurve;
        item.audioBuffer = snap->audioBuffer;
        item.ownerSnapshot = snap;
        item.displayNotes = &item.ownerSnapshot->notes;
    }

    if (curve != nullptr) {
        item.pitchSnapshot = curve->getSnapshot();
        if (item.pitchSnapshot != nullptr && item.pitchSnapshot->size() > 0)
            item.f0Timeline = { item.pitchSnapshot->getHopSize(),
                                item.pitchSnapshot->getSampleRate(),
                                static_cast<int>(item.pitchSnapshot->size()) };
    }

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
}

void PianoRollComponent::setReferenceOverlay(std::optional<PianoRollRenderer::ReferenceOverlay> overlay)
{
    if (overlay && overlay->enabled
        && (!overlay->sourceProjection.isValid() || overlay->timeGrid == nullptr))
        overlay.reset();
    referenceOverlay_ = std::move(overlay);
    contentDirty_ = true;
    rasterizeDirtySurfaces();
    repaint();
}

int PianoRollComponent::getTimelineContentViewportWidth() const
{
    return juce::jmax(0, getTimelineViewportBounds().getWidth() - pianoKeyWidth_);
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
    rasterizeDirtySurfaces();
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
        pixelsPerSemitone_ = static_cast<float>(contentViewportHeight) / range;
        
        // Reset scroll to show top
        verticalScrollOffset_ = 0; 
        staticDirty_ = true;
        contentDirty_ = true;
        // 不在此分支栅格；最终 commitViewportRequest 通过 applyRasterCamera 一次性完整重建
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
    
    // Available width: getWidth() - pianoKeyWidth_
    int viewWidth = timelineViewportBounds.getWidth() - pianoKeyWidth_;
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

float PianoRollComponent::calculateEffectivePIP(Note& note) {
    if (!currentCurve_) return -1.0f;

    if (note.endTime <= note.startTime) return -1.0f;

    // 获取当前全局 pitchRatio，使 originalPitch 始终处于 Effective F0 域
    float pitchRatio = 1.0f;
    const auto contentSnap = readEditedSnapshot();
    if (contentSnap != nullptr)
        pitchRatio = static_cast<float>(contentSnap->pitchShiftSettings.getPitchRatio());

    auto snapshot = currentCurve_->getSnapshot();
    const auto& originalF0 = snapshot->getOriginalF0();

    const auto f0tl = currentF0Timeline();
    if (f0tl.isEmpty()) return -1.0f;
    const auto noteRange = f0tl.nonEmptyRangeForTimes(note.startTime, note.endTime);
    const int startFrame = noteRange.startFrame;
    const int endFrameExclusive = noteRange.endFrameExclusive;
    
    if (startFrame >= endFrameExclusive) return -1.0f;

    int numFrames = endFrameExclusive - startFrame;
    
    std::vector<float> noteF0(static_cast<std::size_t>(numFrames));
    std::copy(originalF0.begin() + startFrame, originalF0.begin() + endFrameExclusive, noteF0.begin());

    std::vector<float> voicedF0;
    voicedF0.reserve(noteF0.size());
    for (float f : noteF0) {
        float eff = f * pitchRatio;
        if (eff > 0.0f) voicedF0.push_back(eff);
    }

    if (voicedF0.empty()) {
        return -1.0f;
    }

    std::sort(voicedF0.begin(), voicedF0.end());
    float medianF0 = voicedF0[voicedF0.size() / 2];
    return medianF0;
}

juce::String PianoRollComponent::AutoTuneApplyResult::message() const
{
    switch (status) {
        case AutoTuneApplyStatus::Applied:
            return juce::String("AUTO has been queued.");
        case AutoTuneApplyStatus::NoChange:
            return juce::String();   // 最终修正已达成：静默，不弹窗
        case AutoTuneApplyStatus::NoCurve:
            return juce::String("AUTO needs an active pitch curve. Run audio analysis first.");
        case AutoTuneApplyStatus::NoProcessor:
            return juce::String("AUTO cannot run because the processor is not attached.");
        case AutoTuneApplyStatus::NoContent:
            return juce::String("AUTO needs an active editable clip.");
        case AutoTuneApplyStatus::MissingContentSnapshot:
            return juce::String("AUTO cannot read the editable content snapshot.");
        case AutoTuneApplyStatus::OriginalF0NotReady:
            return juce::String("AUTO needs OriginalF0 to be ready for this clip.");
        case AutoTuneApplyStatus::MissingCurveSnapshot:
            return juce::String("AUTO cannot read the current pitch-curve snapshot.");
        case AutoTuneApplyStatus::EmptyOriginalF0:
            return juce::String("AUTO needs non-empty OriginalF0 data.");
        case AutoTuneApplyStatus::EmptyTimeline:
            return juce::String("AUTO cannot map this clip to an F0 timeline.");
        case AutoTuneApplyStatus::NoTargetSelection:
            return juce::String("AUTO needs a selected note, F0 range, or selection area.");
        case AutoTuneApplyStatus::EmptyTargetRange:
            return juce::String("AUTO target range is empty.");
    }

    return juce::String("AUTO could not be applied.");
}

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::applyAutoTuneToSelection()
{
    if (!currentCurve_) {
        return { AutoTuneApplyStatus::NoCurve };
    }

    if (!processor_) {
        return { AutoTuneApplyStatus::NoProcessor };
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

    // 单一快照读取：前置检查全部从 snap->pitchCurve->getSnapshot() 派生
    const auto curveSnap = snap->pitchCurve->getSnapshot();
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
    // 分流位于选区解析（getSelectedNotesFrameRange）之前，目标集合由 applyAutoSnapToAllNotes 内部决定
    if (AudioEditingScheme::usesNotesPrimaryScheme(audioEditingScheme_)) {
        return applyAutoSnapToAllNotes(snap, f0tl);
    }

    // ── OpenTune：原选区解析 + autoTuneContentRange 逻辑 ──
    int selectedNotesStartFrame = 0;
    int selectedNotesEndFrameExclusive = 0;
    const bool hasSelectedNotesRange = getSelectedNotesFrameRange(selectedNotesStartFrame,
                                                                   selectedNotesEndFrameExclusive);

    int selectionAreaStartFrame = 0;
    int selectionAreaEndFrameExclusive = 0;
    const bool hasSelectionAreaRange = getSelectionAreaFrameRange(selectionAreaStartFrame,
                                                                   selectionAreaEndFrameExclusive);

    int f0SelectionStartFrame = 0;
    int f0SelectionEndFrameExclusive = 0;
    const bool hasF0SelectionRange = getF0SelectionFrameRange(f0SelectionStartFrame,
                                                              f0SelectionEndFrameExclusive);

    AudioEditingScheme::AutoTuneTargetContext targetContext;
    targetContext.totalFrameCount = f0tl.endFrameExclusive();
    if (hasSelectedNotesRange) {
        targetContext.selectedNotesRange = { selectedNotesStartFrame, selectedNotesEndFrameExclusive };
    }
    if (hasSelectionAreaRange) {
        targetContext.selectionAreaRange = { selectionAreaStartFrame, selectionAreaEndFrameExclusive };
    }
    if (hasF0SelectionRange) {
        targetContext.f0SelectionRange = { f0SelectionStartFrame, f0SelectionEndFrameExclusive };
    }

    const auto targetDecision = AudioEditingScheme::resolveAutoTuneRange(targetContext);
    if (targetDecision.target == AudioEditingScheme::AutoTuneTarget::None) {
        return { AutoTuneApplyStatus::NoTargetSelection };
    }

    const auto targetRange = f0tl.rangeForFrames(targetDecision.range.startFrame,
                                                 targetDecision.range.endFrameExclusive);
    if (targetRange.isEmpty()) {
        return { AutoTuneApplyStatus::EmptyTargetRange };
    }

    const int startFrame = targetRange.startFrame;
    const int endFrame = targetRange.endFrameExclusive - 1;

    const std::optional<ScaleSnapConfig> postSnapCfg = makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);

    captureBeforeUndoSnapshot();
    pendingUndoDescription_ = TRANS("自动调音");

    if (!contentCommands_->autoTuneContentRange(
            editedContentKey_,
            startFrame,
            endFrame + 1,
            getCurrentAutoTuneParams(),
            postSnapCfg)) {
        return { AutoTuneApplyStatus::NoContent };
    }

    {
        auto autoSnap = readEditedSnapshot();
        if (autoSnap) {
            lastKnownNotesRevision_ = autoSnap->notesRevision;
            lastKnownPitchRevision_ = autoSnap->pitchRevision;
        }
    }

    refreshEditedContentNotes();

    auto committedCurve = readEditedSnapshot();
    if (committedCurve != nullptr && committedCurve->pitchCurve != nullptr) {
        setEditedContent(editedContentKey_, committedCurve->pitchCurve, audioBuffer_, static_cast<int>(audioBufferSampleRate_));
    }

    const auto autoTuneRange = PitchCurve::expandNoteBasedCorrectionRange(
        startFrame, endFrame + 1, f0tl.endFrameExclusive());
    recordUndoAction(pendingUndoDescription_, autoTuneRange);

    return { AutoTuneApplyStatus::Applied };
}

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::applyAutoSnapToAllNotes(
    const std::shared_ptr<const EditableContentSnapshot>& contentSnapshot,
    const F0Timeline& f0tl)
{
    // 量化目标：有音阶配置时按音阶吸附；Chromatic（无音阶配置）时吸附到最近半音
    // （ScaleSnapConfig 默认 mode=Chromatic，quantizeMidiToActiveScale = round，
    //   与 AUTO 生成器 quantisePitch 一致）
    auto scaleSnap = makeScaleSnapConfigFromUi(scaleRootNote_, scaleType_);
    if (!scaleSnap.has_value())
        scaleSnap = ScaleSnapConfig{};

    // 单一快照来源：notes/curve 全部从 contentSnapshot 派生
    auto notes = contentSnapshot->notes;
    const auto curveSnap = contentSnapshot->pitchCurve->getSnapshot();

    // 吸附目标 = 选中音符子集；未选中任何音符时才全量吸附
    const bool selectionActive = !interactionState_.noteSelection.empty();
    std::vector<int> targetIndices;
    if (selectionActive) {
        targetIndices = interactionState_.noteSelection.selectedIndices;
    } else {
        targetIndices.resize(notes.size());
        for (size_t i = 0; i < notes.size(); ++i)
            targetIndices[i] = static_cast<int>(i);
    }
    if (targetIndices.empty())
        return { AutoTuneApplyStatus::NoChange };

    // 吸附：修改目标音符 pitchOffset（修正曲线将基于修改后的值计算）
    double dirtyStartTime = 1e30, dirtyEndTime = -1e30;
    bool anyPitchChanged = false;
    for (int noteIndex : targetIndices) {
        auto& note = notes[static_cast<size_t>(noteIndex)];
        // SNAP 按基准音高重投影：pitchOffset 不进入量化输入
        const float baseMidi = PitchUtils::freqToMidi(note.pitch);
        const float snappedOffset = scaleSnap->quantizeMidiToActiveScale(baseMidi) - baseMidi;
        if (std::abs(snappedOffset - note.pitchOffset) < 0.001f)
            continue;
        note.pitchOffset = snappedOffset;
        note.dirty = true;
        anyPitchChanged = true;
        dirtyStartTime = std::min(dirtyStartTime, note.startTime);
        dirtyEndTime = std::max(dirtyEndTime, note.endTime);
    }

    // 目标音符集合（修改后取值）：修正曲线只对目标音符重写，未选中音符的修正保持原样
    std::vector<Note> targetNotes;
    targetNotes.reserve(targetIndices.size());
    double targetMinStartTime = 1e30, targetMaxEndTime = -1e30;
    for (int noteIndex : targetIndices) {
        const auto& note = notes[static_cast<size_t>(noteIndex)];
        targetMinStartTime = std::min(targetMinStartTime, note.startTime);
        targetMaxEndTime = std::max(targetMaxEndTime, note.endTime);
        targetNotes.push_back(note);
    }

    // 目标音符帧范围
    const auto notesRange = f0tl.rangeForTimes(targetMinStartTime, targetMaxEndTime);

    // NoChange 判据 = 最终修正结果已达成（无覆盖空洞）：
    // 目标音符帧范围被 correction 完全覆盖（无空洞）且 pitchOffset 无变化。
    const bool fullyCorrected = isFullyCorrectedInRange(
        *curveSnap, notesRange.startFrame, notesRange.endFrameExclusive);
    if (!anyPitchChanged && fullyCorrected)
        return { AutoTuneApplyStatus::NoChange };

    if (anyPitchChanged) {
        dirtyStartTime = std::min(dirtyStartTime, targetMinStartTime);
        dirtyEndTime = std::max(dirtyEndTime, targetMaxEndTime);
    } else {
        dirtyStartTime = targetMinStartTime;
        dirtyEndTime = targetMaxEndTime;
    }
    const auto editRange = f0tl.rangeForTimes(dirtyStartTime, dirtyEndTime);
    if (editRange.isEmpty())
        return { AutoTuneApplyStatus::EmptyTargetRange };

    // 一次性保存全局参数（音符自身参数优先，PitchCurve.cpp:335-346）
    const auto params = getCurrentAutoTuneParams();
    auto clonedCurve = contentSnapshot->pitchCurve->clone();
    clonedCurve->applyCorrectionToRange(
        targetNotes, editRange.startFrame, editRange.endFrameExclusive,
        static_cast<float>(contentSnapshot->pitchShiftSettings.getPitchRatio()),
        params.retuneSpeed, params.vibratoDepth, params.vibratoRate);

    const auto snap = clonedCurve->getSnapshot();
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
        editRange.startFrame, editRange.endFrameExclusive, f0tl.endFrameExclusive());

    // 修正段 = 目标音符帧的新段 + affectedRange 内非目标帧的既有段。两者帧域互补、
    // 无重叠（渲染端 forEachCorrectionF0Span 先出现的段优先，重叠段会被遮蔽）。
    std::vector<PitchCorrectionSegment> segmentsInRange;
    if (!selectionActive) {
        // 全量吸附：新段覆盖整个 affectedRange，与历史实现一致（间隙帧 = 原始 F0×pitchRatio）
        for (const auto& seg : snap->getCorrectionSegments()) {
            if (seg.startFrame < affectedRange.endFrameExclusive && seg.endFrame > affectedRange.startFrame)
                segmentsInRange.push_back(seg);
        }
    } else {
        // 目标音符帧区间并集（帧 i 属于音符 ⟺ i*spf ∈ [startTime, endTime)，
        // 与 PitchCurve.cpp 逐帧归属一致），排序后合并相邻/重叠区间
        std::vector<F0FrameRange> noteFrames;
        for (const auto& note : targetNotes) {
            const int noteStart = f0tl.exclusiveFrameAt(note.startTime);
            const int noteEnd = f0tl.exclusiveFrameAt(note.endTime);
            if (noteEnd > noteStart)
                noteFrames.push_back(F0FrameRange{noteStart, noteEnd});
        }
        std::sort(noteFrames.begin(), noteFrames.end(),
                  [](const F0FrameRange& a, const F0FrameRange& b) { return a.startFrame < b.startFrame; });
        std::vector<F0FrameRange> targetFrames;
        for (const auto& nf : noteFrames) {
            if (targetFrames.empty() || targetFrames.back().endFrameExclusive < nf.startFrame)
                targetFrames.push_back(nf);
            else
                targetFrames.back().endFrameExclusive = std::max(targetFrames.back().endFrameExclusive, nf.endFrameExclusive);
        }

        // 新段裁剪到目标音符帧（交集）
        for (const auto& seg : snap->getCorrectionSegments()) {
            if (seg.endFrame <= affectedRange.startFrame || seg.startFrame >= affectedRange.endFrameExclusive)
                continue;
            for (const auto& tf : targetFrames) {
                const int clipStart = std::max(seg.startFrame, tf.startFrame);
                const int clipEnd = std::min(seg.endFrame, tf.endFrameExclusive);
                if (clipEnd <= clipStart)
                    continue;
                const int offset = clipStart - seg.startFrame;
                if (offset + (clipEnd - clipStart) > static_cast<int>(seg.f0Data.size()))
                    continue;   // 与系统段裁剪契约一致（extractSegmentsInRange）
                PitchCorrectionSegment clipped = seg;
                clipped.startFrame = clipStart;
                clipped.endFrame = clipEnd;
                clipped.f0Data.assign(seg.f0Data.begin() + offset,
                                      seg.f0Data.begin() + offset + (clipEnd - clipStart));
                segmentsInRange.push_back(std::move(clipped));
            }
        }

        // 非目标帧的既有段原样保留（帧域区间差）
        for (const auto& seg : curveSnap->getCorrectionSegments()) {
            if (seg.endFrame <= affectedRange.startFrame || seg.startFrame >= affectedRange.endFrameExclusive)
                continue;
            std::vector<F0FrameRange> spans{ F0FrameRange{seg.startFrame, seg.endFrame} };
            for (const auto& tf : targetFrames) {
                std::vector<F0FrameRange> remainder;
                for (const auto& span : spans) {
                    if (tf.startFrame >= span.endFrameExclusive || tf.endFrameExclusive <= span.startFrame) {
                        remainder.push_back(span);
                        continue;
                    }
                    if (span.startFrame < tf.startFrame)
                        remainder.push_back(F0FrameRange{span.startFrame, std::min(span.endFrameExclusive, tf.startFrame)});
                    if (tf.endFrameExclusive < span.endFrameExclusive)
                        remainder.push_back(F0FrameRange{std::max(span.startFrame, tf.endFrameExclusive), span.endFrameExclusive});
                }
                spans = std::move(remainder);
                if (spans.empty())
                    break;
            }
            for (const auto& span : spans) {
                const int offset = span.startFrame - seg.startFrame;
                const int len = span.endFrameExclusive - span.startFrame;
                if (len <= 0 || offset + len > static_cast<int>(seg.f0Data.size()))
                    continue;   // 与系统段裁剪契约一致（extractSegmentsInRange）
                PitchCorrectionSegment kept = seg;
                kept.startFrame = span.startFrame;
                kept.endFrame = span.endFrameExclusive;
                kept.f0Data.assign(seg.f0Data.begin() + offset, seg.f0Data.begin() + offset + len);
                segmentsInRange.push_back(std::move(kept));
            }
        }
    }

    pendingUndoDescription_ = TRANS("音高吸附");   // 与双击吸附语义一致
    // 提交全量 notes：affectedRange 内未选中音符以原样保留，range-scoped merge 不丢音符
    if (!commitEditedContentNotesAndSegments(*contentSnapshot, notes, segmentsInRange, affectedRange)) {
        pendingUndoDescription_ = {};   // 提交失败清理本次事务的临时 undo 状态
        return { AutoTuneApplyStatus::NoContent };
    }
    return { AutoTuneApplyStatus::Applied };
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (scrollBar == &verticalScrollBar_) {
        verticalScrollOffset_ = static_cast<float>(newRangeStart);
        staticDirty_ = true;
        contentDirty_ = true;
        rasterizeDirtySurfaces();
        updateScrollBars();
        repaint();
    }
}

std::vector<Note> PianoRollComponent::getEditedContentNotesCopy() const {
    return cachedNotes_;
}

void PianoRollComponent::updateScrollBars() {
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
        pianoKeyWidth_,
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        pixelsPerSemitone_,
        verticalScrollOffset_,
        maxMidi_,
        gridStyle_
    };
}

ViewMapper PianoRollComponent::makeViewMapperForView(const ViewportState& view) const noexcept {
    return ViewMapper{
        view.camera.visibleStartSeconds,
        view.camera.pixelsPerSecond,
        pianoKeyWidth_,
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        view.pixelsPerSemitone,
        view.verticalScrollOffset,
        maxMidi_,
        gridStyle_
    };
}

} // namespace OpenTune
