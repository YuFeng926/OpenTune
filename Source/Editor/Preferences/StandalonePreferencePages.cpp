#include "StandalonePreferencePages.h"

#include <cmath>

#include "Inference/IF0Extractor.h"
#include "Standalone/StandaloneAudioDeviceSync.h"
#include "Standalone/UI/UIColors.h"
#include "Editor/Preferences/SharedPreferencePages.h"
#include "Utils/AppLogger.h"

namespace OpenTune {

namespace {

void initialiseLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, UIColors::textPrimary);
    label.setFont(UIColors::getUIFont(13.0f));
}

void initialiseComboBox(juce::ComboBox& comboBox)
{
    comboBox.setColour(juce::ComboBox::backgroundColourId, UIColors::backgroundMedium);
    comboBox.setColour(juce::ComboBox::textColourId, UIColors::textPrimary);
    comboBox.setColour(juce::ComboBox::outlineColourId, UIColors::panelBorder);
    comboBox.setColour(juce::ComboBox::arrowColourId, UIColors::accent);
}

void initialiseTextButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, UIColors::buttonNormal);
    button.setColour(juce::TextButton::buttonOnColourId, UIColors::accent);
    button.setColour(juce::TextButton::textColourOffId, UIColors::textPrimary);
    button.setColour(juce::TextButton::textColourOnId, UIColors::textPrimary);
}

void initialiseToggleButton(juce::ToggleButton& toggle)
{
    toggle.setColour(juce::ToggleButton::textColourId, UIColors::textPrimary);
    toggle.setColour(juce::ToggleButton::tickColourId, UIColors::accent);
    toggle.setColour(juce::ToggleButton::tickDisabledColourId, UIColors::panelBorder);
}

// 项目主题自绘音频设备设置面板，替代 JUCE 内置设备选择器组件
// （内置组件在设备切换失败时会弹出模态提示窗口）。
class AudioDeviceSettingsContent final : public juce::Component,
                                         public juce::ChangeListener
{
public:
    explicit AudioDeviceSettingsContent(juce::AudioDeviceManager& deviceManager)
        : deviceManager_(deviceManager)
    {
        initialiseLabel(typeLabel_, LOC(kAudioDeviceType));
        initialiseLabel(outputLabel_, LOC(kAudioOutput));
        initialiseLabel(sampleRateLabel_, LOC(kAudioSampleRate));
        initialiseLabel(bufferSizeLabel_, LOC(kAudioBufferSize));
        initialiseLabel(channelsLabel_, LOC(kAudioActiveOutputChannels));
        initialiseLabel(errorLabel_, juce::String());
        errorLabel_.setColour(juce::Label::textColourId, UIColors::statusError);
        errorLabel_.setJustificationType(juce::Justification::centredLeft);

        initialiseComboBox(typeSelector_);
        initialiseComboBox(outputDeviceSelector_);
        initialiseComboBox(sampleRateSelector_);
        initialiseComboBox(bufferSizeSelector_);

        initialiseTextButton(testButton_);
        testButton_.setButtonText(LOC(kAudioTest));
        testButton_.onClick = [this] { deviceManager_.playTestSound(); };

        typeSelector_.onChange = [this] { typeChanged(); };
        outputDeviceSelector_.onChange = [this] { outputDeviceChanged(); };
        sampleRateSelector_.onChange = [this] { sampleRateChanged(); };
        bufferSizeSelector_.onChange = [this] { bufferSizeChanged(); };

        addAndMakeVisible(typeLabel_);
        addAndMakeVisible(typeSelector_);
        addAndMakeVisible(outputLabel_);
        addAndMakeVisible(outputDeviceSelector_);
        addAndMakeVisible(testButton_);
        addAndMakeVisible(sampleRateLabel_);
        addAndMakeVisible(sampleRateSelector_);
        addAndMakeVisible(bufferSizeLabel_);
        addAndMakeVisible(bufferSizeSelector_);
        addAndMakeVisible(channelsLabel_);
        addAndMakeVisible(errorLabel_);

        deviceManager_.addChangeListener(this);
        refresh();
    }

    ~AudioDeviceSettingsContent() override
    {
        deviceManager_.removeChangeListener(this);
    }

    int getPreferredHeight() const
    {
        const int selectorRows = typeSelector_.isVisible() ? 4 : 3;
        const int channelRows = juce::jmax(1, (channelToggles_.size() + 1) / 2);
        return selectorRows * kRowHeight + selectorRows * kRowGap
             + kRowHeight
             + channelRows * kRowHeight + (channelRows - 1) * 4
             + kRowGap + kRowHeight;
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        juce::Rectangle<int> row;

        if (typeSelector_.isVisible()) {
            row = bounds.removeFromTop(kRowHeight);
            typeLabel_.setBounds(row.removeFromLeft(kLabelWidth));
            typeSelector_.setBounds(row.removeFromLeft(kSelectorWidth).reduced(0, 4));
            bounds.removeFromTop(kRowGap);
        } else {
            typeLabel_.setBounds({});
            typeSelector_.setBounds({});
        }

        row = bounds.removeFromTop(kRowHeight);
        outputLabel_.setBounds(row.removeFromLeft(kLabelWidth));
        outputDeviceSelector_.setBounds(row.removeFromLeft(kSelectorWidth).reduced(0, 4));
        row.removeFromLeft(kRowGap);
        testButton_.setBounds(row.removeFromLeft(80).reduced(0, 4));
        bounds.removeFromTop(kRowGap);

        row = bounds.removeFromTop(kRowHeight);
        sampleRateLabel_.setBounds(row.removeFromLeft(kLabelWidth));
        sampleRateSelector_.setBounds(row.removeFromLeft(kSelectorWidth).reduced(0, 4));
        bounds.removeFromTop(kRowGap);

        row = bounds.removeFromTop(kRowHeight);
        bufferSizeLabel_.setBounds(row.removeFromLeft(kLabelWidth));
        bufferSizeSelector_.setBounds(row.removeFromLeft(kSelectorWidth).reduced(0, 4));
        bounds.removeFromTop(kRowGap);

        channelsLabel_.setBounds(bounds.removeFromTop(kRowHeight));

        const int toggleWidth = getWidth() / 2;
        for (int i = 0; i < channelToggles_.size(); ++i) {
            if (i % 2 == 0)
                row = bounds.removeFromTop(kRowHeight);
            channelToggles_[i]->setBounds(row.removeFromLeft(toggleWidth).reduced(0, 4));
        }

        bounds.removeFromTop(kRowGap);
        errorLabel_.setBounds(bounds.removeFromTop(kRowHeight));
    }

private:
    static constexpr int kRowHeight = 34;
    static constexpr int kRowGap = 8;
    static constexpr int kLabelWidth = 160;
    static constexpr int kSelectorWidth = 240;

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        refresh();
    }

    void refresh()
    {
        if (refreshing_)
            return;
        const juce::ScopedValueSetter<bool> guard(refreshing_, true);

        if (auto* currentType = deviceManager_.getCurrentDeviceTypeObject())
            currentType->scanForDevices();

        refreshTypeSelector();
        refreshDeviceSelector();
        refreshSampleRateSelector();
        refreshBufferSizeSelector();
        refreshChannelToggles();
        resized();
    }

    void refreshTypeSelector()
    {
        const auto& types = deviceManager_.getAvailableDeviceTypes();
        const bool showTypes = types.size() > 1;

        typeLabel_.setVisible(showTypes);
        typeSelector_.setVisible(showTypes);
        typeSelector_.clear(juce::dontSendNotification);
        availableTypeNames_.clear();

        for (auto* type : types) {
            if (type == nullptr)
                continue;
            availableTypeNames_.add(type->getTypeName());
            typeSelector_.addItem(type->getTypeName(), availableTypeNames_.size());
        }

        const int currentIndex = availableTypeNames_.indexOf(deviceManager_.getCurrentAudioDeviceType());
        typeSelector_.setSelectedId(currentIndex >= 0 ? currentIndex + 1 : 0, juce::dontSendNotification);
    }

    void refreshDeviceSelector()
    {
        availableDeviceNames_.clear();
        outputDeviceSelector_.clear(juce::dontSendNotification);

        auto* type = deviceManager_.getCurrentDeviceTypeObject();
        if (type != nullptr)
            availableDeviceNames_ = type->getDeviceNames(false);

        outputDeviceSelector_.addItem(LOC(kAudioNoDevice), 1);
        for (int i = 0; i < availableDeviceNames_.size(); ++i)
            outputDeviceSelector_.addItem(availableDeviceNames_[i], i + 2);

        const int currentIndex = availableDeviceNames_.indexOf(deviceManager_.getAudioDeviceSetup().outputDeviceName);
        outputDeviceSelector_.setSelectedId(currentIndex >= 0 ? currentIndex + 2 : 1, juce::dontSendNotification);
        outputDeviceSelector_.setEnabled(type != nullptr);
    }

    void refreshSampleRateSelector()
    {
        availableSampleRates_.clear();
        sampleRateSelector_.clear(juce::dontSendNotification);

        auto* device = deviceManager_.getCurrentAudioDevice();
        sampleRateSelector_.setEnabled(device != nullptr);
        if (device == nullptr)
            return;

        availableSampleRates_ = device->getAvailableSampleRates();
        for (int i = 0; i < availableSampleRates_.size(); ++i) {
            const double rate = availableSampleRates_[i];
            const auto text = rate >= 1000.0
                ? juce::String(rate / 1000.0, 1) + " kHz"
                : juce::String(rate, 0) + " Hz";
            sampleRateSelector_.addItem(text, i + 1);
        }

        const double currentRate = device->getCurrentSampleRate();
        int selectedIndex = -1;
        for (int i = 0; i < availableSampleRates_.size(); ++i) {
            if (std::abs(availableSampleRates_[i] - currentRate) < 0.01) {
                selectedIndex = i;
                break;
            }
        }
        sampleRateSelector_.setSelectedId(selectedIndex >= 0 ? selectedIndex + 1 : 0, juce::dontSendNotification);
    }

    void refreshBufferSizeSelector()
    {
        availableBufferSizes_.clear();
        bufferSizeSelector_.clear(juce::dontSendNotification);

        auto* device = deviceManager_.getCurrentAudioDevice();
        bufferSizeSelector_.setEnabled(device != nullptr);
        if (device == nullptr)
            return;

        availableBufferSizes_ = device->getAvailableBufferSizes();
        const double sampleRate = device->getCurrentSampleRate();
        for (int i = 0; i < availableBufferSizes_.size(); ++i) {
            const int bufferSize = availableBufferSizes_[i];
            const double milliseconds = sampleRate > 0.0
                ? (static_cast<double>(bufferSize) * 1000.0) / sampleRate
                : 0.0;
            bufferSizeSelector_.addItem(Loc::format(LOC(kAudioBufferFormat),
                                                    juce::String(bufferSize),
                                                    juce::String(milliseconds, 1)),
                                        i + 1);
        }

        const int selectedIndex = availableBufferSizes_.indexOf(device->getCurrentBufferSizeSamples());
        bufferSizeSelector_.setSelectedId(selectedIndex >= 0 ? selectedIndex + 1 : 0, juce::dontSendNotification);
    }

    void refreshChannelToggles()
    {
        auto* device = deviceManager_.getCurrentAudioDevice();
        juce::StringArray channelNames;
        juce::BigInteger activeChannels;
        if (device != nullptr) {
            channelNames = device->getOutputChannelNames();
            activeChannels = device->getActiveOutputChannels();
        }

        // Standalone is configured for at most two output channels.
        const int numChannels = juce::jmin(2, juce::jmax(channelNames.size(), activeChannels.getHighestBit() + 1));
        if (device == nullptr || numChannels <= 0) {
            channelToggles_.clear();
            channelsLabel_.setText(LOC(kAudioNoOutputChannels), juce::dontSendNotification);
            return;
        }

        channelsLabel_.setText(LOC(kAudioActiveOutputChannels), juce::dontSendNotification);

        const int numPairs = (numChannels + 1) / 2;
        while (channelToggles_.size() > numPairs)
            channelToggles_.removeLast();

        while (channelToggles_.size() < numPairs) {
            auto* toggle = channelToggles_.add(new juce::ToggleButton());
            initialiseToggleButton(*toggle);
            const int pairIndex = channelToggles_.size() - 1;
            toggle->onClick = [this, pairIndex] { channelPairClicked(pairIndex); };
            addAndMakeVisible(toggle);
        }

        for (int i = 0; i < numPairs; ++i) {
            const int firstChannel = i * 2;
            juce::String text = channelNames.size() > firstChannel
                ? channelNames[firstChannel]
                : Loc::format(LOC(kAudioChannel), juce::String(firstChannel + 1));
            if (firstChannel + 1 < numChannels) {
                text += " + ";
                text += channelNames.size() > firstChannel + 1
                    ? channelNames[firstChannel + 1]
                    : Loc::format(LOC(kAudioChannel), juce::String(firstChannel + 2));
            }

            auto& toggle = *channelToggles_[i];
            toggle.setButtonText(text);
            const bool pairActive = activeChannels[firstChannel]
                || (firstChannel + 1 < numChannels && activeChannels[firstChannel + 1]);
            toggle.setToggleState(pairActive, juce::dontSendNotification);
        }
    }

    void applySetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup,
                    const juce::AudioDeviceManager::AudioDeviceSetup* rollbackSetup = nullptr)
    {
        auto error = deviceManager_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty() && rollbackSetup != nullptr) {
            const auto rollbackError = deviceManager_.setAudioDeviceSetup(*rollbackSetup, true);
            if (rollbackError.isNotEmpty()) {
                AppLogger::error("[AudioDevice] setup rollback failed: " + rollbackError);
                error += "; rollback failed: " + rollbackError;
            }
        }

        errorLabel_.setText(error.isNotEmpty()
                                ? LOC(kAudioDeviceError) + ": " + error
                                : juce::String(),
                            juce::dontSendNotification);

        const auto actualSetup = deviceManager_.getAudioDeviceSetup();
        AppLogger::log("[AudioDevice] setup type=" + deviceManager_.getCurrentAudioDeviceType()
                       + " requestedRate=" + juce::String(setup.sampleRate, 1)
                       + " actualRate=" + juce::String(actualSetup.sampleRate, 1)
                       + " requestedBuffer=" + juce::String(setup.bufferSize)
                       + " actualBuffer=" + juce::String(actualSetup.bufferSize)
                       + (error.isNotEmpty() ? " error=" + error : juce::String()));

        refresh();
    }

    void typeChanged()
    {
        if (refreshing_)
            return;

        const int index = typeSelector_.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow(index, availableTypeNames_.size()))
            return;

        const auto requestedType = availableTypeNames_[index];
        const auto currentType = deviceManager_.getCurrentAudioDeviceType();
        const auto previousSetup = deviceManager_.getAudioDeviceSetup();
        const bool hadDevice = deviceManager_.getCurrentAudioDevice() != nullptr;
        if (currentType == "ASIO" && requestedType != currentType
            && !restoreAsioSampleRateToSystem(deviceManager_, "audio type switch"))
        {
            errorLabel_.setText(LOC(kAudioDeviceSwitchFailed), juce::dontSendNotification);
            refresh();
            return;
        }

        deviceManager_.setCurrentAudioDeviceType(requestedType, true);
        const bool typeMismatch = deviceManager_.getCurrentAudioDeviceType() != requestedType;
        auto* targetType = deviceManager_.getCurrentDeviceTypeObject();
        const bool targetHasDevices = targetType != nullptr
            && (!targetType->getDeviceNames(false).isEmpty()
                || !targetType->getDeviceNames(true).isEmpty());
        const bool openFailed = deviceManager_.getCurrentAudioDevice() == nullptr
            && (hadDevice || targetHasDevices);

        if (typeMismatch || openFailed) {
            if (requestedType == "ASIO" && openFailed)
                AppLogger::error("[AudioDevice] ASIO switch failed before actual sample rate could be verified");

            if (deviceManager_.getCurrentAudioDeviceType() != currentType)
                deviceManager_.setCurrentAudioDeviceType(currentType, true);

            const auto rollbackError = deviceManager_.setAudioDeviceSetup(previousSetup, true);
            if (rollbackError.isNotEmpty())
                AppLogger::error("[AudioDevice] type-switch rollback failed: " + rollbackError);

            errorLabel_.setText(LOC(kAudioDeviceSwitchFailed), juce::dontSendNotification);
        } else {
            errorLabel_.setText(juce::String(), juce::dontSendNotification);
        }

        refresh();
    }

    void outputDeviceChanged()
    {
        if (refreshing_)
            return;

        const int selectedId = outputDeviceSelector_.getSelectedId();
        if (selectedId <= 0)
            return;

        const auto currentSetup = deviceManager_.getAudioDeviceSetup();
        auto setup = currentSetup;
        auto* type = deviceManager_.getCurrentDeviceTypeObject();

        // The selected device may expose a different channel layout. Re-open
        // it with the device default before the user explicitly chooses a pair.
        setup.outputChannels.clear();
        setup.useDefaultOutputChannels = true;

        if (selectedId == 1) {
            setup.outputDeviceName.clear();
            if (type == nullptr || ! type->hasSeparateInputsAndOutputs())
                setup.inputDeviceName.clear();
        } else {
            const int deviceIndex = selectedId - 2;
            if (! juce::isPositiveAndBelow(deviceIndex, availableDeviceNames_.size()))
                return;

            setup.outputDeviceName = availableDeviceNames_[deviceIndex];
            if (type == nullptr || ! type->hasSeparateInputsAndOutputs())
                setup.inputDeviceName = setup.outputDeviceName;
        }

        if (deviceManager_.getCurrentAudioDeviceType() == "ASIO"
            && setup.outputDeviceName != currentSetup.outputDeviceName
            && !restoreAsioSampleRateToSystem(deviceManager_, "audio output switch"))
        {
            errorLabel_.setText(LOC(kAudioDeviceSwitchFailed), juce::dontSendNotification);
            refresh();
            return;
        }

        applySetup(setup, &currentSetup);
    }

    void sampleRateChanged()
    {
        if (refreshing_)
            return;

        const int index = sampleRateSelector_.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow(index, availableSampleRates_.size()))
            return;

        const auto previousSetup = deviceManager_.getAudioDeviceSetup();
        auto setup = previousSetup;
        setup.sampleRate = availableSampleRates_[index];
        applySetup(setup, &previousSetup);
    }

    void bufferSizeChanged()
    {
        if (refreshing_)
            return;

        const int index = bufferSizeSelector_.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow(index, availableBufferSizes_.size()))
            return;

        const auto previousSetup = deviceManager_.getAudioDeviceSetup();
        auto setup = previousSetup;
        setup.bufferSize = availableBufferSizes_[index];
        applySetup(setup, &previousSetup);
    }

    void channelPairClicked(int pairIndex)
    {
        if (refreshing_)
            return;
        if (! juce::isPositiveAndBelow(pairIndex, channelToggles_.size()))
            return;

        auto* device = deviceManager_.getCurrentAudioDevice();
        if (device == nullptr)
            return;

        const int numChannels = juce::jmin(2, juce::jmax(device->getOutputChannelNames().size(),
                                                         device->getActiveOutputChannels().getHighestBit() + 1));

        // 最多 2 个输出通道：按 stereo pair 互斥选择，只启用当前这一对。
        juce::BigInteger channels;
        if (channelToggles_[pairIndex]->getToggleState()) {
            channels.setBit(pairIndex * 2);
            if (pairIndex * 2 + 1 < numChannels)
                channels.setBit(pairIndex * 2 + 1);
        }

        const auto previousSetup = deviceManager_.getAudioDeviceSetup();
        auto setup = previousSetup;
        setup.outputChannels = channels;
        setup.useDefaultOutputChannels = false;
        applySetup(setup, &previousSetup);
    }

    juce::AudioDeviceManager& deviceManager_;
    juce::Label typeLabel_;
    juce::ComboBox typeSelector_;
    juce::Label outputLabel_;
    juce::ComboBox outputDeviceSelector_;
    juce::TextButton testButton_;
    juce::Label sampleRateLabel_;
    juce::ComboBox sampleRateSelector_;
    juce::Label bufferSizeLabel_;
    juce::ComboBox bufferSizeSelector_;
    juce::Label channelsLabel_;
    juce::OwnedArray<juce::ToggleButton> channelToggles_;
    juce::Label errorLabel_;
    juce::StringArray availableTypeNames_;
    juce::StringArray availableDeviceNames_;
    juce::Array<double> availableSampleRates_;
    juce::Array<int> availableBufferSizes_;
    bool refreshing_ = false;
};

class AudioSettingsPage final : public juce::Component
{
public:
    AudioSettingsPage(std::unique_ptr<AudioDeviceSettingsContent> audioContent,
                      std::unique_ptr<juce::Component> renderingPriorityComponent,
                      int renderingPriorityHeight)
        : audioContent_(std::move(audioContent))
        , renderingPriorityComponent_(std::move(renderingPriorityComponent))
        , renderingPriorityHeight_(renderingPriorityHeight)
    {
        if (audioContent_ != nullptr) {
            addAndMakeVisible(audioContent_.get());
        }

        if (renderingPriorityComponent_ != nullptr) {
            addAndMakeVisible(renderingPriorityComponent_.get());
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        if (renderingPriorityComponent_ != nullptr) {
            renderingPriorityComponent_->setBounds(bounds.removeFromTop(renderingPriorityHeight_));
            bounds.removeFromTop(8);
        }
        if (audioContent_ != nullptr) {
            audioContent_->setBounds(bounds.removeFromTop(audioContent_->getPreferredHeight()));
        }
    }

private:
    std::unique_ptr<AudioDeviceSettingsContent> audioContent_;
    std::unique_ptr<juce::Component> renderingPriorityComponent_;
    int renderingPriorityHeight_ = 0;
};

class MouseTrailPage final : public juce::Component
{
public:
    static constexpr int kContentHeight = 74; // 20 + 34 + 20

    MouseTrailPage(AppPreferences& appPreferences, std::function<void()> onPreferencesChanged)
        : appPreferences_(appPreferences)
        , onPreferencesChanged_(std::move(onPreferencesChanged))
    {
        initialiseLabel(themeLabel_, LOC(kMouseTrail));
        addAndMakeVisible(themeLabel_);

        themeSelector_.addItem(LOC(kOff), 1);
        themeSelector_.addItem(LOC(kClassic), 2);
        themeSelector_.addItem(LOC(kNeon), 3);
        themeSelector_.addItem(LOC(kFire), 4);
        themeSelector_.addItem(LOC(kOcean), 5);
        themeSelector_.addItem(LOC(kGalaxy), 6);
        themeSelector_.addItem(LOC(kCherryBlossom), 7);
        themeSelector_.addItem(LOC(kMatrix), 8);
        themeSelector_.setSelectedId(static_cast<int>(appPreferences_.getState().standalone.mouseTrailTheme) + 1,
                                     juce::dontSendNotification);
        themeSelector_.onChange = [this] {
            appPreferences_.setMouseTrailTheme(static_cast<MouseTrailConfig::TrailTheme>(themeSelector_.getSelectedId() - 1));
            notifyChanged();
        };
        initialiseComboBox(themeSelector_);
        addAndMakeVisible(themeSelector_);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UIColors::backgroundDark);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20);
        auto row = bounds.removeFromTop(34);
        themeLabel_.setBounds(row.removeFromLeft(160));
        themeSelector_.setBounds(row.removeFromLeft(220).reduced(0, 4));
    }

private:
    void notifyChanged()
    {
        if (onPreferencesChanged_) {
            onPreferencesChanged_();
        }
    }

    AppPreferences& appPreferences_;
    std::function<void()> onPreferencesChanged_;
    juce::Label themeLabel_;
    juce::ComboBox themeSelector_;
};

} // namespace

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createAudioPages(
    juce::AudioDeviceManager* audioDeviceManager,
    AppPreferences& appPreferences,
    std::function<void()> onPreferencesChanged,
    std::function<void(bool forceCpu)> onRenderingPriorityChanged,
    std::function<void(VocoderModelWeight)> onVocoderModelWeightChanged,
    std::function<bool(F0ModelType)> onF0ModelChanged,
    std::function<void(bool)> onLightPitchCorrectionChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    if (audioDeviceManager != nullptr) {
        auto renderingPriorityPage = SharedPreferencePages::createRenderingPriorityComponent(
            appPreferences, onPreferencesChanged,
            std::move(onRenderingPriorityChanged),
            std::move(onVocoderModelWeightChanged),
            std::move(onF0ModelChanged),
            std::move(onLightPitchCorrectionChanged),
            false);
        auto audioContent = std::make_unique<AudioDeviceSettingsContent>(*audioDeviceManager);
        // renderingPriorityPage.height + 自绘音频设置内容高度
        // + AudioSettingsPage::resized 的上下 reduced(20) 内边距（否则底部错误行被裁掉）
        const int totalHeight = renderingPriorityPage.height + 8 + audioContent->getPreferredHeight() + 40;
        pages.push_back({ LOC(kAudio),
                          std::make_unique<AudioSettingsPage>(std::move(audioContent),
                                                              std::move(renderingPriorityPage.component),
                                                              renderingPriorityPage.height),
                          totalHeight });
    }
    return pages;
}

std::vector<TabbedPreferencesDialog::PageSpec> StandalonePreferencePages::createStandaloneOnlyPages(AppPreferences& appPreferences,
                                                                                                  std::function<void()> onPreferencesChanged)
{
    std::vector<TabbedPreferencesDialog::PageSpec> pages;
    pages.push_back({ LOC(kMouseTrail), std::make_unique<MouseTrailPage>(appPreferences, std::move(onPreferencesChanged)), MouseTrailPage::kContentHeight });
    return pages;
}

} // namespace OpenTune
