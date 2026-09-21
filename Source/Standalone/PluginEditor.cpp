#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "PluginEditor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/UIColors.h"
#include "UI/UiAssets.h"
#include "Editor/Preferences/SharedPreferencePages.h"
#include "Editor/Preferences/StandalonePreferencePages.h"
#include "Editor/Preferences/TabbedPreferencesDialog.h"
#include "Audio/AudioFormatRegistry.h"
#include "Audio/AsyncAudioLoader.h"
#include "StandaloneArrangementHelpers.h"
#include "Utils/ProjectSession.h"
#include "Utils/PitchCurve.h"
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/AppLogger.h"
#include "Utils/EditorUiSync.h"
#include "Utils/ParameterPanelSync.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PitchShiftEditAction.h"
#include "Editor/PitchShiftDialogContent.h"
#include "Editor/ConfirmDialogContent.h"
#include "StandaloneAudioDeviceSync.h"
#include "Utils/TimeCoordinate.h"
#include "Content/StandaloneClipContent.h"
#include "Utils/KeyShortcutConfig.h"
#include "Utils/PlacementActions.h"
#include "DSP/ReferenceFeatures.h"
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <future>
#include <chrono>

namespace OpenTune {

namespace {

constexpr int kHeartbeatHzIdle = 30;
constexpr int kHeartbeatHzInferenceActive = 10;

// 100% 基线窗口尺寸（§4.4）：min 900×500，max 固定 3000×2000，preferred 1200×900。
// 实际外层窗口尺寸在挂入默认 StandaloneFilterWindow 后按 settings 恢复/约束（§5.1）。
constexpr int kBaseMinWidth = 900;
constexpr int kBaseMinHeight = 500;
constexpr int kMaxWindowWidth = 3000;
constexpr int kMaxWindowHeight = 2000;
constexpr int kPreferredWidth = 1200;
constexpr int kPreferredHeight = 900;

// 主题截图验证钩子：仅当设置 OPENTUNE_THEME 环境变量时覆盖配置主题（截图工具用），
// 未设置时返回配置值，行为与之前完全一致。
ThemeId resolveEffectiveTheme(ThemeId configured)
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable("OPENTUNE_THEME", {});
    if (env == "overdose")     return ThemeId::Overdose;
    if (env == "aurora")       return ThemeId::Aurora;
    if (env == "bluebreeze")   return ThemeId::BlueBreeze;
    if (env == "darkbluegrey") return ThemeId::DarkBlueGrey;
    return configured;
}

ContentTimelineProjection makePianoRollProjection(const StandaloneArrangement::Placement& placement,
                                                          OpenTuneAudioProcessor& processor)
{
    ContentTimelineProjection projection;
    projection.timelineStartSeconds = placement.timelineStartSeconds;
    projection.timelineDurationSeconds = placement.durationSeconds;
    auto snap = processor.getContentSnapshot(placement.contentKey);
    const double sourceStartSeconds = placement.clipInSeconds;
    const double sourceEndSeconds = sourceStartSeconds + placement.durationSeconds;
    if (snap != nullptr) {
        projection.contentStartSeconds = snap->timeGrid->tauForward(sourceStartSeconds);
        projection.contentDurationSeconds = snap->timeGrid->tauForward(sourceEndSeconds)
            - projection.contentStartSeconds;
    } else {
        projection.contentStartSeconds = sourceStartSeconds;
        projection.contentDurationSeconds = placement.durationSeconds;
    }
    return projection;
}



static juce::String getImportWildcardFilter()
{
    return AudioFormatRegistry::getImportWildcardFilter();
}

static juce::String getImportExtensionSpec()
{
    const auto wildcard = getImportWildcardFilter();
    juce::StringArray tokens;
    tokens.addTokens(wildcard, ";", "\"");

    juce::StringArray extensions;
    for (auto token : tokens)
    {
        token = token.trim();
        if (token.startsWith("*."))
            token = token.fromFirstOccurrenceOf("*.", false, false);
        token = token.toLowerCase();
        if (token.isNotEmpty())
            extensions.addIfNotAlreadyThere(token);
    }

    return extensions.joinIntoString(";");
}


juce::String renderStatusToString(RenderStatus status)
{
    switch (status) {
        case RenderStatus::Idle: return "idle";
        case RenderStatus::Rendering: return "rendering";
        case RenderStatus::Ready: return "ready";
    }

    return "unknown";
}

// 轨道随机颜色策略：四组权重 Vivid 40% / Muted 30% / 色盲安全 20% / 受约束任意色相 10%
// 深色 DAW 完整不透明色，每组内随机取色，与当前色相同时取同组下一色（无重抽循环）
static juce::Colour pickTrackRandomColor(juce::Colour current)
{
    auto& rng = juce::Random::getSystemRandom();

    // Vivid 组 (40%)：高饱和鲜明色，DAW 深色背景上对比强烈
    static const juce::Colour vivid[] = {
        juce::Colour(0xFFE8543B), juce::Colour(0xFFE89A2D),
        juce::Colour(0xFFA8C93C), juce::Colour(0xFF3DC46B),
        juce::Colour(0xFF35C4C9), juce::Colour(0xFF4E8FE8),
        juce::Colour(0xFFB06CE8), juce::Colour(0xFFE868A8),
    };

    // Muted 组 (30%)：降饱和柔色，不抢视觉焦点
    static const juce::Colour muted[] = {
        juce::Colour(0xFFD97A70), juce::Colour(0xFFD3AD71),
        juce::Colour(0xFF9BC48D), juce::Colour(0xFF85A5C2),
        juce::Colour(0xFFA595B5),
    };

    // 色盲安全组 (20%)：deuteranopia/protanopia 可区分色
    static const juce::Colour cbsafe[] = {
        juce::Colour(0xFFE8A33D), juce::Colour(0xFF66C4F0),
        juce::Colour(0xFF00C68F), juce::Colour(0xFF3D8FD6),
        juce::Colour(0xFFE06A3D), juce::Colour(0xFFD080B0),
    };

    // 从固定数组中取色：随机索引，与当前色完全相同时取下一色
    const auto pickFrom = [&](const juce::Colour* arr, int n) -> juce::Colour {
        int i = rng.nextInt(n);
        if (arr[i] == current)
            i = (i + 1) % n;
        return arr[i];
    };

    // 权重滚轮：[0,40) Vivid / [40,70) Muted / [70,90) CBSafe / [90,100) Arbitrary
    const int roll = rng.nextInt(100);

    if (roll < 40)
        return pickFrom(vivid, 8);
    if (roll < 70)
        return pickFrom(muted, 5);
    if (roll < 90)
        return pickFrom(cbsafe, 6);

    // 受约束任意色相 (10%)：连续均匀 hue [0,1)，S/V 独立随机
    // S [0.55,0.85) 保对比，V [0.50,0.70) 保深色 DAW 可读
    // 黄色段 hue≈0.153-0.208（55°-75°）压低 value 避免刺眼
    constexpr float kYellowLo = 55.0f / 360.0f;  // ≈0.153
    constexpr float kYellowHi = 75.0f / 360.0f;  // ≈0.208
    float h = rng.nextFloat();
    float s = 0.55f + rng.nextFloat() * 0.30f;
    float v = (h >= kYellowLo && h < kYellowHi)
              ? 0.45f + rng.nextFloat() * 0.10f   // 黄色段 V [0.45,0.55)
              : 0.50f + rng.nextFloat() * 0.20f;  // 其余   V [0.50,0.70)
    auto c = juce::Colour::fromHSV(h, s, v, 1.0f);
    if (c == current) {
        h = std::fmod(h + 0.5f, 1.0f);
        s = 0.55f + rng.nextFloat() * 0.30f;
        v = (h >= kYellowLo && h < kYellowHi)
            ? 0.45f + rng.nextFloat() * 0.10f
            : 0.50f + rng.nextFloat() * 0.20f;
        c = juce::Colour::fromHSV(h, s, v, 1.0f);
    }
    return c;
}

} // namespace

#if JUCE_DEBUG
static bool runDebugSelfTests(OpenTuneAudioProcessor& processor, juce::AudioProcessorEditor& editor) {
    {
        PitchCurve curve;
        curve.setHopSize(160);
        curve.setSampleRate(16000);
        std::vector<float> f0(200, 440.0f);
        std::vector<float> energy(200, 1.0f);
        curve.setOriginalF0(f0);
        curve.setOriginalEnergy(energy);
        constexpr int kHopSize = 160;
        constexpr double kF0SampleRate = 16000.0;
        NoteGeneratorParams params;
        params.policy.transitionThresholdCents = 512.0f;
        params.policy.minDurationMs = 100.0f;
        auto notes = LegacyNoteGenerator::generate(
            f0.data(), static_cast<int>(f0.size()),
            energy.size() == f0.size() ? energy.data() : nullptr,
            0, static_cast<int>(f0.size()),
            kHopSize, kF0SampleRate, params);
        if (notes.empty()) {
            return false;
        }
        // LegacyNoteGenerator extends the tail by whole-frame steps derived from tailExtendMs.
        const double hopSecs = 160.0 / 16000.0;
        const double baseEndSeconds = static_cast<double>(f0.size()) * hopSecs;
        const double tailExtendSeconds = (std::ceil(params.policy.tailExtendMs / 1000.0 / hopSecs)) * hopSecs;
        const double expectedEndSeconds = baseEndSeconds + tailExtendSeconds;
        if (std::abs(notes[0].endTime - expectedEndSeconds) > 480.0 / 44100.0) {
            return false;
        }
    }

    {
        const float zoom = 1.5f;  // 变换坐标自检：非 1.0 倍率覆盖缩放路径
        juce::Component root;
        juce::Component child;
        root.setBounds(0, 0, 1600, 1000);
        root.setTransform(juce::AffineTransform::scale(zoom));
        root.addAndMakeVisible(child);
        child.setBounds(200, 120, 240, 160);

        const auto childPoint = juce::Point<float>(24.0f, 36.0f);
        const auto rootPoint = root.getLocalPoint(&child, childPoint);
        if (root.getComponentAt(rootPoint.roundToInt()) != &child)
            return false;

        const auto globalPoint = child.localPointToGlobal(childPoint);
        const auto roundTripPoint = child.getLocalPoint(nullptr, globalPoint);
        if (!juce::approximatelyEqual(roundTripPoint.x, childPoint.x)
            || !juce::approximatelyEqual(roundTripPoint.y, childPoint.y))
            return false;

        const auto globalArea = child.localAreaToGlobal(
            juce::Rectangle<float>(0.0f, 0.0f, 80.0f, 40.0f));
        const auto roundTripArea = child.getLocalArea(nullptr, globalArea);
        if (!juce::approximatelyEqual(roundTripArea.getWidth(), 80.0f)
            || !juce::approximatelyEqual(roundTripArea.getHeight(), 40.0f))
            return false;
    }

    {
        ParameterPanel panel;
        panel.setSize(240, 320);
        auto* viewport = dynamic_cast<juce::Viewport*>(panel.getChildComponent(0));
        if (viewport == nullptr || viewport->getViewedComponent() == nullptr)
            return false;

        auto* content = viewport->getViewedComponent();
        if (content->getHeight() <= viewport->getMaximumVisibleHeight()
            || !viewport->getVerticalScrollBar().isVisible())
            return false;

        viewport->setViewPositionProportionately(0.0, 1.0);
        if (viewport->getViewPositionY()
            != content->getHeight() - viewport->getMaximumVisibleHeight())
            return false;

        panel.setSize(240, 1200);
        if (content->getHeight() > viewport->getMaximumVisibleHeight()
            || viewport->getVerticalScrollBar().isVisible())
            return false;
    }

    // --- TransportBar 生产阈值：两个 LayoutProfile 的 Full/Compact/Overflow 边界与槽位不越界 ---
    {
        using TransportBar = TransportBarComponent;
        const auto sameTierLayout = [](const TransportBar::TierLayout& a,
                                       const TransportBar::TierLayout& b) {
            return a.tier == b.tier
                && a.requiredWidth == b.requiredWidth
                && a.fileButton == b.fileButton
                && a.editButton == b.editButton
                && a.viewButton == b.viewButton
                && a.moreButton == b.moreButton
                && a.playButton == b.playButton
                && a.pauseButton == b.pauseButton
                && a.stopButton == b.stopButton
                && a.loopButton == b.loopButton
                && a.recordButton == b.recordButton
                && a.trackViewButton == b.trackViewButton
                && a.pianoViewButton == b.pianoViewButton
                && a.timeDisplay == b.timeDisplay
                && a.bpmField == b.bpmField
                && a.tapButton == b.tapButton
                && a.scaleRootSelector == b.scaleRootSelector
                && a.scaleTypeSelector == b.scaleTypeSelector;
        };
        const auto slotsWithinBounds = [](const TransportBar::TierLayout& layout,
                                          juce::Rectangle<int> bounds) {
            const juce::Rectangle<int> slots[] = {
                layout.fileButton, layout.editButton, layout.viewButton, layout.moreButton,
                layout.playButton, layout.pauseButton, layout.stopButton, layout.loopButton,
                layout.recordButton, layout.trackViewButton, layout.pianoViewButton,
                layout.timeDisplay, layout.bpmField, layout.tapButton,
                layout.scaleRootSelector, layout.scaleTypeSelector
            };
            for (const auto& slot : slots)
                if (!slot.isEmpty() && !bounds.contains(slot))
                    return false;
            return true;
        };

        const juce::Rectangle<int> fullBounds(0, 0, 4096, 64);
        for (const auto profile : { TransportBar::LayoutProfile::StandaloneFull,
                                    TransportBar::LayoutProfile::VST3AraSingleClip }) {
            const auto full = TransportBar::computeTierLayout(profile, fullBounds);
            if (full.tier != TransportBar::LayoutTier::Full
                || !sameTierLayout(full, TransportBar::computeTierLayout(profile, fullBounds))
                || !slotsWithinBounds(full, fullBounds))
                return false;

            const juce::Rectangle<int> compactBounds(0, 0, full.requiredWidth - 1, 64);
            const auto compact = TransportBar::computeTierLayout(profile, compactBounds);
            if (compact.tier != TransportBar::LayoutTier::Compact
                || !sameTierLayout(compact, TransportBar::computeTierLayout(profile, compactBounds))
                || !slotsWithinBounds(compact, compactBounds))
                return false;

            const juce::Rectangle<int> overflowBounds(0, 0, compact.requiredWidth - 1, 64);
            const auto overflow = TransportBar::computeTierLayout(profile, overflowBounds);
            if (overflow.tier != TransportBar::LayoutTier::Overflow
                || !sameTierLayout(overflow, TransportBar::computeTierLayout(profile, overflowBounds))
                || !slotsWithinBounds(overflow, overflowBounds))
                return false;
        }
    }

    // --- OTSS v3 roundtrip + v2 migration（走生产 get/setStateInformation） ---
    {
        juce::MemoryBlock originalOtss;
        processor.getStateInformation(originalOtss);

        processor.setStateInformation(originalOtss.getData(), static_cast<int>(originalOtss.getSize()));
        juce::MemoryBlock otssRoundTrip;
        processor.getStateInformation(otssRoundTrip);
        if (otssRoundTrip != originalOtss)
            return false;

        const double recordedBpm = processor.getBpm();
        const int recordedNumerator = processor.getTimeSigNumerator();
        const int recordedDenominator = processor.getTimeSigDenominator();
        const int recordedTrackHeight = processor.getTrackHeight();

        processor.setUiZoomPercent(150);

        // v2 完整 36 字节：magic 从原 v3 载荷读取（不复制 version）；
        // bpm/num/den/trackHeight 直接复制原字节；v3 的 int zoom 槽换成任意 legacy double。
        juce::MemoryBlock v2Payload;
        {
            const auto* originalBytes = static_cast<const char*>(originalOtss.getData());
            juce::MemoryOutputStream output(v2Payload, false);
            output.write(originalBytes, 4);       // magic
            output.writeInt(2);                   // legacy v2
            output.write(originalBytes + 8, 16);  // bpm(double) + num(int) + den(int)
            output.writeDouble(123.75);           // legacy timeline zoom, discarded on load
            output.write(originalBytes + 28, 4);  // trackHeight
        }

        processor.setStateInformation(v2Payload.getData(), static_cast<int>(v2Payload.getSize()));
        const bool v2MigrationOk = processor.getUiZoomPercent() == 100
            && processor.getBpm() == recordedBpm
            && processor.getTimeSigNumerator() == recordedNumerator
            && processor.getTimeSigDenominator() == recordedDenominator
            && processor.getTrackHeight() == recordedTrackHeight;

        // 无论迁移断言结果如何：立即恢复原 v3 载荷并确认输出与原字节相等，之后才返回失败。
        processor.setStateInformation(originalOtss.getData(), static_cast<int>(originalOtss.getSize()));
        juce::MemoryBlock otssRestored;
        processor.getStateInformation(otssRestored);
        if (otssRestored != originalOtss || !v2MigrationOk)
            return false;
    }

    // --- 六档 ui zoom 的 min/max limits（与生产相同的 float scale + ceil） ---
    {
        const juce::Rectangle<int> originalEditorBounds = editor.getBounds();
        auto* constrainer = editor.getConstrainer();
        bool zoomLimitsOk = true;
        constexpr int kZoomPercents[] = { 75, 90, 100, 110, 125, 150 };
        for (const int zoomPercent : kZoomPercents) {
            const float uiZoomScale = static_cast<float>(zoomPercent) / 100.0f;
            const int expectedMinWidth = static_cast<int>(std::ceil(kBaseMinWidth * uiZoomScale));
            const int expectedMinHeight = static_cast<int>(std::ceil(kBaseMinHeight * uiZoomScale));
            editor.setResizeLimits(expectedMinWidth, expectedMinHeight, kMaxWindowWidth, kMaxWindowHeight);
            zoomLimitsOk = zoomLimitsOk
                && constrainer->getMinimumWidth() == expectedMinWidth
                && constrainer->getMinimumHeight() == expectedMinHeight
                && constrainer->getMaximumWidth() == kMaxWindowWidth
                && constrainer->getMaximumHeight() == kMaxWindowHeight;
        }

        // 恢复 processor 当前档 limits 与测试前的 editor bounds。
        const float currentScale = static_cast<float>(processor.getUiZoomPercent()) / 100.0f;
        editor.setResizeLimits(static_cast<int>(std::ceil(kBaseMinWidth * currentScale)),
                               static_cast<int>(std::ceil(kBaseMinHeight * currentScale)),
                               kMaxWindowWidth, kMaxWindowHeight);
        editor.setBounds(originalEditorBounds);
        if (!zoomLimitsOk)
            return false;
    }

    return true;
}
#endif

DetectedKey OpenTuneAudioProcessorEditor::resolveScaleForPlacementContent(int trackId,
                                                                                  int placementIndex,
                                                                                  juce::String* sourceOut) const
{
    const auto defaultKey = []() {
        DetectedKey k;  // C Major, 0.0 confidence, origin=Unset —— 无检测信息的兜底显示值
        return k;
    };

    const bool hasPlacement = (trackId >= 0
                            && trackId < OpenTuneAudioProcessor::MAX_TRACKS
                            && placementIndex >= 0
                            && placementIndex < getStandalonePlacementCount(processorRef_, trackId));

    const ContentKey contentKey = hasPlacement ? getStandaloneContentKey(processorRef_, trackId, placementIndex) : ContentKey{};
    if (contentKey.isValid()) {
        auto snap = processorRef_.getContentSnapshot(contentKey);
        const DetectedKey detectedKey = snap ? snap->detectedKey : DetectedKey{};
        if (detectedKey.origin != Origin::Unset) {
            if (sourceOut) *sourceOut = "content";
            return detectedKey;
        }
    }

    if (sourceOut) *sourceOut = "default";
    return defaultKey();
}

void OpenTuneAudioProcessorEditor::applyScaleToUi(int rootNote, int scaleType)
{
    const int clampedRoot = juce::jlimit(0, 11, rootNote);
    const int clampedType = juce::jlimit(1, 8, scaleType);

    suppressScaleChangedCallback_ = true;
    transportBar_.setScale(clampedRoot, clampedType);
    suppressScaleChangedCallback_ = false;

    pianoRoll_.setScale(clampedRoot, clampedType);
}

void OpenTuneAudioProcessorEditor::applyResolvedScaleForPlacementContent(int trackId, int placementIndex)
{
    juce::String source;
    const DetectedKey key = resolveScaleForPlacementContent(trackId, placementIndex, &source);
    const int rootNote = static_cast<int>(key.root);
    const int scaleType = OpenTune::scaleToUiScaleType(key.scale);
    applyScaleToUi(rootNote, scaleType);
    // content→UI 回显的唯一入口：同步观察基线，timer 轮询以此为比较基准
    lastResolvedScaleRootNote_ = rootNote;
    lastResolvedScaleType_ = scaleType;

    const ContentKey contentKey = (trackId >= 0 && placementIndex >= 0)
        ? getStandaloneContentKey(processorRef_, trackId, placementIndex)
        : ContentKey{};
    juce::ignoreUnused(contentKey);
    DBG("ScaleSyncTrace: source=" + source
        + " trackId=" + juce::String(trackId)
        + " placementIndex=" + juce::String(placementIndex)
        + " contentKey=" + juce::String(static_cast<juce::int64>(contentKey.objectId))
        + " root=" + juce::String(rootNote)
        + " scale=" + juce::String(scaleType));
}

void OpenTuneAudioProcessorEditor::setInferenceActive(bool active)
{
    if (inferenceActive_ == active)
        return;

    inferenceActive_ = active;
    inferenceActiveTickCounter_ = 0;

    startTimerHz(inferenceActive_ ? kHeartbeatHzInferenceActive : kHeartbeatHzIdle);
    arrangementView_.setInferenceActive(inferenceActive_);
    pianoRoll_.setInferenceActive(inferenceActive_);
    trackPanel_.setInferenceActive(inferenceActive_);
}

OpenTuneAudioProcessorEditor::OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , processorRef_(p)
    , languageState_(std::make_shared<LocalizationManager::LanguageState>(
          LocalizationManager::LanguageState{ appPreferences_.getState().shared.language }))
    , languageBinding_(languageState_)
    , menuBar_(p, MenuBarComponent::Profile::Standalone)
    , topBar_(menuBar_, transportBar_)
    , arrangementView_(p)
    , pianoRoll_(p.getPlayHeadState(), p.getUndoManager(),
                 [this](SpectrumArray& spectrum, SpectrumArray& peaks) {
                     processorRef_.copyOutputSpectrum(spectrum, peaks);
                 })
    , overviewStrip_(pianoRoll_.getWaveformMipmapCache())
    , projectSession_(p, appPreferences_)
{
    // Wire AppPreferences to processor for getSnapSettings()
    processorRef_.setAppPreferences(&appPreferences_);
    // EQ popup「以后不再提示」偏好直接注入（无中转层）
    pianoRoll_.setAppPreferences(&appPreferences_);

    // Initialize track volumes array
    lastTrackVolumes_.fill(1.0f);
    
    // Hide original menu bar as we moved it to TransportBar
    menuBar_.setVisible(false);

    // 构造顺序固定：limits → resizable → preferred size（§4.4）。
    // uiZoom 唯一真相在 Processor：构造读取一次，之后由心跳同步（§7.3）。
    appliedUiZoomPercent_ = processorRef_.getUiZoomPercent();
    const float uiZoomScale = static_cast<float>(appliedUiZoomPercent_) / 100.0f;
    const int minimumWidth = static_cast<int>(std::ceil(kBaseMinWidth * uiZoomScale));
    const int minimumHeight = static_cast<int>(std::ceil(kBaseMinHeight * uiZoomScale));
    setResizeLimits(minimumWidth,
                    minimumHeight,
                    kMaxWindowWidth, kMaxWindowHeight);
    setResizable(true, true);
    setSize(juce::jmax(kPreferredWidth, minimumWidth),
            juce::jmax(kPreferredHeight, minimumHeight));

    UIColors::applyTheme(resolveEffectiveTheme(appPreferences_.getState().shared.theme));

    // Create Tech Cursor
    juce::Image cursorImg(juce::Image::ARGB, 32, 32, true);
    juce::Graphics g(cursorImg);
    g.setColour(UIColors::accent);
    g.drawLine(16.0f, 4.0f, 16.0f, 28.0f, 2.0f);
    g.drawLine(4.0f, 16.0f, 28.0f, 16.0f, 2.0f);
    g.drawEllipse(10.0f, 10.0f, 12.0f, 12.0f, 2.0f);
    g.fillEllipse(14.0f, 14.0f, 4.0f, 4.0f);
    techCursor_ = juce::MouseCursor(cursorImg, 16, 16);
    
    // Setup Menu Bar
    menuBar_.addListener(this);

    // Undo/Redo 菜单项实时反映撤销栈状态
    menuBar_.canUndoQuery = [this]() { return processorRef_.getUndoManager().canUndo(); };
    menuBar_.canRedoQuery = [this]() { return processorRef_.getUndoManager().canRedo(); };

#if JUCE_MAC
    // Populate the macOS system menu bar with File/Edit/View menus.
    // JUCE automatically adds "About OpenTune" and "Quit OpenTune" to the app menu.
    juce::MenuBarModel::setMacMainMenu(&menuBar_);
#endif

    // Register language change listener
    LocalizationManager::getInstance().addListener(this);

    // Setup Transport Bar Menu Callbacks
// menuName obtained at runtime (auto-reflects after language switch), matched by getMenuForIndex index
    transportBar_.onFileMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(0, menuNames.isEmpty() ? juce::String() : menuNames[0]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getFileButton())
                                                     .withParentComponent(&contentRoot_),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 0);
                           });
    };
    transportBar_.onEditMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(1, menuNames.size() > 1 ? menuNames[1] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getEditButton())
                                                     .withParentComponent(&contentRoot_),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 1);
                           });
    };
    transportBar_.onViewMenuRequested = [this]() {
        auto menuNames = menuBar_.getMenuBarNames();
        juce::PopupMenu menu = menuBar_.getMenuForIndex(2, menuNames.size() > 2 ? menuNames[2] : juce::String());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&transportBar_.getViewButton())
                                                     .withParentComponent(&contentRoot_),
                           [this](int result) {
                               if (result != 0) menuBar_.menuItemSelected(result, 2);
                           });
    };

#if JUCE_DEBUG
    static std::atomic<bool> ran{ false };
    if (!ran.exchange(true)) {
        const bool ok = runDebugSelfTests(processorRef_, *this);
        if (!ok) {
            AppLogger::log("Debug self-tests failed");
            jassertfalse;
        }
        const auto selfTestEnv = juce::SystemStats::getEnvironmentVariable("OPENTUNE_SELFTEST", {});
        if (selfTestEnv == "1") {
            std::exit(ok ? 0 : 1);
        }
    }
#endif

    // Setup Transport Bar (includes Scale controls)
    transportBar_.addListener(this);
    transportBar_.setPlaying(processorRef_.isPlaying());
    transportBar_.setLoopEnabled(processorRef_.isLoopEnabled());
    transportBar_.setBpm(processorRef_.getBpm());

    // Initialize Scale (content > recent > default)
    {
        const int initTrack = getStandaloneActiveTrack(processorRef_);
        const int initPlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, initTrack);
        applyResolvedScaleForPlacementContent(initTrack, initPlacementIndex);
    }

    // content root 是 editor 的唯一直接子组件，承载全部主 UI（§7.3）。
    addAndMakeVisible(contentRoot_);
    contentRoot_.addAndMakeVisible(topBar_);

// Top bar: side panel collapse toggle
    topBar_.onToggleTrackPanel = [this]() {
        isTrackPanelVisible_ = !isTrackPanelVisible_;
        trackPanel_.setVisible(isTrackPanelVisible_);
        topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
        resized();
        repaint();
    };

    topBar_.onToggleParameterPanel = [this]() {
        isParameterPanelVisible_ = !isParameterPanelVisible_;
        parameterPanel_.setVisible(isParameterPanelVisible_);
        topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
        resized();
        repaint();
    };

    topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);

    trackPanel_.addListener(this);
    trackPanel_.setActiveTrack(getStandaloneActiveTrack(processorRef_));
// Initialize track heights (synced with ArrangementView)
    trackPanel_.setTrackHeight(processorRef_.getTrackHeight());
// Initialize state for all 32 tracks
    for (int i = 0; i < MAX_TRACKS; ++i)
    {
        trackPanel_.setTrackMuted(i, getStandaloneTrackMuted(processorRef_, i));
        trackPanel_.setTrackSolo(i, getStandaloneTrackSolo(processorRef_, i));
        trackPanel_.setTrackVolume(i, getStandaloneTrackVolume(processorRef_, i));
    }
    // Initialize track colors
    syncTrackColorsToPanel();
    trackPanel_.setTrackColorMode(appPreferences_.getTrackColorMode());
    menuBar_.setTrackColorMode(appPreferences_.getTrackColorMode());
    contentRoot_.addAndMakeVisible(trackPanel_);

    // Setup Parameter Panel
    parameterPanel_.addListener(this);
    // Pitch Grid 全局开关 → PianoRollToolHandler 吸附模式
    parameterPanel_.onPitchGridModeChanged = [this](PitchGridMode mode) {
        if (auto* handler = pianoRoll_.getToolHandler())
            handler->setPitchGridMode(mode);
    };
    // parameterPanel_.setRetuneSpeed(processorRef_.getRetuneSpeed());
    parameterPanel_.setRetuneSpeed(PitchControlConfig::kDefaultRetuneSpeedPercent);
    pianoRoll_.setRetuneSpeed(PitchControlConfig::kDefaultRetuneSpeedNormalized);

    contentRoot_.addAndMakeVisible(parameterPanel_);

    arrangementView_.addListener(this);
    contentRoot_.addAndMakeVisible(arrangementView_);
    overviewStrip_.addListener(this);
    // Initial sync: track panel visible track count 鈫?arrangement view
    arrangementView_.setVisibleTrackCount(trackPanel_.getVisibleTrackCount());

    // Setup Piano Roll (main editor area)
    pianoRoll_.addListener(this);
    pianoRoll_.setReadContentSnapshot([this](ContentKey key) {
        return processorRef_.getContentSnapshot(key);
    });
    pianoRoll_.setContentCommands(processorRef_.getContentCommands());
    pianoRoll_.setPianoKeyAudition(&processorRef_.getPianoKeyAudition());
    {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(activeTrack, placementIndex) : 0;
        applyPlacementSelectionContext(activeTrack, placementId);
    }
    pianoRoll_.setBpm(processorRef_.getBpm());
    pianoRoll_.setTimeSignature(processorRef_.getTimeSigNumerator(), processorRef_.getTimeSigDenominator());
    pianoRoll_.setShowWaveform(processorRef_.getShowWaveform());
    pianoRoll_.setShowPianoKeyboard(appPreferences_.getState().shared.pianoRollVisualPreferences.showPianoKeyboard);
    pianoRoll_.setScaleAssistEnabled(appPreferences_.getState().shared.pianoRollVisualPreferences.scaleAssistEnabled);
    pianoRoll_.setGridStyle(appPreferences_.getState().shared.gridStyle);
    
    // PianoRoll and ArrangementView read presented position from processor-owned
    // PlayHeadState via getPresentedPositionSeconds(); no second forwarding path needed.
    
    contentRoot_.addAndMakeVisible(pianoRoll_);
    contentRoot_.addAndMakeVisible(overviewStrip_);
    pianoRoll_.setVisible(!isWorkspaceView_);
    arrangementView_.setVisible(isWorkspaceView_);
    overviewStrip_.setVisible(!isWorkspaceView_);
    
    // Each view drives its own camera from presented position; no shared camera echo.
    
    // Add AutoRenderOverlay (initially hidden, covers PianoRoll during AUTO)
    contentRoot_.addAndMakeVisible(autoRenderOverlay_);
    autoRenderOverlay_.setVisible(false);

    contentRoot_.addAndMakeVisible(renderBadge_);
    renderBadge_.setVisible(false);

    // Ensure initial focus
    if (isWorkspaceView_)
        arrangementView_.grabKeyboardFocus();
    else
        pianoRoll_.grabKeyboardFocus();

    // Add Ripple Overlay (Topmost)
    contentRoot_.addAndMakeVisible(rippleOverlay_);
    // No need for setAlwaysOnTop on component level, we handle z-order in resized

    applyThemeToEditor(resolveEffectiveTheme(appPreferences_.getState().shared.theme));

    // Apply the purple theme to the window
    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, UIColors::backgroundDark);

// Playhead render via VBlank overlay; main editor heartbeat reduced to 30Hz to ease message thread pressure
    startTimerHz(kHeartbeatHzIdle);

// Playhead position read from shared projection — each component calls getPresentedPositionSeconds()

    // Hide the standalone "Options" button and Mute Warning if running in standalone mode
    juce::Timer::callAfterDelay(50, [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this)]() {
        if (safeThis == nullptr) return;
        if (auto* topLevel = safeThis->getTopLevelComponent())
        {
            // 2. Hide Options Button & Notification
            for (auto* child : topLevel->getChildren())
            {
                if (auto* button = dynamic_cast<juce::Button*>(child))
                {
                    if (button->getButtonText().trim().equalsIgnoreCase("Options"))
                    {
                        button->setVisible(false);
                    }
                }
                
                // Try to find the Notification Component
                if (auto* label = dynamic_cast<juce::Label*>(child))
                {
                     if (label->getText().containsIgnoreCase("Audio input is muted"))
                         label->getParentComponent()->setVisible(false);
                }
            }
            
            topLevel->repaint();
        }
    });

    syncSharedAppPreferences();

    // Backend reset is asynchronous; F0 model setup must follow its completion
    // while the render worker is still paused.
    const auto sharedPreferences = appPreferences_.getState().shared;
    const auto f0Type = sharedPreferences.f0ModelType;
    auto applyF0Model = [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this), f0Type]() {
        if (safeThis == nullptr)
            return;
        if (!safeThis->processorRef_.setF0ModelType(f0Type))
            safeThis->appPreferences_.setF0ModelType(F0ModelType::FCPE);
    };

    if (sharedPreferences.renderingPriority == RenderingPriority::CpuFirst)
        processorRef_.resetInferenceBackend(true, std::move(applyF0Model));
    else
        applyF0Model();
}

OpenTuneAudioProcessorEditor::~OpenTuneAudioProcessorEditor()
{
    // 退出保存外层窗口实际 width/height（§5.1）：此刻默认 StandaloneFilterWindow 仍存活，
    // windowX/windowY 由该窗口析构函数继续写入同一 OpenTune.settings。
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        if (auto* window = dynamic_cast<juce::StandaloneFilterWindow*>(getTopLevelComponent()))
            if (auto* props = holder->settings.get())
            {
                props->setValue("windowWidth", window->getWidth());
                props->setValue("windowHeight", window->getHeight());
            }

        restoreAsioSampleRateToSystem(holder->deviceManager, "editor exit");
    }

    // Stop timer
#if JUCE_MAC
    // Clear the macOS system menu bar before menuBar_ is destroyed.
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif

    stopTimer();

    // Ensure import/deferred background tasks are fully completed
    // before tearing down UI/listeners to avoid lifetime races.
    waitForBackgroundUiTasks();

    // Safely join export worker thread if it exists
    if (exportWorker_.joinable())
    {
        exportWorker_.join();
    }

    projectWorkerPool_.removeAllJobs(false, -1);

    // Remove custom LookAndFeel
    setLookAndFeel(nullptr);

    // Remove language change listener
    LocalizationManager::getInstance().removeListener(this);
    menuBar_.removeListener(this);

    transportBar_.removeListener(this);
    trackPanel_.removeListener(this);
    arrangementView_.removeListener(this);
    overviewStrip_.removeListener(this);
    parameterPanel_.removeListener(this);
    pianoRoll_.removeListener(this);
}

void OpenTuneAudioProcessorEditor::launchBackgroundUiTask(std::function<void()> task)
{
    if (!task)
        return;

    for (auto it = backgroundTasks_.begin(); it != backgroundTasks_.end();)
    {
        if (it->valid() && it->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try { it->get(); } catch (...) { AppLogger::error("[PluginEditor] Background task unknown exception"); }
            it = backgroundTasks_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    backgroundTasks_.emplace_back(
        std::async(std::launch::async, [task = std::move(task)]() mutable { task(); }));
}

void OpenTuneAudioProcessorEditor::waitForBackgroundUiTasks()
{
    std::vector<std::future<void>> pending;
    pending.swap(backgroundTasks_);

    for (auto& future : pending)
    {
        if (!future.valid())
            continue;

        try { future.get(); } catch (...) { AppLogger::error("[PluginEditor] Wait for background task unknown exception"); }
    }
}

// ============================================================
// Project save/open: message-thread state, single-worker I/O
// ============================================================

bool OpenTuneAudioProcessorEditor::rejectProjectOperationIfBusy()
{
    if (!projectOperationBusy_)
        return false;

    ConfirmDialogContent::showMessage(
        &contentRoot_,
        juce::String::fromUTF8(u8"\u5DE5\u7A0B\u64CD\u4F5C"),
        juce::String::fromUTF8(u8"\u53E6\u4E00\u4E2A\u5DE5\u7A0B\u64CD\u4F5C\u6B63\u5728\u8FDB\u884C\u4E2D\u3002"));
    return true;
}

void OpenTuneAudioProcessorEditor::saveProject(juce::File targetFile,
                                               bool openChooserAfterSave,
                                               juce::File openAfterSave)
{
    if (rejectProjectOperationIfBusy())
        return;

    projectOperationBusy_ = true;
    const auto path = targetFile != juce::File{}
        ? targetFile
        : projectSession_.getCurrentProjectFile();
    auto task = projectSession_.prepareSave(path);
    const uint64_t generation = projectSession_.getDirtyGeneration();

    projectWorkerPool_.addJob(
        [safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this),
         task = std::move(task), generation, path,
         openChooserAfterSave,
         openAfterSave = std::move(openAfterSave)]() mutable {
        auto result = ProjectSession::executeSaveToFile(task);
        juce::MessageManager::callAsync(
            [safeThis, result, generation, path,
             openChooserAfterSave,
             openAfterSave = std::move(openAfterSave)]() mutable {
            if (safeThis == nullptr)
                return;

            if (!result.ok()) {
                ConfirmDialogContent::launch(
                    new ConfirmDialogContent(
                        juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B\u5931\u8D25"),
                        result.error().fullMessage(),
                        { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                    &safeThis->contentRoot_);
                safeThis->projectOperationBusy_ = false;
                return;
            }

            safeThis->projectSession_.setCurrentProjectFile(path);
            if (safeThis->projectSession_.getDirtyGeneration() == generation)
                safeThis->projectSession_.clearDirty();
            safeThis->projectSession_.pushRecentProject(path);
            safeThis->syncRecentProjectsToMenu();
            safeThis->updateTitleWithProjectPath();

            if (openAfterSave != juce::File{}) {
                safeThis->startOpenProject(openAfterSave);
                return;
            }

            if (openChooserAfterSave)
                safeThis->launchOpenProjectChooser();
            safeThis->projectOperationBusy_ = false;
        });
    });
}

void OpenTuneAudioProcessorEditor::openProjectFile(const juce::File& file)
{
    if (rejectProjectOperationIfBusy())
        return;

    projectOperationBusy_ = true;
    startOpenProject(file);
}

void OpenTuneAudioProcessorEditor::startOpenProject(const juce::File& file)
{
    const auto safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this);
    projectWorkerPool_.addJob(
        [this, safeThis, file]() {
        auto preparedResult = std::make_shared<Result<ProjectSession::PreparedOpen>>(
            projectSession_.prepareOpen(file));
        juce::MessageManager::callAsync([safeThis, preparedResult]() mutable {
            if (safeThis == nullptr)
                return;

            if (!preparedResult->ok()) {
                ConfirmDialogContent::launch(
                    new ConfirmDialogContent(
                        juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                        preparedResult->error().fullMessage(),
                        { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                    &safeThis->contentRoot_);
                safeThis->projectSession_.clearRecentProjects();
                safeThis->projectOperationBusy_ = false;
                return;
            }

            auto commitResult = safeThis->projectSession_.commitPreparedOpen(
                std::move(*preparedResult).value());
            if (!commitResult.ok()) {
                ConfirmDialogContent::launch(
                    new ConfirmDialogContent(
                        juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B\u5931\u8D25"),
                        commitResult.error().fullMessage(),
                        { { juce::String::fromUTF8(u8"\u786E\u5B9A"), nullptr, true } }),
                    &safeThis->contentRoot_);
                safeThis->projectOperationBusy_ = false;
                return;
            }

            safeThis->syncRecentProjectsToMenu();
            safeThis->updateTitleWithProjectPath();
            safeThis->refreshAllUIFromProject();
            safeThis->projectOperationBusy_ = false;
        });
    });
}

bool OpenTuneAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Undo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        undoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Redo, key))
    {
        if (!shouldAcceptUndoRedoShortcut()) {
            return true;
        }
        redoRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayPause, key))
    {
        playPauseToggleRequested();
        return true;
    }
    
    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::PlayFromStart, key))
    {
        playFromStartToggleRequested();
        return true;
    }

    if (KeyShortcutConfig::matchesShortcut(shortcutSettings_, KeyShortcutConfig::ShortcutId::Stop, key))
    {
        stopPlaybackRequested();
        return true;
    }

    return false;
}

bool OpenTuneAudioProcessorEditor::shouldAcceptUndoRedoShortcut()
{
    const uint32_t nowMs = juce::Time::getMillisecondCounter();
    constexpr uint32_t debounceMs = 120;

    if (lastUndoRedoShortcutMs_ != 0 && (nowMs - lastUndoRedoShortcutMs_) < debounceMs) {
        return false;
    }

    lastUndoRedoShortcutMs_ = nowMs;
    return true;
}

bool OpenTuneAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    static const juce::String kImportExtensionSpec = getImportExtensionSpec();
    for (const auto& path : files)
    {
        const juce::File f(path);
        if (f.hasFileExtension(kImportExtensionSpec))
            return true;
    }
    return false;
}

void OpenTuneAudioProcessorEditor::filesDropped(const juce::StringArray& files, int x, int y)
{
    if (isImportInProgress_)
    {
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String("Import Audio"),
            juce::String("Audio import is already in progress. Please try again later.")
        );
        return;
    }

    clearImportDropPreview();

    if (files.isEmpty())
        return;

    const juce::File file(files[0]);
    if (!file.existsAsFile())
        return;

    static const juce::String kImportExtensionSpec = getImportExtensionSpec();
    if (!file.hasFileExtension(kImportExtensionSpec))
    {
        const auto wildcard = getImportWildcardFilter().replaceCharacters("*", "");
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String("Import Audio"),
            juce::String("Unsupported file type.\nSupported extensions: ") + wildcard
        );
        return;
    }

    if (files.size() > 1)
    {
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String("Import Audio"),
            juce::String("Multiple files detected. Only the first file will be imported.")
        );
    }

    // Resolve import target from drop position (x,y)
    const ImportDropTarget target = resolveImportDropTarget(x, y);
    applyImportDropTarget(target, file);
}

// ============================================================================
// File Drag Hover Preview (fileDragEnter / fileDragMove / fileDragExit)
// ============================================================================

void OpenTuneAudioProcessorEditor::fileDragEnter(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(files);
    updateImportDropPreview(resolveImportDropTarget(x, y));
}

void OpenTuneAudioProcessorEditor::fileDragMove(const juce::StringArray& files, int x, int y)
{
    juce::ignoreUnused(files);
    updateImportDropPreview(resolveImportDropTarget(x, y));
}

void OpenTuneAudioProcessorEditor::fileDragExit(const juce::StringArray& files)
{
    juce::ignoreUnused(files);
    clearImportDropPreview();
}

void OpenTuneAudioProcessorEditor::updateImportDropPreview(ImportDropTarget target)
{
    ImportDropPreview preview;
    preview.active = true;
    preview.visibleTrackCount = trackPanel_.getVisibleTrackCount();
    preview.trackHeight = processorRef_.getTrackHeight();

    switch (target.kind)
    {
    case ImportDropTarget::Kind::ExistingTrack:
        preview.targetTrackId = target.trackId;
        break;

    case ImportDropTarget::Kind::NewTrack:
        preview.isNewTrack = true;
        break;

    case ImportDropTarget::Kind::FallbackActiveTrack:
    case ImportDropTarget::Kind::Reject:
        preview.active = false;
        break;
    }

    arrangementView_.setImportDropPreview(preview);
}

void OpenTuneAudioProcessorEditor::clearImportDropPreview()
{
    arrangementView_.clearImportDropPreview();
}

// ============================================================================
// Import Drop Target Resolver
// ============================================================================

ImportDropTarget OpenTuneAudioProcessorEditor::resolveImportDropTarget(int globalX, int globalY) const
{
    ImportDropTarget result;

    // Convert global (PluginEditor) coordinates to ArrangementViewComponent local coordinates
    const juce::Point<int> localPt = arrangementView_.getLocalPoint(this, juce::Point<int>(globalX, globalY));

    // Check if the drop point falls within the ArrangementView bounds
    const bool isInsideArrangement = arrangementView_.getLocalBounds().contains(localPt);

    if (!isInsideArrangement)
    {
        // Non-Arrangement drop 鈫?fallback to active track
        result.kind = ImportDropTarget::Kind::FallbackActiveTrack;
        result.trackId = getStandaloneActiveTrack(processorRef_);
        result.timelineStartSeconds = computeTrackAppendStartSeconds(result.trackId);
        return result;
    }

    if (localPt.y < arrangementView_.getRulerHeight())
    {
        result.kind = ImportDropTarget::Kind::FallbackActiveTrack;
        result.trackId = getStandaloneActiveTrack(processorRef_);
        result.timelineStartSeconds = computeTrackAppendStartSeconds(result.trackId);
        return result;
    }

    // Resolve the track from Y coordinate
    const int resolvedTrackId = arrangementView_.trackIdForViewportY(localPt.y);
    const int visibleTracks = trackPanel_.getVisibleTrackCount();

    if (resolvedTrackId < visibleTracks)
    {
        // Drop landed on a visible track lane
        result.kind = ImportDropTarget::Kind::ExistingTrack;
        result.trackId = resolvedTrackId;
        result.timelineStartSeconds = arrangementView_.viewportXToAbsoluteTime(localPt.x);
        if (result.timelineStartSeconds < 0.0)
            result.timelineStartSeconds = 0.0;
        return result;
    }

    // Drop landed below the last visible track 鈫?blank area
    if (visibleTracks >= OpenTuneAudioProcessor::MAX_TRACKS)
    {
        // Already at MAX_TRACKS 鈥?reject with a direct message
        result.kind = ImportDropTarget::Kind::Reject;
        result.rejectReason = juce::String("Maximum track count reached (")
                              + juce::String(OpenTuneAudioProcessor::MAX_TRACKS)
                              + juce::String("). Cannot create more tracks.");
        return result;
    }

    // Blank area 鈫?create a new visible track
    result.kind = ImportDropTarget::Kind::NewTrack;
    result.trackId = visibleTracks;  // new track will be at this index (0-based)
    result.timelineStartSeconds = juce::jmax(0.0, arrangementView_.viewportXToAbsoluteTime(localPt.x));
    return result;
}

// ============================================================================
// Apply Import Drop Target
// ============================================================================

void OpenTuneAudioProcessorEditor::applyImportDropTarget(ImportDropTarget target, const juce::File& file)
{
    switch (target.kind)
    {
    case ImportDropTarget::Kind::ExistingTrack:
        importAudioFileToTrack(target.trackId, file, target.timelineStartSeconds);
        break;

    case ImportDropTarget::Kind::NewTrack:
    {
        // Create one new visible track and import there
        trackPanel_.showMoreTracks();
        arrangementView_.setVisibleTrackCount(trackPanel_.getVisibleTrackCount());
        const int newTrackId = trackPanel_.getVisibleTrackCount() - 1;
        importAudioFileToTrack(newTrackId, file, target.timelineStartSeconds);
        break;
    }

    case ImportDropTarget::Kind::FallbackActiveTrack:
        importAudioFileToTrack(target.trackId, file, target.timelineStartSeconds);
        break;

    case ImportDropTarget::Kind::Reject:
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String::fromUTF8(u8"\u5BFC\u5165\u97F3\u9891"),
            target.rejectReason
        );
        break;
    }
}

void OpenTuneAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (UIColors::currentThemeId() == ThemeId::Overdose)
    {
        UIColors::fillOverdoseEditorBackground(g, getLocalBounds().toFloat(), 0.0f);
        return;
    }

    g.fillAll(UIColors::backgroundDark);
}

void OpenTuneAudioProcessorEditor::resized()
{
    // 外层 editor 绝不 transform；content root 按外窗尺寸/uiZoom 得到逻辑尺寸并整体缩放（§7.3）。
    const float uiZoomScale = static_cast<float>(appliedUiZoomPercent_) / 100.0f;
    contentRoot_.setBounds(0, 0,
                           static_cast<int>(std::ceil(getWidth() / uiZoomScale)),
                           static_cast<int>(std::ceil(getHeight() / uiZoomScale)));
    contentRoot_.setTransform(juce::AffineTransform::scale(uiZoomScale));

    auto bounds = contentRoot_.getLocalBounds();
    rippleOverlay_.setBounds(bounds);
    rippleOverlay_.toFront(false);

// Shadow margin: reserve space for panel shadow rendering
// Each component paint() uses reduced(shadowMargin) for background; shadow renders in margin
    const int shadowMargin = 12;
    const int gap = 6;  // Gap between panels (视觉间距，不含阴影)

    bounds.reduce(gap, gap); // Global padding

// TopBar: height + shadow margin (12px top and bottom)
    const int topBarHeight = menuBar_.isVisible() ? (MENU_BAR_HEIGHT + TRANSPORT_BAR_HEIGHT) : TRANSPORT_BAR_HEIGHT;
    const int topBarHeightWithShadow = topBarHeight + shadowMargin * 2;
    topBar_.setBounds(bounds.removeFromTop(topBarHeightWithShadow));
// Visual gap: subtract bottom shadow margin already consumed
    bounds.removeFromTop(juce::jmax(0, gap - shadowMargin));

// Visual gap: subtract bottom shadow margin already consumed
// Width + shadow margin (12px left and right)
    if (isTrackPanelVisible_)
    {
        trackPanel_.setVisible(true);
        const int trackPanelWidthWithShadow = TRACK_PANEL_WIDTH + shadowMargin * 2;
        trackPanel_.setBounds(bounds.removeFromLeft(trackPanelWidthWithShadow));
        bounds.removeFromLeft(juce::jmax(0, gap - shadowMargin));
    }
    else
    {
        trackPanel_.setVisible(false);
        trackPanel_.setBounds({});
    }

// Right Properties Panel (collapsible)
// Width + shadow margin (12px left and right)
    if (isParameterPanelVisible_)
    {
        parameterPanel_.setVisible(true);
        const int paramPanelWidthWithShadow = PARAMETER_PANEL_WIDTH + shadowMargin * 2;
        parameterPanel_.setBounds(bounds.removeFromRight(paramPanelWidthWithShadow));
        bounds.removeFromRight(juce::jmax(0, gap - shadowMargin));
    }
    else
    {
        parameterPanel_.setVisible(false);
        parameterPanel_.setBounds({});
    }

// Center area (PianoRoll / ArrangementView)
// PianoRoll reserves its scrollbars inside its own bounds; the overview strip
// aligns its right/bottom edges with the piano roll timeline viewport.
    pianoRoll_.setBounds(bounds);

    if (!isWorkspaceView_)
    {
        const auto timelineViewport = pianoRoll_.getTimelineViewportBounds();
        const int overviewX = bounds.getX() + 12;
        const int overviewRight = bounds.getX() + timelineViewport.getRight();
        const int overviewBottom = bounds.getY() + timelineViewport.getBottom();
        // Extend the overview strip to fill the former scrollbar space (20px).
        const int overviewHeight = OVERVIEW_STRIP_HEIGHT + UIColors::scrollBarThickness;
        overviewStrip_.setBounds(overviewX,
                                 overviewBottom - overviewHeight,
                                 overviewRight - overviewX,
                                 overviewHeight);
    }
    else
    {
        overviewStrip_.setBounds({});
    }

    arrangementView_.setBounds(bounds);
    
// AutoRenderOverlay covers entire PianoRoll area
    autoRenderOverlay_.setBounds(bounds);
    autoRenderOverlay_.toFront(false);

    renderBadge_.setBounds(bounds.getRight() - 148, bounds.getY() + 8, 140, 28);
    renderBadge_.toFront(false);

}

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

void OpenTuneAudioProcessorEditor::restoreStandaloneWindowGeometryOnce()
{
    if (standaloneWindowGeometryRestored_)
        return;

    auto* window = dynamic_cast<juce::StandaloneFilterWindow*>(getTopLevelComponent());
    if (window == nullptr || window->getPeer() == nullptr)
        return;  // 尚未挂入默认 StandaloneFilterWindow

    auto* holder = juce::StandalonePluginHolder::getInstance();
    if (holder == nullptr)
        return;

    auto* props = holder->settings.get();
    if (props == nullptr)
        return;

    standaloneWindowGeometryRestored_ = true;

    // 候选外层 bounds = 当前首选结果（editor 1200×900 + 既有窗口边框）；有保存值时覆盖 x/y/w/h（§5.1）。
    auto candidate = window->getBounds();
    if (props->containsKey("windowX") && props->containsKey("windowY"))
        candidate.setPosition(props->getIntValue("windowX"), props->getIntValue("windowY"));
    if (props->containsKey("windowWidth") && props->containsKey("windowHeight"))
        candidate.setSize(props->getIntValue("windowWidth"), props->getIntValue("windowHeight"));

    // 按保存位置所在 display 的 userArea 约束：默认 100% 放得下时不得超工作区；
    // 所需尺寸低于 min 时由 setBoundsConstrained 经 DecoratorConstrainer 抬回 min（min 优先，允许超工作区）。
    const auto userArea = juce::Desktop::getInstance().getDisplays().getDisplayForRect(candidate)->userArea;
    candidate.setSize(juce::jmin(candidate.getWidth(), userArea.getWidth()),
                      juce::jmin(candidate.getHeight(), userArea.getHeight()));
    candidate.setPosition(juce::jlimit(userArea.getX(),
                                       juce::jmax(userArea.getX(), userArea.getRight() - candidate.getWidth()),
                                       candidate.getX()),
                          juce::jlimit(userArea.getY(),
                                       juce::jmax(userArea.getY(), userArea.getBottom() - candidate.getHeight()),
                                       candidate.getY()));

    // 由 StandaloneFilterWindow 现有 constrainer 同步外层窗口与 editor，不手算窗口边框。
    window->setBoundsConstrained(candidate);
}

void OpenTuneAudioProcessorEditor::applyUiZoomIfNeeded()
{
    const auto zoomDecision = resolveEditorUiZoomDecision(processorRef_.getUiZoomPercent(),
                                                          appliedUiZoomPercent_,
                                                          kBaseMinWidth,
                                                          kBaseMinHeight);
    if (!zoomDecision.changed)
        return;

    appliedUiZoomPercent_ = zoomDecision.appliedPercent;

    // 保持当前外层窗口尺寸；仅当新 min 不满足时由 constrainer 扩大（§7.1/§7.3）。
    setResizeLimits(zoomDecision.minWidth,
                    zoomDecision.minHeight,
                    kMaxWindowWidth, kMaxWindowHeight);

    resized();
    repaint();
}

void OpenTuneAudioProcessorEditor::timerCallback()
{
    applyUiZoomIfNeeded();

    restoreStandaloneWindowGeometryOnce();

    syncSharedAppPreferences();

    const bool vocoderReady = processorRef_.isVocoderReady();
    const bool inferenceNow = false;
    setInferenceActive(inferenceNow);

    syncParameterPanelFromSelection();

    if (arrangementView_.isShowing()) {
        arrangementView_.onHeartbeatTick();
    }

    pianoRoll_.onHeartbeatTick();

    if (pianoRoll_.isShowing()) {
        overviewStrip_.onHeartbeatTick(pianoRoll_.editedContentKey(),
                                       pianoRoll_.activeContentProjection(),
                                       pianoRoll_.timelineCamera(),
                                       pianoRoll_.timelinePolicyViewportWidth());
    }

    const bool allowSecondaryRefresh = !inferenceActive_ || ((++inferenceActiveTickCounter_ % 4) == 0);
    if (allowSecondaryRefresh && referenceRefreshPending_)
        refreshReferenceContext();

// Update playhead position from processor (presented position via shared projection)
    double currentPositionSeconds = processorRef_.getPosition();
    double sampleRate = processorRef_.getSampleRate();

    const double bpm = processorRef_.getBpm();
    const int timeSigNum = processorRef_.getTimeSigNumerator();
    const int timeSigDenom = processorRef_.getTimeSigDenominator();

    // Sync BPM from processor to UI (processor is canonical)
    if (bpm > 0.0 && std::abs(bpm - lastSyncedBpm_) > 0.001) {
        transportBar_.setBpm(bpm);
        pianoRoll_.setBpm(bpm);
        lastSyncedBpm_ = bpm;
    }

    // Sync time signature from processor to UI (processor is canonical)
    if (timeSigNum > 0 && timeSigDenom > 0
        && (timeSigNum != lastSyncedTimeSigNum_ || timeSigDenom != lastSyncedTimeSigDenom_)) {
        transportBar_.setTimeSignature(timeSigNum, timeSigDenom);
        pianoRoll_.setTimeSignature(timeSigNum, timeSigDenom);
        lastSyncedTimeSigNum_ = timeSigNum;
        lastSyncedTimeSigDenom_ = timeSigDenom;
    }

    // Sync timeline display mode from AppPreferences
    const auto prefMode = appPreferences_.getTimelineDisplayMode();
    if (prefMode != timelineDisplayMode_) {
        timelineDisplayMode_ = prefMode;
        transportBar_.setTimelineDisplayMode(timelineDisplayMode_);
        pianoRoll_.setTimelineDisplayMode(timelineDisplayMode_);
        arrangementView_.setTimelineDisplayMode(timelineDisplayMode_);
    }
    
    if (allowSecondaryRefresh && sampleRate > 0.0) {
        const int sr = static_cast<int>(sampleRate);
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const ContentKey activeKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        auto snap = processorRef_.getContentSnapshot(activeKey);
        auto curve = snap ? snap->pitchCurve : nullptr;
        std::shared_ptr<const juce::AudioBuffer<float>> contentBuffer =
            snap ? snap->audioBuffer : nullptr;
        const uint64_t currentNotesRevision = activeKey.isValid() && snap
            ? snap->notesRevision
            : 0;
        const uint64_t currentTimeGridRevision = activeKey.isValid() && snap
            ? snap->timeGridRevision
            : 0;
        const uint64_t currentPitchRevision = activeKey.isValid() && snap
            ? snap->pitchRevision
            : 0;
        const bool contentKeyChanged = activeKey != lastPianoRollContentKey_;
        const bool contentChanged =
            contentKeyChanged
            || sr != lastPianoRollSampleRate_
            || curve != lastPianoRollCurve_
            || contentBuffer != lastPianoRollBuffer_;
        if (contentChanged) {
            pianoRoll_.setEditedContent(activeKey, PitchCurve::fromSnapshot(curve), contentBuffer, sr);
            lastPianoRollContentKey_ = activeKey;
            lastPianoRollSampleRate_ = sr;
            lastPianoRollCurve_ = curve;
            lastPianoRollBuffer_ = contentBuffer;
        }
        const auto revisionPulse = resolveContentRevisionPulse(currentNotesRevision,
                                                               currentTimeGridRevision,
                                                               currentPitchRevision,
                                                               lastPianoRollNotesRevision_,
                                                               lastPianoRollTimeGridRevision_,
                                                               lastPianoRollPitchRevision_);
        if (revisionPulse.notesChanged) {
            // Same content, fresh notes – typically GAME's async commit.
            pianoRoll_.onNotesRevisionChanged();
        }
        if (revisionPulse.timeGridChanged) {
            pianoRoll_.onTimeGridRevisionChanged();
        }
        if (revisionPulse.pitchChanged) {
            pianoRoll_.onPitchRevisionChanged();
        }
        if (activeTrack >= 0 && activePlacementIndex >= 0) {
            pianoRoll_.setTrackDisplayColour(getStandaloneTrackColour(processorRef_, activeTrack));
            // 仅当 content 的 detectedKey 与上次观察值不同才回显（导入检测写入/手动设置写入/切换 content）。
            // 比较基准是 content 观察值而非 UI 显示值：手动设置未持久化时不会被回读覆盖。
            const DetectedKey resolvedKey =
                resolveScaleForPlacementContent(activeTrack, activePlacementIndex, nullptr);
            const int resolvedRootNote = static_cast<int>(resolvedKey.root);
            const int resolvedScaleType = OpenTune::scaleToUiScaleType(resolvedKey.scale);
            if (resolvedRootNote != lastResolvedScaleRootNote_ || resolvedScaleType != lastResolvedScaleType_) {
                applyResolvedScaleForPlacementContent(activeTrack, activePlacementIndex);
            }
        }

        // Pitch shift indicator: project from active snapshot
        {
            const PitchShiftSettings currentPitchShift = snap ? snap->pitchShiftSettings : PitchShiftSettings::identity();
            if (currentPitchShift != lastPitchShiftIndicatorSettings_) {
                parameterPanel_.setPitchShiftIndicator(currentPitchShift.semitone, currentPitchShift.cents);
                lastPitchShiftIndicatorSettings_ = currentPitchShift;
            }
        }

        lastPianoRollNotesRevision_ = revisionPulse.nextNotesRevision;
        lastPianoRollTimeGridRevision_ = revisionPulse.nextTimeGridRevision;
        lastPianoRollPitchRevision_ = revisionPulse.nextPitchRevision;
    }

// Playhead position: each component reads presented position from PlayHeadState projection
    transportBar_.setPositionSeconds(currentPositionSeconds);

    const RenderStatusSnapshot statusSnapshot = getRenderStatusSnapshot();

    // RMVPE overlay：与 vocoder 无关，独立于渲染状态
    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
        const ContentKey targetContentKey = rmvpeOverlayTargetContentKey_;

        bool shouldUnlatch = false;
        if (!targetContentKey.isValid()) {
            shouldUnlatch = true;
        } else {
            auto snap = processorRef_.getContentSnapshot(targetContentKey);
            const auto f0State = snap ? snap->originalF0State : OriginalF0State::NotRequested;
            // AUTO 在 F0 Ready 发布前由 F0 完成链同步提交（唯一核心），
            // overlay 只需等待 F0 状态即可覆盖整个导入管线。
            shouldUnlatch = (f0State == OriginalF0State::Ready
                             || f0State == OriginalF0State::Failed);
        }

        if (shouldUnlatch) {
            rmvpeOverlayLatched_ = false;
            rmvpeOverlayTargetContentKey_ = ContentKey{};
        }
    }

    bool shouldShowOverlay = false;

    if (rmvpeOverlayLatched_ && !isWorkspaceView_) {
        autoRenderOverlay_.setMessageText(juce::String::fromUTF8(u8"\u6B63\u5728\u5904\u7406\u97F3\u9891"));
        shouldShowOverlay = true;
    }

    // Reference-note analysis shares the RMVPE overlay.
    // shares the same overlay system as RMVPE extraction.
    if (!shouldShowOverlay && !isWorkspaceView_) {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
        const ContentKey activeContentKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        if (activeContentKey.isValid()) {
            const ReferenceFeatureSet refFeatures = processorRef_.getReferenceFeatures(activeContentKey);
            if (refFeatures.status == ReferenceFeatureStatus::Extracting) {
                autoRenderOverlay_.setMessageText(juce::String::fromUTF8(u8"正在分析参考 Clip"));
                shouldShowOverlay = true;
            }
        }
    }

    // ============================================================================
    // Render badge logic — Stage 2 (time-stretch) is now synchronous inside CRS,
    // so there is no async "in-flight" state to display. Badge reflects Stage 1 only.
    // ============================================================================
    const bool isAutoProcessing = false;

    // Stage 1 — meaningful only when vocoder loaded
    bool stage1HasWork = false;
    int  stage1Done = 0;
    int  stage1Total = 0;
    if (vocoderReady) {
        const int activeTrack = getStandaloneActiveTrack(processorRef_);
        const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);

        const ContentKey activeContentKey = (activeTrack >= 0 && activePlacementIndex >= 0)
            ? getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex)
            : ContentKey{};
        const auto chunkStats = processorRef_.getReadableContentChunkStats(activeContentKey);
        stage1HasWork = chunkStats.hasActiveWork();
        stage1Done    = chunkStats.idle + chunkStats.blank;
        stage1Total   = chunkStats.total();

        if (isAutoProcessing) {
            const float olProgress = (stage1Total > 0)
                ? static_cast<float>(stage1Done) / static_cast<float>(stage1Total) : 0.0f;
            autoRenderOverlay_.setMessageText(buildRenderingOverlayTitle(stage1Done, stage1Total, olProgress));
            shouldShowOverlay = true;
        }
    }

    // Combined badge visibility — Stage 1 only (Stage 2 is synchronous in CRS).
    const bool shouldShowBadge = stage1HasWork && !isAutoProcessing;
    if (shouldShowBadge) {
        renderBadge_.setMessageText(juce::String::fromUTF8(u8"\u6e32\u67d3\u4e2d (")
            + juce::String(stage1Done) + "/" + juce::String(stage1Total) + ")");
    }
    if (renderBadge_.isVisible() != shouldShowBadge) {
        renderBadge_.setVisible(shouldShowBadge);
    }
    transportBar_.setRenderStatusText(juce::String());

    if (autoRenderOverlay_.isVisible() != shouldShowOverlay) {
        autoRenderOverlay_.setVisible(shouldShowOverlay);
    }

#if JUCE_DEBUG
    if (++diagnosticHeartbeatCounter_ >= 300) {
        diagnosticHeartbeatCounter_ = 0;
        const auto diagnosticInfo = processorRef_.getDiagnosticInfo(getStandaloneActiveTrack(processorRef_), statusSnapshot.placementId);
        AppLogger::log("StandaloneEditor: render status=" + renderStatusToString(statusSnapshot.status)
            + " contentKey=" + juce::String(static_cast<juce::int64>(diagnosticInfo.contentKey.objectId))
            + " placementId=" + juce::String(static_cast<juce::int64>(diagnosticInfo.placementId))
            + " desiredRev=" + juce::String(static_cast<juce::int64>(diagnosticInfo.desiredRevision))
            + " publishedRev=" + juce::String(static_cast<juce::int64>(diagnosticInfo.publishedRevision))
            + " pending=" + juce::String(diagnosticInfo.chunkStats.pending)
            + " running=" + juce::String(diagnosticInfo.chunkStats.running)
            + " lastControl=" + diagnosticInfo.lastControlCall);
    }
#endif

    // Sync playing state (Fix for inconsistent UI state)
    if (transportBar_.isPlaying() != processorRef_.isPlaying())
    {
        transportBar_.setPlaying(processorRef_.isPlaying());
    }

    if (allowSecondaryRefresh) {
        for (int i = 0; i < OpenTuneAudioProcessor::MAX_TRACKS; ++i) {
            const float rmsDb = getStandaloneTrackRms(processorRef_, i);
            trackPanel_.setTrackLevel(i, rmsDb);
        }
    }
}

void OpenTuneAudioProcessorEditor::syncSharedAppPreferences()
{
    const auto preferencesState = appPreferences_.getState();
    const auto& sharedPreferences = preferencesState.shared;
    const auto& visualPreferences = sharedPreferences.pianoRollVisualPreferences;
    const bool experimentalFeaturesEnabled = sharedPreferences.experimentalFeaturesEnabled;
    const auto referenceAlignMode = sharedPreferences.experimentalReferenceAlignMode;

    // EQ popup「以后不再提示」偏好直接注入（无中转层）
    pianoRoll_.setAppPreferences(&appPreferences_);

    processorRef_.setExperimentalReferenceAlignMode(referenceAlignMode);
    if (appliedReferenceAlignMode_ != referenceAlignMode
        || appliedExperimentalFeaturesEnabled_ != experimentalFeaturesEnabled) {
        appliedReferenceAlignMode_ = referenceAlignMode;
        appliedExperimentalFeaturesEnabled_ = experimentalFeaturesEnabled;
        referenceRefreshPending_ = true;
    }

    if (languageState_ != nullptr) {
        languageState_->language = sharedPreferences.language;
    }

    if (appliedLanguage_ != sharedPreferences.language) {
        appliedLanguage_ = sharedPreferences.language;
        LocalizationManager::getInstance().notifyLanguageChanged(sharedPreferences.language);
    }

    const auto effectiveTheme = resolveEffectiveTheme(sharedPreferences.theme);
    if (appliedThemeId_ != effectiveTheme)
        applyThemeToEditor(effectiveTheme);

    pianoRoll_.setAudioEditingScheme(sharedPreferences.audioEditingScheme);
    pianoRoll_.setExperimentalFeaturesEnabled(experimentalFeaturesEnabled);
    pianoRoll_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    pianoRoll_.setNoteNameMode(visualPreferences.noteNameMode);
    pianoRoll_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);
    pianoRoll_.setBackgroundBrightness(visualPreferences.backgroundBrightness);
    pianoRoll_.setGridStyle(sharedPreferences.gridStyle);
    pianoRoll_.setShowPianoKeyboard(visualPreferences.showPianoKeyboard);
    pianoRoll_.setScaleAssistEnabled(visualPreferences.scaleAssistEnabled);
    menuBar_.setShowPianoKeyboard(visualPreferences.showPianoKeyboard);
    menuBar_.setScaleAssistEnabled(visualPreferences.scaleAssistEnabled);
    parameterPanel_.setExperimentalFeaturesEnabled(experimentalFeaturesEnabled);
    parameterPanel_.setOpenDyneMode(AudioEditingScheme::usesNotesPrimaryScheme(sharedPreferences.audioEditingScheme));
    parameterPanel_.setActiveTool(static_cast<int>(pianoRoll_.getCurrentTool()));
    arrangementView_.setZoomSensitivity(sharedPreferences.zoomSensitivity);
    arrangementView_.setExperimentalReferenceControlsEnabled(experimentalFeaturesEnabled);
    menuBar_.setNoteNameMode(visualPreferences.noteNameMode);
    menuBar_.setShowUnvoicedFrames(visualPreferences.showUnvoicedFrames);

    // When the editing scheme changes, relayout to show/hide the horizontal scrollbar
    // and resize the overview strip for Melodyne-style integration.
    if (appliedAudioEditingScheme_ != sharedPreferences.audioEditingScheme) {
        appliedAudioEditingScheme_ = sharedPreferences.audioEditingScheme;
        resized();
    }

    shortcutSettings_ = preferencesState.shared.shortcuts;
    pianoRoll_.setShortcutSettings(shortcutSettings_);
    arrangementView_.setShortcutSettings(shortcutSettings_);
    menuBar_.setMouseTrailTheme(preferencesState.standalone.mouseTrailTheme);
    rippleOverlay_.setTrailTheme(preferencesState.standalone.mouseTrailTheme);
    menuBar_.setCursorStyle(preferencesState.standalone.cursorStyle);
    CursorThemeManager::getInstance().setStyle(preferencesState.standalone.cursorStyle);
    menuBar_.setTrackColorMode(sharedPreferences.trackColorMode);
    trackPanel_.setTrackColorMode(sharedPreferences.trackColorMode);
}

void OpenTuneAudioProcessorEditor::syncTrackColorsToPanel()
{
    for (int i = 0; i < MAX_TRACKS; ++i)
        trackPanel_.setTrackColour(i, getStandaloneTrackColour(processorRef_, i));
}

RenderStatusSnapshot OpenTuneAudioProcessorEditor::getRenderStatusSnapshot() const
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(trackId, placementIndex) : 0;
    const ContentKey contentKey = getStandaloneContentKey(processorRef_, trackId, placementIndex);

    RenderStatusSnapshot snapshot;
    snapshot.contentKey = contentKey;
    snapshot.placementId = placementId;
    if (!contentKey.isValid()) {
        return snapshot;
    }

    auto renderCache = processorRef_.getContentRenderService()->getRenderCache(contentKey);
    if (renderCache == nullptr) {
        snapshot.contentKey = ContentKey{};
        snapshot.placementId = 0;
        return snapshot;
    }

    return makeRenderStatusSnapshot(contentKey, placementId, renderCache->getStateSnapshot());
}

void OpenTuneAudioProcessorEditor::syncPianoRollFromPlacementSelection(int trackId, int placementIndex)
{
    StandaloneArrangement::Placement placement;
    const bool hasPlacement = (placementIndex >= 0)
        && getStandalonePlacementByIndex(processorRef_, trackId, placementIndex, placement);
    const ContentKey contentKey = hasPlacement ? placement.contentKey : ContentKey{};

    const bool projectionChanged = pianoRoll_.setContentProjection(
        hasPlacement ? makePianoRollProjection(placement, processorRef_)
                     : ContentTimelineProjection{});

    const int sr = static_cast<int>(processorRef_.getSampleRate());
    auto snap = processorRef_.getContentSnapshot(contentKey);
    std::shared_ptr<const juce::AudioBuffer<float>> contentBuffer =
        snap ? snap->audioBuffer : nullptr;
    auto curve = snap ? snap->pitchCurve : nullptr;
    const bool contentChanged = contentKey != lastPianoRollContentKey_;

    pianoRoll_.setEditedContent(contentKey, PitchCurve::fromSnapshot(curve), contentBuffer, sr);
    pianoRoll_.setTrackDisplayColour(getStandaloneTrackColour(processorRef_, trackId));

    lastPianoRollContentKey_ = contentKey;
    lastPianoRollSampleRate_ = sr;
    lastPianoRollCurve_ = curve;
    lastPianoRollBuffer_ = contentBuffer;
    lastPianoRollNotesRevision_ = snap ? snap->notesRevision : 0;
    lastPianoRollTimeGridRevision_ = snap ? snap->timeGridRevision : 0;
    lastPianoRollPitchRevision_ = snap ? snap->pitchRevision : 0;

    if (hasPlacement && (contentChanged || projectionChanged))
        pianoRoll_.requestInitialF0View(contentKey);

    applyResolvedScaleForPlacementContent(trackId, placementIndex);

}

void OpenTuneAudioProcessorEditor::applyPlacementSelectionContext(int trackId, uint64_t placementId)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS)
    {
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneActiveTrack(processorRef_, trackId);
    trackPanel_.setActiveTrack(trackId);

    if (placementId == 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    const int placementIndex = processorRef_.findPlacementIndexById(trackId, placementId);
    if (placementIndex < 0)
    {
        setStandaloneSelectedPlacementIndex(processorRef_, trackId, -1);
        pianoRoll_.setContentProjection({});
        pianoRoll_.setEditedContent(ContentKey{}, nullptr, nullptr, static_cast<int>(processorRef_.getSampleRate()));
        lastPianoRollContentKey_ = ContentKey{};
        lastPianoRollCurve_.reset();
        lastPianoRollBuffer_.reset();
        return;
    }

    setStandaloneSelectedPlacementIndex(processorRef_, trackId, placementIndex);
    syncPianoRollFromPlacementSelection(trackId, placementIndex);
}

void OpenTuneAudioProcessorEditor::toolSelected(int toolId)
{
    if (toolId < 0 || toolId > static_cast<int>(ToolId::Eq)) {
        return;
    }

    auto tool = static_cast<ToolId>(toolId);
    pianoRoll_.setCurrentTool(tool);
}

// ============================================================================
// ParameterPanel::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::retuneSpeedChanged(float speed)
{
    float normalizedSpeed = speed / 100.0f;
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::RetuneSpeed, normalizedSpeed);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::RetuneSpeed, normalizedSpeed);
    }
    if (result.changed)
        projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::vibratoDepthChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoDepth, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoDepth, value);
    }
    if (result.changed)
        projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::vibratoRateChanged(float value)
{
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::VibratoRate, value);
    if (result.status == AudioEditingScheme::ParameterEditStatus::NoTarget) {
        pianoRoll_.setCreationDefault(AudioEditingScheme::ParameterId::VibratoRate, value);
    }
    if (result.changed)
        projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::noteSplitChanged(float value)
{
    // NoTarget 时 editParameter 内部已回落更新创建默认值
    auto result = pianoRoll_.editParameter(AudioEditingScheme::ParameterId::NoteSplit, value);
    if (result.changed)
        projectSession_.markDirty();
}

// ============================================================================
// MenuBarComponent::Listener Implementation
// ============================================================================

// Import mode enumeration
enum class ImportMode
{
    SameTrack,      // 按顺序导入到同一个轨道
    SeparateTracks  // 分别导入到多个轨道（齐头）
};

void OpenTuneAudioProcessorEditor::importAudioRequested()
{
// Direct file chooser dialog with multi-select support
// Import destination track determined by currently selected track
    DBG("OpenTuneAudioProcessorEditor::importAudioRequested called");

    if (isImportInProgress_)
    {
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String("Import Audio"),
            juce::String("Audio import is already in progress. Please try again later.")
        );
        return;
    }

    const auto wildcardFilter = getImportWildcardFilter();

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"\u9009\u62E9\u8981\u5BFC\u5165\u7684\u97F3\u9891\u6587\u4EF6"),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        wildcardFilter);
    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectMultipleItems;

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const juce::Array<juce::File>& selectedFiles = fc.getResults();
        if (selectedFiles.isEmpty())
        {
            DBG("No files selected");
            return;
        }

// Get currently selected track
        int currentTrack = getStandaloneActiveTrack(safeThis->processorRef_);
        int visibleTracks = safeThis->trackPanel_.getVisibleTrackCount();

        if (selectedFiles.size() == 1)
        {
// Single file: import directly to currently selected track
            safeThis->importAudioFileToTrack(currentTrack, selectedFiles[0]);
        }
        else
        {
// Multiple files: prompt for import mode
            auto filesPtr = std::make_shared<juce::Array<juce::File>>(selectedFiles);

            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Choose Import Mode"),
                    juce::String("You selected ") + juce::String(selectedFiles.size()) + juce::String(" audio files. Choose an import mode."),
                    {
                        { juce::String("Import Sequentially To Current Track"), [=]() {
                            if (safeThis == nullptr)
                                return;

// Sequential import into same track (currently selected)
                            const int batchId = safeThis->nextImportBatchId_++;
                            safeThis->importBatchNextStartSeconds_[batchId] = safeThis->computeTrackAppendStartSeconds(currentTrack);
                            safeThis->importBatchRemainingItems_[batchId] = filesPtr->size();

                            for (int i = 0; i < filesPtr->size(); ++i)
                            {
                                OpenTuneAudioProcessorEditor::PendingImport pending;
                                pending.placement.trackId = currentTrack;
                                pending.file = (*filesPtr)[i];
                                pending.batchId = batchId;
                                pending.appendSequentially = true;
                                safeThis->queuePendingImport(std::move(pending));
                            }
                        }, true },
                        { juce::String("Import To Separate Tracks"), [=]() {
                            if (safeThis == nullptr)
                                return;

// Aligned import across multiple tracks
                            const int remainingTrackCapacity = juce::jmax(0, OpenTuneAudioProcessor::MAX_TRACKS - currentTrack);
                            const int acceptedFileCount = juce::jmin(filesPtr->size(), remainingTrackCapacity);
                            if (acceptedFileCount <= 0)
                            {
                                ConfirmDialogContent::showMessage(
                                    &safeThis->contentRoot_,
                                    juce::String("Import Failed"),
                                    juce::String("There are no available tracks after the current track.")
                                );
                                return;
                            }

// Auto-expand visible track count
                            int requiredTracks = currentTrack + acceptedFileCount;
                            if (requiredTracks > visibleTracks)
                            {
                                int newVisibleTracks = std::min(requiredTracks, OpenTuneAudioProcessor::MAX_TRACKS);
                                safeThis->trackPanel_.setVisibleTrackCount(newVisibleTracks);
                                safeThis->arrangementView_.setVisibleTrackCount(newVisibleTracks);
                            }

// Starting from current track, import into subsequent tracks in order
                            for (int i = 0; i < acceptedFileCount; ++i)
                            {
                                OpenTuneAudioProcessorEditor::PendingImport pending;
                                pending.placement.trackId = currentTrack + i;
                                pending.placement.timelineStartSeconds = 0.0;
                                pending.file = (*filesPtr)[i];
                                safeThis->queuePendingImport(std::move(pending));
                            }

                            if (acceptedFileCount < filesPtr->size())
                            {
                                ConfirmDialogContent::showMessage(
                                    &safeThis->contentRoot_,
                                    juce::String("Import Count Trimmed"),
                                    juce::String("Only ")
                                        + juce::String(acceptedFileCount)
                                        + juce::String(" tracks are available after the current track. Extra files were not queued.")
                                );
                            }
                        } },
                        { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr }
                    }
                ),
                &safeThis->contentRoot_
            );
        }
    });
}

void OpenTuneAudioProcessorEditor::importAudioFileToTrack(int trackId, const juce::File& file,
                                                            double timelineStartSeconds)
{
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = trackId;
    placement.timelineStartSeconds = (timelineStartSeconds >= 0.0)
        ? timelineStartSeconds
        : computeTrackAppendStartSeconds(trackId);

    PendingImport pendingImport;
    pendingImport.placement = placement;
    pendingImport.file = file;
    queuePendingImport(std::move(pendingImport));
}

void OpenTuneAudioProcessorEditor::queuePendingImport(PendingImport pendingImport)
{
    if (isImportInProgress_)
    {
        importQueue_.push_back(std::move(pendingImport));
        return;
    }

    startPendingImport(std::move(pendingImport));
}

void OpenTuneAudioProcessorEditor::startPendingImport(PendingImport pendingImport)
{
    if (!pendingImport.placement.isValid())
    {
        processNextImportInQueue();
        return;
    }

    isImportInProgress_ = true;

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    const auto sourceFile = pendingImport.file;
    const auto fileName = sourceFile.getFileName();
    const auto sourceFilePath = sourceFile.getFullPathName();

    asyncAudioLoader_.loadAudioFile(
        sourceFile,
        {},
        [safeThis, pendingImport = std::move(pendingImport), fileName, sourceFilePath](AsyncAudioLoader::LoadResult result) mutable
        {
            if (safeThis == nullptr)
                return;

            if (!result.success)
            {
                safeThis->isImportInProgress_ = false;
                safeThis->releaseImportBatchSlot(pendingImport.batchId);
                safeThis->processNextImportInQueue();
                ConfirmDialogContent::showMessage(
                    &safeThis->contentRoot_,
                    juce::String::fromUTF8(u8"\u5BFC\u5165\u5931\u8D25"),
                    result.errorMessage
                );
                return;
            }

            OpenTuneAudioProcessor::ImportPlacement placement = pendingImport.placement;
            if (pendingImport.appendSequentially)
            {
                const auto cursorIt = safeThis->importBatchNextStartSeconds_.find(pendingImport.batchId);
                placement.timelineStartSeconds = cursorIt != safeThis->importBatchNextStartSeconds_.end()
                    ? cursorIt->second
                    : safeThis->computeTrackAppendStartSeconds(placement.trackId);
            }

            safeThis->launchBackgroundUiTask([safeThis,
                                              pendingImport,
                                              placement,
                                              fileName,
                                              sourceFilePath,
                                              sampleRate = result.sampleRate,
                                              audioBuffer = std::move(result.audioBuffer)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                OpenTuneAudioProcessor::PreparedImport preparedImport;
                {
                    if (!safeThis->processorRef_.prepareImport(std::move(audioBuffer), sampleRate, fileName, sourceFilePath, preparedImport))
                    {
                        juce::MessageManager::callAsync([safeThis, batchId = pendingImport.batchId]()
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->isImportInProgress_ = false;
                            safeThis->releaseImportBatchSlot(batchId);
                            safeThis->processNextImportInQueue();
                            ConfirmDialogContent::showMessage(
                                &safeThis->contentRoot_,
                                juce::String("Import Failed"),
                                juce::String("Audio import preprocessing failed. Please try again.")
                            );
                        });
                        return;
                    }
                }

                const double preparedImportDurationSeconds = TimeCoordinate::samplesToSeconds(preparedImport.storedAudioBuffer.getNumSamples(), TimeCoordinate::kRenderSampleRate);

                juce::MessageManager::callAsync([safeThis,
                                                pendingImport,
                                                placement,
                                                preparedImportDurationSeconds,
                                                preparedImport = std::move(preparedImport)]() mutable
                {
                    if (safeThis == nullptr)
                        return;

                    const auto committedPlacement = safeThis->processorRef_.commitPreparedImportAsPlacement(std::move(preparedImport), placement);
                    if (!committedPlacement.isValid())
                    {
                        safeThis->isImportInProgress_ = false;
                        safeThis->releaseImportBatchSlot(pendingImport.batchId);
                        safeThis->processNextImportInQueue();
                        ConfirmDialogContent::showMessage(
                            &safeThis->contentRoot_,
                            juce::String("Import Failed"),
                            juce::String("Audio import commit failed. Please try again.")
                        );
                        return;
                    }

                    if (pendingImport.appendSequentially)
                    {
                        safeThis->importBatchNextStartSeconds_[pendingImport.batchId] = placement.timelineStartSeconds + preparedImportDurationSeconds;
                    }

                    safeThis->isImportInProgress_ = false;
                    safeThis->releaseImportBatchSlot(pendingImport.batchId);

                    safeThis->arrangementView_.requestContentRedraw();
                    safeThis->arrangementView_.grabKeyboardFocus();
                    safeThis->applyPlacementSelectionContext(placement.trackId, committedPlacement.placementId);
                    auto importSnap = safeThis->processorRef_.getContentSnapshot(committedPlacement.contentKey);
                    auto importBuf = importSnap ? importSnap->audioBuffer : nullptr;
                    auto importCurve = importSnap ? importSnap->pitchCurve : nullptr;
                    safeThis->pianoRoll_.setEditedContent(committedPlacement.contentKey,
                                                          PitchCurve::fromSnapshot(importCurve),
                                                          importBuf,
                                                          static_cast<int>(safeThis->processorRef_.getSampleRate()));
                    safeThis->lastPianoRollContentKey_ = committedPlacement.contentKey;
                    safeThis->lastPianoRollCurve_ = importCurve;
                    safeThis->lastPianoRollBuffer_ = importBuf;

                    OpenTuneAudioProcessor::ContentRefreshRequest refreshRequest;
                    refreshRequest.contentKey = committedPlacement.contentKey;
                    if (safeThis->pianoRoll_.isOpenDyne()) {
                        refreshRequest.generateNotesWholeContentOnReady = true;
                        // 导入音符生成使用 canonical PianoRoll 参数（与手动 AUTO 同一来源）
                        refreshRequest.noteGenerationParams = safeThis->pianoRoll_.getCurrentAutoTuneParams();
                        // 自动导入音符生成作为导入派生事务：不创建独立 Undo，
                        // 提交成功回调推进 dirty generation（message-thread）。
                        refreshRequest.onNotesGenerated = [safeThis]() {
                            if (safeThis != nullptr) safeThis->projectSession_.markDirty();
                        };
                    }
                    if (!safeThis->processorRef_.requestContentRefresh(refreshRequest)) {
                        AppLogger::log("ClipDerivedRefresh: standalone request rejected contentKey.objectId="
                            + juce::String(static_cast<juce::int64>(committedPlacement.contentKey.objectId)));
                    } else {
                        safeThis->rmvpeOverlayLatched_ = true;
                        safeThis->rmvpeOverlayTargetContentKey_ = committedPlacement.contentKey;
                    }

                    safeThis->arrangementView_.resetUserZoomFlag();
                    safeThis->pianoRoll_.resetUserZoomFlag();

                    safeThis->processNextImportInQueue();
                });
            });
        }
    );
}

// Process next file in the import queue
void OpenTuneAudioProcessorEditor::processNextImportInQueue()
{
    if (importQueue_.empty())
        return;
    
// Dequeue the first pending import item
    auto next = importQueue_.front();
    importQueue_.erase(importQueue_.begin());

    startPendingImport(std::move(next));
}

double OpenTuneAudioProcessorEditor::computeTrackAppendStartSeconds(int trackId) const
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) {
        return 0.0;
    }

    const auto* arrangement = processorRef_.getStandaloneArrangement();
    jassert(arrangement != nullptr);

    double appendStartSeconds = 0.0;
    const int placementCount = arrangement->getNumPlacements(trackId);
    for (int placementIndex = 0; placementIndex < placementCount; ++placementIndex)
    {
        StandaloneArrangement::Placement placement;
        if (!arrangement->getPlacementByIndex(trackId, placementIndex, placement) || !placement.contentKey.isValid()) {
            continue;
        }

        auto* clip = processorRef_.getStandaloneContentRepository()
            ? processorRef_.getStandaloneContentRepository()->findClip(placement.contentKey) : nullptr;
        const auto buffer = clip ? clip->content().audioBuffer : nullptr;
        if (buffer == nullptr) {
            continue;
        }

        appendStartSeconds = std::max(appendStartSeconds, placement.timelineEndSeconds());
    }

    return appendStartSeconds;
}

void OpenTuneAudioProcessorEditor::releaseImportBatchSlot(int batchId)
{
    if (batchId == 0) {
        return;
    }

    const auto remainingIt = importBatchRemainingItems_.find(batchId);
    if (remainingIt == importBatchRemainingItems_.end()) {
        importBatchNextStartSeconds_.erase(batchId);
        return;
    }

    remainingIt->second -= 1;
    if (remainingIt->second > 0) {
        return;
    }

    importBatchRemainingItems_.erase(remainingIt);
    importBatchNextStartSeconds_.erase(batchId);
}

void OpenTuneAudioProcessorEditor::exportAudioRequested(MenuBarComponent::ExportType exportType)
{
    using ExportType = MenuBarComponent::ExportType;
    
    // Check if export is already in progress
    if (exportInProgress_.load())
    {
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String("Export Audio"),
            juce::String("An export task is already in progress. Please try again later."));
        return;
    }
    
// Determine default filename based on export type
    juce::String defaultFileName;
    switch (exportType)
    {
        case ExportType::SelectedClip:
            defaultFileName = "selected_clip.wav";
            break;
        case ExportType::Track:
            defaultFileName = "track_" + juce::String(getStandaloneActiveTrack(processorRef_) + 1) + ".wav";
            break;
        case ExportType::Bus:
            defaultFileName = "master_mix.wav";
            break;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    auto chooser = std::make_shared<juce::FileChooser>(
        "Export Audio File",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(defaultFileName),
        "*.wav");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync(chooserFlags, [safeThis, exportType, chooser](const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        if (!file.hasFileExtension(".wav"))
            file = file.withFileExtension(".wav");

        // 从构造导出请求到启动导出线程的完整流程；目标文件已存在时先经主题确认再调用
        auto startExport = [safeThis, exportType, file]()
        {
            if (safeThis == nullptr)
                return;

            struct ExportRequest final
            {
                ExportType type{ ExportType::Bus };
                int trackId{ -1 };
                int placementIndex{ -1 };
                juce::String targetName;
            };

            ExportRequest request;
            request.type = exportType;

            switch (exportType)
            {
                case ExportType::SelectedClip:
                {
                    request.trackId = getStandaloneActiveTrack(safeThis->processorRef_);
                    request.placementIndex = getStandaloneSelectedPlacementIndex(safeThis->processorRef_, request.trackId);

                    if (request.placementIndex < 0)
                    {
                        ConfirmDialogContent::showMessage(
                            &safeThis->contentRoot_,
                            juce::String("Export Failed"),
                            juce::String("No audio clip is selected. Select a clip on the track first."));
                        return;
                    }

                    request.targetName = "Selected Placement (Track "
                        + juce::String(request.trackId + 1)
                        + ", Clip " + juce::String(request.placementIndex + 1) + ")";
                    break;
                }

                case ExportType::Track:
                {
                    request.trackId = getStandaloneActiveTrack(safeThis->processorRef_);
                    request.targetName = "Track " + juce::String(request.trackId + 1);
                    break;
                }

                case ExportType::Bus:
                {
                    request.targetName = "Bus (Master Mix)";
                    break;
                }
            }

            auto* processor = &safeThis->processorRef_;
            const auto outFile = file;
            const auto outRequest = request;
            const juce::Component::SafePointer<OpenTuneAudioProcessorEditor> uiSafe = safeThis;

            // Join previous export thread if it exists
            if (safeThis->exportWorker_.joinable())
            {
                safeThis->exportWorker_.join();
            }

            // Set export in progress flag
            safeThis->exportInProgress_.store(true);

            // Create new controlled export thread
            safeThis->exportWorker_ = std::thread([processor, outFile, outRequest, uiSafe]()
                {
                    bool ok = false;
                    juce::String errorText;

                    switch (outRequest.type)
                    {
                        case ExportType::SelectedClip:
                            ok = processor->exportPlacementAudio(outRequest.trackId, outRequest.placementIndex, outFile);
                            break;
                        case ExportType::Track:
                            ok = processor->exportTrackAudio(outRequest.trackId, outFile);
                            break;
                        case ExportType::Bus:
                            ok = processor->exportMasterMixAudio(outFile);
                            break;
                    }

                    if (!ok)
                    {
                        errorText = processor->getLastExportError();
                    }

                    juce::MessageManager::callAsync([ok, outFile, outRequest, errorText, uiSafe]()
                    {
                        // Check if editor is still alive
                        if (uiSafe == nullptr)
                            return;

                        // Clear export in progress flag
                        uiSafe->exportInProgress_.store(false);

                        if (ok)
                        {
                            DBG("Successfully exported " + outRequest.targetName);
                            ConfirmDialogContent::showMessage(
                                &uiSafe->contentRoot_,
                                juce::String::fromUTF8(u8"\u5BFC\u51FA\u5B8C\u6210"),
                                outRequest.targetName + juce::String::fromUTF8(u8" \u5DF2\u5BFC\u51FA\u5230: ") + outFile.getFullPathName());
                            return;
                        }

                        juce::String failText = juce::String::fromUTF8(u8"\u65E0\u6CD5\u5BFC\u51FA\u97F3\u9891\u5230 ") + outFile.getFullPathName();
                        if (errorText.isNotEmpty())
                        {
                            failText += juce::String::fromUTF8(u8"\n\u539F\u56E0: ") + errorText;
                        }

                        ConfirmDialogContent::showMessage(
                            &uiSafe->contentRoot_,
                            juce::String::fromUTF8(u8"\u5BFC\u51FA\u5931\u8D25"),
                            failText);
                    });
                });
        };

        if (file.existsAsFile())
        {
            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Overwrite Existing File?"),
                    juce::String("The target file already exists. Overwrite it?"),
                    { { juce::String::fromUTF8(u8"\u8986\u76D6"), startExport, true },
                      { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                &safeThis->contentRoot_);
            return;
        }

        startExport();
    });
}

void OpenTuneAudioProcessorEditor::saveProjectRequested()
{
    if (!projectSession_.hasProjectPath()) {
        saveProjectAsRequested();
        return;
    }
    saveProject(juce::File());
}

void OpenTuneAudioProcessorEditor::openProjectRequested()
{
    if (rejectProjectOperationIfBusy())
        return;

    if (!projectSession_.isDirty()) {
        launchOpenProjectChooser();
        return;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    ConfirmDialogContent::launch(
        new ConfirmDialogContent(
            juce::String::fromUTF8(u8"\u5F53\u524D\u5DE5\u7A0B\u5C1A\u672A\u4FDD\u5B58"),
            juce::String::fromUTF8(u8"\u6253\u5F00\u5176\u4ED6\u5DE5\u7A0B\u524D\uFF0C\u662F\u5426\u4FDD\u5B58\u5F53\u524D\u5DE5\u7A0B\u7684\u66F4\u6539\uFF1F"),
            {               { juce::String::fromUTF8(u8"\u4FDD\u5B58"), [safeThis] {
                    if (safeThis == nullptr) return;
                    if (!safeThis->projectSession_.hasProjectPath()) {
                        safeThis->saveProjectAsThenOpenProject();
                        return;
                    }
                    safeThis->saveProject(juce::File(), true);
                }, true },
              { juce::String("Do Not Save"), [safeThis] {
                    if (safeThis == nullptr) return;
                    safeThis->launchOpenProjectChooser();
                }, false },
              { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
        &contentRoot_);
}

void OpenTuneAudioProcessorEditor::launchOpenProjectChooser()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"\u6253\u5F00\u5DE5\u7A0B"), juce::File(), "*.otproj");
    const auto chooserFlags = juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;

        safeThis->openProjectFile(file);
    });
}

void OpenTuneAudioProcessorEditor::saveProjectAsThenOpenProject()
{
    if (rejectProjectOperationIfBusy())
        return;

    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"), juce::File(), "*.otproj");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;
        if (!file.hasFileExtension(".otproj"))
            file = file.withFileExtension(".otproj");

        if (file.existsAsFile()) {
            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Overwrite Existing Project?"),
                    juce::String("The target project file already exists. Overwrite it?"),
                    { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, file] {
                            if (safeThis == nullptr) return;
                            safeThis->saveProject(file, true);
                        }, true },
                      { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                &safeThis->contentRoot_);
            return;
        }

        safeThis->saveProject(file, true);
    });
}

void OpenTuneAudioProcessorEditor::preferencesRequested()
{
    showPreferencesDialog();
}

void OpenTuneAudioProcessorEditor::showPreferencesDialog()
{
    auto* holder = juce::StandalonePluginHolder::getInstance();
    auto onVocoderModelWeightChanged = [this](VocoderModelWeight weight) {
        processorRef_.setVocoderModelWeight(weight);
    };
    auto onF0ModelChanged = [this](F0ModelType type) {
        return processorRef_.setF0ModelType(type);
    };
    auto onLightPitchCorrectionChanged = [this](bool) {
        processorRef_.invalidateAllContentCaches();
    };
    auto pages = StandalonePreferencePages::createAudioPages(
        holder != nullptr ? &holder->deviceManager : nullptr,
        appPreferences_,
        [this] { syncSharedAppPreferences(); },
        [this](bool forceCpu) { processorRef_.resetInferenceBackend(forceCpu); },
        std::move(onVocoderModelWeightChanged),
        std::move(onF0ModelChanged),
        std::move(onLightPitchCorrectionChanged));

    auto sharedPages = SharedPreferencePages::create(appPreferences_, [this] { syncSharedAppPreferences(); }, false);
    pages.insert(pages.end(),
                 std::make_move_iterator(sharedPages.begin()),
                 std::make_move_iterator(sharedPages.end()));

    auto standalonePages = StandalonePreferencePages::createStandaloneOnlyPages(appPreferences_, [this] {
        syncSharedAppPreferences();
    });
    pages.insert(pages.end(),
                 std::make_move_iterator(standalonePages.begin()),
                 std::make_move_iterator(standalonePages.end()));

    auto* dialogContent = new TabbedPreferencesDialog(std::move(pages));
    dialogContent->setDialogParent(&contentRoot_);

    // 根据当前屏幕可用区域计算对话框尺寸，适配不同显示器和分辨率
    const auto usable = getParentMonitorArea();
    const int maxW = juce::jmin(640, usable.getWidth() - 48);
    const int maxH = juce::jmin(560, usable.getHeight() - 48);
    dialogContent->setSize(juce::jmax(480, maxW), juce::jmax(480, maxH));

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialogContent);
    options.dialogTitle = "Preferences";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.useBottomRightCornerResizer = true;
    // 独立弹窗以 transformed content root 为锚点（与 VST3 版一致，§7.4）。
    options.componentToCentreAround = &contentRoot_;
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::helpRequested()
{
#if JUCE_MAC
    // macOS: docs are in Contents/Resources/docs/ (executable is in Contents/MacOS/)
    auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeDir = exeFile.getParentDirectory();
    auto helpFile = exeDir.getParentDirectory().getChildFile("Resources").getChildFile("docs").getChildFile("UserGuide.html");
#else
    // Windows: docs are alongside the executable
    auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto exeDir = exeFile.getParentDirectory();
    auto helpFile = exeDir.getChildFile("docs").getChildFile("UserGuide.html");
#endif
    
    if (helpFile.exists())
    {
        helpFile.startAsProcess();
    }
    else
    {
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            LOC(kClose),
            juce::String("Help file not found: ") + helpFile.getFullPathName()
        );
    }
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

    if (themeId == ThemeId::Aurora)
    {
        setLookAndFeel(&auroraLookAndFeel_);
    }
    else
    {
        setLookAndFeel(&openTuneLookAndFeel_);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
        openTuneLookAndFeel_.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
        openTuneLookAndFeel_.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
    }

    // Install process-wide default so independent DialogWindow title bars and
    // ordinary JUCE controls use the project theme.
    AuroraLookAndFeel::installAsDefault();

    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, UIColors::backgroundDark);

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        window->setColour(juce::DocumentWindow::backgroundColourId, UIColors::backgroundMedium);
        window->repaint();
    }

    topBar_.applyTheme();
    topBar_.setSidePanelsVisible(isTrackPanelVisible_, isParameterPanelVisible_);
    trackPanel_.applyTheme();
    parameterPanel_.applyTheme();

// Sync playhead color to high-performance playhead overlay
    pianoRoll_.setPlayheadColour(UIColors::playhead);
    arrangementView_.setPlayheadColour(UIColors::playhead);

    sendLookAndFeelChange();
    pianoRoll_.requestThemeRedraw();
    arrangementView_.requestThemeRedraw();
    overviewStrip_.repaint();
    repaint();
}

void OpenTuneAudioProcessorEditor::mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme)
{
    if (appPreferences_.getState().standalone.mouseTrailTheme != theme) {
        appPreferences_.setMouseTrailTheme(theme);
    }

    syncSharedAppPreferences();
    menuBar_.repaint();
    rippleOverlay_.repaint();
}

void OpenTuneAudioProcessorEditor::cursorStyleChanged(CursorStyleId style)
{
    if (appPreferences_.getState().standalone.cursorStyle != style)
        appPreferences_.setCursorStyle(style);

    syncSharedAppPreferences();
    menuBar_.repaint();
    juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
}

void OpenTuneAudioProcessorEditor::trackColorModeChanged(TrackColorMode mode)
{
    appPreferences_.setTrackColorMode(mode);
    trackPanel_.setTrackColorMode(mode);
    menuBar_.setTrackColorMode(mode);
}

void OpenTuneAudioProcessorEditor::performUndoRedoAction(bool isUndo)
{
    auto* action = isUndo
        ? processorRef_.getUndoManager().undo()
        : processorRef_.getUndoManager().redo();
    if (action == nullptr)
        return;

    projectSession_.markDirty();
    arrangementView_.requestContentRedraw();
    referenceRefreshPending_ = true;
    refreshReferenceContext();
}

void OpenTuneAudioProcessorEditor::undoRequested() { performUndoRedoAction(true); }

void OpenTuneAudioProcessorEditor::redoRequested() { performUndoRedoAction(false); }

void OpenTuneAudioProcessorEditor::languageChanged(Language newLanguage)
{
    juce::ignoreUnused(newLanguage);
    
// Refresh menu bar - JUCE requires menuItemsChanged() to rebuild menu
    menuBar_.menuItemsChanged();
    menuBar_.repaint();
    
// Refresh top toolbar
    transportBar_.refreshLocalizedText();
    topBar_.refreshLocalizedText();
    
// Refresh parameter panel
    parameterPanel_.refreshLocalizedText();
    
// Refresh entire UI
    repaint();
}

// ============================================================================
// TransportBarComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::playRequested()
{
    processorRef_.play();
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Play);
    transportBar_.setPlaying(true);
    // PianoRoll and ArrangementView read processor-owned PlayHeadState directly.
}

void OpenTuneAudioProcessorEditor::pauseRequested()
{
    processorRef_.pause();
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Pause);
    transportBar_.setPlaying(false);
    // PianoRoll and ArrangementView read processor-owned PlayHeadState directly.
}

void OpenTuneAudioProcessorEditor::stopRequested()
{
    processorRef_.stop();
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Stop);
    transportBar_.setPlaying(false);
    // PianoRoll and ArrangementView read processor-owned PlayHeadState directly.
}

void OpenTuneAudioProcessorEditor::loopToggled(bool enabled)
{
    // Standalone: write directly to processor-owned PlayHeadState via setter.
    processorRef_.setLoopEnabled(enabled);
    transportBar_.setLoopEnabled(enabled);
}

void OpenTuneAudioProcessorEditor::bpmChanged(double newBpm)
{
    // Write raw value to processor (canonical truth)
    processorRef_.setBpm(newBpm);

    // Read back canonical value from processor
    double canonicalBpm = processorRef_.getBpm();
    lastSyncedBpm_ = canonicalBpm;

    // Sync to UI
    transportBar_.setBpm(canonicalBpm);
    pianoRoll_.setBpm(canonicalBpm);
    // ArrangementView reads from processor directly

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::timeSignatureChanged(int numerator, int denominator)
{
    // Write raw value to processor (canonical truth)
    processorRef_.setTimeSignature(numerator, denominator);

    // Read back canonical value from processor
    int canonicalNum = processorRef_.getTimeSigNumerator();
    int canonicalDenom = processorRef_.getTimeSigDenominator();
    lastSyncedTimeSigNum_ = canonicalNum;
    lastSyncedTimeSigDenom_ = canonicalDenom;

    // Sync to UI
    transportBar_.setTimeSignature(canonicalNum, canonicalDenom);
    pianoRoll_.setTimeSignature(canonicalNum, canonicalDenom);
    // ArrangementView reads from processor directly

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::scaleChanged(int rootNote, int scaleType)
{
    if (suppressScaleChangedCallback_) {
        return;
    }

    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int activePlacementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    const ContentKey activeContentKey = getStandaloneContentKey(processorRef_, activeTrack, activePlacementIndex);

    const int newRoot = juce::jlimit(0, 11, rootNote);
    const int newScaleType = juce::jlimit(1, 8, scaleType);

    const DetectedKey oldResolved = resolveScaleForPlacementContent(activeTrack, activePlacementIndex, nullptr);
    const int oldRootNote = static_cast<int>(oldResolved.root);
    const int oldScaleType = OpenTune::scaleToUiScaleType(oldResolved.scale);

    // 已落写的 Manual 且 root/scale 未变：仅回显 UI，不重复写入。
    // 当前为 Automatic/Unset 时用户再次选择相同值 = 显式手动确认，必须走落写路径。
    if (oldRootNote == newRoot && oldScaleType == newScaleType && oldResolved.origin == Origin::Manual) {
        applyScaleToUi(newRoot, newScaleType);
        return;
    }

    const DetectedKey newKey = OpenTune::makeDetectedKeyFromUi(newRoot, newScaleType);

    if (activeContentKey.isValid()) {
        processorRef_.setContentDetectedKey(activeContentKey, newKey);
    }
    applyScaleToUi(newRoot, newScaleType);

    DBG("ScaleSyncTrace: source=manual trackId=" + juce::String(activeTrack)
        + " placementIndex=" + juce::String(activePlacementIndex)
        + " contentKey.objectId=" + juce::String(static_cast<juce::int64>(activeContentKey.objectId))
        + " root=" + juce::String(newRoot)
        + " scale=" + juce::String(newScaleType));

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::timelineDisplayModeChanged(TimelineDisplayMode mode)
{
    timelineDisplayMode_ = mode;
    appPreferences_.setTimelineDisplayMode(mode);
    transportBar_.setTimelineDisplayMode(mode);
    pianoRoll_.setTimelineDisplayMode(mode);
    arrangementView_.setTimelineDisplayMode(mode);
}

void OpenTuneAudioProcessorEditor::viewToggled(bool workspaceView)
{
    // Capture the timeline camera from the outgoing view before toggling visibility.
    const TimelineViewportCamera camera = isWorkspaceView_
        ? arrangementView_.timelineCamera()
        : pianoRoll_.timelineCamera();

    // 进入 PianoRoll 前先结算一次 heartbeat：隐藏状态下也能在 preserve 判定前
    // 收束 Failed/Ready-无目标 的 pending F0 初始视图请求。
    if (!workspaceView)
        pianoRoll_.onHeartbeatTick();

    // F0 Ready 初始视图定位优先于编排相机转移：pending 请求在 setVisible →
    // visibilityChanged → tryConsumeInitialF0View 中消费并定位到 F0 首帧（经当前投影
    // 映射到位移后的时间轴位置）。若此时存在未消费请求，跳过编排相机覆盖，保留
    // F0 定位结果；否则维持"切换保持时间轴位置"行为。
    const bool preserveInitialF0View = !workspaceView && pianoRoll_.hasPendingInitialF0View();

    isWorkspaceView_ = workspaceView;
    arrangementView_.setVisible(isWorkspaceView_);
    pianoRoll_.setVisible(!isWorkspaceView_);
    overviewStrip_.setVisible(!isWorkspaceView_);

    // Explicitly grab focus for the active view to ensure keyboard shortcuts work immediately
    if (isWorkspaceView_) {
        arrangementView_.grabKeyboardFocus();
    } else {
        pianoRoll_.grabKeyboardFocus();
    }

    // Push the captured camera into the incoming view so the timeline viewport survives the switch.
    // activateTimelineCamera rebuilds coverage, repositions the tile canvas, and repaints.
    // The now-hidden view is left untouched, so playback never replays camera into it.
    if (isWorkspaceView_)
        arrangementView_.activateTimelineCamera(camera);
    else if (!preserveInitialF0View)
        pianoRoll_.activateTimelineCamera(camera);

    resized();
    repaint();
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

// ============================================================================
// TrackPanelComponent::Listener Implementation
// ============================================================================

void OpenTuneAudioProcessorEditor::trackSelected(int trackId)
{
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t placementId = (placementIndex >= 0) ? processorRef_.getPlacementId(trackId, placementIndex) : 0;
    applyPlacementSelectionContext(trackId, placementId);
}

void OpenTuneAudioProcessorEditor::trackMuteToggled(int trackId, bool muted)
{
    setStandaloneTrackMuted(processorRef_, trackId, muted);
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackSoloToggled(int trackId, bool solo)
{
    setStandaloneTrackSolo(processorRef_, trackId, solo);
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackVolumeChanged(int trackId, float volume)
{
    if (trackId < 0 || trackId >= OpenTuneAudioProcessor::MAX_TRACKS) return;
    
    setStandaloneTrackVolume(processorRef_, trackId, volume);
    lastTrackVolumes_[static_cast<size_t>(trackId)] = volume;
    projectSession_.markDirty();
}

// Y-axis zoom sync: when TrackPanel or ArrangementView zooms via Ctrl+scrollwheel, sync the other component
void OpenTuneAudioProcessorEditor::trackHeightChanged(int newHeight)
{
// Update track height in processor
    const bool needsArrangementRebuild = processorRef_.getTrackHeight() != newHeight;
    processorRef_.setTrackHeight(newHeight);
    
// Sync TrackPanel if not triggered by it
    if (trackPanel_.getTrackHeight() != newHeight)
    {
        trackPanel_.setTrackHeight(newHeight);
    }
    
    // 鍒锋柊ArrangementView
    if (needsArrangementRebuild)
        arrangementView_.requestContentRedraw();
}

void OpenTuneAudioProcessorEditor::visibleTrackCountChanged(int newCount)
{
    arrangementView_.setVisibleTrackCount(newCount);
}

void OpenTuneAudioProcessorEditor::trackColorChangeRequested(int trackId)
{
    // Wrapper component: holds ColourSelector, applies result when dialog closes via destructor
    struct ColourPickerContent : public juce::Component,
                                 private juce::ComponentListener
    {
        ColourPickerContent(OpenTuneAudioProcessorEditor* owner, int tid, juce::Colour current)
            : owner_(owner), trackId_(tid)
        {
            if (owner_ != nullptr)
                owner_->addComponentListener(this);

            selector_ = std::make_unique<juce::ColourSelector>(
                juce::ColourSelector::showColourAtTop |
                juce::ColourSelector::showSliders |
                juce::ColourSelector::showColourspace);
            selector_->setCurrentColour(current);
            selector_->setSize(380, 300);
            addAndMakeVisible(selector_.get());
        }

        ~ColourPickerContent() override
        {
            if (owner_ != nullptr)
                owner_->removeComponentListener(this);

            if (selector_ && owner_ != nullptr)
            {
                juce::Colour selected = selector_->getCurrentColour();
                setStandaloneTrackColour(owner_->processorRef_, trackId_, selected);
                owner_->trackPanel_.setTrackColour(trackId_, selected);
                owner_->arrangementView_.requestContentRedraw();
                owner_->projectSession_.markDirty();
            }
        }

        void resized() override
        {
            if (selector_)
                selector_->setBounds(getLocalBounds());
        }

        /** 非原生标题栏时移除 DialogWindow 默认标题栏，仅保留主题内容 */
        void parentHierarchyChanged() override
        {
            if (auto* dialogWindow = findParentComponentOfClass<juce::DialogWindow>())
            {
                if (! dialogWindow->isUsingNativeTitleBar())
                {
                    const int contentWidth = getWidth();
                    const int contentHeight = getHeight();
                    dialogWindow->setTitleBarHeight(0);
                    dialogWindow->setContentComponentSize(contentWidth, contentHeight);
                }
            }
        }

    private:
        void componentBeingDeleted(juce::Component&) override
        {
            if (owner_ != nullptr)
                owner_->removeComponentListener(this);
            owner_ = nullptr;

            if (auto* dialogWindow = findParentComponentOfClass<juce::DialogWindow>())
                dialogWindow->exitModalState(0);
        }

        juce::Component::SafePointer<OpenTuneAudioProcessorEditor> owner_;
        int trackId_;
        std::unique_ptr<juce::ColourSelector> selector_;
    };

    juce::Colour current = getStandaloneTrackColour(processorRef_, trackId);
    auto* content = new ColourPickerContent(this, trackId, current);
    content->setSize(380, 300);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle = "Track " + juce::String(trackId + 1) + " Color";
    opts.content.setOwned(content);
    opts.dialogBackgroundColour = UIColors::backgroundDark;
    opts.componentToCentreAround = &contentRoot_;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.launchAsync();
}

void OpenTuneAudioProcessorEditor::trackAddRequested()
{
    const int current = trackPanel_.getVisibleTrackCount();
    if (current >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    trackPanel_.setVisibleTrackCount(current + 1);
    arrangementView_.setVisibleTrackCount(current + 1);
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackDuplicateRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    const int visibleCount = trackPanel_.getVisibleTrackCount();
    if (visibleCount >= OpenTuneAudioProcessor::MAX_TRACKS)
        return;

    const int targetSlot = visibleCount;

    // Copy track mix state via public API
    arrangement->setTrackMuted(targetSlot, arrangement->isTrackMuted(trackId));
    arrangement->setTrackSolo(targetSlot, arrangement->isTrackSolo(trackId));
    arrangement->setTrackVolume(targetSlot, arrangement->getTrackVolume(trackId));
    arrangement->setTrackColour(targetSlot, arrangement->getTrackColour(trackId));

    // Copy placements
    const int numPlacements = arrangement->getNumPlacements(trackId);
    for (int i = 0; i < numPlacements; ++i) {
        StandaloneArrangement::Placement p;
        if (arrangement->getPlacementByIndex(trackId, i, p)) {
            p.placementId = 0;  // Let insertPlacement assign a new ID
            arrangement->insertPlacement(targetSlot, p);
        }
    }

    // Expand visible count to show the new track
    trackPanel_.setVisibleTrackCount(visibleCount + 1);
    arrangementView_.setVisibleTrackCount(visibleCount + 1);
    trackPanel_.setTrackColour(targetSlot, arrangement->getTrackColour(trackId));
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackDeleteRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    const int visibleCount = trackPanel_.getVisibleTrackCount();
    if (visibleCount <= 1)
        return; // 鑷冲皯淇濈暀涓€鏉¤建閬?

    if (trackId < 0 || trackId >= visibleCount)
        return;

// Atomically shift subsequent tracks up, clear the last slot
    arrangement->removeTrackAndShift(trackId, visibleCount);

// Sync TrackPanel UI state (color, mute/solo/volume)
    const int newVisibleCount = visibleCount - 1;
    for (int i = 0; i < newVisibleCount; ++i) {
        trackPanel_.setTrackMuted(i, arrangement->isTrackMuted(i));
        trackPanel_.setTrackSolo(i, arrangement->isTrackSolo(i));
        trackPanel_.setTrackVolume(i, arrangement->getTrackVolume(i));
        trackPanel_.setTrackColour(i, arrangement->getTrackColour(i));
    }

// Reduce visible track count (triggers resized + repaint + listener notification)
    trackPanel_.setVisibleTrackCount(newVisibleCount);
    arrangementView_.setVisibleTrackCount(newVisibleCount);
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::trackColorRandomizeRequested(int trackId)
{
    auto* arrangement = processorRef_.getStandaloneArrangement();
    if (arrangement == nullptr)
        return;

    // 四组随机颜色策略，与当前色相同时取同组下一色
    const auto currentColour = arrangement->getTrackColour(trackId);
    const auto newColour = pickTrackRandomColor(currentColour);

    arrangement->setTrackColour(trackId, newColour);
    trackPanel_.setTrackColour(trackId, newColour);
    arrangementView_.requestContentRedraw();
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::placementSelectionChanged(int trackId, uint64_t placementId)
{
    applyPlacementSelectionContext(trackId, placementId);

    // 更新 reference context
    referenceRefreshPending_ = true;
    refreshReferenceContext();
}

void OpenTuneAudioProcessorEditor::placementTimingChanged(int trackId, int placementIndex)
{
    if (getStandaloneActiveTrack(processorRef_) == trackId
        && getStandaloneSelectedPlacementIndex(processorRef_, trackId) == placementIndex)
    {
        syncPianoRollFromPlacementSelection(trackId, placementIndex);
    }

    projectSession_.markDirty();
}

// Y-axis scroll sync: notify other component when ArrangementView or TrackPanel scrolls
void OpenTuneAudioProcessorEditor::verticalScrollChanged(int newOffset)
{
    // 同步TrackPanel
    trackPanel_.setVerticalScrollOffset(newOffset);
    // 同步ArrangementView
    arrangementView_.setVerticalScrollOffset(newOffset);
}

void OpenTuneAudioProcessorEditor::scrollModeChanged(bool isContinuous)
{
    pianoRoll_.setScrollMode(isContinuous
        ? PianoRollComponent::ScrollMode::Continuous
        : PianoRollComponent::ScrollMode::Page);
}



void OpenTuneAudioProcessorEditor::placementDoubleClicked(int trackId, int placementIndex)
{
    // 1. Switch to Piano Roll View
    if (isWorkspaceView_)
    {
        transportBar_.setWorkspaceView(false);
        viewToggled(false);
    }

    // 2. Select the placement
    placementSelectionChanged(trackId, processorRef_.getPlacementId(trackId, placementIndex));

}


// ============================================================================
// PianoRollComponent::Listener Implementation
// ============================================================================

bool OpenTuneAudioProcessorEditor::playheadPositionChangeRequested(double timeSeconds)
{
    // Standalone: canonical state is already synced via processorRef_.setPosition().
    processorRef_.setPosition(timeSeconds);
    processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Seek);
    return false;
}

void OpenTuneAudioProcessorEditor::playPauseToggleRequested()
{
    // Toggle play/pause
    if (processorRef_.isPlaying()) {
        pauseRequested();
    } else {
        playRequested();
    }
}

void OpenTuneAudioProcessorEditor::stopPlaybackRequested()
{
    stopRequested();
}

void OpenTuneAudioProcessorEditor::playFromStartToggleRequested()
{
    if (processorRef_.isPlaying()) {
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.pauseAtPosition(startPos);
        processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Pause);
        transportBar_.setPlaying(false);
        // PianoRoll and ArrangementView read processor-owned PlayHeadState directly.
    } else {
        double startPos = processorRef_.getPlayStartPosition();
        processorRef_.setPosition(startPos);
        processorRef_.play();
        processorRef_.recordControlCall(OpenTuneAudioProcessor::DiagnosticControlCall::Play);
        transportBar_.setPlaying(true);
        // PianoRoll and ArrangementView read processor-owned PlayHeadState directly.
    }
}

void OpenTuneAudioProcessorEditor::autoTuneRequested()
{
    const auto autoRefUiState = evaluateAutoRefUiState();
    if (autoRefUiState.shouldRunReferenceAuto()) {
        if (handleAutoRefExecute()) {
            projectSession_.markDirty();
        }
        return;
    }

    const auto result = pianoRoll_.applyAutoTuneToSelection();
    if (!result.applied()) {
        if (result.status != PianoRollComponent::AutoTuneApplyStatus::NoChange) {
            ConfirmDialogContent::showMessage(&contentRoot_,
                                              juce::String("AUTO"),
                                              result.message());
        }
        return;
    }

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::pitchShiftRequested()
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    if (placementIndex < 0) return;

    const ContentKey contentKey = getStandaloneContentKey(processorRef_, trackId, placementIndex);
    if (!contentKey.isValid()) return;

    const auto currentSettings = processorRef_.getPitchShiftSettings(contentKey);

    auto* content = new PitchShiftDialogContent(currentSettings);
    content->setDialogParent(&contentRoot_);

    auto commands = processorRef_.getContentCommands();
    content->setOnConfirm([safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this),
                           contentKey, currentSettings, commands](const PitchShiftSettings& newSettings) {
        if (safeThis == nullptr) return;
        if (newSettings != currentSettings) {
            auto action = commands->commitPitchShiftEdit(contentKey, newSettings);
            if (action != nullptr) {
                safeThis->processorRef_.getUndoManager().addAction(std::move(action));
                safeThis->projectSession_.markDirty();
            }
        }
    });

    content->setOnReset([safeThis = juce::Component::SafePointer<OpenTuneAudioProcessorEditor>(this),
                         contentKey, currentSettings, commands]() {
        if (safeThis == nullptr) return;
        const auto identity = PitchShiftSettings::identity();
        if (identity != currentSettings) {
            auto action = commands->commitPitchShiftEdit(contentKey, identity);
            if (action != nullptr) {
                safeThis->processorRef_.getUndoManager().addAction(std::move(action));
                safeThis->projectSession_.markDirty();
            }
        }
    });

    auto options = juce::DialogWindow::LaunchOptions();
    options.content.setOwned(content);
    options.dialogTitle = "Pitch Shift";
    options.dialogBackgroundColour = UIColors::backgroundDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = &contentRoot_;
    options.launchAsync();
}

void OpenTuneAudioProcessorEditor::pitchCurveEdited(int startFrame, int endFrame)
{
    DBG("Editor: Pitch curve edited frames " + juce::String(startFrame) + " to " + juce::String(endFrame));

    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::contentEdited()
{
    projectSession_.markDirty();
}

void OpenTuneAudioProcessorEditor::escapeKeyPressed()
{
// ESC equivalent to view toggle button: switch PianoRoll/ArrangementView and sync button state
    const bool targetWorkspaceView = !isWorkspaceView_;
    transportBar_.setWorkspaceView(targetWorkspaceView);
    viewToggled(targetWorkspaceView);
}

void OpenTuneAudioProcessorEditor::currentToolChanged(ToolId tool)
{
    parameterPanel_.setActiveTool(static_cast<int>(tool));
}

void OpenTuneAudioProcessorEditor::saveProjectAsRequested()
{
    if (rejectProjectOperationIfBusy())
        return;

    auto chooser = std::make_shared<juce::FileChooser>(
        juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"), juce::File(), "*.otproj");
    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);

    chooser->launchAsync(chooserFlags, [safeThis, chooser](const juce::FileChooser& fc) {
        if (safeThis == nullptr) return;
        auto file = fc.getResult();
        if (file == juce::File{}) return;
        if (!file.hasFileExtension(".otproj"))
            file = file.withFileExtension(".otproj");

        if (file.existsAsFile()) {
            ConfirmDialogContent::launch(
                new ConfirmDialogContent(
                    juce::String("Overwrite Existing Project?"),
                    juce::String("The target project file already exists. Overwrite it?"),
                    { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, file] {
                            if (safeThis == nullptr) return;
                            safeThis->saveProject(file);
                        }, true },
                      { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                &safeThis->contentRoot_);
            return; // Don't continue in outer callback — the inner callback handles save
        }

        safeThis->saveProject(file);
    });
}

void OpenTuneAudioProcessorEditor::openRecentProjectRequested(const juce::File& file)
{
    if (rejectProjectOperationIfBusy())
        return;

    if (!projectSession_.isDirty()) {
        openProjectFile(file);
        return;
    }

    juce::Component::SafePointer<OpenTuneAudioProcessorEditor> safeThis(this);
    ConfirmDialogContent::launch(
        new ConfirmDialogContent(
            juce::String::fromUTF8(u8"\u5F53\u524D\u5DE5\u7A0B\u5C1A\u672A\u4FDD\u5B58"),
            juce::String::fromUTF8(u8"\u6253\u5F00\u5176\u4ED6\u5DE5\u7A0B\u524D\uFF0C\u662F\u5426\u4FDD\u5B58\u5F53\u524D\u5DE5\u7A0B\u7684\u66F4\u6539\uFF1F"),
            {               { juce::String::fromUTF8(u8"\u4FDD\u5B58"), [safeThis, file] {
                    if (safeThis == nullptr) return;
                    if (!safeThis->projectSession_.hasProjectPath()) {
                        // No project path: async save-as, then open recent file
                        auto chooser = std::make_shared<juce::FileChooser>(
                            juce::String::fromUTF8(u8"\u4FDD\u5B58\u5DE5\u7A0B"),
                            juce::File(),
                            "*.otproj");
                        const auto chooserFlags = juce::FileBrowserComponent::saveMode
                                                 | juce::FileBrowserComponent::canSelectFiles;
                        chooser->launchAsync(chooserFlags, [safeThis, chooser, file](const juce::FileChooser& fc) {
                            if (safeThis == nullptr) return;
                            auto saveFile = fc.getResult();
                            if (saveFile == juce::File{}) return;
                            if (!saveFile.hasFileExtension(".otproj"))
                                saveFile = saveFile.withFileExtension(".otproj");

                            auto handleSaveAndOpen = [safeThis, file](const juce::File& saveFile) {
                                if (safeThis == nullptr) return;
                                safeThis->saveProject(saveFile, false, file);
                            };

                            if (saveFile.existsAsFile()) {
                                ConfirmDialogContent::launch(
                                    new ConfirmDialogContent(
                                        juce::String("Overwrite Existing Project?"),
                                        juce::String("The target project file already exists. Overwrite it?"),
                                        { { juce::String::fromUTF8(u8"\u8986\u76D6"), [safeThis, saveFile, handleSaveAndOpen] {
                                                handleSaveAndOpen(saveFile);
                                            }, true },
                                          { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
                                    &safeThis->contentRoot_);
                                return;
                            }
                            handleSaveAndOpen(saveFile);
                        });
                        return;
                    }
                    safeThis->saveProject(juce::File(), false, file);
                  }, true },
{ juce::String("Do Not Save"), [safeThis, file] {
                     if (safeThis == nullptr) return;
                     safeThis->openProjectFile(file);
                 }, false },
              { juce::String::fromUTF8(u8"\u53D6\u6D88"), nullptr, false } }),
        &contentRoot_);
}

void OpenTuneAudioProcessorEditor::clearRecentProjectsRequested()
{
    projectSession_.clearRecentProjects();
    syncRecentProjectsToMenu();
}

void OpenTuneAudioProcessorEditor::updateTitleWithProjectPath()
{
    const auto name = projectSession_.getProjectName();
    juce::String title = "OpenTune - " + name;
    if (projectSession_.isDirty()) {
        title += " *";
    }
    if (auto* dw = getTopLevelComponent()) {
        dw->setName(title);
    }
}

void OpenTuneAudioProcessorEditor::syncRecentProjectsToMenu()
{
    const auto files = projectSession_.getRecentProjects();
    menuBar_.setRecentProjects(files);
}

void OpenTuneAudioProcessorEditor::refreshAllUIFromProject()
{
    // Sync track panel with restored arrangement
    syncTrackColorsToPanel();
    for (int i = 0; i < OpenTuneAudioProcessor::MAX_TRACKS; ++i) {
        trackPanel_.setTrackMuted(i, getStandaloneTrackMuted(processorRef_, i));
        trackPanel_.setTrackSolo(i, getStandaloneTrackSolo(processorRef_, i));
        trackPanel_.setTrackVolume(i, getStandaloneTrackVolume(processorRef_, i));
    }
    const int visibleCount = trackPanel_.getVisibleTrackCount();
    trackPanel_.setVisibleTrackCount(visibleCount);

    // Sync arrangement view
    arrangementView_.setVisibleTrackCount(visibleCount);

    // Sync piano roll to current selection
    const int activeTrack = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, activeTrack);
    syncPianoRollFromPlacementSelection(activeTrack, placementIndex);
    arrangementView_.requestContentRedraw();
    referenceRefreshPending_ = true;
    refreshReferenceContext();
}

// ============================================================================
// Reference Auto-Align Methods
// ============================================================================

// Look up a reference placement's content across all tracks.
static bool findReferencePlacementInfo(OpenTuneAudioProcessor& processor,
                                       uint64_t refPlacementId,
                                       StandaloneArrangement::Placement& out)
{
    for (int t = 0; t < MAX_TRACKS; ++t) {
        if (processor.getPlacementById(t, refPlacementId, out)) {
            return true;
        }
    }
    return false;
}

OpenTuneAudioProcessorEditor::AutoRefUiState OpenTuneAudioProcessorEditor::evaluateAutoRefUiState() const
{
    AutoRefUiState uiState;
    uiState.presentation.mode = ParameterPanel::AutoButtonPresentation::Mode::StandardAuto;
    uiState.presentation.tooltip = juce::String("Auto tune to nearby notes");

    // OpenDyne 模式下 AUTO 按钮永远走 snap-to-scale，不走 Ref 路径
    if (AudioEditingScheme::usesNotesPrimaryScheme(appliedAudioEditingScheme_)) {
        return uiState;
    }

    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    const uint64_t targetPlacementId = placementIndex >= 0
        ? processorRef_.getPlacementId(trackId, placementIndex)
        : 0;

    const auto preferencesState = appPreferences_.getState();
    const bool experimentalFeaturesEnabled = preferencesState.shared.experimentalFeaturesEnabled;

    uiState.availability = processorRef_.queryAutoRefAvailability(targetPlacementId);
    if (!experimentalFeaturesEnabled) {
        return uiState;
    }

    if (uiState.availability.status == OpenTuneAudioProcessor::AutoRefAvailability::Status::Ready) {
        uiState.presentation.mode = ParameterPanel::AutoButtonPresentation::Mode::ReferenceAuto;
        uiState.presentation.tooltip = juce::String("Auto tune to the reference clip");
        return uiState;
    }

    return uiState;
}

void OpenTuneAudioProcessorEditor::refreshReferenceContext()
{
    const auto autoRefUiState = evaluateAutoRefUiState();
    parameterPanel_.setAutoButtonPresentation(autoRefUiState.presentation);

    if (!autoRefUiState.availability.hasReferenceBinding()) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        referenceRefreshPending_ = false;
        return;
    }

    const auto preferencesState = appPreferences_.getState();
    const bool experimentalFeaturesEnabled = preferencesState.shared.experimentalFeaturesEnabled;
    if (!experimentalFeaturesEnabled) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        referenceRefreshPending_ = false;
        return;
    }

    const int targetTrackId = getStandaloneActiveTrack(processorRef_);
    StandaloneArrangement::Placement targetPlacement;
    if (!processorRef_.getPlacementById(targetTrackId,
                                        autoRefUiState.availability.targetPlacementId,
                                        targetPlacement)) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        referenceRefreshPending_ = false;
        return;
    }

    // Resolve reference content
    StandaloneArrangement::Placement refPlacement;
    if (!findReferencePlacementInfo(processorRef_, autoRefUiState.availability.referencePlacementId, refPlacement)) {
        pianoRoll_.setReferenceOverlay(std::nullopt);
        referenceRefreshPending_ = false;
        return;
    }

    const auto targetPreheat = processorRef_.preheatReferenceAlignmentFeatures(targetPlacement.contentKey);
    const auto referencePreheat = processorRef_.preheatReferenceAlignmentFeatures(refPlacement.contentKey);
    const auto isPending = [](OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus status) {
        return status == OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::Queued
            || status == OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::WaitingForSource;
    };
    referenceRefreshPending_ = isPending(targetPreheat) || isPending(referencePreheat);

    // If reference features are ready, set up piano roll overlay
    if (refPlacement.contentKey.isValid()) {
        const auto refSnapshot = processorRef_.getContentSnapshot(refPlacement.contentKey);
        const ReferenceFeatureSet refFeatures = refSnapshot
            ? refSnapshot->referenceFeatures
            : ReferenceFeatureSet{};
        if (refSnapshot != nullptr
            && refFeatures.isReady()
            && (refFeatures.producer == ReferenceFeatureProducer::StandardAuto
                || refFeatures.producer == ReferenceFeatureProducer::Game))
        {
            PianoRollRenderer::ReferenceOverlay overlay;
            const double visibleSourceStart = refPlacement.clipInSeconds;
            const double visibleSourceEnd = visibleSourceStart + refPlacement.durationSeconds;
            for (const auto& note : refFeatures.pitch.notes) {
                Note visibleNote = note;
                visibleNote.startTime = std::max(visibleNote.startTime, visibleSourceStart);
                visibleNote.endTime = std::min(visibleNote.endTime, visibleSourceEnd);
                if (visibleNote.endTime > visibleNote.startTime)
                    overlay.ghostNotes.push_back(std::move(visibleNote));
            }
            overlay.ghostColour = juce::Colours::steelblue;
            overlay.enabled = true;
            overlay.sourceProjection = makePianoRollProjection(refPlacement, processorRef_);
            overlay.timeGrid = refSnapshot->timeGrid;
            pianoRoll_.setReferenceOverlay(overlay);
            referenceRefreshPending_ = referenceRefreshPending_
                || processorRef_.getReferenceFeatures(targetPlacement.contentKey).status
                    == ReferenceFeatureStatus::Extracting;
        } else {
            pianoRoll_.setReferenceOverlay(std::nullopt);
            referenceRefreshPending_ = referenceRefreshPending_
                || refFeatures.status == ReferenceFeatureStatus::Extracting;
        }
    } else {
        pianoRoll_.setReferenceOverlay(std::nullopt);
    }
}

void OpenTuneAudioProcessorEditor::referenceButtonClicked(int trackId, uint64_t placementId,
                                                           juce::Rectangle<int> buttonScreenArea)
{
    if (!appPreferences_.getState().shared.experimentalFeaturesEnabled) {
        return;
    }

    resolveReferenceBindingMenu(trackId, placementId, buttonScreenArea);
}

void OpenTuneAudioProcessorEditor::resolveReferenceBindingMenu(int trackId, uint64_t targetPlacementId,
                                                                juce::Rectangle<int> buttonScreenArea)
{
    if (!appPreferences_.getState().shared.experimentalFeaturesEnabled) {
        return;
    }

    auto* arrangement = processorRef_.getStandaloneArrangement();
    juce::PopupMenu menu;

// Get target placement info
    StandaloneArrangement::Placement targetPlacement;
    if (!processorRef_.getPlacementById(trackId, targetPlacementId, targetPlacement)) {
        return;
    }

// Check if already has reference binding
    const uint64_t existingRef = arrangement->getPlacementReferencePlacement(trackId, targetPlacementId);
    if (existingRef != 0) {
        menu.addItem(juce::String::fromUTF8(u8"\u4E0D\u4F7F\u7528\u53C2\u8003Clip"), [this, arrangement, trackId, targetPlacementId]() {
            const uint64_t beforeReference = arrangement->getPlacementReferencePlacement(trackId, targetPlacementId);
            if (beforeReference == 0)
                return;
            if (!arrangement->setPlacementReferencePlacement(trackId, targetPlacementId, 0)) {
                ConfirmDialogContent::showMessage(
                    &contentRoot_,
                    juce::String::fromUTF8(u8"\u53C2\u8003 Clip"),
                    juce::String::fromUTF8(u8"\u65E0\u6CD5\u6E05\u9664\u5F53\u524D\u53C2\u8003 Clip \u7ED1\u5B9A\u3002"));
                return;
            }
            processorRef_.getUndoManager().addAction(std::make_unique<ReferenceBindingAction>(
                processorRef_, trackId, targetPlacementId, beforeReference, 0));
            projectSession_.markDirty();
            arrangementView_.requestContentRedraw();
            referenceRefreshPending_ = true;
            refreshReferenceContext();
        });
        menu.addSeparator();
    }

// Submenu: Select Reference Clip (list other clips in same view, excluding self)
    juce::PopupMenu refMenu;
    bool hasCandidates = false;

    for (int t = 0; t < MAX_TRACKS; ++t) {
        const int numPlacements = arrangement->getNumPlacements(t);
        for (int pi = 0; pi < numPlacements; ++pi) {
            StandaloneArrangement::Placement candidate;
            if (!processorRef_.getPlacementByIndex(t, pi, candidate)) continue;
            if (candidate.placementId == targetPlacementId) continue; // 排除自身
            if (candidate.isRetired) continue;

            if (!arrangement->canSetPlacementReferencePlacement(
                    trackId, targetPlacementId, candidate.placementId)) continue;

            hasCandidates = true;
            const juce::String label = juce::String("Track ") + juce::String(t + 1)
                + " - " + (candidate.name.isNotEmpty() ? candidate.name : "Clip")
                + juce::String(" (Mat#") + juce::String(static_cast<juce::int64>(candidate.contentKey.objectId)) + ")";
            refMenu.addItem(label, [this, arrangement, trackId, targetPlacementId, candidate]() {
                const uint64_t beforeReference = arrangement->getPlacementReferencePlacement(trackId, targetPlacementId);
                if (beforeReference == candidate.placementId)
                    return;
                if (!arrangement->setPlacementReferencePlacement(
                        trackId, targetPlacementId, candidate.placementId)) {
                    ConfirmDialogContent::showMessage(
                        &contentRoot_,
                        juce::String::fromUTF8(u8"\u53C2\u8003 Clip"),
                        juce::String::fromUTF8(u8"\u8BE5 Clip \u5DF2\u4E0D\u6EE1\u8DB3\u53C2\u8003\u7ED1\u5B9A\u6761\u4EF6\u3002"));
                    return;
                }
                processorRef_.getUndoManager().addAction(std::make_unique<ReferenceBindingAction>(
                    processorRef_, trackId, targetPlacementId,
                    beforeReference, candidate.placementId));
                projectSession_.markDirty();
                arrangementView_.requestContentRedraw();
                referenceRefreshPending_ = true;
                refreshReferenceContext();
            });
        }
    }

    if (hasCandidates) {
        menu.addSubMenu(juce::String::fromUTF8(u8"\u9009\u62E9\u53C2\u8003Clip"), refMenu);
    } else {
        menu.addItem(juce::String::fromUTF8(u8"(\u65E0\u53EF\u7528\u7684\u53C2\u8003Clip)"), false, false, nullptr);
    }

    if (buttonScreenArea.isEmpty())
    {
        const auto localAnchor = juce::Rectangle<float>(
            0.0f,
            static_cast<float>(arrangementView_.getHeight()),
            200.0f,
            1.0f);
        buttonScreenArea = arrangementView_.localAreaToGlobal(localAnchor).toNearestInt();
    }
    menu.showMenuAsync(juce::PopupMenu::Options()
        .withTargetScreenArea(buttonScreenArea)
        .withParentComponent(&contentRoot_));
}

bool OpenTuneAudioProcessorEditor::handleAutoRefExecute()
{
    const int trackId = getStandaloneActiveTrack(processorRef_);
    const int placementIndex = getStandaloneSelectedPlacementIndex(processorRef_, trackId);
    if (placementIndex < 0) {
        return false;
    }

    const uint64_t targetPlacementId = processorRef_.getPlacementId(trackId, placementIndex);
    auto result = processorRef_.executeReferenceAlignmentForPlacement(targetPlacementId);
    if (!result.succeeded()) {
        if (result.status == OpenTuneAudioProcessor::ReferenceAlignmentResult::Status::TargetAnalysisNotReady
            || result.status == OpenTuneAudioProcessor::ReferenceAlignmentResult::Status::ReferenceAnalysisNotReady) {
            referenceRefreshPending_ = true;
        }
        const juce::String message = result.message.isNotEmpty()
            ? result.message
            : juce::String::fromUTF8(u8"AUTO Ref alignment failed.");
        ConfirmDialogContent::showMessage(
            &contentRoot_,
            juce::String::fromUTF8(u8"AUTO Ref"),
            message);
        refreshReferenceContext();
        return false;
    }

    syncPianoRollFromPlacementSelection(trackId, placementIndex);
    referenceRefreshPending_ = true;
    refreshReferenceContext();
    return true;
}

} // namespace OpenTune
