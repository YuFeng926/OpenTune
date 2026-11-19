#include "../Source/Utils/AudioDeviceSettings.h"
#include "../Source/Utils/AppLogger.h"

#include <cstdio>
#include <vector>

namespace OpenTune {

void AppLogger::logNoThrow(const juce::String&) noexcept {}

} // namespace OpenTune

namespace {

using OpenTune::AudioDeviceSettings::ApplyKind;
using OpenTune::AudioDeviceSettings::Recovery;

struct FakeDeviceRecord
{
    struct OpenCall
    {
        double sampleRate;
        int bufferSize;
    };

    juce::String name;
    juce::String openError;
    int createCount = 0;
    int openCount = 0;
    std::vector<double> sampleRates { 44100.0, 48000.0 };
    std::vector<int> bufferSizes { 128, 256 };
    double defaultSampleRate = 48000.0;
    int defaultBufferSize = 256;
    double forcedSampleRate = 0.0;
    int forcedBufferSize = 0;
    bool forceOutputChannels = false;
    juce::BigInteger forcedOutputChannels;
    juce::String inputDeviceName;
    juce::String outputDeviceName;
    std::vector<OpenCall> openCalls;
};

struct FakeDriver
{
    juce::String typeName;
    juce::String defaultOutput;
    std::vector<FakeDeviceRecord> devices;
    bool separateInputsAndOutputs = false;
    juce::String defaultInput;
    std::vector<juce::String> inputDeviceNames;
    std::vector<juce::String> outputDeviceNames;

    FakeDeviceRecord* find(const juce::String& name)
    {
        for (auto& device : devices)
            if (device.name == name)
                return &device;
        return nullptr;
    }

    FakeDeviceRecord* findEndpoints(const juce::String& outputName, const juce::String& inputName)
    {
        for (auto& device : devices)
            if (device.outputDeviceName == outputName && device.inputDeviceName == inputName)
                return &device;
        return nullptr;
    }

};

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    FakeAudioDevice(FakeDeviceRecord& record, const juce::String& typeName)
        : AudioIODevice(record.name, typeName), record_(record), sampleRate_(record.defaultSampleRate)
    {
    }

    ~FakeAudioDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "Output L", "Output R" }; }
    juce::StringArray getInputChannelNames() override { return { "Input L", "Input R" }; }
    juce::Array<double> getAvailableSampleRates() override
    {
        juce::Array<double> rates;
        for (const auto rate : record_.sampleRates)
            rates.add(rate);
        return rates;
    }
    juce::Array<int> getAvailableBufferSizes() override
    {
        juce::Array<int> sizes;
        for (const auto size : record_.bufferSizes)
            sizes.add(size);
        return sizes;
    }
    int getDefaultBufferSize() override { return record_.defaultBufferSize; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate,
                      int bufferSizeSamples) override
    {
        ++record_.openCount;
        record_.openCalls.push_back({ sampleRate, bufferSizeSamples });
        if (record_.openError.isNotEmpty())
            return record_.openError;

        inputChannels_ = inputChannels;
        outputChannels_ = record_.forceOutputChannels ? record_.forcedOutputChannels : outputChannels;
        sampleRate_ = record_.forcedSampleRate > 0.0 ? record_.forcedSampleRate : sampleRate;
        bufferSizeSamples_ = record_.forcedBufferSize > 0 ? record_.forcedBufferSize : bufferSizeSamples;
        open_ = true;
        return {};
    }

    void close() override
    {
        if (open_)
        {
            stop();
            open_ = false;
        }
    }

    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback*) override
    {
        playing_ = true;
    }
    void stop() override
    {
        playing_ = false;
    }
    bool isPlaying() override { return playing_; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSizeSamples_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return inputChannels_; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    FakeDeviceRecord& record_;
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    double sampleRate_ = 48000.0;
    int bufferSizeSamples_ = 256;
    bool open_ = false;
    bool playing_ = false;
};

class FakeAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    explicit FakeAudioDeviceType(FakeDriver& driver)
        : AudioIODeviceType(driver.typeName), driver_(driver)
    {
    }

    void scanForDevices() override {}

    juce::StringArray getDeviceNames(bool isInput) const override
    {
        juce::StringArray names;
        if (driver_.separateInputsAndOutputs)
        {
            const auto& endpoints = isInput ? driver_.inputDeviceNames : driver_.outputDeviceNames;
            for (const auto& name : endpoints)
                names.add(name);
            return names;
        }

        for (const auto& device : driver_.devices)
            names.add(device.name);
        return names;
    }

    int getDefaultDeviceIndex(bool isInput) const override
    {
        const auto names = getDeviceNames(isInput);
        const auto& defaultName = driver_.separateInputsAndOutputs && isInput
            ? driver_.defaultInput
            : driver_.defaultOutput;
        const int index = names.indexOf(defaultName);
        return index >= 0 ? index : (names.isEmpty() ? -1 : 0);
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool isInput) const override
    {
        if (device == nullptr)
            return -1;

        if (driver_.separateInputsAndOutputs)
        {
            for (const auto& record : driver_.devices)
            {
                if (record.name == device->getName())
                    return getDeviceNames(isInput).indexOf(isInput ? record.inputDeviceName : record.outputDeviceName);
            }
            return -1;
        }

        for (int i = 0; i < static_cast<int>(driver_.devices.size()); ++i)
            if (driver_.devices[static_cast<size_t>(i)].name == device->getName())
                return i;
        return -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return driver_.separateInputsAndOutputs; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override
    {
        const auto& name = outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName;
        auto* record = driver_.separateInputsAndOutputs
            ? driver_.findEndpoints(outputDeviceName, inputDeviceName)
            : driver_.find(name);
        if (record == nullptr)
            return nullptr;

        ++record->createCount;
        return new FakeAudioDevice(*record, driver_.typeName);
    }

private:
    FakeDriver& driver_;
};

class FakeAudioDeviceManager final : public juce::AudioDeviceManager
{
public:
    FakeAudioDeviceManager(FakeDriver& windows, FakeDriver& asio, FakeDriver* separate = nullptr)
        : windows_(windows), asio_(asio), separate_(separate)
    {
    }

    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override
    {
        types.add(new FakeAudioDeviceType(windows_));
        types.add(new FakeAudioDeviceType(asio_));
        if (separate_ != nullptr)
            types.add(new FakeAudioDeviceType(*separate_));
    }

private:
    FakeDriver& windows_;
    FakeDriver& asio_;
    FakeDriver* separate_;
};

FakeDriver makeWindowsDriver()
{
    FakeDriver driver;
    driver.typeName = "FakeWindows";
    driver.defaultOutput = "WindowsActive";
    driver.devices.push_back({ "WindowsActive" });
    return driver;
}

FakeDriver makeAsioDriver()
{
    FakeDriver driver;
    driver.typeName = "FakeASIO";
    driver.defaultOutput = "DefaultBroken";
    driver.devices.push_back({ "DefaultBroken", "default device must not open" });
    driver.devices.push_back({ "TargetGood" });
    driver.devices.push_back({ "TargetFail", "target open failed" });
    return driver;
}

FakeDriver makeSeparateDriver()
{
    FakeDriver driver;
    driver.typeName = "FakeSeparate";
    driver.separateInputsAndOutputs = true;
    driver.defaultInput = "InputPort";
    driver.defaultOutput = "OutputPort";
    driver.inputDeviceNames.push_back("InputPort");
    driver.outputDeviceNames.push_back("OutputPort");

    FakeDeviceRecord device;
    device.name = "CombinedDevice";
    device.inputDeviceName = "InputPort";
    device.outputDeviceName = "OutputPort";
    driver.devices.push_back(device);
    return driver;
}

struct TestRig
{
    FakeDriver windows = makeWindowsDriver();
    FakeDriver asio = makeAsioDriver();
    FakeAudioDeviceManager manager { windows, asio };

    bool openInitialWindowsDevice()
    {
        manager.getAvailableDeviceTypes();
        auto setup = manager.getAudioDeviceSetup();
        setup.outputDeviceName = "WindowsActive";
        setup.inputDeviceName.clear();
        setup.sampleRate = 48000.0;
        setup.bufferSize = 128;
        return manager.setAudioDeviceSetup(setup, true).isEmpty()
            && manager.getCurrentAudioDevice() != nullptr;
    }

    bool openInitialWindowsDeviceWithFourDefaultOutputs()
    {
        manager.getAvailableDeviceTypes();
        auto setup = manager.getAudioDeviceSetup();
        setup.outputDeviceName = "WindowsActive";
        setup.inputDeviceName.clear();
        setup.sampleRate = 48000.0;
        setup.bufferSize = 128;
        return manager.initialise(0, 4, nullptr, false, {}, &setup).isEmpty()
            && manager.getCurrentAudioDevice() != nullptr
            && manager.getAudioDeviceSetup().outputChannels.countNumberOfSetBits() == 4;
    }
};

bool savedSetupIs(juce::AudioDeviceManager& manager,
                  const juce::String& type,
                  const juce::String& output)
{
    const auto xml = manager.createStateXml();
    return xml != nullptr
        && xml->getStringAttribute("deviceType") == type
        && xml->getStringAttribute("audioOutputDeviceName") == output;
}

bool savedSetupHasFormat(juce::AudioDeviceManager& manager, double sampleRate, int bufferSize)
{
    const auto xml = manager.createStateXml();
    return xml != nullptr
        && xml->getDoubleAttribute("audioDeviceRate") == sampleRate
        && xml->getIntAttribute("audioDeviceBufferSize") == bufferSize;
}

juce::String savedSetupXml(juce::AudioDeviceManager& manager)
{
    const auto xml = manager.createStateXml();
    return xml != nullptr ? xml->toString() : juce::String{};
}

bool testBrowsingAnotherTypeLeavesActiveDeviceOpen()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    auto* const activeDevice = rig.manager.getCurrentAudioDevice();
    const int windowsCreates = rig.windows.devices[0].createCount;
    const int windowsOpens = rig.windows.devices[0].openCount;
    const auto snapshot = OpenTune::AudioDeviceSettings::scanAndRead(rig.manager, "FakeASIO");

    return snapshot.device.open
        && snapshot.device.deviceType == "FakeWindows"
        && snapshot.device.outputDevice == "WindowsActive"
        && snapshot.available.outputDevices.contains("DefaultBroken")
        && snapshot.available.outputDevices.contains("TargetGood")
        && rig.manager.getCurrentAudioDevice() == activeDevice
        && rig.manager.getCurrentAudioDeviceType() == "FakeWindows"
        && rig.windows.devices[0].createCount == windowsCreates
        && rig.windows.devices[0].openCount == windowsOpens
        && rig.asio.devices[0].createCount == 0
        && rig.asio.devices[1].createCount == 0
        && rig.asio.devices[2].createCount == 0;
}

bool testExplicitAsioTargetOpensAndPersists()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetGood";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.deviceType == "FakeASIO"
        && result.after.outputDevice == "TargetGood"
        && rig.asio.devices[0].createCount == 0
        && rig.asio.devices[0].openCount == 0
        && rig.asio.devices[1].createCount == 1
        && rig.asio.devices[1].openCount == 1
        && rig.asio.devices[2].createCount == 0
        && rig.asio.devices[2].openCount == 0
        && savedSetupIs(rig.manager, "FakeASIO", "TargetGood");
}

bool testCrossTypeWithoutOutputRejectsAmbiguousTarget()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    auto* const activeDevice = rig.manager.getCurrentAudioDevice();
    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();

    return result.kind == ApplyKind::unavailable
        && result.rejectedSetting == OpenTune::AudioDeviceSettings::RejectedSetting::outputDevice
        && result.before.open
        && result.before.deviceType == "FakeWindows"
        && result.before.outputDevice == "WindowsActive"
        && result.after.open
        && result.after.deviceType == "FakeWindows"
        && result.after.outputDevice == "WindowsActive"
        && rig.manager.getCurrentAudioDevice() == activeDevice
        && rig.manager.getCurrentAudioDeviceType() == "FakeWindows"
        && setup.outputDeviceName == "WindowsActive"
        && setup.sampleRate == 48000.0
        && setup.bufferSize == 128
        && rig.asio.devices[0].createCount == 0
        && rig.asio.devices[1].createCount == 0
        && rig.asio.devices[2].createCount == 0;
}

bool testCrossTypeSingleOutputCanBeInferred()
{
    TestRig rig;
    rig.asio.devices.clear();
    rig.asio.defaultOutput = "OnlyOutput";
    rig.asio.devices.push_back({ "OnlyOutput" });
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto& target = rig.asio.devices[0];

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.deviceType == "FakeASIO"
        && result.after.outputDevice == "OnlyOutput"
        && target.createCount == 1
        && target.openCount == 1
        && target.openCalls.size() == 1;
}

bool testSameNameExplicitTargetCreatesRequestedType()
{
    TestRig rig;
    rig.asio.devices.push_back({ "WindowsActive" });
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "WindowsActive";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.deviceType == "FakeASIO"
        && result.after.outputDevice == "WindowsActive"
        && currentDevice != nullptr
        && currentDevice->getTypeName() == "FakeASIO"
        && rig.asio.devices.back().createCount == 1
        && rig.asio.devices.back().openCount == 1
        && savedSetupIs(rig.manager, "FakeASIO", "WindowsActive");
}

bool testOmittedFormatUsesAndPersistsTargetDefaults()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    auto& target = rig.asio.devices[1];
    target.sampleRates = { 96000.0, 88200.0 };
    target.bufferSizes = { 512, 1024 };
    target.defaultSampleRate = 96000.0;
    target.defaultBufferSize = 512;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetGood";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.deviceType == "FakeASIO"
        && result.after.outputDevice == "TargetGood"
        && result.after.sampleRateHz == 96000.0
        && result.after.bufferSizeFrame == 512
        && setup.sampleRate == 96000.0
        && setup.bufferSize == 512
        && currentDevice != nullptr
        && currentDevice->getCurrentSampleRate() == 96000.0
        && currentDevice->getCurrentBufferSizeSamples() == 512
        && savedSetupIs(rig.manager, "FakeASIO", "TargetGood")
        && savedSetupHasFormat(rig.manager, 96000.0, 512);
}

bool testSameTypeOmittedFormatUsesTargetDefaults()
{
    TestRig rig;
    FakeDeviceRecord target { "WindowsTarget" };
    target.sampleRates = { 44100.0, 48000.0, 96000.0 };
    target.bufferSizes = { 128, 256, 512 };
    target.defaultSampleRate = 96000.0;
    target.defaultBufferSize = 512;
    rig.windows.devices.push_back(target);
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeWindows";
    request.outputDevice = "WindowsTarget";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();
    const auto& openedTarget = rig.windows.devices[1];

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.outputDevice == "WindowsTarget"
        && result.after.sampleRateHz == 96000.0
        && result.after.bufferSizeFrame == 512
        && setup.sampleRate == 96000.0
        && setup.bufferSize == 512
        && currentDevice != nullptr
        && currentDevice->getCurrentSampleRate() == 96000.0
        && currentDevice->getCurrentBufferSizeSamples() == 512
        && openedTarget.createCount == 1
        && openedTarget.openCount == 1
        && openedTarget.openCalls.size() == 1
        && openedTarget.openCalls[0].sampleRate == 96000.0
        && openedTarget.openCalls[0].bufferSize == 512;
}

bool testSameTypeExplicitFormatOpensFinalValuesOnce()
{
    TestRig rig;
    FakeDeviceRecord target { "WindowsExplicitTarget" };
    target.sampleRates = { 44100.0, 48000.0, 96000.0 };
    target.bufferSizes = { 128, 256, 512 };
    target.defaultSampleRate = 44100.0;
    target.defaultBufferSize = 256;
    rig.windows.devices.push_back(target);
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeWindows";
    request.outputDevice = "WindowsExplicitTarget";
    request.sampleRateHz = 96000.0;
    request.bufferSizeFrame = 512;
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto& openedTarget = rig.windows.devices[1];

    return result.kind == ApplyKind::applied
        && result.after.open
        && result.after.outputDevice == "WindowsExplicitTarget"
        && result.after.sampleRateHz == 96000.0
        && result.after.bufferSizeFrame == 512
        && openedTarget.createCount == 1
        && openedTarget.openCount == 1
        && openedTarget.openCalls.size() == 1
        && openedTarget.openCalls[0].sampleRate == 96000.0
        && openedTarget.openCalls[0].bufferSize == 512;
}

bool mismatchRestoresPriorSetup(TestRig& rig,
                                OpenTune::AudioDeviceSettings::Request request)
{
    const auto originalXml = savedSetupXml(rig.manager);
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();

    return result.kind != ApplyKind::applied
        && result.recovery == Recovery::restored
        && result.before.open
        && result.before.deviceType == "FakeWindows"
        && result.before.outputDevice == "WindowsActive"
        && result.before.sampleRateHz == 48000.0
        && result.before.bufferSizeFrame == 128
        && result.after.open
        && result.after.deviceType == "FakeWindows"
        && result.after.outputDevice == "WindowsActive"
        && result.after.sampleRateHz == 48000.0
        && result.after.bufferSizeFrame == 128
        && setup.outputDeviceName == "WindowsActive"
        && setup.sampleRate == 48000.0
        && setup.bufferSize == 128
        && currentDevice != nullptr
        && currentDevice->getTypeName() == "FakeWindows"
        && currentDevice->getCurrentSampleRate() == 48000.0
        && currentDevice->getCurrentBufferSizeSamples() == 128
        && rig.asio.devices[1].createCount == 1
        && rig.asio.devices[1].openCount == 1
        && !originalXml.isEmpty()
        && savedSetupXml(rig.manager) == originalXml
        && savedSetupIs(rig.manager, "FakeWindows", "WindowsActive")
        && savedSetupHasFormat(rig.manager, 48000.0, 128);
}

bool sameTypeMismatchRestoresPriorSetup(TestRig& rig,
                                        OpenTune::AudioDeviceSettings::Request request)
{
    const auto originalXml = savedSetupXml(rig.manager);
    const int originalCreates = rig.windows.devices[0].createCount;
    const int originalOpens = rig.windows.devices[0].openCount;
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();

    return result.kind == ApplyKind::openFailed
        && result.cause == ApplyKind::openFailed
        && result.recovery == Recovery::restored
        && result.before.sampleRateHz == 48000.0
        && result.before.bufferSizeFrame == 128
        && result.after.open
        && result.after.deviceType == "FakeWindows"
        && result.after.outputDevice == "WindowsActive"
        && result.after.sampleRateHz == 48000.0
        && result.after.bufferSizeFrame == 128
        && setup.outputDeviceName == "WindowsActive"
        && setup.sampleRate == 48000.0
        && setup.bufferSize == 128
        && currentDevice != nullptr
        && currentDevice->getTypeName() == "FakeWindows"
        && currentDevice->getCurrentSampleRate() == 48000.0
        && currentDevice->getCurrentBufferSizeSamples() == 128
        && rig.windows.devices[0].createCount == originalCreates
        && rig.windows.devices[0].openCount == originalOpens + 1
        && savedSetupXml(rig.manager) == originalXml
        && savedSetupIs(rig.manager, "FakeWindows", "WindowsActive")
        && savedSetupHasFormat(rig.manager, 48000.0, 128);
}

bool testSameTypeSampleRateMismatchRestoresPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.windows.devices[0].forcedSampleRate = 48000.0;
    OpenTune::AudioDeviceSettings::Request request;
    request.sampleRateHz = 44100.0;
    return sameTypeMismatchRestoresPriorSetup(rig, request);
}

bool testSameTypeBufferMismatchRestoresPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.windows.devices[0].forcedBufferSize = 128;
    OpenTune::AudioDeviceSettings::Request request;
    request.bufferSizeFrame = 256;
    return sameTypeMismatchRestoresPriorSetup(rig, request);
}

bool testExplicitChannelMismatchRestoresDefaultChannels()
{
    TestRig rig;
    if (!rig.openInitialWindowsDeviceWithFourDefaultOutputs())
        return false;

    auto& target = rig.asio.devices[1];
    target.forceOutputChannels = true;
    target.forcedOutputChannels.setBit(0);

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetGood";
    request.outputChannelPair = 0;
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);
    const auto setup = rig.manager.getAudioDeviceSetup();
    auto* const currentDevice = rig.manager.getCurrentAudioDevice();

    return result.kind == ApplyKind::openFailed
        && result.recovery == Recovery::restored
        && result.after.open
        && result.after.deviceType == "FakeWindows"
        && result.after.activeOutputChannels.countNumberOfSetBits() == 4
        && setup.useDefaultOutputChannels
        && setup.outputChannels.countNumberOfSetBits() == 4
        && currentDevice != nullptr
        && currentDevice->getTypeName() == "FakeWindows"
        && currentDevice->getActiveOutputChannels().countNumberOfSetBits() == 4;
}

bool testExplicitSampleRateMismatchRestoresPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.asio.devices[1].sampleRates = { 96000.0, 48000.0 };
    rig.asio.devices[1].bufferSizes = { 512, 256 };
    rig.asio.devices[1].defaultBufferSize = 512;
    rig.asio.devices[1].forcedSampleRate = 48000.0;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetGood";
    request.sampleRateHz = 96000.0;
    request.bufferSizeFrame = 512;
    return mismatchRestoresPriorSetup(rig, request);
}

bool testExplicitBufferMismatchRestoresPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.asio.devices[1].sampleRates = { 96000.0, 48000.0 };
    rig.asio.devices[1].bufferSizes = { 512, 256 };
    rig.asio.devices[1].defaultBufferSize = 512;
    rig.asio.devices[1].forcedBufferSize = 256;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetGood";
    request.sampleRateHz = 96000.0;
    request.bufferSizeFrame = 512;
    return mismatchRestoresPriorSetup(rig, request);
}

bool testTargetOpenFailureRestoresPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetFail";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);

    return result.kind == ApplyKind::openFailed
        && result.deviceError == "target open failed"
        && result.recovery == Recovery::restored
        && result.after.open
        && result.after.deviceType == "FakeWindows"
        && result.after.outputDevice == "WindowsActive"
        && rig.manager.getAudioDeviceSetup().outputDeviceName == "WindowsActive"
        && savedSetupIs(rig.manager, "FakeWindows", "WindowsActive");
}

bool testFailedRecoveryReportsClosedPriorSetup()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.windows.devices[0].openError = "windows restore failed";
    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetFail";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);

    return result.kind == ApplyKind::priorSetupNotRestored
        && result.cause == ApplyKind::openFailed
        && result.deviceError == "target open failed"
        && result.recovery == Recovery::failed
        && !result.after.open
        && rig.windows.devices[0].createCount == 2
        && rig.windows.devices[0].openCount == 2;
}

bool testRecoveryFormatMismatchReportsPriorSetupNotRestored()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    rig.windows.devices[0].forcedSampleRate = 44100.0;
    rig.windows.devices[0].forcedBufferSize = 256;
    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetFail";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);

    return result.kind == ApplyKind::priorSetupNotRestored
        && result.cause == ApplyKind::openFailed
        && result.deviceError == "target open failed"
        && result.recovery == Recovery::failed
        && result.recoveryError.isNotEmpty()
        && !result.after.open
        && rig.manager.getCurrentAudioDevice() == nullptr
        && rig.windows.devices[0].createCount == 2
        && rig.windows.devices[0].openCount == 2
        && rig.windows.devices[0].openCalls.size() == 2
        && rig.windows.devices[0].openCalls[1].sampleRate == 48000.0
        && rig.windows.devices[0].openCalls[1].bufferSize == 128;
}

bool testClosedPriorSetupFailureDoesNotReopenPriorDevice()
{
    TestRig rig;
    if (!rig.openInitialWindowsDevice())
        return false;

    const auto priorSetup = rig.manager.getAudioDeviceSetup();
    const auto priorXml = savedSetupXml(rig.manager);
    const int windowsCreates = rig.windows.devices[0].createCount;
    rig.manager.closeAudioDevice();

    OpenTune::AudioDeviceSettings::Request request;
    request.deviceType = "FakeASIO";
    request.outputDevice = "TargetFail";
    const auto result = OpenTune::AudioDeviceSettings::apply(rig.manager, request);

    return result.kind == ApplyKind::openFailed
        && result.recovery == Recovery::restored
        && !result.after.open
        && rig.manager.getCurrentAudioDevice() == nullptr
        && rig.manager.getCurrentAudioDeviceType() == "FakeWindows"
        && rig.manager.getAudioDeviceSetup() == priorSetup
        && rig.windows.devices[0].createCount == windowsCreates
        && savedSetupXml(rig.manager) == priorXml;
}

bool testClosedSetupPersistsFormatAndExplicitChannels()
{
    TestRig rig;
    auto setup = rig.manager.getAudioDeviceSetup();
    setup.inputDeviceName = "TargetGood";
    setup.outputDeviceName = "TargetGood";
    setup.sampleRate = 44100.0;
    setup.bufferSize = 128;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setBit(0);
    setup.inputChannels.setBit(2);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setBit(1);
    setup.outputChannels.setBit(3);

    if (rig.manager.setCurrentAudioDeviceTypeAndSetup("FakeASIO", setup, true, false).isNotEmpty()
        || rig.manager.getCurrentAudioDevice() != nullptr)
        return false;

    const auto xml = rig.manager.createStateXml();
    if (xml == nullptr
        || xml->getDoubleAttribute("audioDeviceRate") != setup.sampleRate
        || xml->getIntAttribute("audioDeviceBufferSize") != setup.bufferSize
        || xml->getStringAttribute("audioDeviceInChans") != setup.inputChannels.toString(2)
        || xml->getStringAttribute("audioDeviceOutChans") != setup.outputChannels.toString(2))
        return false;

    TestRig imported;
    if (imported.manager.initialise(0, 0, xml.get(), false).isNotEmpty())
        return false;

    const auto importedSetup = imported.manager.getAudioDeviceSetup();
    auto* const importedDevice = imported.manager.getCurrentAudioDevice();
    return importedDevice != nullptr
        && importedDevice->isOpen()
        && importedSetup.sampleRate == setup.sampleRate
        && importedSetup.bufferSize == setup.bufferSize
        && ! importedSetup.useDefaultInputChannels
        && importedSetup.inputChannels == setup.inputChannels
        && ! importedSetup.useDefaultOutputChannels
        && importedSetup.outputChannels == setup.outputChannels
        && importedDevice->getActiveInputChannels() == setup.inputChannels
        && importedDevice->getActiveOutputChannels() == setup.outputChannels;
}

bool testClosedDefaultOutputSetupKeepsRequestedChannelCount()
{
    TestRig rig;
    if (!rig.openInitialWindowsDeviceWithFourDefaultOutputs())
        return false;

    auto setup = rig.manager.getAudioDeviceSetup();
    setup.outputChannels.clear();
    setup.useDefaultOutputChannels = true;

    if (rig.manager.setCurrentAudioDeviceTypeAndSetup("FakeWindows", setup, true, false).isNotEmpty()
        || rig.manager.getCurrentAudioDevice() != nullptr
        || ! rig.manager.getAudioDeviceSetup().outputChannels.isZero())
        return false;

    if (rig.manager.setCurrentAudioDeviceTypeAndSetup("FakeWindows", setup, true, true).isNotEmpty())
        return false;

    auto* const device = rig.manager.getCurrentAudioDevice();
    const auto openedSetup = rig.manager.getAudioDeviceSetup();
    return device != nullptr
        && device->isOpen()
        && openedSetup.useDefaultOutputChannels
        && openedSetup.outputChannels.countNumberOfSetBits() == 4
        && device->getActiveOutputChannels().countNumberOfSetBits() == 4;
}

bool testSeparateInputAndOutputSetupAcceptsCombinedDeviceName()
{
    auto windows = makeWindowsDriver();
    auto asio = makeAsioDriver();
    auto separate = makeSeparateDriver();
    FakeAudioDeviceManager manager { windows, asio, &separate };

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = "InputPort";
    setup.outputDeviceName = "OutputPort";
    setup.sampleRate = 44100.0;
    setup.bufferSize = 128;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setBit(0);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setBit(0);

    if (manager.setCurrentAudioDeviceTypeAndSetup("FakeSeparate", setup, true, true).isNotEmpty())
        return false;

    auto* const device = manager.getCurrentAudioDevice();
    auto* const type = manager.getCurrentDeviceTypeObject();
    const auto currentSetup = manager.getAudioDeviceSetup();
    return device != nullptr
        && device->isOpen()
        && device->getName() == "CombinedDevice"
        && currentSetup.inputDeviceName == "InputPort"
        && currentSetup.outputDeviceName == "OutputPort"
        && currentSetup.sampleRate == setup.sampleRate
        && currentSetup.bufferSize == setup.bufferSize
        && type != nullptr
        && type->getIndexOfDevice(device, true) == 0
        && type->getIndexOfDevice(device, false) == 0
        && device->getActiveInputChannels() == setup.inputChannels
        && device->getActiveOutputChannels() == setup.outputChannels;
}

} // namespace

int main()
{
    if (!testBrowsingAnotherTypeLeavesActiveDeviceOpen())
    {
        std::fputs("FAIL: browsing FakeASIO leaves the active FakeWindows device unchanged\n", stderr);
        return 1;
    }
    if (!testExplicitAsioTargetOpensAndPersists())
    {
        std::fputs("FAIL: explicit ASIO target opens without trying the default and persists\n", stderr);
        return 1;
    }
    if (!testCrossTypeWithoutOutputRejectsAmbiguousTarget())
    {
        std::fputs("FAIL: cross-type request without an output rejects an ambiguous target\n", stderr);
        return 1;
    }
    if (!testCrossTypeSingleOutputCanBeInferred())
    {
        std::fputs("FAIL: cross-type request infers and opens the sole output device once\n", stderr);
        return 1;
    }
    if (!testSameNameExplicitTargetCreatesRequestedType())
    {
        std::fputs("FAIL: same-name explicit target creates the requested device type\n", stderr);
        return 1;
    }
    if (!testOmittedFormatUsesAndPersistsTargetDefaults())
    {
        std::fputs("FAIL: omitted format uses and persists the target defaults\n", stderr);
        return 1;
    }
    if (!testSameTypeOmittedFormatUsesTargetDefaults())
    {
        std::fputs("FAIL: same-type switch uses target defaults when format is omitted\n", stderr);
        return 1;
    }
    if (!testSameTypeExplicitFormatOpensFinalValuesOnce())
    {
        std::fputs("FAIL: same-type switch opens explicit final format once\n", stderr);
        return 1;
    }
    if (!testExplicitSampleRateMismatchRestoresPriorSetup())
    {
        std::fputs("FAIL: explicit sample rate mismatch restores the prior setup and XML\n", stderr);
        return 1;
    }
    if (!testExplicitBufferMismatchRestoresPriorSetup())
    {
        std::fputs("FAIL: explicit buffer mismatch restores the prior setup and XML\n", stderr);
        return 1;
    }
    if (!testSameTypeSampleRateMismatchRestoresPriorSetup())
    {
        std::fputs("FAIL: same-type sample-rate mismatch restores the prior setup and XML\n", stderr);
        return 1;
    }
    if (!testSameTypeBufferMismatchRestoresPriorSetup())
    {
        std::fputs("FAIL: same-type buffer mismatch restores the prior setup and XML\n", stderr);
        return 1;
    }
    if (!testExplicitChannelMismatchRestoresDefaultChannels())
    {
        std::fputs("FAIL: explicit-channel mismatch restores the prior default channel count\n", stderr);
        return 1;
    }
    if (!testTargetOpenFailureRestoresPriorSetup())
    {
        std::fputs("FAIL: target open error is returned and the prior setup is restored\n", stderr);
        return 1;
    }
    if (!testFailedRecoveryReportsClosedPriorSetup())
    {
        std::fputs("FAIL: failed recovery reports priorSetupNotRestored with no open device\n", stderr);
        return 1;
    }
    if (!testRecoveryFormatMismatchReportsPriorSetupNotRestored())
    {
        std::fputs("FAIL: recovery format mismatch reports both errors and leaves no device open\n", stderr);
        return 1;
    }
    if (!testClosedPriorSetupFailureDoesNotReopenPriorDevice())
    {
        std::fputs("FAIL: restoring a closed prior setup does not recreate its device\n", stderr);
        return 1;
    }
    if (!testClosedSetupPersistsFormatAndExplicitChannels())
    {
        std::fputs("FAIL: a closed setup persists format and explicit channels in manager XML\n", stderr);
        return 1;
    }
    if (!testClosedDefaultOutputSetupKeepsRequestedChannelCount())
    {
        std::fputs("FAIL: a closed default-output setup keeps its requested channel count\n", stderr);
        return 1;
    }
    if (!testSeparateInputAndOutputSetupAcceptsCombinedDeviceName())
    {
        std::fputs("FAIL: separate input and output endpoints accept a combined device name\n", stderr);
        return 1;
    }

    std::puts("All AudioDeviceSettings tests passed.");
    return 0;
}
