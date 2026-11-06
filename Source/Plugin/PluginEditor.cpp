#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

#include "Editor/ConfirmDialogContent.h"
#include "Editor/AboutDialogContent.h"
#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Plugin/Capture/CaptureSession.h"
#include "Utils/AppLogger.h"
#include "Utils/LocalizationManager.h"
#include "Utils/EditorUiSync.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/ScaleUiMapping.h"
#include "Utils/TimeCoordinate.h"
#include "UI/UiAssets.h"
#include "Utils/ScreenReaderAnnouncer.h"
#include "Utils/ContextMenuKey.h"

#if JucePlugin_Enable_ARA
#include "ARA/OpenTuneDocumentController.h"
#endif
#include "Content/ContentKey.h"

namespace OpenTune::PluginUI {

namespace {

#if JucePlugin_Build_AU
constexpr const char* kHostPluginFormatName = "Audio Unit";
#else
constexpr const char* kHostPluginFormatName = "VST3";
#endif

// 100% 基线窗口尺寸（§4.4）：min 660×500，max 固定 3000×2000；默认值由 AppPreferences 提供。
// VST3 外层窗口归宿主，不按 display.userArea 夹紧（§5.2）。
constexpr int kBaseMinWidth = 660;
constexpr int kBaseMinHeight = 500;
constexpr int kMaxWindowWidth = 3000;
constexpr int kMaxWindowHeight = 2000;

void showHostManagedMessage(juce::Component* parent, const juce::String& title, const juce::String& detail)
{
    AppLogger::log(juce::String(kHostPluginFormatName) + "Editor: " + title + " requested, delegated to host DAW");
#if JucePlugin_Build_AU
    const auto prefix = "In " + juce::String(kHostPluginFormatName)
        + " mode this action is managed by your DAW.\n\n";
#else
    const auto prefix = LOC(kHostManagedActionPrefix);
#endif
    OpenTune::ConfirmDialogContent::showMessage(parent,
                                                title,
                                                prefix + detail);
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
    const OpenTuneAudioProcessor& processor,
    const OpenTuneDocumentController::PlaybackRegionProjection& region)
{
    const auto snapshot = processor.getContentSnapshot(region.contentKey);
    if (snapshot == nullptr)
        return {};

    ContentTimelineProjection projection;
    projection.timelineStartSeconds = region.startInPlaybackTime;
    projection.timelineDurationSeconds = region.durationInPlaybackTime;
    projection.contentStartSeconds = region.startInModificationTime
        - snapshot->sourceWindow.sourceStartSeconds;
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
    , appPreferences_(processor.getAppPreferences())
    , languageState_(std::make_shared<LocalizationManager::LanguageState>(
          LocalizationManager::LanguageState{ appPreferences_.getState().shared.language }))
    , languageBinding_(languageState_)
    , menuBar_(processor, MenuBarComponent::Profile::Plugin)
    , topBar_(menuBar_, transportBar_)
    , pianoRoll_(processor.getPlayHeadState(), processor.getUndoManager(),
                 [this](SpectrumArray& spectrum, SpectrumArray& peaks) {
                     processorRef_.copyOutputSpectrum(spectrum, peaks);
                 },
                 false)
    , overviewStrip_(pianoRoll_.getWaveformMipmapCache())
{
    // 构造顺序固定：limits → resizable → global default size（§4.4）。
    // uiZoom 唯一真相在 Processor：构造读取一次，之后由心跳同步（§7.3）；
    // 默认尺寸不随 uiZoom 换算，min 按 uiZoom 取整、max 固定（§5.2）。
    appliedUiZoomPercent_ = processorRef_.getUiZoomPercent();
    const float uiZoomScale = static_cast<float>(appliedUiZoomPercent_) / 100.0f;
    const int minimumWidth = static_cast<int>(std::ceil(kBaseMinWidth * uiZoomScale));
    const int minimumHeight = static_cast<int>(std::ceil(kBaseMinHeight * uiZoomScale));
    setResizeLimits(minimumWidth,
                    minimumHeight,
                    kMaxWindowWidth, kMaxWindowHeight);
    setResizable(true, true);
    const auto defaultEditorSize = appPreferences_.getDefaultPluginEditorSize();
    setSize(juce::jlimit(minimumWidth, kMaxWindowWidth, defaultEditorSize.width),
            juce::jlimit(minimumHeight, kMaxWindowHeight, defaultEditorSize.height));
    lastSavedEditorSize_ = { getWidth(), getHeight() };

    UIColors::applyTheme(appPreferences_.getState().shared.theme);

    menuBar_.addListener(this);

    // Undo/Redo 菜单项实时反映撤销栈状态
    menuBar_.canUndoQuery = [this]() { return processorRef_.getUndoManager().canUndo(); };
    menuBar_.canRedoQuery = [this]() { return processorRef_.getUndoManager().canRedo(); };
    LocalizationManager::getInstance().addListener(this);

    transportBar_.addListener(this);

    // VST3 ARA layout: show record button, hide standalone transport group
    transportBar_.setLayoutProfile(TransportBarComponent::LayoutProfile::VST3AraSingleClip);

    // External Edit 按钮只在实例以 Capture Session 形式运行时可用；
    // ARA 绑定实例的 CaptureSession 为 nullptr，隐藏该按钮。
    transportBar_.setExternalEditAvailable(processorRef_.getCaptureSession() != nullptr);

    // Sync initial record button state for ARA mode.
    // Button is available once the instance's content scope is non-empty
    // (assigned playback regions, or the editor view's own selection).
    if (!processorRef_.getCaptureSession())
        transportBar_.setRecordButtonEnabled(!resolveContentScopeRegions().empty());

    // Sync initial transport state from processor
    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    transportBar_.setPositionSeconds(processorRef_.getPosition());
    transportBar_.setBpm(processorRef_.getBpm());

    // Piano key audition
    pianoRoll_.setPianoKeyAudition(&processorRef_.getPianoKeyAudition());
    pianoRoll_.setNoteAudition(&processorRef_.getNoteAudition());
    pianoRoll_.setRenderCacheProvider([this](ContentKey key) { return processorRef_.getReadableRenderCache(key); });

    // Menu popup callbacks — MenuBarComponent stays hidden, provides menu content
    // via TransportBar icon buttons (File/Edit/View)
    menuBar_.setVisible(false);

    // Accessibility: when a menu closes, Esc included, focus goes back to its button; it used
    // to be lost and NVDA said nothing. A dialog opened from the menu takes focus after this.
    const auto showTransportMenu = [this](int menuIndex, juce::Component& button) {
        const auto safeEditor = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this);
        const juce::Component::SafePointer<juce::Component> safeButton(&button);
        auto menuNames = menuBar_.getMenuBarNames();
        auto menu = menuBar_.getMenuForIndex(menuIndex, menuIndex < menuNames.size() ? menuNames[menuIndex]
                                                                                    : juce::String());
        // Accessibility: open with the first item highlighted, so the first Enter works
        // (user test: NVDA read "Options" on open, but Enter did nothing until an arrow key).
        auto options = juce::PopupMenu::Options()
                           .withTargetComponent(&button)
                           .withParentComponent(&contentRoot_);
        if (const int firstId = a11y::firstSelectableItemId(menu); firstId != 0)
            options = options.withInitiallySelectedItem(firstId);
        menu.showMenuAsync(options,
                           [safeEditor, safeButton, menuIndex](int result) {
                               if (safeEditor == nullptr)
                                   return;
                               // The menu lives inside contentRoot_ and is deleted after this
                               // callback, taking focus with it, so a focus grab here was lost
                               // (user test: silence after Esc). Do it a moment later.
                               juce::Timer::callAfterDelay(100, [safeEditor, safeButton, cancelled = result == 0] {
                                   if (safeEditor == nullptr || safeButton == nullptr || !safeButton->isShowing())
                                       return;
                                   auto* focused = juce::Component::getCurrentlyFocusedComponent();
                                   if (focused == safeButton.getComponent()) {
                                       // Focus never left the button: there is no focus event, so name it.
                                       if (cancelled)
                                           a11y::announce(*safeButton, safeButton->getTitle().isNotEmpty()
                                                                           ? safeButton->getTitle()
                                                                           : safeButton->getName());
                                       return;
                                   }
                                   // A dialog opened from the menu has focus now: leave it there.
                                   if (focused != nullptr && focused != safeEditor.getComponent()
                                       && focused != &safeEditor->contentRoot_)
                                       return;
                                   safeButton->grabKeyboardFocus();
                               });
                               if (result != 0)
                                   safeEditor->menuBar_.menuItemSelected(result, menuIndex);
                           });
    };

    transportBar_.onFileMenuRequested = [this, showTransportMenu]() {
        showTransportMenu(0, transportBar_.getFileButton());
    };

    transportBar_.onEditMenuRequested = [this, showTransportMenu]() {
        showTransportMenu(1, transportBar_.getEditButton());
    };

    transportBar_.onViewMenuRequested = [this, showTransportMenu]() {
        showTransportMenu(2, transportBar_.getViewButton());
    };

    // Accessibility: the plugin has no track panel and cannot hide the parameter panel, so the
    // Tracks and Props toggles do nothing here. They stay on screen as in the original, but leave
    // the Tab order and the screen reader tree, where they read as working "pressed" buttons.
    topBar_.setSidePanelTogglesAccessible(false);

    // The plugin's piano roll has no use for Esc: REAPER gets it and closes the FX window.
    pianoRoll_.setEscapePassesToHost(true);

    // content root 是 editor 的唯一直接子组件，承载全部主 UI（§7.3）。
    addAndMakeVisible(contentRoot_);
    contentRoot_.addAndMakeVisible(topBar_);
    contentRoot_.addAndMakeVisible(parameterPanel_);
    parameterPanel_.addListener(this);
    parameterPanel_.onPitchGridModeChanged = [this](PitchGridMode mode) {
        if (auto* handler = pianoRoll_.getToolHandler())
            handler->setPitchGridMode(mode);
    };

    contentRoot_.addAndMakeVisible(pianoRoll_);
    pianoRoll_.addListener(this);

    overviewStrip_.addListener(this);
    contentRoot_.addAndMakeVisible(overviewStrip_);

    contentRoot_.addAndMakeVisible(autoRenderOverlay_);
    autoRenderOverlay_.setVisible(false);
    contentRoot_.addAndMakeVisible(renderBadge_);
    renderBadge_.setVisible(false);

    contentCommands_ = processorRef_.getContentCommands();
    pianoRoll_.setProcessor(&processorRef_);
    pianoRoll_.setContentCommands(contentCommands_);
    pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
        return processorRef_.getContentSnapshot(key);
    });
    // EQ popup「以后不再提示」偏好直接注入（无中转层）
    pianoRoll_.setAppPreferences(&appPreferences_);
    pianoRoll_.setShortcutSettings(appPreferences_.getState().shared.shortcuts);

    applyThemeToEditor(appPreferences_.getState().shared.theme);

    onboardingOverlay_ = std::make_unique<OnboardingOverlayComponent>(
        appPreferences_, contentRoot_, topBar_, transportBar_, parameterPanel_, pianoRoll_,
        overviewStrip_, processorRef_.getCaptureSession() != nullptr
            ? OnboardingOverlayComponent::Environment::Capture
            : OnboardingOverlayComponent::Environment::Ara,
        [this](AudioEditingScheme::Scheme scheme) {
            appPreferences_.setAudioEditingScheme(scheme);
            syncSharedAppPreferences();
        },
        [this](bool) { resized(); },
        [this] { pianoRoll_.grabKeyboardFocus(); });
    contentRoot_.addChildComponent(*onboardingOverlay_);

    // Upstream 59387696 dropped the per-tick preference sync and left no call here, so a
    // new editor stayed in the default scheme: OpenDyne users got the classic mode, F1-F6
    // went to the host and Read Audio made no notes (user test). Apply the saved state once.
    syncSharedAppPreferences();

    startTimerHz(kHeartbeatHz);
    // Host transport metadata (BPM/time signature): applied on demand from the
    // heartbeat when the processor's RT-safe generation counter moves; also
    // synced once here for the initial state.
    syncHostTransportMetadata();

    const auto f0Type = appPreferences_.getState().shared.f0ModelType;
    juce::String f0InitializationError;
    if (!processorRef_.setF0ModelType(f0Type, &f0InitializationError)) {
        appPreferences_.setF0ModelType(F0ModelType::FCPE);
        ConfirmDialogContent::showDiagnostic(
            &contentRoot_, LOC_RAW("OriginalF0 model initialization failed"), f0InitializationError,
            AppLogger::makeDiagnosticText("OriginalF0 model initialization", f0InitializationError));
    }

    grabKeyboardFocus();

    // Accessibility: focus the piano roll right away so arrow keys work without Tab.
    // Deferred because the editor is not on screen yet in the constructor. The first-run
    // onboarding manages focus itself.
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    juce::Timer::callAfterDelay(150, [safeThis] {
        if (safeThis == nullptr)
            return;
        if (safeThis->onboardingOverlay_ != nullptr && safeThis->onboardingOverlay_->isActive())
            return;
        if (safeThis->pianoRoll_.isShowing())
            safeThis->pianoRoll_.grabKeyboardFocus();
    });
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    preferencesDialog_.reset();
    onboardingOverlay_.reset();
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
    const juce::Point<int> editorSize(getWidth(), getHeight());
    if (editorSizePersistenceEnabled_
        && (editorSize.x != lastSavedEditorSize_.x || editorSize.y != lastSavedEditorSize_.y)) {
        appPreferences_.setDefaultPluginEditorSize({ editorSize.x, editorSize.y });
        lastSavedEditorSize_ = editorSize;
    }

    // 外层 editor 绝不 transform；content root 按外窗尺寸/uiZoom 得到逻辑尺寸并整体缩放（§7.3）。
    const float uiZoomScale = static_cast<float>(appliedUiZoomPercent_) / 100.0f;
    contentRoot_.setBounds(0, 0,
                           static_cast<int>(std::ceil(getWidth() / uiZoomScale)),
                           static_cast<int>(std::ceil(getHeight() / uiZoomScale)));
    contentRoot_.setTransform(juce::AffineTransform::scale(uiZoomScale));

    auto bounds = contentRoot_.getLocalBounds();
    constexpr int gap = 4;

    maybeStartOnboarding();

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
        pianoRoll_.setZoomControlsArea({ 0, 0, bounds.getWidth(),
                                         overviewBottom - overviewHeight - bounds.getY() });
    }

    // Overlay 覆盖 PianoRoll 区域
    autoRenderOverlay_.setBounds(pianoRoll_.getBounds());
    if (onboardingOverlay_ == nullptr || !onboardingOverlay_->isActive())
        autoRenderOverlay_.toFront(false);

    renderBadge_.setBounds(pianoRoll_.getRight() - 148, pianoRoll_.getY() + 8, 140, 28);
    if (onboardingOverlay_ == nullptr || !onboardingOverlay_->isActive())
        renderBadge_.toFront(false);

    if (onboardingOverlay_ != nullptr)
    {
        onboardingOverlay_->updateLayout();
        if (onboardingOverlay_->isActive())
            onboardingOverlay_->toFront(false);
    }
}

void OpenTuneAudioProcessorEditor::visibilityChanged()
{
    if (!editorSizePersistenceEnabled_ && isShowing()) {
        lastSavedEditorSize_ = { getWidth(), getHeight() };
        editorSizePersistenceEnabled_ = true;
    }
    maybeStartOnboarding();
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

    if (!appPreferences_.getNoteTransposePreviewEnabled())
        processorRef_.stopNotePitchAudition();

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
    pianoRoll_.setCorrectedF0Colour(juce::Colour(visualPreferences.correctedF0Colour));
    pianoRoll_.setOriginalF0Colour(juce::Colour(visualPreferences.originalF0Colour));
    pianoRoll_.setGridStyle(sharedPreferences.gridStyle);
    pianoRoll_.setShowPianoKeyboard(visualPreferences.showPianoKeyboard);
    pianoRoll_.setScaleAssistEnabled(visualPreferences.scaleAssistEnabled);
    menuBar_.setShowPianoKeyboard(visualPreferences.showPianoKeyboard);
    menuBar_.setScaleAssistEnabled(visualPreferences.scaleAssistEnabled);
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

void OpenTuneAudioProcessorEditor::applyUiZoomIfNeeded()
{
    const auto zoomDecision = resolveEditorUiZoomDecision(processorRef_.getUiZoomPercent(),
                                                          appliedUiZoomPercent_,
                                                          kBaseMinWidth,
                                                          kBaseMinHeight);
    if (!zoomDecision.changed)
        return;

    appliedUiZoomPercent_ = zoomDecision.appliedPercent;

    // 保持当前外层窗口尺寸（宿主所有）；仅当新 min 不满足时由 constrainer 扩大（§7.1/§7.3）。
    setResizeLimits(zoomDecision.minWidth,
                    zoomDecision.minHeight,
                    kMaxWindowWidth, kMaxWindowHeight);

    resized();
    repaint();
}

void OpenTuneAudioProcessorEditor::maybeStartOnboarding()
{
    if (onboardingOverlay_ == nullptr || onboardingOverlay_->isActive())
        return;
    if (appPreferences_.getState().shared.onboardingShown)
        return;
    if (!isShowing() || getPeer() == nullptr || getWidth() <= 0 || getHeight() <= 0)
        return;
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() != 0)
        return;
    if (originalF0OverlayLatched_)
        return;
    onboardingRequested();
}

void OpenTuneAudioProcessorEditor::timerCallback()
{
    applyUiZoomIfNeeded();

#if !JucePlugin_Build_Standalone
    // Host transport metadata (BPM/time signature): one acquire load.
    // The audio thread bumps the generation counter on change (RT-safe, no
    // message posting); UI refresh happens only when it moved. Acquire pairs
    // with the release bump so the metadata stores are visible.
    const uint32_t generation = processorRef_.getHostTransportMetadataGeneration();
    if (generation != lastHostMetadataGeneration_) {
        lastHostMetadataGeneration_ = generation;
        syncHostTransportMetadata();
    }
#endif

    // Cheap predicate re-check: host may close modals without any editor-side
    // hook (visibility/resized), so keep this as a fallback trigger.
    maybeStartOnboarding();

    // External Edit 按钮可用性跟随 Capture Session 对象的存在（ARA 绑定会清空
    // CaptureSession）。心跳内低成本比对，setter 相同值时为 no-op，不触发 resized。
    transportBar_.setExternalEditAvailable(processorRef_.getCaptureSession() != nullptr);

    // Non-ARA capture state is driven by the processor's own Timer (tick()).
    // Editor only syncs UI state from the capture session.
    if (auto* session = processorRef_.getCaptureSession()) {
        // Sync record button visual state
        using OpenTune::Capture::SessionState;
        const auto captureState = session->getGlobalState();
        pianoRoll_.setTrackCaptureTint(captureState == SessionState::HasCapturing);
        if (captureState == SessionState::HasCapturing)
            transportBar_.setRecordButtonState(RecordButtonState::Capturing);
        else if (captureState == SessionState::HasProcessing)
            transportBar_.setRecordButtonState(RecordButtonState::Processing);
        else
            transportBar_.setRecordButtonState(RecordButtonState::Idle);
    }

    // External edit 返回检查由 Processor 拥有（pollReturnedExternalEdit，在
    // Processor 的 30Hz timer 内执行）：Editor 销毁（宿主关窗）不会中断进行中的
    // 外部编辑会话；损坏曲线的一次性提示也由 Processor 发出（无父组件依赖，
    // 窗口关闭后仍可见）。

    // Accessibility: the notes change without any other sign; say so. The check itself lives
    // in the processor now, so watch its counter (-1: first tick, nothing to say yet).
    const int externalEditApplied = processorRef_.getExternalEditAppliedCount();
    if (lastExternalEditAppliedCount_ >= 0 && externalEditApplied != lastExternalEditAppliedCount_)
        a11y::announce(pianoRoll_, LOC_RAW("External edit applied"), a11y::SpeechQueue::keepAll);
    lastExternalEditAppliedCount_ = externalEditApplied;

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

    // Regular VST3 capture is managed above via setRecordButtonState.
    // ARA mode: button enabled once the instance's content scope has placements.
    if (!sync.isRegularVst3Capture)
        transportBar_.setRecordButtonEnabled(sync.hasPlacements());

    // Per-tick snapshot cache: makeContentSnapshot copies notes/gaps/features,
    // so repeated reads of the same ContentKey within one tick are expensive.
    // All consumers below (overview sample rate, F0 state observation, revision
    // detection, pitch-shift indicator, F0 overlay) share this cache.
    std::map<ContentKey, std::shared_ptr<const EditableContentSnapshot>> tickSnapshotCache;
    const auto getTickSnapshot = [this, &tickSnapshotCache](ContentKey key)
        -> std::shared_ptr<const EditableContentSnapshot> {
        auto it = tickSnapshotCache.find(key);
        if (it != tickSnapshotCache.end())
            return it->second;
        auto snap = processorRef_.getContentSnapshot(key);
        tickSnapshotCache.emplace(key, std::move(snap));
        return tickSnapshotCache.find(key)->second;
    };

    // Overview update: regular capture uses multi-segment overview path;
    // ARA/other paths continue using the single-content onHeartbeatTick.
    if (pianoRoll_.isShowing()) {
        if (sync.isRegularVst3Capture)
        {
            std::vector<double> overviewSampleRates;
            overviewSampleRates.reserve(sync.placements.size());
            for (const auto& placement : sync.placements) {
                const auto snap = getTickSnapshot(placement.contentKey);
                overviewSampleRates.push_back(snap != nullptr ? snap->audioSampleRate : 0.0);
            }
            overviewStrip_.onHeartbeatTickRegular(sync.placements,
                                                  sync.timelineViewStartSeconds,
                                                  sync.timelineViewEndSeconds,
                                                  pianoRoll_.timelineCamera(),
                                                  pianoRoll_.timelinePolicyViewportWidth(),
                                                  std::move(overviewSampleRates));
        }
        else
        {
            double overviewSampleRate = 0.0;
            if (const auto snap = getTickSnapshot(pianoRoll_.editedContentKey()))
                overviewSampleRate = snap->audioSampleRate;
            overviewStrip_.onHeartbeatTick(pianoRoll_.editedContentKey(),
                                           pianoRoll_.activeContentProjection(),
                                           pianoRoll_.timelineCamera(),
                                           pianoRoll_.timelinePolicyViewportWidth(),
                                           overviewSampleRate);
        }
    }

    // Viewport save points: placement switch (syncContentProjectionToPianoRoll,
    // before identity change) and editor destructor. No per-tick save needed —
    // restore only ever reads at placement switch, right after the switch-time save.

    // Revision detection (aligned with Standalone pattern)
    const ContentKey activeKey = sync.activeContentKey;
    std::set<ContentKey> observedF0Keys;

    const auto observeOriginalF0State = [this, &getTickSnapshot, &observedF0Keys](ContentKey contentKey) {
        observedF0Keys.insert(contentKey);
        const auto snapshot = getTickSnapshot(contentKey);
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

    // Prune stale F0 observation state: keys no longer observed this tick were
    // removed from every placement/segment list (or their content was
    // destroyed) — their map entries would otherwise leak forever.
    for (auto it = lastObservedOriginalF0States_.begin();
         it != lastObservedOriginalF0States_.end();) {
        if (observedF0Keys.count(it->first) != 0)
            ++it;
        else
            it = lastObservedOriginalF0States_.erase(it);
    }

    auto f0FailureEvents = processorRef_.takeOriginalF0FailureEvents();
#if JucePlugin_Enable_ARA
    if (auto* dc = processorRef_.getDocumentController()) {
        auto araEvents = dc->takeOriginalF0FailureEvents();
        f0FailureEvents.insert(f0FailureEvents.end(),
                               std::make_move_iterator(araEvents.begin()),
                               std::make_move_iterator(araEvents.end()));
    }
#endif
    for (const auto& event : f0FailureEvents) {
        const auto reason = event.reason.isNotEmpty()
            ? event.reason
            : LOC(kOriginalF0FailureReasonUnavailable);
        const auto summary = Loc::format(
            LOC(kOriginalF0FailureDetail),
            juce::String(static_cast<juce::int64>(event.contentKey.objectId)),
            getDomainKindName(event.contentKey.domainKind), reason);
        OpenTune::ConfirmDialogContent::showDiagnostic(
            &contentRoot_, LOC(kOriginalF0FailureTitle), summary,
            AppLogger::makeDiagnosticText(
                "OriginalF0/FCPE domain="
                    + juce::String(getDomainKindName(event.contentKey.domainKind))
                    + " contentKey="
                    + juce::String(static_cast<juce::int64>(event.contentKey.objectId)),
                reason));
    }

    // Content 切换检测
    bool contentJustSwitched = false;
    if (activeKey != lastRevisionObservedContentKey_) {
        lastRevisionObservedContentKey_ = activeKey;
        contentJustSwitched = true;

        // 同步到当前 snapshot 的 revision baseline（与 Standalone 对齐）
        if (activeKey.isValid()) {
            auto snap = getTickSnapshot(activeKey);
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
        auto snap = getTickSnapshot(activeKey);
        const auto revisionPulse = resolveContentRevisionPulse(snap ? snap->notesRevision : 0,
                                                               snap ? snap->timeGridRevision : 0,
                                                               snap ? snap->pitchRevision : 0,
                                                               lastPianoRollNotesRevision_,
                                                               lastPianoRollTimeGridRevision_,
                                                               lastPianoRollPitchRevision_);

        if (revisionPulse.notesChanged)
            pianoRoll_.onNotesRevisionChanged();

        if (revisionPulse.timeGridChanged)
            pianoRoll_.onTimeGridRevisionChanged();

        if (revisionPulse.pitchChanged)
            pianoRoll_.onPitchRevisionChanged();

        lastPianoRollNotesRevision_ = revisionPulse.nextNotesRevision;
        lastPianoRollTimeGridRevision_ = revisionPulse.nextTimeGridRevision;
        lastPianoRollPitchRevision_ = revisionPulse.nextPitchRevision;
    }

    // Pitch shift indicator: single source of truth is active content snapshot
    PitchShiftSettings currentPitchShift = PitchShiftSettings::identity();
    if (activeKey.isValid()) {
        if (auto snap = getTickSnapshot(activeKey))
            currentPitchShift = snap->pitchShiftSettings;
    }
    if (currentPitchShift != lastPitchShiftIndicatorSettings_) {
        lastPitchShiftIndicatorSettings_ = currentPitchShift;
        parameterPanel_.setPitchShiftIndicator(currentPitchShift.semitone, currentPitchShift.cents);
    }

    // Original F0 overlay：读取音频后的 F0 提取 + note 生成期间显示"正在处理音频"遮罩
    if (originalF0OverlayLatched_) {
        bool allDone = true;
        for (const auto& key : originalF0OverlayTargetContentKeys_) {
            auto snap = getTickSnapshot(key);
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
            // Accessibility: sighted users see the "Processing audio" overlay disappear and notes
            // appear. Announce the result: how many notes were found or that analysis failed.
            int noteCount = 0;
            bool failed = false;
            for (const auto& key : originalF0OverlayTargetContentKeys_) {
                if (auto snap = processorRef_.getContentSnapshot(key)) {
                    failed = failed || snap->originalF0State == OriginalF0State::Failed;
                    noteCount += static_cast<int>(snap->notes.size());
                }
            }
            a11y::announce(pianoRoll_,
                           failed ? LOC_RAW("Audio analysis failed")
                                  : Loc::format(LOC_RAW("Audio read, notes: {0}"), juce::String(noteCount)),
                           a11y::SpeechQueue::keepAll);

            originalF0OverlayLatched_ = false;
            originalF0OverlayTargetContentKeys_.clear();
            // F0 overlay no longer blocks onboarding; modal windows may still be
            // open (the guard inside maybeStartOnboarding covers that).
            maybeStartOnboarding();
        }
    }

    bool shouldShowBadge = false;
    const auto chunkStats = processorRef_.getReadableContentChunkStats(activeKey);
    if (chunkStats.hasActiveWork()) {
        renderBadge_.setMessageText(LOC_RAW("Rendering (")
            + juce::String(chunkStats.idle + chunkStats.blank) + "/" + juce::String(chunkStats.total()) + ")");
        shouldShowBadge = true;
    }

    bool shouldShowOverlay = false;
    if (originalF0OverlayLatched_) {
        autoRenderOverlay_.setMessageText(LOC_RAW("Processing audio"));
        shouldShowOverlay = true;
    }

    // Accessibility: sighted users see the "Rendering" badge go away. Say when a render that took
    // a while is done, so it is clear playback now has the correction. Short renders stay silent
    // so this does not talk over the note speech after every key press.
    if (shouldShowBadge && !renderBadgeWasShown_) {
        renderStartedMs_ = juce::Time::getMillisecondCounter();
    } else if (!shouldShowBadge && renderBadgeWasShown_ && !shouldShowOverlay
               && juce::Time::getMillisecondCounter() - renderStartedMs_ >= kRenderDoneSpeechMinMs) {
        a11y::announce(pianoRoll_, LOC_RAW("Rendering done"), a11y::SpeechQueue::keepAll);
    }
    renderBadgeWasShown_ = shouldShowBadge;

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

    // Timeline display mode sync from preferences
    const auto prefMode = appPreferences_.getTimelineDisplayMode();
    if (prefMode != timelineDisplayMode_) {
        timelineDisplayMode_ = prefMode;
        transportBar_.setTimelineDisplayMode(timelineDisplayMode_);
        pianoRoll_.setTimelineDisplayMode(timelineDisplayMode_);
    }
}

// =========================================================================
// syncHostTransportMetadata �?host transport metadata (BPM / time signature)
// applied to the UI. Called from the heartbeat when the processor's RT-safe
// generation counter moved (audio thread bumps it on change; never posts
// messages, no locks/allocations on the real-time path).
// =========================================================================

void OpenTuneAudioProcessorEditor::syncHostTransportMetadata()
{
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
    pianoRoll_.refreshLocalizedText();

    repaint();
    if (onboardingOverlay_ != nullptr)
        onboardingOverlay_->refreshLanguage();
}

void OpenTuneAudioProcessorEditor::onboardingRequested()
{
    if (onboardingOverlay_ != nullptr && !onboardingOverlay_->isActive())
        onboardingOverlay_->start();
}

ContentKey OpenTuneAudioProcessorEditor::resolveCurrentContentKey()
{
    return resolveCurrentContentSync().activeContentKey;
}

std::vector<juce::ARAPlaybackRegion*>
OpenTuneAudioProcessorEditor::resolveContentScopeRegions() const
{
#if JucePlugin_Enable_ARA
    // ARA 角色分派：PlaybackRenderer 实例使用宿主分配的 regions；
    // 只有 EditorView 的实例使用宿主选择（直接选择，其次有效选择）。
    // 选择命中分配列表时排到首位，作为当前编辑目标。
    auto* view = getARAEditorView();
    auto selectedRegions = view != nullptr
        ? view->getViewSelection().getPlaybackRegions<juce::ARAPlaybackRegion>()
        : std::vector<juce::ARAPlaybackRegion*>{};
    if (selectedRegions.empty() && view != nullptr)
        selectedRegions = view->getViewSelection().getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();

    auto regions = processorRef_.getAssignedPlaybackRegions();
    if (regions.empty())
        return selectedRegions;

    if (!selectedRegions.empty())
    {
        const auto focused = std::find(regions.begin(), regions.end(), selectedRegions.front());
        if (focused != regions.end())
            std::rotate(regions.begin(), focused, focused + 1);
    }
    return regions;
#else
    return {};
#endif
}

void OpenTuneAudioProcessorEditor::readScopeAudio()
{
#if JucePlugin_Enable_ARA
    auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr)
        return;

    const auto projections = dc->getPlaybackRegionProjectionsFor(resolveContentScopeRegions());
    if (projections.empty() || projections.front().playbackRegion == nullptr)
        return;

    const auto& target = projections.front();
    const int refreshed = dc->requestReadAudioForPlaybackRegion(target.playbackRegion);
    if (refreshed < 0) {
        AppLogger::log("ReadAudio: auto-read failed");
        ++autoReadFailureCount_;
        return;
    }
    if (refreshed == 0)
        return;

    autoReadFailureCount_ = 0;
    AppLogger::log("ReadAudio: refreshed " + juce::String(refreshed)
        + " AudioModification(s) for scoped playback region");

    syncContentProjectionToPianoRoll();

    if (!target.contentKey.isValid())
        return;
    originalF0OverlayTargetContentKeys_ = { target.contentKey };
    lastObservedOriginalF0States_[target.contentKey] = OriginalF0State::NotRequested;
    if (pianoRoll_.isOpenDyne())
        pendingNoteGenerationOnReady_[target.contentKey] = pianoRoll_.getCurrentAutoTuneParams();
    else
        pendingNoteGenerationOnReady_.erase(target.contentKey);
    originalF0OverlayLatched_ = true;
#endif
}

OpenTuneAudioProcessorEditor::PianoRollContentSync
OpenTuneAudioProcessorEditor::resolveCurrentContentSync()
{
    PianoRollContentSync sync;

#if JucePlugin_Enable_ARA
    if (const auto* dc = processorRef_.getDocumentController()) {
        const auto projections = dc->getPlaybackRegionProjectionsFor(resolveContentScopeRegions());
        processorRef_.setNotePitchAuditionTargetRegion(
            projections.empty() ? nullptr : projections.front().playbackRegion);
        for (const auto& region : projections) {
            if (!region.contentKey.isValid())
                continue;
            const auto projection = makePianoRollLocalProjection(processorRef_, region);
            if (!projection.isValid())
                continue;
            sync.placements.push_back(makePlacement(region.contentKey, projection,
                region.displayColour.value_or(UIColors::noteBlock)));
        }

        if (sync.placements.empty())
            return sync;

        sync.activeContentKey = sync.placements.front().contentKey;
        sync.activePlacementIdentity = PianoRollPlacementIdentity{
            sync.activeContentKey, sync.placements.front().projection};
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
    if (onboardingOverlay_ != nullptr && onboardingOverlay_->isActive())
        return true;

    return handleEditorShortcut(key);
}

bool OpenTuneAudioProcessorEditor::handleEditorShortcut(const juce::KeyPress& key)
{
    const int keyCode = key.getKeyCode();
    const bool unmodifiedArrow = !key.getModifiers().isAnyModifierKeyDown()
        && (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey
            || keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey);
    if (unmodifiedArrow && pianoRoll_.hasKeyboardFocus(true))
        return pianoRoll_.keyPressed(key);

    if (pianoRoll_.handleViewportShortcut(key))
        return true;

    const auto& shortcutSettings = appPreferences_.getState().shared.shortcuts;

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Undo, key)) {
        undoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::Redo, key)) {
        redoRequested();
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
    announceParameterEdit(AudioEditingScheme::ParameterId::RetuneSpeed, result);
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoDepth, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoDepth, value);
    }
    announceParameterEdit(AudioEditingScheme::ParameterId::VibratoDepth, result);
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoRate, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoRate, value);
    }
    announceParameterEdit(AudioEditingScheme::ParameterId::VibratoRate, result);
}

void OpenTuneAudioProcessorEditor::noteSplitChanged(float value)
{
    // NoTarget 时 editParameter 内部已回落更新创建默认值
    const auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::NoteSplit, value);
    announceParameterEdit(AudioEditingScheme::ParameterId::NoteSplit, result);
}

void OpenTuneAudioProcessorEditor::announceParameterEdit(AudioEditingScheme::ParameterId id,
                                                          const AudioEditingScheme::ParameterEditResult& result)
{
    // Only while a panel control has focus (keyboard use). Arrow changes arrive here once the
    // keys stop (AccessibleKnob), typed values and Backspace at once.
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused == nullptr || !parameterPanel_.isParentOf(focused))
        return;
    const auto text = pianoRoll_.describeParameterEdit(id, result);
    if (text.isNotEmpty())
        a11y::announce(*focused, text, a11y::SpeechQueue::keepAll);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::Eraser)) {
        return;
    }

    const auto tool = static_cast<ToolId>(toolId);
    pianoRoll_.setCurrentTool(tool);
}

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
#if JucePlugin_Build_AU
    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                "Import Audio",
                                                "Please import audio from your DAW in "
                                                    + juce::String(kHostPluginFormatName) + " mode.");
#else
    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                 LOC(kImportAudioDialog),
                                                 LOC(kVst3ImportAudio));
#endif
}

void OpenTuneAudioProcessorEditor::exportAudioRequested(MenuBarComponent::ExportType exportType)
{
    juce::ignoreUnused(exportType);
#if JucePlugin_Build_AU
    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                "Export Audio",
                                                "Please render/export from your DAW in "
                                                    + juce::String(kHostPluginFormatName) + " mode.");
#else
    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                 LOC(kExportAudio),
                                                 LOC(kVst3ExportAudio));
#endif
}

void OpenTuneAudioProcessorEditor::openProjectRequested()
{
    showHostManagedMessage(&contentRoot_, LOC(kOpenProjectDialog),
                           LOC(kProjectFileManagementStandalone));
}

void OpenTuneAudioProcessorEditor::saveProjectRequested()
{
    showHostManagedMessage(&contentRoot_, LOC(kSaveProject),
                           LOC(kProjectFileManagementStandalone));
}

void OpenTuneAudioProcessorEditor::saveProjectAsRequested()
{
    showHostManagedMessage(&contentRoot_, LOC(kSaveProjectAs),
                           LOC(kProjectFileManagementStandalone));
}

void OpenTuneAudioProcessorEditor::openRecentProjectRequested(const juce::File&)
{
    showHostManagedMessage(&contentRoot_, LOC(kOpenRecentProject),
                           LOC(kProjectFileManagementStandalone));
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
    if (preferencesDialog_ != nullptr)
        return;

    auto pages = SharedPreferencePages::create(
        appPreferences_, [this] { syncSharedAppPreferences(); }, true);

    // Insert Audio page (with rendering priority) at the beginning
    auto onVocoderModelWeightChanged = [this](VocoderModelWeight weight) {
        processorRef_.setVocoderModelWeight(weight);
    };
    auto onF0ModelChanged = [this](F0ModelType type) {
        juce::String reason;
        if (processorRef_.setF0ModelType(type, &reason))
            return true;
        ConfirmDialogContent::showDiagnostic(
            &contentRoot_, LOC_RAW("OriginalF0 model initialization failed"), reason,
            AppLogger::makeDiagnosticText("OriginalF0 model initialization", reason));
        return false;
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
        [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this)] {
            if (safeThis != nullptr)
                safeThis->onboardingRequested();
        },
        true);
    pages.insert(pages.begin(), { LOC(kAudio), std::move(audioPage.component), audioPage.height });

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));

    // 根据当前屏幕可用区域计算对话框尺寸，适配不同显示器和分辨率
    const auto usable = getParentMonitorArea();
    const int maxW = juce::jmin(640, usable.getWidth() - 48);
    const int maxH = juce::jmin(560, usable.getHeight() - 48);
    dialogContent->setSize(juce::jmax(480, maxW), juce::jmax(480, maxH));

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = LOC(kPreferences);
    options.componentToCentreAround = &contentRoot_;
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.useBottomRightCornerResizer = true;
    // 确保对话框打开前 PianoRoll 持有焦点，JUCE 模态管理器会在关闭时自动恢复
    pianoRoll_.grabKeyboardFocus();
    preferencesDialog_.reset(options.create());

    const auto safeEditor = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this);
    const auto safeDialog = juce::Component::SafePointer<juce::DialogWindow>(preferencesDialog_.get());
    preferencesDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeEditor, safeDialog](int) {
            if (safeEditor != nullptr
                && safeDialog != nullptr
                && safeEditor->preferencesDialog_.get() == safeDialog.getComponent())
                safeEditor->preferencesDialog_.reset();
        }),
        false);
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
#if JucePlugin_Build_AU
    showHostManagedMessage(&contentRoot_, "Help",
                           "Open the host DAW plugin help/manual entry for "
                               + juce::String(kHostPluginFormatName) + " usage guidance.");
#else
    showHostManagedMessage(&contentRoot_, LOC(kHelpDialog),
                           LOC(kVst3Help));
#endif
}

void OpenTuneAudioProcessorEditor::aboutRequested()
{
    AboutDialogContent::show(&contentRoot_);
}

void OpenTuneAudioProcessorEditor::showWaveformToggled(bool shouldShow)
{
    pianoRoll_.setShowWaveform(shouldShow);
}

void OpenTuneAudioProcessorEditor::showPianoKeyboardToggled(bool shouldShow)
{
    appPreferences_.setShowPianoKeyboard(shouldShow);
    syncSharedAppPreferences();
}

void OpenTuneAudioProcessorEditor::scaleAssistToggled(bool enabled)
{
    appPreferences_.setScaleAssistEnabled(enabled);
    syncSharedAppPreferences();
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

    // Install process-wide default so independent DialogWindow title bars and
    // ordinary JUCE controls use the project theme.
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
    // Accessibility: sighted users see what came back; tell screen readers which action was undone.
    if (auto* action = processorRef_.getUndoManager().undo()) {
        syncContentProjectionToPianoRoll();
        a11y::announce(pianoRoll_, Loc::format(LOC_RAW("Undone: {0}"), action->getDescription()));
    } else {
        a11y::announce(pianoRoll_, LOC_RAW("Nothing to undo"));
    }
}

void OpenTuneAudioProcessorEditor::redoRequested()
{
    if (auto* action = processorRef_.getUndoManager().redo()) {
        syncContentProjectionToPianoRoll();
        a11y::announce(pianoRoll_, Loc::format(LOC_RAW("Redone: {0}"), action->getDescription()));
    } else {
        a11y::announce(pianoRoll_, LOC_RAW("Nothing to redo"));
    }
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
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
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
    // VST3/ARA: loop 归宿主所有，插件不发送任何 transport 请求。
    juce::ignoreUnused(enabled);
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



void OpenTuneAudioProcessorEditor::externalEditRequested()
{
    // 会话状态与启动委托给 Processor（Editor 销毁不清会话）。
    const auto activeKey = resolveCurrentContentKey();
    const auto snapshot = processorRef_.getContentSnapshot(activeKey);
    if (processorRef_.externalEditLaunchRequested(activeKey, snapshot ? *snapshot : EditableContentSnapshot{})) {
        syncContentProjectionToPianoRoll();
        return;
    }

    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                LOC(kExternalEdit),
                                                snapshot != nullptr && snapshot->audioBuffer != nullptr
                                                    && snapshot->audioBuffer->getNumSamples() > 0
                                                    ? LOC(kExternalEditUnavailable)
                                                    : LOC(kExternalEditNeedsAudio));
}

void OpenTuneAudioProcessorEditor::recordRequested()
{
    if (auto* session = processorRef_.getCaptureSession()) {
        AppLogger::log("VST3 recordRequested mode=regular-vst3 processor="
            + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_)));

        // 录音语义：已存在 Capturing 段时本次请求停止录音；否则创建一个新 take。
        using OpenTune::Capture::SessionState;
        if (session->getGlobalState() == SessionState::HasCapturing) {
            session->stopCapture();
            AppLogger::log("VST3 Capture: take stopped");
        } else if (session->armNewCapture()) {
            AppLogger::log("VST3 Capture: take armed");
        }
        return;
    }

#if !JucePlugin_Enable_ARA
    OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
#if JucePlugin_Build_AU
                                                "Read Audio",
                                                "This " + juce::String(kHostPluginFormatName)
                                                    + " instance is not ready for audio capture or ARA reading.");
#else
                                                LOC(kTooltipRecord),
                                                LOC(kVst3ReadAudioNotReady));
#endif
    return;
#else
    auto* dc = processorRef_.getDocumentController();

    AppLogger::log("VST3 recordRequested mode=ara-bound processor="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(&processorRef_))
        + " dc=" + juce::String::toHexString(reinterpret_cast<uintptr_t>(dc)));

    const auto scopedProjections = dc->getPlaybackRegionProjectionsFor(resolveContentScopeRegions());
    if (scopedProjections.empty() || scopedProjections.front().playbackRegion == nullptr)
    {
        AppLogger::log("VST3 recordRequested mode=ara-bound scoped region unavailable");
        transportBar_.setRecordButtonEnabled(false);
        OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                    LOC(kTooltipRecord),
                                                    LOC(kVst3ReadAudioSelectionNotReady));
        return;
    }

    const auto targetPlaybackRegion = scopedProjections.front().playbackRegion;
    dc->requestReadAudioForPlaybackRegionAsync(
        targetPlaybackRegion,
        [this, dc, targetPlaybackRegion](int refreshed) {
            if (refreshed < 0) {
                OpenTune::ConfirmDialogContent::showMessage(&contentRoot_,
                                                            LOC(kTooltipRecord),
                                                            LOC(kVst3ReadAudioRegionsFailed));
                return;
            }

            if (refreshed == 0) {
                // The audio is read but OpenDyne has no notes (the auto read ran in the wrong
                // scheme, or its notes were removed): pressing Read again was a dead end
                // (user test), so make the notes from the existing pitch, as the first read does.
                const auto projections = dc->getPlaybackRegionProjectionsFor(
                    std::vector<juce::ARAPlaybackRegion*>{targetPlaybackRegion});
                if (pianoRoll_.isOpenDyne() && !projections.empty()
                    && projections.front().contentKey.isValid()) {
                    const auto key = projections.front().contentKey;
                    const auto snap = processorRef_.getContentSnapshot(key);
                    if (snap != nullptr && snap->originalF0State == OriginalF0State::Ready
                        && snap->notes.empty()
                        && contentCommands_->generateNotesOnly(key, pianoRoll_.getCurrentAutoTuneParams())) {
                        syncContentProjectionToPianoRoll();
                        const auto after = processorRef_.getContentSnapshot(key);
                        a11y::announce(pianoRoll_, Loc::format(LOC_RAW("Notes made: {0}"),
                            juce::String(after != nullptr ? static_cast<int>(after->notes.size()) : 0)));
                        return;
                    }
                }
                // This audio was already read and is unchanged: nothing visible happens, and without
                // a message it is unclear whether the button worked.
                a11y::announce(pianoRoll_, LOC_RAW("Audio already read"));
                return;
            }

            AppLogger::log("ReadAudio: refreshed " + juce::String(refreshed)
                + " AudioModification(s) for scoped playback region");

            // ARA Read 成功且已清旧 curve 后，立即同步投影，让旧 F0 不等待 timer 才消失
            syncContentProjectionToPianoRoll();

            // 遮罩只覆盖本次读取的 focused modification。
            originalF0OverlayTargetContentKeys_.clear();
            const auto targetProjections = dc->getPlaybackRegionProjectionsFor(
                std::vector<juce::ARAPlaybackRegion*>{targetPlaybackRegion});
            if (targetProjections.empty() || !targetProjections.front().contentKey.isValid())
                return;

            const auto targetKey = targetProjections.front().contentKey;
            originalF0OverlayTargetContentKeys_.push_back(targetKey);
            // F0 状态机基线重置：防止 F0 完成早于首次 timer 观察导致跳变丢失
            lastObservedOriginalF0States_[targetKey] = OriginalF0State::NotRequested;
            // 捕获 OpenDyne 一次性音符生成意图，F0 Ready 跳变时消费
            if (pianoRoll_.isOpenDyne())
                pendingNoteGenerationOnReady_[targetKey] = pianoRoll_.getCurrentAutoTuneParams();
            else
                pendingNoteGenerationOnReady_.erase(targetKey);
            originalF0OverlayLatched_ = true;
            a11y::announce(pianoRoll_, LOC_RAW("Processing audio"), a11y::SpeechQueue::keepAll);
        });
#endif
}

bool OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController())
        return docController->requestCommitPlaybackPosition(timeSeconds);
#endif
    // Hosts without a playback controller keep the in-plugin edit cursor.
    // Do not call setPosition() and do not surface a status warning: the
    // DAW playhead stays where the user left it.
    juce::ignoreUnused(timeSeconds);
    return false;
}

bool OpenTuneAudioProcessorEditor::playheadPositionPreviewRequested(double timeSeconds)
{
#if JucePlugin_Enable_ARA
    if (auto* docController = processorRef_.getDocumentController())
        return docController->requestSetPlaybackPosition(timeSeconds);
#endif
    juce::ignoreUnused(timeSeconds);
    return false;
}

void OpenTuneAudioProcessorEditor::playPauseToggleRequested()
{
}

void OpenTuneAudioProcessorEditor::stopPlaybackRequested()
{
}

bool OpenTuneAudioProcessorEditor::pointerShortcutRequested(KeyShortcutConfig::ShortcutId id)
{
    switch (id) {
        case KeyShortcutConfig::ShortcutId::Undo:
            undoRequested();
            return true;
        case KeyShortcutConfig::ShortcutId::Redo:
            redoRequested();
            return true;
        default:
            return false;
    }
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    const auto activeKey = resolveCurrentContentKey();
    AppLogger::log("AutoTune: vst3 request contentKey.objectId=" + juce::String(static_cast<juce::int64>(activeKey.objectId)));
    if (!activeKey.isValid()) {
        OpenTune::ConfirmDialogContent::showMessage(
            &contentRoot_,
            LOC(kAutoDialog),
            LOC(kAutoNeedsActiveAra));
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
        OpenTune::ConfirmDialogContent::showMessage(
            &contentRoot_,
            LOC(kOriginalF0),
            LOC(kOriginalF0Extracting));
        return;
    }

    if (f0State == OriginalF0State::Failed) {
        auto reason = processorRef_.getContentSnapshot(activeKey);
        juce::String failureReason = reason != nullptr
            ? juce::String(reason->originalF0FailureReason)
            : juce::String{};
#if JucePlugin_Enable_ARA
        if (failureReason.isEmpty())
            if (auto* dc = processorRef_.getDocumentController())
                failureReason = juce::String(dc->readOriginalF0FailureReason(activeKey));
#endif
        if (failureReason.isEmpty())
            failureReason = "OriginalF0 failure detail is unavailable at the content owner";
        OpenTune::ConfirmDialogContent::showMessage(
            &contentRoot_,
            LOC(kOriginalF0),
            LOC(kOriginalF0ExtractionFailed) + "\n\nReason: " + failureReason);
        return;
    }

    if (f0State != OriginalF0State::Ready) {
        OpenTune::ConfirmDialogContent::showMessage(
            &contentRoot_,
            LOC(kOriginalF0),
            LOC(kOriginalF0NotReadyForClip));
        return;
    }

    const auto result = pianoRoll_.applyAutoTuneToSelection();
    AppLogger::log("AutoTune: vst3 apply result=" + juce::String(result.applied() ? "true" : "false")
        + " message=" + result.message());
    if (!result.applied()) {
        // NoChange = 最终修正已达成：静默，不弹窗
        if (result.status != PianoRollComponent::AutoTuneApplyStatus::NoChange) {
            OpenTune::ConfirmDialogContent::showMessage(
                &contentRoot_,
                LOC(kAutoDialog),
                result.message());
        } else {
            // No dialog by design, but without sight the button seemed to do nothing.
            a11y::announce(pianoRoll_, LOC_RAW("Already corrected"));
        }
        return;
    }
    a11y::announce(pianoRoll_, LOC_RAW("AUTO correction applied"));

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
    content->setDialogParent(&contentRoot_);

    auto commands = getContentCommandsShared();
    // 直接使用 content 的确认/重置回调提交 undo；PitchShiftDialogContent 在回调后自行
    // closeParentDialog()，不额外创建非拥有的生命周期 helper（避免裸组件泄漏）。
    content->setOnConfirm([safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this),
                           activeKey, currentSettings, commands](const OpenTune::PitchShiftSettings& newSettings) {
        if (safeThis == nullptr) return;
        if (newSettings != currentSettings) {
            auto action = commands != nullptr
                ? commands->commitPitchShiftEdit(activeKey, newSettings)
                : nullptr;
            if (action != nullptr) {
                safeThis->processorRef_.getUndoManager().addAction(std::move(action));
            }
        }
    });

    content->setOnReset([safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this),
                         activeKey, currentSettings, commands]() {
        if (safeThis == nullptr) return;
        const auto identity = OpenTune::PitchShiftSettings::identity();
        if (identity != currentSettings) {
            auto action = commands != nullptr
                ? commands->commitPitchShiftEdit(activeKey, identity)
                : nullptr;
            if (action != nullptr) {
                safeThis->processorRef_.getUndoManager().addAction(std::move(action));
            }
        }
    });

    auto options = juce::DialogWindow::LaunchOptions();
    options.content.setOwned(content);
    options.dialogTitle = LOC(kPitchShift);
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = &contentRoot_;
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
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        lastPianoRollSampleRate_ = 0;
        pianoRoll_.setTimelineContentPlacements({});
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()),
                                    identityChanged);
        return sync;
    }

    if (!sync.hasActiveContent()) {
        presentedPlacementIdentity_.reset();
        // 同一 ContentKey 经空状态重新进入时强制首显
        lastResolvedScaleContentKey_ = ContentKey{};
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        lastPianoRollSampleRate_ = 0;
        pianoRoll_.setEditedContent(ContentKey{},
                                    nullptr,
                                    nullptr,
                                    static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate()),
                                    identityChanged);
        pianoRoll_.setTimelineContentPlacements(sync.placements);
        return sync;
    }

    std::shared_ptr<const juce::AudioBuffer<float>> syncBuffer;
    std::shared_ptr<const PitchCurveSnapshot> curveSnapshot;
    DetectedKey detectedKey;
#if JucePlugin_Enable_ARA
    if (sync.activeContentKey.domainKind == DomainKind::ARAAudioModification) {
        if (const auto* dc = processorRef_.getDocumentController()) {
            syncBuffer = dc->readAudioBuffer(sync.activeContentKey);
            curveSnapshot = dc->readPitchCurve(sync.activeContentKey);
            detectedKey = dc->readDetectedKey(sync.activeContentKey);
        }
    } else
#endif
    {
        auto snap = processorRef_.getContentSnapshot(sync.activeContentKey);
        syncBuffer = snap ? snap->audioBuffer : nullptr;
        curveSnapshot = snap ? snap->pitchCurve : nullptr;
        detectedKey = snap ? snap->detectedKey : DetectedKey{};
    }

    const int syncSampleRate = static_cast<int>(OpenTuneAudioProcessor::getStoredAudioSampleRate());

    // ARA snapshots intentionally do not carry PCM; the waveform source lives
    // in CRS and is registered by setEditedContent(). Install explicit
    // placements first so setEditedContent() cannot derive an empty placement
    // set and prune that freshly registered ARA waveform source.
    pianoRoll_.setTimelineContentPlacements(sync.placements);

    // 稳态 tick 不重建可变曲线：仅 identity/snapshot/buffer/sample rate 变化时提交。
    if (identityChanged
        || sync.activeContentKey != lastPianoRollContentKey_
        || curveSnapshot != lastPianoRollCurve_
        || syncBuffer != lastPianoRollBuffer_
        || syncSampleRate != lastPianoRollSampleRate_) {
        pianoRoll_.setEditedContent(sync.activeContentKey,
                                    curveSnapshot,
                                    syncBuffer,
                                    syncSampleRate,
                                    identityChanged);
        lastPianoRollContentKey_ = sync.activeContentKey;
        lastPianoRollCurve_ = curveSnapshot;
        lastPianoRollBuffer_ = syncBuffer;
        lastPianoRollSampleRate_ = syncSampleRate;
    }

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

    // First-insert auto Read: run once when the instance scope has placements.
    // Set the flag before the call so the recursive sync from the Read callback
    // cannot trigger a second read.
    if (!sync.isRegularVst3Capture && sync.hasPlacements()
        && !autoReadAttempted_ && autoReadFailureCount_ < kMaxAutoReadFailures) {
        autoReadAttempted_ = true;
        readScopeAudio();
    }

    return sync;
}

} // namespace OpenTune::PluginUI
