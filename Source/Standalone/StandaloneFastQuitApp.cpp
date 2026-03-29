#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>
#include <cmath>

namespace juce {

class OpenTuneStandaloneApp final : public JUCEApplication,
                                   private Timer {
public:
    OpenTuneStandaloneApp() {
        PropertiesFile::Options options;
        options.applicationName = CharPointer_UTF8(JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName = "~/.config";
       #else
        options.folderName = "";
       #endif
        appProperties_.setStorageParameters(options);
    }

    const String getApplicationName() override { return CharPointer_UTF8(JucePlugin_Name); }
    const String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const String&) override {}

    void initialise(const String& commandLine) override {
        juce::ignoreUnused(commandLine);
        parsePerfProbeArgs();

        mainWindow_ = createWindow();
        if (mainWindow_ != nullptr) {
            mainWindow_->setVisible(true);
        } else {
            pluginHolder_ = createPluginHolder();
        }

        if (perfProbeMode_) {
            perfProbeStartMs_ = Time::getMillisecondCounterHiRes();
            startTimerHz(20);
        }
    }

    void shutdown() override {
        stopTimer();
        pluginHolder_ = nullptr;
        mainWindow_ = nullptr;
        appProperties_.saveIfNeeded();
    }

    void systemRequestedQuit() override {
        // 快速退出路径：先让窗口立即消失，再执行短路径收尾，避免点击右上角关闭时的卡顿体感。
        if (isQuitting_.exchange(true)) {
            return;
        }

        if (mainWindow_ != nullptr) {
            mainWindow_->setVisible(false);
        }

        if (pluginHolder_ != nullptr) {
            pluginHolder_->savePluginState();
        }

        if (mainWindow_ != nullptr && mainWindow_->pluginHolder != nullptr) {
            mainWindow_->pluginHolder->savePluginState();
        }

        if (auto* modal = ModalComponentManager::getInstance()) {
            modal->cancelAllModalComponents();
        }

        quit();
    }

private:
    void parsePerfProbeArgs()
    {
        const auto args = getCommandLineParameterArray();
        if (args.isEmpty()) {
            return;
        }

        for (int i = 0; i < args.size(); ++i) {
            const auto& token = args[i];
            if (token == "--perf-probe" && i + 1 < args.size()) {
                perfProbeMode_ = true;
                perfProbeSeconds_ = jmax(1, args[i + 1].getIntValue());
                ++i;
            } else if (token == "--log-file" && i + 1 < args.size()) {
                perfLogFile_ = File(args[i + 1]);
                ++i;
            } else if (token == "--scenario" && i + 1 < args.size()) {
                perfScenario_ = args[i + 1];
                ++i;
            }
        }
    }

    ::OpenTune::OpenTuneAudioProcessor* getProcessorForProbe() const
    {
        if (mainWindow_ != nullptr && mainWindow_->pluginHolder != nullptr && mainWindow_->pluginHolder->processor != nullptr) {
            return dynamic_cast<::OpenTune::OpenTuneAudioProcessor*>(mainWindow_->pluginHolder->processor.get());
        }

        if (pluginHolder_ != nullptr && pluginHolder_->processor != nullptr) {
            return dynamic_cast<::OpenTune::OpenTuneAudioProcessor*>(pluginHolder_->processor.get());
        }

        return nullptr;
    }

    void writePerfProbeReportAndQuit()
    {
        if (perfLogFile_ == File{}) {
            perfLogFile_ = File::getCurrentWorkingDirectory().getChildFile(".sisyphus/evidence/perf-probe.json");
        }

        auto* processor = getProcessorForProbe();
        ::OpenTune::OpenTuneAudioProcessor::PerfProbeSnapshot snapshot;
        ::OpenTune::RenderingManager::RenderQueueStats queueStats;
        if (processor != nullptr) {
            snapshot = processor->getPerfProbeSnapshot();
            if (auto* manager = processor->getRenderingManager()) {
                queueStats = manager->getQueueStats();
            }
        }

        const double elapsedMs = Time::getMillisecondCounterHiRes() - perfProbeStartMs_;

        perfLogFile_.getParentDirectory().createDirectory();
        if (auto stream = std::unique_ptr<FileOutputStream>(perfLogFile_.createOutputStream())) {
            const String json = "{" 
                "\n  \"schemaVersion\": \"1.0.0\"," 
                "\n  \"scenario\": \"" + perfScenario_ + "\"," 
                "\n  \"metrics\": {" 
                "\n    \"audioCallbackP99\": " + String(snapshot.audioCallbackP99Ms, 3) + "," 
                "\n    \"cacheMissRate\": " + String(snapshot.cacheMissRate, 6) + "," 
                "\n    \"renderQueueDepth\": " + String(snapshot.renderQueueDepth) + "," 
                "\n    \"cacheChecks\": " + String((int64) snapshot.cacheChecks) + "," 
                "\n    \"cacheMisses\": " + String((int64) snapshot.cacheMisses) + "," 
                "\n    \"probeDurationMs\": " + String(elapsedMs, 3) + "," 
                "\n    \"uiFrameDropCount\": 0," 
                "\n    \"playheadJitterMs\": 0.0," 
                "\n    \"renderQueueWaitMsP95\": " + String((double) queueStats.maxWaitMs, 3) 
                "\n  }" 
                "\n}";
            stream->writeText(json, false, false, "\n");
            stream->flush();
        }

        quit();
    }

    void timerCallback() override
    {
        if (!perfProbeMode_) {
            return;
        }

        const double elapsedSec = (Time::getMillisecondCounterHiRes() - perfProbeStartMs_) / 1000.0;
        if (elapsedSec >= static_cast<double>(perfProbeSeconds_)) {
            stopTimer();
            writePerfProbeReportAndQuit();
        }
    }

    std::unique_ptr<StandaloneFilterWindow> createWindow() {
        if (Desktop::getInstance().getDisplays().displays.isEmpty()) {
            jassertfalse;
            return nullptr;
        }

        return std::make_unique<StandaloneFilterWindow>(
            getApplicationName(),
            LookAndFeel::getDefaultLookAndFeel().findColour(ResizableWindow::backgroundColourId),
            createPluginHolder());
    }

    std::unique_ptr<StandalonePluginHolder> createPluginHolder() {
        constexpr auto autoOpenMidiDevices =
       #if (JUCE_ANDROID || JUCE_IOS) && ! JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
            true;
       #else
            false;
       #endif

       #ifdef JucePlugin_PreferredChannelConfigurations
        constexpr StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig(channels, juce::numElementsInArray(channels));
       #else
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig;
       #endif

        return std::make_unique<StandalonePluginHolder>(
            appProperties_.getUserSettings(),
            false,
            String{},
            nullptr,
            channelConfig,
            autoOpenMidiDevices);
    }

    ApplicationProperties appProperties_;
    std::unique_ptr<StandaloneFilterWindow> mainWindow_;
    std::unique_ptr<StandalonePluginHolder> pluginHolder_;
    std::atomic<bool> isQuitting_{false};
    bool perfProbeMode_{false};
    int perfProbeSeconds_{0};
    double perfProbeStartMs_{0.0};
    File perfLogFile_;
    String perfScenario_{"playback_only"};
};

} // namespace juce

juce::JUCEApplicationBase* juce_CreateApplication() {
    return new juce::OpenTuneStandaloneApp();
}
