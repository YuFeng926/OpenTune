/**
 * OpenTuneThemeScreenshots — 后台静默主题截图工具 + F0 初始视图消费回归。
 *
 * 截图部分无窗口、单线程、同步执行：AppPreferences(临时目录) → OpenTuneAudioProcessor
 * → OpenTuneAudioProcessorEditor，对整窗与各直接子组件软件渲染截图输出 BMP。
 *
 * F0 回归在截图后运行：临时 addToDesktop + setVisible，走真实 viewToggled →
 * visibilityChanged 消费路径，断言只用生产公共 API，不复制投影实现，结束 removeFromDesktop。
 *
 * 用法:
 *   OpenTuneThemeScreenshots [theme] [outputDir]
 *     theme      overdose(默认) | aurora | bluebreeze | darkbluegrey
 *     outputDir  默认 .opencode/screenshots (相对当前工作目录)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Standalone/PluginEditor.h"
#include "Standalone/UI/PianoRollComponent.h"
#include "UI/ThemeTokens.h"
#include "Utils/AppPreferences.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"
#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "StandaloneArrangement.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
void step(const char* msg)
{
    std::printf("[shots] %s\n", msg);
    std::fflush(stdout);
}
} // namespace

namespace {

OpenTune::ThemeId parseThemeId(const juce::String& themeName)
{
    if (themeName == "aurora")
        return OpenTune::ThemeId::Aurora;
    if (themeName == "bluebreeze")
        return OpenTune::ThemeId::BlueBreeze;
    if (themeName == "darkbluegrey")
        return OpenTune::ThemeId::DarkBlueGrey;
    return OpenTune::ThemeId::Overdose;   // "overdose" 默认
}

juce::File resolveOutputDirectory(const juce::String& arg)
{
    if (arg.isNotEmpty())
        return juce::File(arg);
    return juce::File::getCurrentWorkingDirectory()
        .getChildFile(".opencode")
        .getChildFile("screenshots");
}

    // 诊断：把 Image 内存原样写为 32bpp BGRA BMP（自顶向下），验证内存→文件链路
bool writeBmpRaw(const juce::Image& image, const juce::File& file)
{
    // JUCE FileOutputStream 为 append 模式（OPEN_ALWAYS + FILE_END），必须先删除旧文件，
    // 否则多次运行的数据会累积，读图工具读到的是文件头部的历史渲染。
    file.deleteFile();

    juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    const int w = data.width;
    const int h = data.height;
    const int stride = w * 4;

    juce::MemoryBlock mb;
    mb.setSize(54 + static_cast<size_t>(h) * static_cast<size_t>(stride));
    juce::uint8* out = static_cast<juce::uint8*>(mb.getData());

    out[0] = 'B'; out[1] = 'M';
    const juce::uint32 fileSize = static_cast<juce::uint32>(mb.getSize());
    std::memcpy(out + 2, &fileSize, 4);
    const juce::uint16 zero16 = 0;
    std::memcpy(out + 6, &zero16, 2);
    std::memcpy(out + 8, &zero16, 2);
    const juce::uint32 pixelOffset = 54;
    std::memcpy(out + 10, &pixelOffset, 4);

    juce::uint8* info = out + 14;
    const juce::uint32 biSize = 40;
    const juce::int32 negH = -h;
    const juce::uint32 planes = 1;
    const juce::uint32 bpp = 32;
    const juce::uint32 compression = 0;
    const juce::uint32 imageSize = static_cast<juce::uint32>(static_cast<size_t>(h) * stride);
    std::memcpy(info, &biSize, 4);
    std::memcpy(info + 4, &w, 4);
    std::memcpy(info + 8, &negH, 4);
    std::memcpy(info + 12, &planes, 2);
    std::memcpy(info + 14, &bpp, 2);
    std::memcpy(info + 16, &compression, 4);
    std::memcpy(info + 20, &imageSize, 4);
    const juce::uint32 zero = 0;
    std::memcpy(info + 24, &zero, 4);
    std::memcpy(info + 28, &zero, 4);
    std::memcpy(info + 32, &zero, 4);
    std::memcpy(info + 36, &zero, 4);

    juce::uint8* pixels = out + 54;
    for (int y = 0; y < h; ++y)
        std::memcpy(pixels + y * stride, data.getLinePointer(y), static_cast<size_t>(stride));

    juce::FileOutputStream stream(file);
    return stream.write(mb.getData(), mb.getSize());
}

bool writePng(juce::Component& component, const juce::File& file)
{
    const juce::Image snapshot = component.createComponentSnapshot(
        component.getLocalBounds(), true, 1.0f, juce::SoftwareImageType{});
    if (snapshot.isNull())
        return false;

    // 注1：JUCE FileOutputStream 为 append 模式，必须先删除旧文件（否则多运行数据累积，
    //     此前"截图变色"现象即由此产生）。
    // 注2：本构建中 JUCE 内置 libpng 编码损坏（writeImageToStream 后文件无法解码，
    //     读回为全零/错色，deleteFile 后复测仍失败），故输出原始 BMP（字节级正确，
    //     已用像素采样验证内存→文件一致）。
    file.deleteFile();
    const juce::File bmpFile = file.withFileExtension(".bmp");
    const bool ok = writeBmpRaw(snapshot, bmpFile);
    if (ok)
    {
        const auto centre = snapshot.getPixelAt(snapshot.getWidth() / 2, snapshot.getHeight() / 2);
        const auto top = snapshot.getPixelAt(snapshot.getWidth() / 2, 2);
        std::printf("[shots]   %s centre=%08X top=%08X fmt=%d alpha=%d size=%dx%d\n",
                    bmpFile.getFileName().toRawUTF8(), centre.getARGB(), top.getARGB(),
                    static_cast<int>(snapshot.getFormat()),
                    snapshot.hasAlphaChannel() ? 1 : 0,
                    snapshot.getWidth(), snapshot.getHeight());
        std::fflush(stdout);

        // BitmapData 原始字节采样（中心像素 4 字节）
        {
            juce::Image::BitmapData rawData(snapshot, juce::Image::BitmapData::readOnly);
            const int cx = rawData.width / 2;
            const int cy = rawData.height / 2;
            const juce::uint8* p = rawData.getPixelPointer(cx, cy);
            std::printf("[shots]   rawbytes[%d] = %02X %02X %02X %02X stride=%d pixStride=%d\n",
                        static_cast<int>(snapshot.getFormat()),
                        p[0], p[1], p[2], p[3], rawData.lineStride, rawData.pixelStride);
            std::fflush(stdout);
        }
    }
    return ok;
}

} // namespace

// ============================================================================
// F0 初始视图 pending 消费回归（真实可见环境）
//
// 生产语义：selection/移动（投影变化）经 syncPianoRollFromPlacementSelection 建立
// pending；真实 viewToggled → visibilityChanged 消费：Failed 与 Ready-无有效 F0 为
// 终态清 pending，正常 Ready 定位相机到投影区间内首个有效 F0。本回归只用一个
// 2 秒 identity TimeGrid 的 clip，相机断言直接与生产 placement 字段比较，
// 不复制投影实现、不做帧换算。
// ============================================================================
namespace {

int behaviorFailures = 0;

void expectBehavior(bool condition, const char* message)
{
    if (condition)
    {
        std::printf("[shots]   PASS %s\n", message);
    }
    else
    {
        ++behaviorFailures;
        std::printf("[shots]   FAIL %s\n", message);
    }
    std::fflush(stdout);
}

OpenTune::PianoRollComponent* findPianoRoll(juce::Component& root)
{
    for (int i = 0; i < root.getNumChildComponents(); ++i)
    {
        if (auto* roll = dynamic_cast<OpenTune::PianoRollComponent*>(root.getChildComponent(i)))
            return roll;
    }
    return nullptr;
}

std::shared_ptr<OpenTune::PitchCurve> makeF0Curve(float fillValue, int frameCount)
{
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setHopSize(160);
    curve->setSampleRate(16000.0);
    std::vector<float> f0(static_cast<size_t>(frameCount), fillValue);
    curve->setOriginalF0(f0);
    return curve;
}

void runF0ViewConsumptionRegression(OpenTune::OpenTuneAudioProcessor& processor,
                                    OpenTune::OpenTuneAudioProcessorEditor& editor,
                                    OpenTune::PianoRollComponent* roll)
{
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repo = processor.getStandaloneContentRepository();
    constexpr int track = 0;

    // ── 内容准备（窗口之前）：单个 2 秒 clip，identity TimeGrid ──
    juce::AudioBuffer<float> buffer(1, 44100 * 2);
    buffer.clear();
    OpenTune::OpenTuneAudioProcessor::PreparedImport prepared;
    if (!processor.prepareImport(std::move(buffer), 44100.0, "f0-consume", {}, prepared))
    {
        expectBehavior(false, "prepareImport accepts the regression buffer");
        return;
    }
    OpenTune::OpenTuneAudioProcessor::ImportPlacement spec;
    spec.trackId = track;
    spec.timelineStartSeconds = 0.0;
    const auto committed = processor.commitPreparedImportAsPlacement(std::move(prepared), spec, 0);
    expectBehavior(committed.isValid(), "imported placement is committed");
    if (!committed.isValid())
        return;
    auto* clip = repo->findClip(committed.contentKey);
    expectBehavior(clip != nullptr, "clip is found in the standalone content repository");
    if (clip == nullptr)
        return;
    clip->applyTimeGrid(OpenTune::TimeGridSnapshot::makeIdentity(2.0));
    const int placementIndex = arrangement->getSelectedPlacementIndex(track);

    const auto readTimelineStart = [&] {
        OpenTune::StandaloneArrangement::Placement p;
        arrangement->getPlacementById(track, committed.placementId, p);
        return p.timelineStartSeconds;
    };

    // ── 进入真实可见环境：桌面窗口 + 可见（PianoRoll 仍在 workspace 视图下隐藏）──
    editor.addToDesktop(0, nullptr);
    editor.setVisible(true);
    expectBehavior(editor.isShowing(), "editor is showing on the desktop");
    expectBehavior(!roll->isShowing(), "piano roll stays hidden in the workspace view");

    // Fresh import 提取失败时没有 curve；Failed 仍必须在进入 PianoRoll 前收束 pending。
    step("fresh Failed settles pending without a curve");
    editor.placementSelectionChanged(track, committed.placementId);
    expectBehavior(roll->hasPendingInitialF0View(),
                   "fresh Failed: selection establishes the pending before extraction");
    clip->applyOriginalF0State(OpenTune::OriginalF0State::Failed);
    editor.viewToggled(false);
    expectBehavior(!roll->hasPendingInitialF0View(),
                   "fresh Failed: terminal state clears pending without a curve");
    editor.viewToggled(true);
    clip->applyOriginalF0State(OpenTune::OriginalF0State::Extracting);

    // ══════════════════════════════════════════════════════════════════════
    // 场景 A：移动先 Ready —— selection/移动双通知建 pending，Ready 发布后
    // 真实 viewToggled 消费；identity 下首帧 0 有效 → 相机落在移动后的 timelineStart。
    // ══════════════════════════════════════════════════════════════════════
    step("scenario A: move first, then Ready");

    expectBehavior(arrangement->setPlacementTimelineStartSeconds(track, committed.placementId, 5.0),
                   "A: placement timeline start moves");
    editor.placementSelectionChanged(track, committed.placementId);
    editor.placementTimingChanged(track, placementIndex);
    expectBehavior(roll->hasPendingInitialF0View(),
                   "A: moved projection re-establishes the pending");

    clip->applyOriginalF0(makeF0Curve(220.0f, 400));
    clip->applyOriginalF0State(OpenTune::OriginalF0State::Ready);
    editor.placementTimingChanged(track, placementIndex);   // 同步曲线 no-op，不得吞 pending
    expectBehavior(roll->hasPendingInitialF0View(),
                   "A: synchronous no-op sync after Ready does not swallow the pending");

    editor.viewToggled(false);
    expectBehavior(!roll->hasPendingInitialF0View(), "A: visible view toggle consumes the pending");
    expectBehavior(roll->isShowing() && roll->isVisible(),
                   "A: piano roll is showing after the view toggle");
    expectBehavior(std::abs(roll->timelineCamera().visibleStartSeconds - readTimelineStart()) < 1.0e-9,
                   "A: camera lands on the moved timeline start (first valid F0 is frame 0)");

    // ══════════════════════════════════════════════════════════════════════
    // 场景 B：Ready 先移动 —— 内容已 Ready，再次 selection/移动建 pending，
    // viewToggled 消费到新 timelineStart。
    // ══════════════════════════════════════════════════════════════════════
    step("scenario B: Ready first, then move");

    editor.viewToggled(true);
    editor.placementSelectionChanged(track, committed.placementId);
    expectBehavior(arrangement->setPlacementTimelineStartSeconds(track, committed.placementId, 8.0),
                   "B: placement timeline start moves on Ready content");
    editor.placementSelectionChanged(track, committed.placementId);
    editor.placementTimingChanged(track, placementIndex);
    expectBehavior(roll->hasPendingInitialF0View(),
                   "B: moved projection re-establishes the pending on Ready content");
    editor.viewToggled(false);
    expectBehavior(!roll->hasPendingInitialF0View(), "B: visible view toggle consumes the Ready-first pending");
    expectBehavior(std::abs(roll->timelineCamera().visibleStartSeconds - readTimelineStart()) < 1.0e-9,
                   "B: camera lands on the moved timeline start");

    // ══════════════════════════════════════════════════════════════════════
    // 场景 C：左 Trim —— clipIn 1.0s、duration 1.0s。相机必须定位在
    // placement 可见时间区间内，不得落到被裁掉的帧所在时间。
    // ══════════════════════════════════════════════════════════════════════
    step("scenario C: left trim locates the first visible F0");

    editor.viewToggled(true);
    expectBehavior(arrangement->setPlacementTrim(track, committed.placementId, 1.0, 1.0),
                   "C: placement trims its left edge");
    editor.placementTimingChanged(track, placementIndex);
    expectBehavior(roll->hasPendingInitialF0View(),
                   "C: trimmed projection re-establishes the pending");
    editor.viewToggled(false);
    expectBehavior(!roll->hasPendingInitialF0View(), "C: visible view toggle consumes the trimmed pending");
    const double trimmedTimelineStart = readTimelineStart();
    expectBehavior(roll->timelineCamera().visibleStartSeconds >= trimmedTimelineStart
                       && roll->timelineCamera().visibleStartSeconds < trimmedTimelineStart + 1.0,
                   "C: camera lands inside the visible placement after the left trim");

    // ══════════════════════════════════════════════════════════════════════
    // 场景 D：Ready 纯静音 —— 投影区间内无有效 F0，终态清 pending。
    // ══════════════════════════════════════════════════════════════════════
    step("scenario D: Ready silence settles the pending without a camera move");

    editor.viewToggled(true);
    clip->applyOriginalF0(makeF0Curve(0.0f, 400));   // 全静音：无有效 F0 帧
    clip->applyOriginalF0State(OpenTune::OriginalF0State::Ready);
    expectBehavior(arrangement->setPlacementTimelineStartSeconds(track, committed.placementId, 14.0),
                   "D: placement moves before the silence settle");
    editor.placementTimingChanged(track, placementIndex);   // 同步曲线
    editor.viewToggled(false);
    expectBehavior(!roll->hasPendingInitialF0View(), "D: Ready silence settles the pending");

    // ══════════════════════════════════════════════════════════════════════
    // 场景 E：Gain/Fade 无投影变化 —— pending 清空后通知不重建。
    // ══════════════════════════════════════════════════════════════════════
    step("scenario E: gain/fade notifications do not establish a pending");

    expectBehavior(arrangement->setPlacementGain(track, committed.placementId, 0.5f),
                   "E: placement gain change applies");
    editor.placementTimingChanged(track, placementIndex);
    expectBehavior(!roll->hasPendingInitialF0View(),
                   "E: gain-only timing notification does not establish a pending");
    expectBehavior(arrangement->setPlacementFade(track, committed.placementId, 0.1, 0.2),
                   "E: placement fade change applies");
    editor.placementTimingChanged(track, placementIndex);
    expectBehavior(!roll->hasPendingInitialF0View(),
                   "E: fade-only timing notification does not establish a pending");

    // ── 收尾：移除桌面窗口，恢复无窗口状态 ──
    editor.removeFromDesktop();
    step("regression desktop window removed");
}

} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // 创建 MessageManager（软件渲染所需）

    const juce::String themeName = (argc > 1) ? juce::String(argv[1]) : juce::String("overdose");
    const juce::File outputDir = resolveOutputDirectory((argc > 2) ? juce::String(argv[2]) : juce::String());
    const OpenTune::ThemeId themeId = parseThemeId(themeName);

    // 通过 Windows 环境变量传递主题，确保编辑器构造时能正确读取
    const char* envThemeStr = "overdose";
    switch (themeId) {
        case OpenTune::ThemeId::Aurora:       envThemeStr = "aurora"; break;
        case OpenTune::ThemeId::BlueBreeze:   envThemeStr = "bluebreeze"; break;
        case OpenTune::ThemeId::DarkBlueGrey: envThemeStr = "darkbluegrey"; break;
        case OpenTune::ThemeId::Overdose:     envThemeStr = "overdose"; break;
    }
#ifdef _WIN32
    _putenv_s("OPENTUNE_THEME", envThemeStr);
#else
    setenv("OPENTUNE_THEME", envThemeStr, 1);
#endif
    std::printf("[shots] Set OPENTUNE_THEME=%s\n", envThemeStr);

    // 临时配置目录：编辑器构造时读 AppPreferences 主题，必须先用临时目录构造并 setTheme，
    // 绝不读写用户真实 %APPDATA%\OpenTune 配置。
    OpenTune::AppPreferences::StorageOptions storageOptions;
    storageOptions.settingsDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                           .getChildFile("OpenTuneThemeShots");
    OpenTune::AppPreferences appPreferences(storageOptions);
    appPreferences.setTheme(themeId);
    step("AppPreferences constructed");

    const auto envTheme = juce::SystemStats::getEnvironmentVariable("OPENTUNE_THEME", {});
    std::printf("[shots] OPENTUNE_THEME env = '%s'\n", envTheme.toRawUTF8());
    std::fflush(stdout);

    OpenTune::OpenTuneAudioProcessor processor;
    step("processor constructed");

    OpenTune::OpenTuneAudioProcessorEditor editor(processor);
    std::printf("[shots] UIColors currentThemeId = %d\n", static_cast<int>(OpenTune::UIColors::currentThemeId()));
    std::printf("[shots] backgroundDark = %08X backgroundLight = %08X textPrimary = %08X\n",
                OpenTune::UIColors::backgroundDark.getARGB(),
                OpenTune::UIColors::backgroundLight.getARGB(),
                OpenTune::UIColors::textPrimary.getARGB());
    std::fflush(stdout);
    step("editor constructed");

    editor.setSize(1200, 900);
    editor.resized();   // 无窗口时 JUCE 不会自动触发布局，显式跑一次
    step("layout done");

    if (!outputDir.createDirectory())
    {
        std::printf("OpenTuneThemeScreenshots: cannot create output directory %s\n",
                    outputDir.getFullPathName().toRawUTF8());
        return 1;
    }

    int savedCount = 0;

    step("saving full window");
    const juce::File fullFile = outputDir.getChildFile(themeName + "_full.png");
    if (writePng(editor, fullFile))
    {
        std::printf("saved %s\n", fullFile.getFileName().toRawUTF8());
        ++savedCount;
    }
    else
    {
        std::printf("FAILED to save %s\n", fullFile.getFileName().toRawUTF8());
    }

    step("saving child components");
    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        juce::Component* child = editor.getChildComponent(i);
        if (child == nullptr)
            continue;
        std::printf("[shots]   child[%d] name='%s' id='%s' type='%s' size=%dx%d visible=%d\n",
                    i,
                    child->getName().toRawUTF8(),
                    child->getComponentID().toRawUTF8(),
                    typeid(*child).name(),
                    child->getWidth(), child->getHeight(),
                    child->isVisible() ? 1 : 0);
        std::fflush(stdout);

        const juce::Rectangle<int> bounds = child->getLocalBounds();
        if (bounds.isEmpty())
            continue;

        const juce::File childFile = outputDir.getChildFile(
            themeName + "_child_" + juce::String(i) + "_"
            + juce::String(bounds.getWidth()) + "x" + juce::String(bounds.getHeight()) + ".png");
        if (writePng(*child, childFile))
        {
            std::printf("saved %s\n", childFile.getFileName().toRawUTF8());
            ++savedCount;
        }
        else
        {
            std::printf("FAILED to save %s\n", childFile.getFileName().toRawUTF8());
        }
    }

    // 行为回归：截图之后运行，不影响截图输出；临时桌面窗口在回归末尾移除。
    step("running F0 view pending consumption regression");
    runF0ViewConsumptionRegression(processor, editor, findPianoRoll(editor));
    if (behaviorFailures != 0)
    {
        std::printf("OpenTuneThemeScreenshots: %d behavior regression(s) failed\n", behaviorFailures);
        return 1;
    }

    std::printf("OpenTuneThemeScreenshots: theme=%s output=%s files=%d\n",
                themeName.toRawUTF8(), outputDir.getFullPathName().toRawUTF8(), savedCount);

    return savedCount > 0 ? 0 : 1;
}
