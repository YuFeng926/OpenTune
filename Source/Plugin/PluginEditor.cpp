#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/AppLogger.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/ScaleUiMapping.h"
#include "Utils/TimeCoordinate.h"
#include "UI/UiAssets.h"

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif
#include "Content/ContentKey.h"

namespace OpenTune::PluginUI {

namespace {

void showHostManagedMessage(const juce::String& title, const juce::String& detail)
{
    AppLogger::log("VST3Editor: " + title + " requested, delegated to host DAW");
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           title,
                                           "In VST3 mode this action is managed by your DAW.\n\n"
                                               + detail);
}

bool nearlyEqualSeconds(double a, double b)
{
    return std::abs(a - b) <= (1.0 / TimeCoordinate::kRenderSampleRate);
}

ContentTimelineProjection makeCaptureSegmentProjection(const Capture::SegmentInfo& segment)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = segment.T_start;
    projection.timelineDurationSeconds = segment.durationSeconds;
    projection.contentDurationSeconds = segment.durationSeconds;
    return projection;
}

TimelineContentPlacement makePlacement(ContentKey contentKey,
                                                const ContentTimelineProjection& projection,
                                                juce::Colour displayColour)
{
    TimelineContentPlacement placement;
    placement.contentKey = contentKey;
    placement.projection = projection;
    placement.displayColour = displayColour;
    return placement;
}

ContentKey chooseActiveCaptureContentKey(Capture::CaptureSession& session,
                                      double hostTimeSeconds)
{
    Capture::SegmentInfo activeSegment;
    if (session.resolveDisplaySegment(hostTimeSeconds, activeSegment))
        return activeSegment.contentKey;
    return {};
}

#if JucePlugin_Enable_ARA
ContentTimelineProjection makePianoRollLocalProjection(
    const OpenTuneDocumentController::PlaybackRegionProjection& region)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = region.startInPlaybackTime;
    projection.timelineDurationSeconds = region.durationInPlaybackTime;
    projection.contentStartSeconds = region.startInModificationTime;
    projection.contentDurationSeconds = region.durationInModificationTime;
    return projection;
}
#endif

} // anonymous namespace

// =========================================================================
// 构造函数 & 析构函数
// =========================================================================

OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
#if JucePlugin_Enable_ARA
    , AudioProcessorEditorARAExtension(&processor)
#endif
    , processorRef_(processor)
    , languageState_(std::make_shared<LocalizationManager::LanguageState>(
          LocalizationManager::LanguageState{ appPreferences_.getState().shared.language }))
    , languageBinding_(languageState_)
    , menuBar_(processor, MenuBarComponent::Profile::Plugin)
    , topBar_(menuBar_, transportBar_)
    , pianoRoll_(processor.getPlayHeadState())
    , overviewStrip_(pianoRoll_.getWaveformMipmapCache())
{
    setResizable(true, true);
    // 最小宽度 855 = TransportBar 固定内容 711 + reduced(4,4) 8 + TopBar 边距 136（reduced 12×2 + pad 3×2 + 左右侧栏切换钮各 53）
    setResizeLimits(855, ParameterPanel::kMinimumPanelHeight + TOP_BAR_HEIGHT + 12, 2000, 1400);
    setSize(1000, 900);

    UIColors::applyTheme(appPreferences_.getState().shared.theme);

    menuBar_.addListener(this);

    // Undo/Redo 菜单项实时反映撤销栈状态
    menuBar_.canUndoQuery = [this]() { return processorRef_.getUndoManager().canUndo(); };
    menuBar_.canRedoQuery = [this]() { return processorRef_.getUndoManager().canRedo(); };
    LocalizationManager::getInstance().addListener(this);

    transportBar_.addListener(this);

    // VST3 ARA layout: show record button, hide standalone transport group
    transportBar_.setLayoutProfile(TransportBarComponent::LayoutProfile::VST3AraSingleClip);

    // Sync initial transport state from processor
    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    transportBar_.setPositionSeconds(processorRef_.getPosition());
    transportBar_.setBpm(processorRef_.getBpm());

    // Piano key audition
    pianoRoll_.setPianoKeyAudition(&processorRef_.getPianoKeyAudition());

    // Menu popup callbacks — MenuBarComponent stays hidden, provides menu content
    // via TransportBar icon buttons (File/Edit/View)
    menuBar_.setVisible(false);

    transportBar_.onFileMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(0, menuNames.isEmpty() ? juce::String() : menuNames[0]);
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(&transportBar_.getFileButton())
                               .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 0);
                           });
    };

    transportBar_.onEditMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(1, menuNames.size() > 1 ? menuNames[1] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(&transportBar_.getEditButton())
                               .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 1);
                           });
    };

    transportBar_.onViewMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(2, menuNames.size() > 2 ? menuNames[2] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(&transportBar_.getViewButton())
                               .withParentComponent(this),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 2);
                           });
    };

    addAndMakeVisible(topBar_);
    addAndMakeVisible(parameterPanel_);
    parameterPanel_.addListener(this);

    addAndMakeVisible(pianoRoll_);
    pianoRoll_.addListener(this);

    overviewStrip_.addListener(this);
    addAndMakeVisible(overviewStrip_);

    addAndMakeVisible(autoRenderOverlay_);
    autoRenderOverlay_.setVisible(false);
    addAndMakeVisible(renderBadge_);
    renderBadge_.setVisible(false);

    contentCommands_ = processorRef_.getContentCommands();
    pianoRoll_.setProcessor(&processorRef_);
    pianoRoll_.setContentCommands(contentCommands_);
    pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
        return processorRef_.getContentSnapshot(key);
    });
    // EQ popup「以后不再提示」偏好直接注入（无中转层）
    pianoRoll_.setAppPreferences(&appPreferences_);

    applyThemeToEditor(appPreferences_.getState().shared.theme);

    startTimerHz(kHeartbeatHz);

    const auto f0Type = appPreferences_.getState().shared.f0ModelType;
    if (!processorRef_.setF0ModelType(f0Type))
        appPreferences_.setF0ModelType(F0ModelType::FCPE);

    grabKeyboardFocus();
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    // Save current viewport state before teardown — unconditionally,
    // because the host removes the editor from the hierarchy before
    // destruction, making isShowing() false while state objects still exist.
    rememberPresentedPianoRollViewport();
    setLookAndFeel(nullptr);
    stopTimer();
    overviewStrip_.removeListener(this);
    LocalizationManager::getInstance().removeListener(this);
    menuBar_.removeListener(this);
    transportBar_.removeListener(this);
    parameterPanel_.removeListener(this);
    pianoRoll_.removeListener(this);
}

// =========================================================================
// rememberPresentedPianoRollViewport
// =========================================================================

void OpenTuneAudioProcessorEditor::rememberPresentedPianoRollViewport()
{
    if (!presentedPlacementIdentity_.has_value())
        return;
    const auto vs = pianoRoll_.viewportState();
    PianoRollViewportPrimitive prim;
    prim.cameraStartSeconds = vs.camera.visibleStartSeconds;
    prim.cameraPixelsPerSecond = vs.camera.pixelsPerSecond;
    prim.pixelsPerSemitone = vs.pixelsPerSemitone;
    prim.verticalScrollOffset = vs.verticalScrollOffset;
    processorRef_.rememberPianoRollViewport(*presentedPlacementIdentity_, prim);
}

// =========================================================================
// paint / resized / mouseDown
// =========================================================================

void OpenTuneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(UIColors::backgroundDark);
}

void OpenTuneAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    constexpr int gap = 4;

    bounds.reduce(gap, gap);

    // TopBar: 固定高度
    constexpr int topBarHeight = TOP_BAR_HEIGHT;
    topBar_.setBounds(bounds.removeFromTop(topBarHeight));
    bounds.removeFromTop(gap);

    // 右侧 ParameterPanel
    constexpr int paramPanelWidth = PARAMETER_PANEL_WIDTH;
    parameterPanel_.setBounds(bounds.removeFromRight(paramPanelWidth));
    bounds.removeFromRight(gap);

    // 中央 PianoRoll
    pianoRoll_.setBounds(bounds);

    // Overview strip: overlay at bottom, aligned with PianoRoll's timeline viewport.
    {
        const auto timelineViewport = pianoRoll_.getTimelineViewportBounds();
        const int overviewX = bounds.getX() + 12;
        const int overviewRight = bounds.getX() + timelineViewport.getRight();
        const int overviewBottom = bounds.getY() + timelineViewport.getBottom();
        const int overviewHeight = OVERVIEW_STRIP_HEIGHT + UIColors::scrollBarThickness;
        overviewStrip_.setBounds(overviewX,
                                 overviewBottom - overviewHeight,
                                 overviewRight - overviewX,
                                 overviewHeight);
    }

    // Overlay 覆盖 PianoRoll 区域
    autoRenderOverlay_.setBounds(pianoRoll_.getBounds());
    autoRenderOverlay_.toFront(false);

    renderBadge_.setBounds(pianoRoll_.getRight() - 148, pianoRoll_.getY() + 8, 140, 28);
    renderBadge_.toFront(false);
}

void OpenTuneAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
}

// =========================================================================
// syncSharedAppPreferences
// =========================================================================

void OpenTuneAudioProcessorEditor::syncSharedAppPreferences()
{
    const auto preferencesState = appPreferences_.getState();
    const auto& sharedPreferences = preferencesState.shared;
    const auto& visualPreferences = sharedPreferences.pianoRollVisualPreferences;

    // EQ popup「以后不再提示」偏好直接注入（无中转层）
    pianoRoll_.setAppPreferences(&appPreferences_);

    languageState_->language = sharedPreferences.language;

    if (appliedLanguage_ != sharedPreferences.language) {
        appliedLanguage_ = sharedPreferences.language;
        LocalizationManager::getInstance().notifyLanguageChanged(sharedPreferences.language);
    }

    if (appliedThemeId_ != sharedPreferences.theme)
        applyThemeToEditor(sharedPreferences.theme);

    pianoRoll_.setAudioEditingScheme(sharedPreferences.audioEditingScheme);
    pianoRoll_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    pianoRoll_.setNoteNameMode(visualPreferences.noteNameMode);
    pianoRoll_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);
    pianoRoll_.setBackgroundBrightness(visualPreferences.backgroundBrightness);
    pianoRoll_.setGridStyle(sharedPreferences.gridStyle);
    pianoRoll_.setShortcutSettings(sharedPreferences.shortcuts);
    menuBar_.setNoteNameMode(visualPreferences.noteNameMode);
    menuBar_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);

    pianoRoll_.setExperimentalFeaturesEnabled(false);
    parameterPanel_.setExperimentalFeaturesEnabled(false);
    parameterPanel_.setOpenDyneMode(AudioEditingScheme::usesNotesPrimaryScheme(sharedPreferences.audioEditingScheme));
    parameterPanel_.setActiveTool(static_cast<int>(pianoRoll_.getCurrentTool()));
}

// =========================================================================
// timerCallback
// =========================================================================

void OpenTuneAudioProcessorEditor::timerCallback()
{
    syncSharedAppPreferences();

    // Non-ARA capture state is driven by the processor's own Timer (tick()).
    // Editor only syncs UI state from the capture session.
    if (auto* session = processorRef_.getCaptureSession()) {
        // Sync record button visual state
        using OpenTune::Capture::SessionState;
        switch (session->getGlobalState()) {
            case SessionState::Idle:
                transportBar_.setRecordButtonState(RecordButtonState::Idle);
                break;
            case SessionState::HasCapturing:
                transportBar_.setRecordButtonState(RecordButtonState::Capturing);
                break;
            case SessionState::HasProcessing:
                transportBar_.setRecordButtonState(RecordButtonState::Processing);
                break;
        }
    }

    syncParameterPanelFromSelection();

    // Heartbeat ticks first — match Standalone pattern: overview/camera
    // state is stable before any content projection runs.
    // pianoRoll_ heartbeat runs unconditionally so VST3/ARA can advance
    // WaveformMipmapCache even before the PianoRoll is showing.
    pianoRoll_.onHeartbeatTick();

    // Content projection after heartbeat: publish the previous frame's stable
    // camera first (heartbeat ticks have settled), then sync this frame's content.
    // Running before heartbeat would cause a duplicate resolve race.
    const auto sync = syncContentProjectionToPianoRoll();

    // Overview update: regular capture uses multi-segment overview path;
    // ARA/other paths continue using the single-content onHeartbeatTick.
    if (pianoRoll_.isShowing()) {
        if (sync.isRegularVst3Capture)
        {
            overviewStrip_.onHeartbeatTickRegular(sync.placements,
                                                  sync.timelineViewStartSeconds,
                                                  sync.timelineViewEndSeconds,
                                                  pianoRoll_.timelineCamera(),
                                                  pianoRoll_.timelinePolicyViewportWidth());
        }
        else
        {
            overviewStrip_.onHeartbeatTick(pianoRoll_.editedContentKey(),
                                           pianoRoll_.activeContentProjection(),
                                           pianoRoll_.timelineCamera(),
                                           pianoRoll_.timelinePolicyViewportWidth());
        }
    }

    // Continuously remember viewport for stable placement restore
    rememberPresentedPianoRollViewport();

    // Revision detection (aligned with Standalone pattern)
    const ContentKey activeKey = sync.activeContentKey;

    const auto observeOriginalF0State = [this](ContentKey contentKey) {
        const auto snapshot = processorRef_.getContentSnapshot(contentKey);
        const auto currentState = snapshot
            ? snapshot->originalF0State
            : OriginalF0State::NotRequested;
        const auto previous = lastObservedOriginalF0States_.find(contentKey);
        if (previous != lastObservedOriginalF0States_.end()
            && (previous->second == OriginalF0State::Extracting
                || previous->second == OriginalF0State::NotRequested)
            && currentState == OriginalF0State::Ready) {
            const auto intentIt = pendingNoteGenerationOnReady_.find(contentKey);
            if (intentIt != pendingNoteGenerationOnReady_.end()) {
                // 仅生成音符，不写修正曲线（还原 Melodyne 初始状态）。
                // f0Count 由 generateNotesOnly 内部从同一 snapshot 派生，调用方不传范围。
                contentCommands_->generateNotesOnly(contentKey, intentIt->second);
                pendingNoteGenerationOnReady_.erase(intentIt);
            }
        }
        lastObservedOriginalF0States_[contentKey] = currentState;
    };

    for (const auto& placement : sync.placements)
        observeOriginalF0State(placement.contentKey);

    if (auto* session = processorRef_.getCaptureSession()) {
        for (const auto& segment : session->listSegments())
            observeOriginalF0State(segment.contentKey);
    }

    // Content 切换检测
    bool contentJustSwitched = false;
    if (activeKey != lastRevisionObservedContentKey_) {
        lastRevisionObservedContentKey_ = activeKey;
        contentJustSwitched = true;

        // 同步到当前 snapshot 的 revision baseline（与 Standalone 对齐）
        if (activeKey.isValid()) {
            auto snap = processorRef_.getContentSnapshot(activeKey);
            lastPianoRollNotesRevision_ = snap ? snap->notesRevision : 0;
            lastPianoRollTimeGridRevision_ = snap ? snap->timeGridRevision : 0;
            lastPianoRollPitchRevision_ = snap ? snap->pitchRevision : 0;
        } else {
            lastPianoRollNotesRevision_ = 0;
            lastPianoRollTimeGridRevision_ = 0;
            lastPianoRollPitchRevision_ = 0;
        }
    }

    // Revision 检测（只在有效 content 且未切换时执行）
    if (activeKey.isValid() && !contentJustSwitched) {
        auto snap = processorRef_.getContentSnapshot(activeKey);
        const uint64_t currentNotesRevision = snap ? snap->notesRevision : 0;
        const uint64_t currentTimeGridRevision = snap ? snap->timeGridRevision : 0;
        const uint64_t currentPitchRevision = snap ? snap->pitchRevision : 0;

        if (currentNotesRevision != lastPianoRollNotesRevision_) {
            pianoRoll_.onNotesRevisionChanged();
            lastPianoRollNotesRevision_ = currentNotesRevision;
        }

        if (currentTimeGridRevision != lastPianoRollTimeGridRevision_) {
            pianoRoll_.onTimeGridRevisionChanged();
            lastPianoRollTimeGridRevision_ = currentTimeGridRevision;
        }

        if (currentPitchRevision != lastPianoRollPitchRevision_) {
            pianoRoll_.onPitchRevisionChanged();
            lastPianoRollPitchRevision_ = currentPitchRevision;
        }
    }

    // Pitch shift indicator: single source of truth is active content snapshot
    PitchShiftSettings currentPitchShift = PitchShiftSettings::identity();
    if (activeKey.isValid()) {
        if (auto snap = processorRef_.getContentSnapshot(activeKey))
            currentPitchShift = snap->pitchShiftSettings;
    }
    if (currentPitchShift != lastPitchShiftIndicatorSettings_) {
        lastPitchShiftIndicatorSettings_ = currentPitchShift;
        parameterPanel_.setPitchShiftIndicator(currentPitchShift.semitone, currentPitchShift.cents);
    }

    // RMVPE overlay：读取音频后的 F0 提取 + note 生成期间显示"正在处理音频"遮罩
    if (rmvpeOverlayLatched_) {
        bool allDone = true;
        for (const auto& key : rmvpeOverlayTargetContentKeys_) {
            auto snap = processorRef_.getContentSnapshot(key);
            if (snap == nullptr)
                continue;  // content 已被移除，视为完成
            // AUTO 由 Read 意图在 F0 Ready 跳变时消费（唯一核心），
            // overlay 仅需跟踪 OriginalF0State。
            if (snap->originalF0State != OriginalF0State::Ready
                    && snap->originalF0State != OriginalF0State::Failed) {
                allDone = false;
                break;
            }
        }
        if (allDone) {
            rmvpeOverlayLatched_ = false;
            rmvpeOverlayTargetContentKeys_.clear();
        }
    }

    bool shouldShowOverlay = false;
    bool shouldShowBadge = false;
    const auto chunkStats = processorRef_.getReadableContentChunkStats(activeKey);
    const bool isAutoProcessing = false;
    const int completedTasks = chunkStats.idle + chunkStats.blank;
    const int totalTasks = chunkStats.total();

    if (isAutoProcessing) {
        const float progress = totalTasks > 0
            ? static_cast<float>(completedTasks) / static_cast<float>(totalTasks)
            : 0.0f;
        autoRenderOverlay_.setMessageText(
            buildRenderingOverlayTitle(completedTasks, totalTasks, progress));
        shouldShowOverlay = true;
    } else if (chunkStats.hasActiveWork()) {
        renderBadge_.setMessageText(juce::String::fromUTF8(u8"\u6e32\u67d3\u4e2d (")
            + juce::String(completedTasks) + "/" + juce::String(totalTasks) + ")");
        shouldShowBadge = true;
    }

    if (rmvpeOverlayLatched_) {
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8(u8"\u6B63\u5728\u5904\u7406\u97F3\u9891"));
        shouldShowOverlay = true;
    }

    if (renderBadge_.isVisible() != shouldShowBadge)
        renderBadge_.setVisible(shouldShowBadge);
    if (autoRenderOverlay_.isVisible() != shouldShowOverlay)
        autoRenderOverlay_.setVisible(shouldShowOverlay);

    // Playhead position: TransportBar gets presented position via processorRef_.getPosition()
    // which maps to the shared PlayHeadState projection. PianoRoll reads presented position
    // directly from PlayHeadState in its VBlank callback.
    const double positionSeconds = processorRef_.getPosition();
    transportBar_.setPositionSeconds(positionSeconds);

    // Playing state sync: TransportBar reflects canonical state from the processor.
    if (transportBar_.isPlaying() != processorRef_.isPlaying()) {
        transportBar_.setPlaying(processorRef_.isPlaying());
    }

    // Loop state sync: TransportBar reflects canonical state from the processor.
    if (transportBar_.isLoopEnabled() != processorRef_.isLoopEnabled()) {
        transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    }

    // BPM sync (host-owned, read-only)
    const double bpm = processorRef_.getBpm();
    if (bpm > 0.0 && std::abs(bpm - lastSyncedBpm_) > 0.001) {
        transportBar_.setBpm(bpm);
        pianoRoll_.setBpm(bpm);
        lastSyncedBpm_ = bpm;
    }

    // Time signature sync (host-owned, read-only)
    const int timeSigNum = processorRef_.getTimeSigNumerator();
    const int timeSigDenom = processorRef_.getTimeSigDenominator();
    if (timeSigNum > 0 && timeSigDenom > 0
        && (timeSigNum != lastSyncedTimeSigNum_ || timeSigDenom != lastSyncedTimeSigDenom_)) {
        transportBar_.setTimeSignature(timeSigNum, timeSigDenom);
        pianoRoll_.setTimeSignature(timeSigNum, timeSigDenom);
        lastSyncedTimeSigNum_ = timeSigNum;
        lastSyncedTimeSigDenom_ = timeSigDenom;
    }

    // Timeline display mode sync from preferences
    const auto prefMode = appPreferences_.getTimelineDisplayMode();
    if (prefMode != timelineDisplayMode_) {
        timelineDisplayMode_ = prefMode;
        transportBar_.setTimelineDisplayMode(timelineDisplayMode_);
        pianoRoll_.setTimelineDisplayMode(timelineDisplayMode_);
    }
}

// =========================================================================
// syncParameterPanelFromSelection
// =========================================================================

void OpenTuneAudioProcessorEditor::syncParameterPanelFromSelection()
{
    ParameterPanelSyncContext context;
    context.clipRetuneSpeedPercent = pianoRoll_.getCurrentRetuneSpeed() * 100.0f;
    context.clipVibratoDepth = pianoRoll_.getCurrentVibratoDepth();
    context.clipVibratoRate = pianoRoll_.getCurrentVibratoRate();
    context.wasShowingSelectionParameters = showingSingleNoteParams_;

    context.hasSelectedNoteParameters = pianoRoll_.getSingleSelectedNoteParameters(
        context.selectedNoteRetuneSpeedPercent,
        context.selectedNoteVibratoDepth,
        context.selectedNoteVibratoRate);

    const auto decision = resolveParameterPanelSyncDecision(context);
    if (decision.shouldSetRetuneSpeed) {
        parameterPanel_.setRetuneSpeed(decision.retuneSpeedPercent);
    }
    if (decision.shouldSetVibratoDepth) {
        parameterPanel_.setVibratoDepth(decision.vibratoDepth);
    }
    if (decision.shouldSetVibratoRate) {
        parameterPanel_.setVibratoRate(decision.vibratoRate);
    }

    showingSingleNoteParams_ = decision.nextShowingSelectionParameters;
}

// =========================================================================
// languageChanged
// =========================================================================

void OpenTuneAudioProcessorEditor::languageChanged(Language newLanguage)
{
    juce::ignoreUnused(newLanguage);

    menuBar_.menuItemsChanged();
    menuBar_.repaint();

    transportBar_.refreshLocalizedText();
    topBar_.refreshLocalizedText();
    parameterPanel_.refreshLocalizedText();

    repaint();
}

ContentKey OpenTuneAudioProcessorEditor::resolveCurrentContentKey()
{
    return resolveCurrentContentSync().activeContentKey;
}

OpenTuneAudioProcessorEditor::PianoRollContentSync
OpenTuneAudioProcessorEditor::resolveCurrentContentSync()
{
    PianoRollContentSync sync;

#if JucePlugin_Enable_ARA
    if (const auto* dc = processorRef_.getDocumentController()) {
        const auto regions = dc->getPlaybackRegionProjections();

        // Build identity+placement pairs for all valid regions.
        struct RegionEntry {
            PianoRollPlacementIdentity identity;
            TimelineContentPlacement placement;
        };
        std::vector<RegionEntry> entries;
        for (const auto& region : regions) {
            if (!region.contentKey.isValid())
                continue;
            const auto projection = makePianoRollLocalProjection(region);
            if (!projection.isValid())
                continue;
            RegionEntry entry;
            entry.identity.contentKey = region.contentKey;
            entry.identity.projection = projection;
            // 宿主未提供 ARA 标准颜色时采用插件产品默认色
            entry.placement = makePlacement(region.contentKey, projection,
                                            region.displayColour.value_or(UIColors::noteBlock));
            entries.push_back(std::move(entry));
        }

        if (entries.empty())
            return sync;

        // Resolve active placement: focused → lastActive → earliest
        const auto focusedRegion = dc->getFocusedEditorPlaybackRegionProjection();
        std::optional<PianoRollPlacementIdentity> activeIdentity;

        if (focusedRegion.has_value() && focusedRegion->contentKey.isValid()) {
            const auto focusedProj = makePianoRollLocalProjection(*focusedRegion);
            if (focusedProj.isValid()) {
                PianoRollPlacementIdentity focusedIdentity{
                    focusedRegion->contentKey, focusedProj};
                // Only adopt focused identity if it matches an entry exactly
                const bool matched = std::any_of(entries.begin(), entries.end(),
                    [&](const auto& e) { return e.identity == focusedIdentity; });
                if (matched)
                    activeIdentity = focusedIdentity;
            }
        }

        // last-active → earliest fallback
        if (!activeIdentity.has_value()) {
            activeIdentity = processorRef_.lastActivePianoRollPlacement();
            if (activeIdentity.has_value()) {
                // Verify it still exists among current entries
                const bool found = std::any_of(entries.begin(), entries.end(),
                    [&](const auto& e) { return e.identity == *activeIdentity; });
                if (!found)
                    activeIdentity = std::nullopt;
            }
        }

        if (!activeIdentity.has_value()) {
            // Fallback: earliest timeline item
            const auto* earliest = &entries.front();
            for (const auto& e : entries) {
                if (e.identity.projection.timelineStartSeconds
                    < earliest->identity.projection.timelineStartSeconds)
                    earliest = &e;
            }
            activeIdentity = earliest->identity;
        }

        sync.activePlacementIdentity = activeIdentity;
        sync.activeContentKey = activeIdentity->contentKey;

        // Put active placement at front so findEditedPlacement() resolves correctly.
        auto activeIt = std::find_if(entries.begin(), entries.end(),
            [&](const auto& e) { return e.identity == *activeIdentity; });
        if (activeIt != entries.end()) {
            sync.placements.push_back(std::move(activeIt->placement));
            entries.erase(activeIt);
        }
        for (auto& e : entries)
            sync.placements.push_back(std::move(e.placement));

        return sync;
    }
#endif

    if (auto* session = processorRef_.getCaptureSession()) {
        sync.isRegularVst3Capture = true;
        double viewEndSeconds = 0.0;
        for (const auto& segment : session->listEditedSegments()) {
            const auto projection = makeCaptureSegmentProjection(segment);
            sync.placements.push_back(makePlacement(segment.contentKey, projection, UIColors::noteBlock));
            viewEndSeconds = std::max(viewEndSeconds, projection.timelineEndSeconds());
        }

        if (!sync.placements.empty()) {
            sync.activeContentKey = chooseActiveCaptureContentKey(*session, processorRef_.getPosition());
            const auto activeIt = std::find_if(sync.placements.begin(),
                                               sync.placements.end(),
                                               [&sync](const auto& placement) {
                                                   return placement.contentKey == sync.activeContentKey;
                                               });
            if (activeIt == sync.placements.end()) {
                sync.activeContentKey = {};
            } else {
                sync.activePlacementIdentity = PianoRollPlacementIdentity{
                    activeIt->contentKey, activeIt->projection};
            }

            sync.timelineViewStartSeconds = 0.0;
            sync.timelineViewEndSeconds = viewEndSeconds;
        }
    }

    return sync;
}

bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    return handleEditorShortcut(key);
}

bool OpenTuneAudioProcessorEditor::handleEditorShortcut(const juce::KeyPress& key)
{
    const auto& shortcutSettings = appPreferences_.getState().shared.shortcuts;

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Undo, key)) {
        undoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Redo, key)) {
        redoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::PlayPause, key)) {
        playPauseToggleRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Stop, key)) {
        stopPlaybackRequested();
        return true;
    }

    return false;
}

void OpenTuneAudioProcessorEditor::retuneSpeedChanged(float speed)
{
    const float normalized = speed / 100.0f;
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::RetuneSpeed, normalized);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::RetuneSpeed, normalized);
    }
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoDepth, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoDepth, value);
    }
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoRate, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoRate, value);
    }
}

void OpenTuneAudioProcessorEditor::noteSplitChanged(float value)
{
    // NoTarget 时 editParameter 内部已回落更新创建默认值
    pianoRoll_.editParameter(AudioEditingScheme::ParameterId::NoteSplit, value);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::Eq)) {
        return;
    }

    const auto tool = static_cast<ToolId>(toolId);
    pianoRoll_.setCurrentTool(tool);
}

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           "Import Audio",
                                           "Please import audio from your DAW in VST3 mode.");
}

void OpenTuneAudioProcessorEditor::exportAudioRequested(MenuBarComponent::ExportType exportType)
{
    juce::ignoreUnused(exportType);
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           "Export Audio",
                                           "Please render/export from your DAW in VST3 mode.");
}

void OpenTuneAudioProcessorEditor::openProjectRequested()
{
    showHostManagedMessage("Open Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::saveProjectRequested()
{
    showHostManagedMessage("Save Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::saveProjectAsRequested()
{
    showHostManagedMessage("Save Project As...",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::openRecentProjectRequested(const juce::File&)
{
    showHostManagedMessage("Open Recent Project",
                           "Project file management is handled in the Standalone version.");
}

void OpenTuneAudioProcessorEditor::clearRecentProjectsRequested()
{
    // No-op in VST3 mode
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto pages = SharedPreferencePages::create(appPreferences_, [this] { syncSharedAppPreferences(); }, true);

    // Insert Audio page (with rendering priority) at the beginning
    auto onVocoderModelWeightChanged = [this](VocoderModelWeight weight) {
        processorRef_.setVocoderModelWeight(weight);
    };
    auto onF0ModelChanged = [this](F0ModelType type) {
        return processorRef_.setF0ModelType(type);
    };
    auto onLightPitchCorrectionChanged = [this](bool) {
        processorRef_.invalidateAllContentCaches();
    };
    auto audioPage = SharedPreferencePages::createRenderingPriorityComponent(
        appPreferences_, [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); },
        std::move(onVocoderModelWeightChanged),
        std::move(onF0ModelChanged),
        std::move(onLightPitchCorrectionChanged),
        true);
    const int audioPageHeight = SharedPreferencePages::getRenderingPriorityPageHeight(*audioPage);
    pages.insert(pages.begin(), { LOC(kAudio), std::move(audioPage), audioPageHeight });

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));

    // 根据当前屏幕可用区域计算对话框尺寸，适配不同显示器和分辨率
    const auto usable = getParentMonitorArea();
    const int maxW = juce::jmin(640, usable.getWidth() - 48);
    const int maxH = juce::jmin(560, usable.getHeight() - 48);
    dialogContent->setSize(juce::jmax(480, maxW), juce::jmax(480, maxH));

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = "Preferences";
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.useBottomRightCornerResizer = true;
    // 确保对话框打开前 PianoRoll 持有焦点，JUCE 模态管理器会在关闭时自动恢复
    pianoRoll_.grabKeyboardFocus();
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
    showHostManagedMessage("Help",
                           "Open the host DAW plugin help/manual entry for VST3 usage guidance.");
}

void OpenTuneAudioProcessorEditor::showWaveformToggled(bool shouldShow)
{
    pianoRoll_.setShowWaveform(shouldShow);
}

void OpenTuneAudioProcessorEditor::showLanesToggled(bool shouldShow)
{
    pianoRoll_.setShowLanes(shouldShow);
}

void OpenTuneAudioProcessorEditor::noteNameModeChanged(NoteNameMode noteNameMode)
{
    if (appPreferences_.getState().shared.pianoRollVisualPreferences.noteNameMode != noteNameMode) {
        appPreferences_.setNoteNameMode(noteNameMode);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
}

void OpenTuneAudioProcessorEditor::showUnvoicedFramesToggled(bool shouldShow)
{
    if (appPreferences_.getState().shared.pianoRollVisualPreferences.showUnvoicedFrames != shouldShow) {
        appPreferences_.setShowUnvoicedFrames(shouldShow);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
}

void OpenTuneAudioProcessorEditor::themeChanged(ThemeId themeId)
{
    if (appPreferences_.getState().shared.theme != themeId) {
        appPreferences_.setTheme(themeId);
    }

    applyThemeToEditor(themeId);
}

void OpenTuneAudioProcessorEditor::applyThemeToEditor(ThemeId themeId)
{

    appliedThemeId_ = themeId;
    UIColors::applyTheme(themeId);

    if (themeId == ThemeId::Aurora) {
        setLookAndFeel(&auroraLookAndFeel_);
    } else {
        setLookAndFeel(&openTuneLookAndFeel_);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
    }

    // Install process-wide default so orphaned AlertWindows / DialogWindow title
    // bars always use Aurora glass styling regardless of current editor theme.
    AuroraLookAndFeel::installAsDefault();

    topBar_.applyTheme();
    parameterPanel_.applyTheme();
    pianoRoll_.setPlayheadColour(UIColors::playhead);
    sendLookAndFeelChange();
    pianoRoll_.requestThemeRedraw();
    repaint();
}

void OpenTuneAudioProcessorEditor::undoRequested()
{
    if (processorRef_.getUndoManager().undo() != nullptr)
        syncContentProjectionToPianoRoll();
}

void OpenTuneAudioProcessorEditor::redoRequested()
{
    if (processorRef_.getUndoManager().redo() != nullptr)
        syncContentProjectionToPianoRoll();
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    juce::ignoreUnused(theme);
}

void OpenTuneAudioProcessorEditor::cursorStyleChanged(CursorStyleId style)
{
    juce::ignoreUnused(style);
}

void OpenTuneAudioProcessorEditor::playRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        const double pendingSeek = pianoRoll_.consumePendingSeekTime();
        if (!docController->requestStartPlayback(pendingSeek))
            AppLogger::log("ARA: requestStartPlayback failed — host playback controller unavailable");
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("play");
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        if (!docController->requestStopPlayback())
            AppLogger::log("ARA: requestStopPlayback failed — host playback controller unavailable");
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("pause");
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        if (!docController->requestStopPlayback())
            AppLogger::log("ARA: stop request failed — host playback controller unavailable");
        return;
    }
#endif
    surfaceRegularVst3HostControlledTransport("stop");
}

void OpenTuneAudioProcessorEditor::surfaceRegularVst3HostControlledTransport(const char* actionName)
{
    const juce::String action(actionName);
    const juce::String message = "Regular VST3 " + action + " requested: host-controlled transport";
    AppLogger::log("VST3Editor: " + message);
    transportBar_.setRenderStatusText("Host-controlled transport");
}

void OpenTuneAudioProcessorEditor::timelineDisplayModeChanged(TimelineDisplayMode mode)
{
    timelineDisplayMode_ = mode;
    appPreferences_.setTimelineDisplayMode(mode);
    transportBar_.setTimelineDisplayMode(mode);
    pianoRoll_.setTimelineDisplayMode(mode);
}

void OpenTuneAudioProcessorEditor::viewToggled(bool workspaceView)
{
    juce::ignoreUnused(workspaceView);
    // VST3: always single-clip piano view, ignore workspace view toggle
    transportBar_.setWorkspaceView(false);
    pianoRoll_.setVisible(true);
    pianoRoll_.grabKeyboardFocus();
    resized();
    repaint();
}

void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)
{
#if JucePlugin_Enable_ARA
    // ARA-bound VST3: emit the official one-way HostPlaybackController request.
    // Host may ignore/delay/quantize; loop truth is observed via companion
    // PositionInfo, never written here. No local canonical write.
    if (auto* docController = processorRef_.getDocumentController()) {
        if (!docController->requestEnableCycle(enabled))
            AppLogger::log("ARA: requestEnableCycle failed — host playback controller unavailable");
        return;
    }
#endif
    // Regular (non-ARA) VST3: loop is host-controlled only. No local write —
    // the host owns loop state and surfaces it via PositionInfo.
    juce::ignoreUnused(enabled);
    surfaceRegularVst3HostControlledTransport("loop");
}

void OpenTuneAudioProcessorEditor::bpmChanged(double newBpm)
{
    // VST3/ARA: host owns BPM. Do not write to processor.
    juce::ignoreUnused(newBpm);
}

void OpenTuneAudioProcessorEditor::timeSignatureChanged(int numerator, int denominator)
{
    // VST3/ARA: host owns time signature. Do not write to processor.
    juce::ignoreUnused(numerator);
    juce::ignoreUnused(denominator);
}

void OpenTuneAudioProcessorEditor::scaleChanged(int rootNote, int scaleType)
{
    if (suppressScaleChangedCallback_) {
        return;
    }

    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);
    pianoRoll_.setScale(clampedRoot, clampedType);

    const auto activeKey = resolveCurrentContentKey();
    if (activeKey.isValid()) {
        // 手动设置：唯一映射入口 makeDetectedKeyFromUi 固定 confidence=1.0 + origin=Manual
        const DetectedKey key = OpenTune::makeDetectedKeyFromUi(clampedRoot, clampedType);
        contentCommands_->setDetectedKey(activeKey, key);
    }
}



void OpenTuneAudioProcessorEditor::recordRequested()
{
    if (auto* session = processorRef_.getCaptureSession()) {
        AppLogger::log("VST3 recordRequested mode=regular-vst3 processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_)));
        using OpenTune::Capture::SessionState;
        switch (session->getGlobalState()) {
            case SessionState::Idle:
                session->armNewCapture();
                AppLogger::log("VST3 Capture: armNewCapture (regular-vst3 path)");
                break;
            case SessionState::HasCapturing:
                session->stopCapture();
                AppLogger::log("VST3 Capture: stopCapture (regular-vst3 path)");
                break;
            case SessionState::HasProcessing:
                AppLogger::log("VST3 Capture: ignored (Processing - wait for render)");
                break;
        }
        return;
    }

#if !JucePlugin_Enable_ARA
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                           "Read Audio",
                                           "This VST3 instance is not ready for audio capture or ARA reading.");
    return;
#else
    auto* dc = processorRef_.getDocumentController();

    AppLogger::log("VST3 recordRequested mode=ara-bound processor="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_))
        + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc)));

    const auto allRegions = dc->getPlaybackRegionProjections();
    if (allRegions.empty()) {
        return;  // 无 region 时静默返回
    }

    dc->requestReadAudioForPlaybackRegionsAsync([this, dc, regionCount = static_cast<int>(allRegions.size())](int refreshed) {
        if (refreshed < 0) {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Read Audio",
                                                   "Audio regions could not be processed.");
            return;
        }

        if (refreshed == 0)
            return;

        AppLogger::log("ReadAudio: refreshed " + juce::String(refreshed)
            + " AudioModification(s) from " + juce::String(regionCount)
            + " playback region(s)");

        // 遮罩覆盖本次读取的全部 modification：F0 提取 + note 生成完成前保持"正在处理音频"
        rmvpeOverlayTargetContentKeys_.clear();
        for (const auto& projection : dc->getPlaybackRegionProjections()) {
            if (projection.contentKey.isValid()) {
                rmvpeOverlayTargetContentKeys_.push_back(projection.contentKey);
                // F0 状态机基线重置：防止 F0 完成早于首次 timer 观察导致跳变丢失
                lastObservedOriginalF0States_[projection.contentKey] = OriginalF0State::NotRequested;
                // 捕获 OpenDyne 一次性音符生成意图，F0 Ready 跳变时消费
                if (pianoRoll_.isOpenDyne())
                    pendingNoteGenerationOnReady_[projection.contentKey] = pianoRoll_.getCurrentAutoTuneParams();
                else
                    pendingNoteGenerationOnReady_.erase(projection.contentKey);
            }
        }
        rmvpeOverlayLatched_ = true;
    });
#endif
}

bool OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        return docController->requestSetPlaybackPosition(timeSeconds);
    }
#endif
    // Non-ARA VST3: playhead is host-controlled only. Do NOT call setPosition().
    // the host would ignore it and the next processBlock would overwrite the value.
    // PianoRoll click/drag on timeline should not change plugin-internal position.
    juce::ignoreUnused(timeSeconds);
    return false;
}

void OpenTuneAudioProcessorEditor::playPauseToggleRequested()
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController()) {
        const double pendingSeek = pianoRoll_.consumePendingSeekTime();
        if (!docController->requestTogglePlayback(processorRef_.isPlaying(), pendingSeek))
            AppLogger::log("ARA: requestTogglePlayback failed — host playback controller unavailable");
        return;
    }
#endif
    // Non-ARA VST3: host-controlled transport. Use per-processor isPlaying as fallback.
    const bool isPlaying = processorRef_.isPlaying();
    if (isPlaying) {
        pauseRequested();
    } else {
        playRequested();
    }
}

void OpenTuneAudioProcessorEditor::stopPlaybackRequested()
{
    stopRequested();
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    const auto activeKey = resolveCurrentContentKey();
    AppLogger::log("AutoTune: vst3 request contentKey.objectId=" + juce::String(static_cast<juce::int64>(activeKey.objectId)));
    if (!activeKey.isValid()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "AUTO",
            "AUTO needs an active ARA audio modification.");
        return;
    }

    OriginalF0State f0State = OriginalF0State::NotRequested;
    if (auto snap = processorRef_.getContentSnapshot(activeKey))
        f0State = snap->originalF0State;
#if JucePlugin_Enable_ARA
    else if (auto* dc = processorRef_.getDocumentController())
        f0State = dc->readOriginalF0State(activeKey);
#endif

    if (f0State == OriginalF0State::Extracting) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "OriginalF0",
            "OriginalF0 is being extracted. Please retry in a moment.");
        return;
    }

    if (f0State == OriginalF0State::Failed) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "OriginalF0",
            "OriginalF0 extraction failed for this clip. Re-import the audio to regenerate OriginalF0.");
        return;
    }

    if (f0State != OriginalF0State::Ready) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "OriginalF0",
            "OriginalF0 is not ready for this clip.");
        return;
    }

    const auto result = pianoRoll_.applyAutoTuneToSelection();
    AppLogger::log("AutoTune: vst3 apply result=" + juce::String(result.applied() ? "true" : "false")
        + " message=" + result.message());
    if (!result.applied()) {
        // NoChange = 最终修正已达成：静默，不弹窗
        if (result.status != PianoRollComponent::AutoTuneApplyStatus::NoChange) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "AUTO",
                result.message());
        }
        return;
    }

}

void OpenTuneAudioProcessorEditor::pitchShiftRequested()
{
    const auto activeKey = resolveCurrentContentKey();
    if (!activeKey.isValid()) return;

    PitchShiftSettings currentSettings = PitchShiftSettings::identity();
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController())
        currentSettings = dc->readPitchShift(activeKey);
    else
#endif
        currentSettings = processorRef_.getPitchShiftSettings(activeKey);

    auto* content = new OpenTune::PitchShiftDialogContent(currentSettings);

    auto commands = getContentCommandsShared();
    // 生命周期绑定 content：作为其子组件托管，DialogWindow 关闭删除 content 时自动析构，
    // Esc/关闭按钮/确认/重置四条关闭路径均安全释放。
    struct DialogHelper : public juce::Component, public OpenTune::PitchShiftDialogContent::Listener
    {
        OpenTuneAudioProcessorEditor* owner;
        ContentKey activeContentKey;
        OpenTune::PitchShiftSettings oldSettings;
        std::shared_ptr<ContentEditCommands> commands;
        juce::Component::SafePointer<juce::Component> contentPtr;

        DialogHelper(OpenTuneAudioProcessorEditor* o, ContentKey k,
                     const OpenTune::PitchShiftSettings& s,
                     std::shared_ptr<ContentEditCommands> cmds,
                     juce::Component::SafePointer<juce::Component> c)
            : owner(o), activeContentKey(k), oldSettings(s), commands(std::move(cmds)), contentPtr(std::move(c))
        {
            // 纯托管载体：不显示、不拦截鼠标
            setVisible(false);
            setInterceptsMouseClicks(false, false);
        }

        void pitchShiftConfirmed(const OpenTune::PitchShiftSettings& newSettings) override
        {
            if (!owner) return;
            if (newSettings != oldSettings) {
                auto action = commands != nullptr
                    ? commands->commitPitchShiftEdit(activeContentKey, newSettings)
                    : nullptr;
                if (action != nullptr) {
                    owner->processorRef_.getUndoManager().addAction(std::move(action));
                }
            }
            closeDialog();
        }

        void pitchShiftReset() override
        {
            if (!owner) return;
            const auto identity = OpenTune::PitchShiftSettings::identity();
            if (identity != oldSettings) {
                auto action = commands != nullptr
                    ? commands->commitPitchShiftEdit(activeContentKey, identity)
                    : nullptr;
                if (action != nullptr) {
                    owner->processorRef_.getUndoManager().addAction(std::move(action));
                }
            }
            closeDialog();
        }

        void closeDialog()
        {
            if (contentPtr != nullptr) {
                if (auto* dw = contentPtr->findParentComponentOfClass<juce::DialogWindow>()) {
                    dw->exitModalState(0);
                }
            }
        }
    };

    auto* helper = new DialogHelper{this, activeKey, currentSettings,
                                    std::move(commands),
                                    juce::Component::SafePointer<juce::Component>(content)};
    content->addChildComponent(helper);
    content->addListener(helper);

    auto options = juce::DialogWindow::LaunchOptions();
    options.content.setOwned(content);
    options.dialogTitle = "Pitch Shift";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = this;
    pianoRoll_.grabKeyboardFocus();
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::currentToolChanged(ToolId tool)
{
    parameterPanel_.setActiveTool(static_cast<int>(tool));
}

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    AppLogger::log("AutoTune: pitchCurveEdited startFrame=" + juce::String(startFrame) + " endFrame=" + juce::String(endFrame));
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
    // 插件版无工作区/钢琴卷帘视图切换，Esc 无操作——空实现是有意为之。
}

void OpenTuneAudioProcessorEditor::overviewNavigateRequested(double visibleStartSeconds,
                                                             double pixelsPerSecond)
{
    pianoRoll_.navigateFromOverview({
        TimelineViewportRequest::Kind::Manual,
        TimelineViewportRequest::ViewKind::PianoRoll,
        visibleStartSeconds,
        0.0,  // Manual 分支不使用 currentVisibleStartSeconds
        0.0,
        pianoRoll_.timelinePolicyViewportWidth(),
        pixelsPerSecond
    });
}

OpenTuneAudioProcessorEditor::PianoRollContentSync
OpenTuneAudioProcessorEditor::syncContentProjectionToPianoRoll()
{
    if (!contentCommands_) {
        contentCommands_ = processorRef_.getContentCommands();
        pianoRoll_.addListener(this);
        pianoRoll_.setProcessor(&processorRef_);
        pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
            return processorRef_.getContentSnapshot(key);
        });
        pianoRoll_.setContentCommands(contentCommands_);
    }

    const auto sync = resolveCurrentContentSync();
    const bool identityChanged = sync.activePlacementIdentity.has_value() != presentedPlacementIdentity_.has_value()
        || (sync.activePlacementIdentity.has_value() && !(*sync.activePlacementIdentity == *presentedPlacementIdentity_));

    // Save old placement viewport before switch
    if (identityChanged)
        rememberPresentedPianoRollViewport();

    if (!sync.hasPlacements()) {
        presentedPlacementIdentity_.reset();
        // 同一 ContentKey 经空状态重新进入时强制首显
        lastResolvedScaleContentKey_ = ContentKey{};
        pianoRoll_.setTimelineContentPlacements({});
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        return sync;
    }

    if (!sync.hasActiveContent()) {
        presentedPlacementIdentity_.reset();
        // 同一 ContentKey 经空状态重新进入时强制首显
        lastResolvedScaleContentKey_ = ContentKey{};
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));
        pianoRoll_.setTimelineContentPlacements(sync.placements);
        return sync;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> syncBuffer;
    std::shared_ptr<PitchCurve> curve;
    DetectedKey detectedKey;
#if JucePlugin_Enable_ARA
    if (sync.activeContentKey.domainKind == DomainKind::ARAAudioModification) {
        if (const auto* dc = processorRef_.getDocumentController()) {
            syncBuffer = dc->readAudioBuffer(sync.activeContentKey);
            curve = dc->readPitchCurve(sync.activeContentKey);
            detectedKey = dc->readDetectedKey(sync.activeContentKey);
        }
    } else
#endif
    {
        auto snap = processorRef_.getContentSnapshot(sync.activeContentKey);
        syncBuffer = snap ? snap->audioBuffer : nullptr;
        curve = snap ? snap->pitchCurve : nullptr;
        detectedKey = snap ? snap->detectedKey : DetectedKey{};
    }

    // ARA snapshots intentionally do not carry PCM; the waveform source lives
    // in CRS and is registered by setEditedContent(). Install explicit
    // placements first so setEditedContent() cannot derive an empty placement
    // set and prune that freshly registered ARA waveform source.
    pianoRoll_.setTimelineContentPlacements(sync.placements);
    pianoRoll_.setEditedContent(sync.activeContentKey,
                                curve,
                                syncBuffer,
                                static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()));

    // Viewport restore or fit on placement switch
    if (identityChanged && sync.activePlacementIdentity.has_value()) {
        const auto savedViewport = processorRef_.readPianoRollViewport(*sync.activePlacementIdentity);
        if (savedViewport.has_value()) {
            pianoRoll_.restoreViewportState({
                {savedViewport->cameraStartSeconds, savedViewport->cameraPixelsPerSecond},
                savedViewport->pixelsPerSemitone,
                savedViewport->verticalScrollOffset
            });
        } else {
            pianoRoll_.resetUserZoomFlag();
            pianoRoll_.fitToScreen();
            if (sync.activeContentKey.domainKind == DomainKind::RegularVST3Capture)
                pianoRoll_.requestInitialF0View(sync.activeContentKey);
        }
    }

    presentedPlacementIdentity_ = sync.activePlacementIdentity;

    if (syncBuffer != nullptr) {
        const int rootNote = static_cast<int>(detectedKey.root);
        const int scaleType = OpenTune::scaleToUiScaleType(detectedKey.scale);

        // 仅当 ContentKey 或 detectedKey 映射出的 root/type 与观察基线不同才回显；
        // 基线是 content 观察值而非 UI 显示值：手动设置未持久化时不会被回读覆盖。
        if (sync.activeContentKey != lastResolvedScaleContentKey_
            || rootNote != lastResolvedScaleRootNote_
            || scaleType != lastResolvedScaleType_) {
            suppressScaleChangedCallback_ = true;
            transportBar_.setScale(rootNote, scaleType);
            suppressScaleChangedCallback_ = false;

            pianoRoll_.setScale(rootNote, scaleType);

            lastResolvedScaleContentKey_ = sync.activeContentKey;
            lastResolvedScaleRootNote_ = rootNote;
            lastResolvedScaleType_ = scaleType;
        }
    }

    return sync;
}

} // namespace OpenTune::PluginUI
