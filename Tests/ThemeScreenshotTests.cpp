/**
 * OpenTuneThemeScreenshots — 后台静默主题截图工具
 *
 * 无窗口、单线程、同步执行：构造 AppPreferences(临时目录) → OpenTuneAudioProcessor
 * → OpenTuneAudioProcessorEditor，然后对整窗与各直接子组件做软件渲染截图
 * (Component::createComponentSnapshot)，输出 BMP 供设计师对比主题绘制效果。
 * 绝不进入消息循环，不建窗口，执行完即退出。
 *
 * 用法:
 *   OpenTuneThemeScreenshots [theme] [outputDir]
 *     theme      overdose(默认) | aurora | bluebreeze | darkbluegrey
 *     outputDir  默认 .opencode/screenshots (相对当前工作目录)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Standalone/PluginEditor.h"
#include "UI/ThemeTokens.h"
#include "Utils/AppPreferences.h"

#include <cstdio>

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

    std::printf("OpenTuneThemeScreenshots: theme=%s output=%s files=%d\n",
                themeName.toRawUTF8(), outputDir.getFullPathName().toRawUTF8(), savedCount);

    return savedCount > 0 ? 0 : 1;
}
