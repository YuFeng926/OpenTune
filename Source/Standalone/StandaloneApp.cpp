// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StandaloneApp.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
// After the modules it uses, as in PluginEditor.cpp.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include "PluginEditor.h"
#include "Runtime/ProcessRenderRuntime.h"

#include <memory>
#include <utility>

namespace OpenTune {
namespace StandaloneApp {

namespace {

/** The Standalone window: the stock window owns the plug-in holder, the holder owns the processor, and the holder's
    destruction is what releases the processor's runtime owner. The close button asks the editor to resolve project
    changes before entering the exit coordinator. */
class OpenTuneStandaloneWindow final : public juce::StandaloneFilterWindow
{
public:
    OpenTuneStandaloneWindow(const juce::String& title,
                             juce::Colour backgroundColour,
                             std::unique_ptr<juce::StandalonePluginHolder> pluginHolder)
        : juce::StandaloneFilterWindow(title, backgroundColour, std::move(pluginHolder))
    {
    }

    void closeButtonPressed() override
    {
        if (auto* processor = getAudioProcessor())
            if (auto* editor = dynamic_cast<OpenTuneAudioProcessorEditor*>(processor->getActiveEditor()))
            {
                editor->requestExit();
                return;
            }

        OpenTune::StandaloneApp::requestQuit();
    }
};

} // namespace

/** The Standalone application: the default Standalone application's window and holder setup, with one exit
    coordinator for the OS quit request, the window close and the editor's save-and-return. */
class Application final : public juce::JUCEApplication
{
public:
    Application()
    {
        // The default Standalone application's property file, read by the plug-in holder for its state, its audio
        // device setup and its window position.
        juce::PropertiesFile::Options options;
        options.applicationName     = juce::CharPointer_UTF8(JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif

        appProperties.setStorageParameters(options);
    }

    const juce::String getApplicationName() override    { return juce::CharPointer_UTF8(JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override          { return true; }
    void anotherInstanceStarted(const juce::String&) override {}

    void initialise(const juce::String&) override
    {
        mainWindow.reset(createWindow());

        if (mainWindow != nullptr)
        {
           #if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            juce::Desktop::getInstance().setKioskModeComponent(mainWindow.get(), false);
           #endif

            mainWindow->setVisible(true);
        }
        else
        {
            pluginHolder = createPluginHolder();
        }
    }

    void shutdown() override
    {
        // iOS may call shutdownApp directly from applicationWillTerminate, bypassing systemRequestedQuit.
        if (!quitRequested_)
        {
            quitRequested_ = true;
            if (mainWindow != nullptr)
                mainWindow->pluginHolder->savePluginState();
            else if (pluginHolder != nullptr)
                pluginHolder->savePluginState();
        }

        mainWindow = nullptr;
        pluginHolder = nullptr;
        ProcessRenderRuntime::getInstance().shutdownAndJoin();
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->closeButtonPressed();
            return;
        }

        if (pluginHolder != nullptr && pluginHolder->processor != nullptr)
            if (auto* editor = dynamic_cast<OpenTuneAudioProcessorEditor*>(pluginHolder->processor->getActiveEditor()))
            {
                editor->requestExit();
                return;
            }

        requestQuit();
    }

    void requestQuit()
    {
        if (quitRequested_)
            return;
        quitRequested_ = true;

        // Exactly one save of the plug-in state, made when the quit is requested as the stock window's close button
        // made it. The processor is still alive here; the save is the state the window restores.
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();
        else if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        // The routes run inside the close button's handler or inside a control-API reply step, where the editor and
        // its window must survive until the handler returns. The teardown is therefore the next message of its own.
        // Cancelling the modal components posts their callbacks before that message, so an open dialog leaves the
        // modal state first. The default application waited with a timer here; the final close is asynchronous, so
        // the coordinator needs none.
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        juce::MessageManager::callAsync([this] { performQuit(); });
    }

private:
    OpenTuneStandaloneWindow* createWindow()
    {
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            // No displays are available, so no window will be created; the holder still owns the processor.
            jassertfalse;
            return nullptr;
        }

        return new OpenTuneStandaloneWindow(getApplicationName(),
                                            juce::LookAndFeel::getDefaultLookAndFeel()
                                                .findColour(juce::ResizableWindow::backgroundColourId),
                                            createPluginHolder());
    }

    std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
    {
        constexpr auto autoOpenMidiDevices =
       #if (JUCE_ANDROID || JUCE_IOS) && ! JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
            true;
       #else
            false;
       #endif

       #ifdef JucePlugin_PreferredChannelConfigurations
        constexpr juce::StandalonePluginHolder::PluginInOuts channels[] {
            JucePlugin_PreferredChannelConfigurations
        };
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig(
            channels, juce::numElementsInArray(channels));
       #else
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig;
       #endif

        return std::make_unique<juce::StandalonePluginHolder>(appProperties.getUserSettings(),
                                                              false,
                                                              juce::String{},
                                                              nullptr,
                                                              channelConfig,
                                                              autoOpenMidiDevices);
    }

    void performQuit()
    {
        // Destroy the window, the holder and the processor on the message thread, before the process runtime's final
        // close. The holder's destruction releases the processor's runtime owner.
        mainWindow = nullptr;
        pluginHolder = nullptr;

        // Request the final close of the process runtime and keep the dispatch loop live. shutdownAsync delivers
        // its completion on the message thread after the worker has exited and been joined; that completion ends
        // the application, and shutdown() then finds the objects above already closed.
        ProcessRenderRuntime::getInstance().shutdownAsync(
            [] { juce::JUCEApplicationBase::quit(); });
    }

    juce::ApplicationProperties appProperties;
    std::unique_ptr<OpenTuneStandaloneWindow> mainWindow;
    std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
    bool quitRequested_{false};
};

void requestQuit()
{
    if (auto* app = dynamic_cast<Application*>(juce::JUCEApplicationBase::getInstance()))
        app->requestQuit();
}

} // namespace StandaloneApp
} // namespace OpenTune

// The custom Standalone application entry that JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 expects. On iOS the macro
// also defines juce_GetIOSCustomDelegateClass(), which returns nullptr as the default application's entry did.
START_JUCE_APPLICATION(OpenTune::StandaloneApp::Application)
