#include "RealtimeAudit.h"

#if !defined(_WIN32)
    #error RealtimeCallbackTests requires the Windows IAT audit hooks.
#endif

#define NOMINMAX
#include <windows.h>

#include "../Source/PluginProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_events/juce_events.h>

#if !JucePlugin_Build_Standalone
    #include <ARA_Library/Dispatch/ARAHostDispatch.h>
    #include "../Source/Plugin/Capture/CaptureSession.h"
    #include "../Source/ARA/OpenTuneDocumentController.h"
    #include "../Source/DSP/ResamplingManager.h"
    #include "../Source/Render/ContentRenderService.h"
    #include "../Source/Utils/PlaybackSampleGeometry.h"
    #include "../Source/Utils/TimeCoordinate.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <malloc.h>
#include <memory>
#include <thread>

#if !JucePlugin_Build_Standalone
extern const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
#endif

namespace
{
using OpenTune::RealtimeAudit::Result;
using OpenTune::RealtimeAuditTest::WindowsHookReport;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;
constexpr int pcmSampleCount = 48000;
constexpr uint64_t captureSegmentId = 4810;

int failures = 0;

void check(bool passed, const char* reason)
{
    if (! passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        ++failures;
    }
}

template <typename Callback>
Result audit(const char* label, Callback&& callback)
{
    OpenTune::RealtimeAudit::Scope scope(label);
    callback();
    return scope.finish();
}

void checkClean(const Result& result)
{
    OpenTune::RealtimeAuditTest::reportResult(result);
    check(OpenTune::RealtimeAuditTest::isClean(result), result.label);
}

juce::AudioPlayHead::PositionInfo makePlayingPosition()
{
    juce::AudioPlayHead::PositionInfo position;
    position.setTimeInSamples(0);
    position.setTimeInSeconds(0.0);
    position.setIsPlaying(true);
    return position;
}

class FixedPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override { return position_; }
    void setPosition(juce::Optional<PositionInfo> position) { position_ = std::move(position); }

private:
    juce::Optional<PositionInfo> position_ = makePlayingPosition();
};

void fillFixedPcm(juce::AudioBuffer<float>& pcm)
{
    for (int channel = 0; channel < pcm.getNumChannels(); ++channel)
    {
        auto* samples = pcm.getWritePointer(channel);
        std::fill_n(samples, static_cast<size_t>(pcm.getNumSamples()), 0.25f);
    }
}

bool hasAudibleOutput(const juce::AudioBuffer<float>& buffer)
{
    return buffer.getMagnitude(0, 0, buffer.getNumSamples()) > 0.0001f;
}

#if !JucePlugin_Build_Standalone
bool publishCaptureTestPlaybackSource(OpenTune::OpenTuneAudioProcessor& processor,
                                      OpenTune::ContentKey key)
{
    auto* renderService = processor.getContentRenderService();
    auto snapshot = processor.getContentSnapshot(key);
    if (renderService == nullptr || snapshot == nullptr
        || snapshot->nativeSource.buffer == nullptr)
        return false;

    const auto& native = snapshot->nativeSource;
    constexpr double canonicalRate = OpenTune::TimeCoordinate::kRenderSampleRate;
    const int64_t firstCanonicalSample = OpenTune::PlaybackSampleGeometry::projectToSample(
        snapshot->sourceWindow.sourceStartSeconds * canonicalRate);
    const int64_t canonicalEndSample = std::max(firstCanonicalSample + 1,
        OpenTune::PlaybackSampleGeometry::projectToSample(
            snapshot->sourceWindow.sourceEndSeconds * canonicalRate));
    const int canonicalLength = static_cast<int>(canonicalEndSample - firstCanonicalSample);

    auto canonicalBuffer = std::make_shared<juce::AudioBuffer<float>>(
        native.buffer->getNumChannels(), canonicalLength);
    OpenTune::ResamplingManager resampler;
    for (int channel = 0; channel < canonicalBuffer->getNumChannels(); ++channel)
    {
        const auto samples = resampler.resampleRange(
            native.buffer->getReadPointer(channel),
            static_cast<size_t>(native.buffer->getNumSamples()),
            native.sampleRate, canonicalRate, native.firstSourceSample,
            firstCanonicalSample, canonicalLength);
        canonicalBuffer->copyFrom(channel, 0, samples.data(), canonicalLength);
    }

    auto source = OpenTune::makePlaybackReadSource(
        key, std::move(snapshot), native, std::move(canonicalBuffer), canonicalRate,
        firstCanonicalSample, renderService->getOrCreateRenderCache(key),
        renderService->getTimeStretchCache());
    renderService->publishPlaybackSource(key, std::move(source));
    processor.republishPlaybackSource(key);
    return true;
}
#endif

bool installPlayableContent(OpenTune::OpenTuneAudioProcessor& processor,
                            const char* failureReason)
{
#if JucePlugin_Build_Standalone
    juce::AudioBuffer<float> pcm(2, pcmSampleCount);
    fillFixedPcm(pcm);
    OpenTune::OpenTuneAudioProcessor::PreparedImport prepared;
    if (! OpenTune::OpenTuneAudioProcessor::prepareImport(
            std::move(pcm), sampleRate, "RealtimeCallbackTests", {}, prepared))
    {
        check(false, failureReason);
        return false;
    }

    const auto committed = processor.commitPreparedImportAsPlacement(
        std::move(prepared), { 0, 0.0 }, true);
    check(committed.isValid(), failureReason);
    if (! committed.isValid())
        return false;

    processor.refreshStandalonePlaybackSnapshot();
    processor.play();
    return true;
#else
    auto* session = processor.getCaptureSession();
    check(session != nullptr, failureReason);
    if (session == nullptr)
        return false;

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(2, pcmSampleCount);
    fillFixedPcm(*pcm);
    const auto injectedId = session->testInjectEditedSegment(
        0.0, 1.0, captureSegmentId, pcm, sampleRate);
    const bool published = publishCaptureTestPlaybackSource(
        processor, { OpenTune::DomainKind::RegularVST3Capture, injectedId, 0 });
    check(published, failureReason);
    return published;
#endif
}

struct FakeAudioDeviceLifecycle
{
    int opens = 0;
    int closes = 0;
    int starts = 0;
    int stops = 0;
};

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    explicit FakeAudioDevice(FakeAudioDeviceLifecycle& lifecycle)
        : AudioIODevice("RealtimeAuditFake", "RealtimeAudit"), lifecycle_(lifecycle)
    {
        inputChannels_.setBit(0);
        inputChannels_.setBit(1);
        outputChannels_.setBit(0);
        outputChannels_.setBit(1);
    }

    ~FakeAudioDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "Left", "Right" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, sampleRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, blockSize }; }
    int getDefaultBufferSize() override { return blockSize; }
    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRateToUse,
                      int bufferSizeToUse) override
    {
        close();
        inputChannels_ = inputChannels;
        outputChannels_ = outputChannels;
        sampleRate_ = sampleRateToUse;
        bufferSize_ = bufferSizeToUse;
        open_ = true;
        ++lifecycle_.opens;
        return {};
    }
    void close() override
    {
        if (open_)
        {
            stop();
            open_ = false;
            ++lifecycle_.closes;
        }
    }
    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback* callback) override
    {
        callback_ = callback;
        if (! playing_)
            ++lifecycle_.starts;
        playing_ = true;
        callback_->audioDeviceAboutToStart(this);
    }
    void stop() override
    {
        if (playing_)
        {
            callback_->audioDeviceStopped();
            playing_ = false;
            ++lifecycle_.stops;
        }
    }
    bool isPlaying() override { return playing_; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return inputChannels_; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    void processOneBlock()
    {
        std::array<float, blockSize> inputLeft{};
        std::array<float, blockSize> inputRight{};
        outputLeft_.fill(0.0f);
        outputRight_.fill(0.0f);
        inputLeft.fill(0.125f);
        inputRight.fill(0.125f);
        const float* inputs[] = { inputLeft.data(), inputRight.data() };
        float* outputs[] = { outputLeft_.data(), outputRight_.data() };
        const juce::AudioIODeviceCallbackContext context{};
        callback_->audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2,
                                                    bufferSize_, context);
        outputPeak_ = 0.0f;
        for (int sample = 0; sample < bufferSize_; ++sample)
            outputPeak_ = std::max(outputPeak_, std::abs(outputLeft_[static_cast<size_t>(sample)]));
    }

    float outputFirstLeft() const noexcept { return outputLeft_[0]; }
    float outputPeak() const noexcept { return outputPeak_; }
    bool matchesPreparedPcm(const juce::AudioBuffer<float>& expected) const noexcept
    {
        return expected.getNumChannels() == 2 && expected.getNumSamples() >= bufferSize_
            && std::equal(outputLeft_.begin(), outputLeft_.begin() + bufferSize_,
                          expected.getReadPointer(0))
            && std::equal(outputRight_.begin(), outputRight_.begin() + bufferSize_,
                          expected.getReadPointer(1));
    }

private:
    FakeAudioDeviceLifecycle& lifecycle_;
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    juce::AudioIODeviceCallback* callback_ = nullptr;
    double sampleRate_ = sampleRate;
    int bufferSize_ = blockSize;
    std::array<float, blockSize> outputLeft_{};
    std::array<float, blockSize> outputRight_{};
    float outputPeak_ = 0.0f;
    bool open_ = false;
    bool playing_ = false;
};

#if !JucePlugin_Build_Standalone
struct HostAudioAccess final : ARA::Host::AudioAccessControllerInterface
{
    static constexpr ARA::ARASamplePosition pcmSampleCount = 48000;

    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef,
                                                          bool) noexcept override
    {
        return reinterpret_cast<ARA::ARAAudioReaderHostRef>(this);
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef, ARA::ARASamplePosition samplePosition,
                          ARA::ARASampleCount samplesPerChannel, void* const buffers[]) noexcept override
    {
        const auto sampleCount = static_cast<ARA::ARASamplePosition>(samplesPerChannel);
        if (samplePosition < 0 || samplePosition + sampleCount > pcmSampleCount)
            return false;
        std::fill_n(static_cast<float*>(buffers[0]), static_cast<size_t>(samplesPerChannel), 0.25f);
        ++sampleReads_;
        return true;
    }

    void destroyAudioReader(ARA::ARAAudioReaderHostRef) noexcept override {}
    int sampleReads() const noexcept { return sampleReads_; }

private:
    int sampleReads_ = 0;
};

struct HostArchiving final : ARA::Host::ArchivingControllerInterface
{
    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef, ARA::ARASize,
                              ARA::ARASize, ARA::ARAByte[]) noexcept override { return false; }
    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef, ARA::ARASize,
                             ARA::ARASize, const ARA::ARAByte[]) noexcept override { return false; }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return "RealtimeCallbackTests";
    }
};

class AraFixture final
{
public:
    explicit AraFixture(const ARA::ARAFactory* factory)
        : factory_(factory), host_(&audioAccess_, &archiving_)
    {
    }

    ~AraFixture()
    {
        if (processor_ != nullptr)
            processor_->releaseResources();

        if (processor_ != nullptr && playbackRegionRef_ != nullptr && removePlaybackRegion_)
        {
            removePlaybackRegion_();
            processor_->collectPlaybackSnapshots();
        }
        processor_.reset();
        if (documentController_ != nullptr)
        {
            if (audioSourceRef_ != nullptr)
                documentController_->enableAudioSourceSamplesAccess(audioSourceRef_, false);
            if (playbackRegionRef_ != nullptr)
                documentController_->destroyPlaybackRegion(playbackRegionRef_);
            if (audioModificationRef_ != nullptr)
                documentController_->destroyAudioModification(audioModificationRef_);
            if (audioSourceRef_ != nullptr)
                documentController_->destroyAudioSource(audioSourceRef_);
            if (regionSequenceRef_ != nullptr)
                documentController_->destroyRegionSequence(regionSequenceRef_);
            if (musicalContextRef_ != nullptr)
                documentController_->destroyMusicalContext(musicalContextRef_);
            documentController_->destroyDocumentController();
            documentController_.reset();
        }
        if (initialized_)
            factory_->uninitializeARA();
    }

    bool initialize()
    {
        ARA::ARAInterfaceConfiguration configuration{};
        configuration.structSize = sizeof(configuration);
        configuration.desiredApiGeneration = factory_->highestSupportedApiGeneration;
        factory_->initializeARAWithConfiguration(&configuration);
        initialized_ = true;

        ARA::ARADocumentProperties properties{};
        properties.structSize = sizeof(properties);
        properties.name = "RealtimeCallbackTests";
        const auto* instance = factory_->createDocumentControllerWithDocument(
            &host_, &properties);
        if (instance == nullptr)
            return false;

        documentController_ = std::make_unique<ARA::Host::DocumentController>(instance);
        documentControllerRef_ = instance->documentControllerRef;

        ARA::ARAMusicalContextProperties musicalContextProperties{};
        musicalContextProperties.structSize = sizeof(musicalContextProperties);
        musicalContextProperties.orderIndex = 0;
        musicalContextRef_ = documentController_->createMusicalContext(
            nullptr, &musicalContextProperties);
        if (musicalContextRef_ == nullptr)
            return false;

        ARA::ARARegionSequenceProperties sequenceProperties{};
        sequenceProperties.structSize = sizeof(sequenceProperties);
        sequenceProperties.orderIndex = 0;
        sequenceProperties.musicalContextRef = musicalContextRef_;
        regionSequenceRef_ = documentController_->createRegionSequence(nullptr, &sequenceProperties);
        if (regionSequenceRef_ == nullptr)
            return false;

        ARA::ARAAudioSourceProperties sourceProperties{};
        sourceProperties.structSize = sizeof(sourceProperties);
        sourceProperties.name = "RealtimeCallbackPCM";
        sourceProperties.persistentID = "RealtimeCallbackPCM";
        sourceProperties.sampleCount = HostAudioAccess::pcmSampleCount;
        sourceProperties.sampleRate = sampleRate;
        sourceProperties.channelCount = 1;
        audioSourceRef_ = documentController_->createAudioSource(nullptr, &sourceProperties);
        if (audioSourceRef_ == nullptr)
            return false;

        ARA::ARAAudioModificationProperties modificationProperties{};
        modificationProperties.structSize = sizeof(modificationProperties);
        modificationProperties.persistentID = "RealtimeCallbackModification";
        audioModificationRef_ = documentController_->createAudioModification(
            audioSourceRef_, nullptr, &modificationProperties);
        if (audioModificationRef_ == nullptr)
            return false;

        ARA::ARAPlaybackRegionProperties regionProperties{};
        regionProperties.structSize = sizeof(regionProperties);
        regionProperties.regionSequenceRef = regionSequenceRef_;
        regionProperties.startInModificationTime = 0.0;
        regionProperties.durationInModificationTime = 1.0;
        regionProperties.startInPlaybackTime = 0.0;
        regionProperties.durationInPlaybackTime = 1.0;
        playbackRegionRef_ = documentController_->createPlaybackRegion(
            audioModificationRef_, nullptr, &regionProperties);
        if (playbackRegionRef_ == nullptr)
            return false;

        processor_ = std::make_unique<OpenTune::OpenTuneAudioProcessor>();
        playbackRole_ = static_cast<ARA::ARAPlugInInstanceRoleFlags>(ARA::kARAPlaybackRendererRole);
        const auto* pluginInstance = processor_->bindToARA(
            documentControllerRef_, playbackRole_, playbackRole_);
        if (pluginInstance == nullptr || pluginInstance->playbackRendererInterface == nullptr)
            return false;
        pluginInstance->playbackRendererInterface->addPlaybackRegion(
            pluginInstance->playbackRendererRef, playbackRegionRef_);
        auto* rendererInterface = pluginInstance->playbackRendererInterface;
        const auto rendererRef = pluginInstance->playbackRendererRef;
        const auto playbackRegionRef = playbackRegionRef_;
        removePlaybackRegion_ = [rendererInterface, rendererRef, playbackRegionRef]
        {
            rendererInterface->removePlaybackRegion(rendererRef, playbackRegionRef);
        };

        processor_->setRateAndBufferSizeDetails(sampleRate, blockSize);
        processor_->prepareToPlay(sampleRate, blockSize);
        if (processor_->getSampleRate() != sampleRate)
            return false;

        documentController_->enableAudioSourceSamplesAccess(audioSourceRef_, true);
        auto* playbackRegion = ARA::PlugIn::fromRef<juce::ARAPlaybackRegion>(playbackRegionRef_);
        auto* controller = processor_->getDocumentController();
        if (controller == nullptr || controller->requestReadAudioForPlaybackRegion(playbackRegion) != 1)
            return false;
        processor_->collectPlaybackSnapshots();
        return audioAccess_.sampleReads() > 0;
    }

    OpenTune::OpenTuneAudioProcessor& processor() noexcept { return *processor_; }
    bool hasPlaybackRegion() const noexcept { return playbackRegionRef_ != nullptr; }

private:
    const ARA::ARAFactory* factory_;
    HostAudioAccess audioAccess_;
    HostArchiving archiving_;
    ARA::Host::DocumentControllerHostInstance host_;
    std::unique_ptr<ARA::Host::DocumentController> documentController_;
    ARA::ARADocumentControllerRef documentControllerRef_{};
    ARA::ARAMusicalContextRef musicalContextRef_{};
    ARA::ARARegionSequenceRef regionSequenceRef_{};
    ARA::ARAAudioSourceRef audioSourceRef_{};
    ARA::ARAAudioModificationRef audioModificationRef_{};
    ARA::ARAPlaybackRegionRef playbackRegionRef_{};
    ARA::ARAPlugInInstanceRoleFlags playbackRole_{};
    std::function<void()> removePlaybackRegion_;
    std::unique_ptr<OpenTune::OpenTuneAudioProcessor> processor_;
    bool initialized_ = false;
};
#endif

class FakeAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    explicit FakeAudioDeviceType(FakeAudioDeviceLifecycle& lifecycle)
        : AudioIODeviceType("RealtimeAudit"), lifecycle_(lifecycle) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return { "RealtimeAuditFake" }; }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool) const override
    {
        return device != nullptr && device->getName() == "RealtimeAuditFake" ? 0 : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& outputName,
                                      const juce::String& inputName) override
    {
        if ((outputName.isNotEmpty() && outputName != "RealtimeAuditFake")
            || (inputName.isNotEmpty() && inputName != "RealtimeAuditFake"))
            return nullptr;
        return new FakeAudioDevice(lifecycle_);
    }

private:
    FakeAudioDeviceLifecycle& lifecycle_;
};

void runManagerCallback(juce::AudioDeviceManager& manager,
                        OpenTune::OpenTuneAudioProcessor& processor,
                        const char* label)
{
    auto* device = dynamic_cast<FakeAudioDevice*>(manager.getCurrentAudioDevice());
    check(device != nullptr, "audio_manager_fake_device_missing");
    if (device != nullptr)
    {
        check(processor.getSampleRate() == device->getCurrentSampleRate(),
              "audio_manager_processor_callback_sample_rate_mismatch");
        check(processor.getBlockSize() == device->getCurrentBufferSizeSamples(),
              "audio_manager_processor_callback_block_size_mismatch");
#if !JucePlugin_Build_Standalone
        juce::AudioBuffer<float> expectedPcm(2, device->getCurrentBufferSizeSamples());
        bool sourceReady = false;
        {
            OpenTune::PlaybackReadSource source;
            auto* renderService = processor.getContentRenderService();
            sourceReady = renderService != nullptr
                && renderService->getPlaybackReadSource(
                    { OpenTune::DomainKind::RegularVST3Capture, captureSegmentId, 0 }, source)
                && source.preparedDry.buffer != nullptr
                && source.preparedDry.sampleRate == device->getCurrentSampleRate()
                && source.preparedDry.firstPreparedSample == 0
                && source.preparedDry.buffer->getNumChannels() == 2
                && source.preparedDry.buffer->getNumSamples() >= expectedPcm.getNumSamples();
            if (sourceReady)
                for (int channel = 0; channel < 2; ++channel)
                    expectedPcm.copyFrom(channel, 0, *source.preparedDry.buffer, channel,
                                         0, expectedPcm.getNumSamples());
        }
#else
        juce::ignoreUnused(processor);
#endif
        checkClean(audit(label, [&] { device->processOneBlock(); }));
        std::printf("{\"event\":\"device_callback_output\",\"scope\":\"%s\","
                    "\"firstSample\":%.9g,\"peak\":%.9g}\n",
                    label, device->outputFirstLeft(), device->outputPeak());
#if JucePlugin_Build_Standalone
        check(device->outputPeak() > 0.0001f, "audio_manager_player_output_silent");
#else
        check(sourceReady && device->matchesPreparedPcm(expectedPcm),
              "audio_manager_capture_replacement_pcm_mismatch");
        check(device->outputPeak() > 0.2f, "audio_manager_capture_replacement_output_missing");
#endif
    }
}

bool runAllocationAndWaitSelfTest()
{
    CRITICAL_SECTION criticalSection;
    InitializeCriticalSection(&criticalSection);
    SRWLOCK srwLock = SRWLOCK_INIT;
    HANDLE event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE file = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    juce::SpinLock spinLock;
    check(event != nullptr, "self_test_event_create_failed");
    check(file != INVALID_HANDLE_VALUE, "self_test_file_open_failed");

    const auto result = audit("intentional_violation_self_test", [&]
    {
        void* cppMemory = ::operator new(32);
        ::operator delete(cppMemory);

        void* alignedCppMemory = ::operator new(64, std::align_val_t{ 32 });
        ::operator delete(alignedCppMemory, std::align_val_t{ 32 });

        void* crtMemory = std::malloc(32);
        crtMemory = std::realloc(crtMemory, 64);
        std::free(crtMemory);

        void* zeroMemory = std::calloc(2, 16);
        std::free(zeroMemory);

        void* alignedMemory = _aligned_malloc(64, 32);
        _aligned_free(alignedMemory);

        void* heapMemory = HeapAlloc(GetProcessHeap(), 0, 48);
        HeapFree(GetProcessHeap(), 0, heapMemory);

        EnterCriticalSection(&criticalSection);
        LeaveCriticalSection(&criticalSection);
        if (TryEnterCriticalSection(&criticalSection))
            LeaveCriticalSection(&criticalSection);

        AcquireSRWLockExclusive(&srwLock);
        ReleaseSRWLockExclusive(&srwLock);
        if (TryAcquireSRWLockShared(&srwLock))
            ReleaseSRWLockShared(&srwLock);
        WaitForSingleObject(event, 0);

        if (spinLock.tryEnter())
            spinLock.exit();
        spinLock.enter();
        spinLock.exit();

        char byte = 0;
        DWORD transferred = 0;
        if (auto* crtFile = std::fopen("NUL", "w+b"))
        {
            std::fwrite(&byte, 1, 1, crtFile);
            std::fflush(crtFile);
            std::fseek(crtFile, 0, SEEK_SET);
            std::fread(&byte, 1, 1, crtFile);
            std::fclose(crtFile);
        }
        ReadFile(file, &byte, 1, &transferred, nullptr);
        WriteFile(file, &byte, 1, &transferred, nullptr);
    });

    OpenTune::RealtimeAuditTest::reportResult(result);
    const auto& c = result.counters;
    const bool detected = c.allocations > 0 && c.deallocations > 0
        && c.lockAttempts > 0 && c.lockWaits > 0 && c.fileIO > 0;
    check(detected, "intentional_violations_not_detected_in_all_categories");
    bool detectedJuceSpinLock = false;
    for (uint16_t i = 0; i < result.recordCount; ++i)
        detectedJuceSpinLock = detectedJuceSpinLock
            || result.records[i].api == OpenTune::RealtimeAudit::Api::JuceSpinLock;
    check(detectedJuceSpinLock, "juce_spinlock_hooks_not_detected");

    std::atomic<uint32_t> spinLockWaitObserved{ 0 };
    juce::SpinLock contendedSpinLock;
    contendedSpinLock.enter();
    Result contendedSpinLockResult;
    std::thread spinLockWaiter([&]
    {
        OpenTune::RealtimeAuditTest::setSpinLockWaitSignal(&spinLockWaitObserved);
        contendedSpinLockResult = audit("intentional_spinlock_wait_self_test", [&]
        {
            contendedSpinLock.enter();
        });
        OpenTune::RealtimeAuditTest::setSpinLockWaitSignal(nullptr);
        contendedSpinLock.exit();
    });
    while (spinLockWaitObserved.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    contendedSpinLock.exit();
    spinLockWaiter.join();
    OpenTune::RealtimeAuditTest::reportResult(contendedSpinLockResult);

    bool detectedJuceSpinLockAttempt = false;
    bool detectedJuceSpinLockWait = false;
    for (uint16_t i = 0; i < contendedSpinLockResult.recordCount; ++i)
    {
        const auto& record = contendedSpinLockResult.records[i];
        if (record.api == OpenTune::RealtimeAudit::Api::JuceSpinLock
            && record.event == OpenTune::RealtimeAudit::Event::LockAttempt)
            detectedJuceSpinLockAttempt = true;
        if (record.api == OpenTune::RealtimeAudit::Api::JuceSpinLock
            && record.event == OpenTune::RealtimeAudit::Event::LockWait)
            detectedJuceSpinLockWait = true;
    }
    check(contendedSpinLockResult.counters.lockAttempts > 0
              && detectedJuceSpinLockAttempt,
          "contended_juce_spinlock_attempt_not_detected");
    check(contendedSpinLockResult.counters.lockWaits > 0
              && detectedJuceSpinLockWait,
          "contended_juce_spinlock_wait_not_detected");

    CloseHandle(file);
    CloseHandle(event);
    DeleteCriticalSection(&criticalSection);
    return detected && detectedJuceSpinLock && detectedJuceSpinLockAttempt
        && detectedJuceSpinLockWait;
}

void runNestedScopeSelfTest()
{
    OpenTune::RealtimeAudit::Scope outer("nested_scope_outer");
    const auto inner = audit("nested_scope_inner", []
    {
        OpenTune::RealtimeAudit::recordAllocation(OpenTune::RealtimeAudit::Api::CppNew);
        OpenTune::RealtimeAudit::recordLockAttempt(OpenTune::RealtimeAudit::Api::JuceSpinLock);
    });
    const auto outerResult = outer.finish();
    check(inner.counters.allocations == 1 && inner.counters.lockAttempts == 1,
          "nested_scope_inner_counters_missing");
    check(outerResult.counters.allocations == 1 && outerResult.counters.lockAttempts == 1,
          "nested_scope_events_not_included_in_outer_scope");
}

void runProcessorCallbacks(OpenTune::OpenTuneAudioProcessor& processor)
{
    FixedPlayHead playHead;
#if !JucePlugin_Build_Standalone
    processor.setPlayHead(&playHead);
#endif
    installPlayableContent(processor, "processor_playable_content_setup_failed");

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    check(processor.getSampleRate() == sampleRate, "processor_prepare_sample_rate_mismatch");

    buffer.clear();
    const auto firstBlock = audit("processor_first_float_block", [&]
    {
        processor.processBlock(buffer, midi);
    });
    checkClean(firstBlock);
    check(hasAudibleOutput(buffer), "processor_first_float_block_silent");
#if JucePlugin_Build_Standalone
    check(processor.isPlaying(), "standalone_processor_not_playing_on_first_block");
#else
    check(std::abs(buffer.getSample(0, 0) - 0.25f) < 0.000001f,
          "processor_capture_replacement_pcm_mismatch");
#endif

    const bool playingBeforeZeroSampleBlock = processor.getPlayHeadState().isPlaying.load(
        std::memory_order_acquire);
    juce::AudioBuffer<float> zeroSamples;
    checkClean(audit("processor_zero_sample_block", [&]
    {
        processor.processBlock(zeroSamples, midi);
    }));
    check(processor.getPlayHeadState().isPlaying.load(std::memory_order_acquire)
              == playingBeforeZeroSampleBlock,
          "processor_zero_sample_block_changed_transport_state");

#if !JucePlugin_Build_Standalone
    const auto& transport = processor.getPlayHeadState();
    const auto initialEpoch = transport.presentationEpoch.load();
    auto stoppedPosition = makePlayingPosition();
    stoppedPosition.setTimeInSeconds(0.75);
    stoppedPosition.setTimeInSamples(36000);
    stoppedPosition.setIsPlaying(false);
    playHead.setPosition(stoppedPosition);
    checkClean(audit("processor_zero_sample_seek_and_stop", [&]
    {
        processor.processBlock(zeroSamples, midi);
    }));
    check(transport.timeInSeconds.load() == 0.75 && !transport.isPlaying.load()
              && transport.presentationEpoch.load() == initialEpoch + 1,
          "zero_sample_seek_and_stop_did_not_update_transport");

    checkClean(audit("processor_zero_sample_duplicate_observation", [&]
    {
        processor.processBlock(zeroSamples, midi);
    }));
    check(transport.presentationEpoch.load() == initialEpoch + 1,
          "zero_sample_duplicate_observation_advanced_epoch");

    juce::AudioPlayHead::PositionInfo stateOnlyPosition;
    stateOnlyPosition.setIsPlaying(true);
    playHead.setPosition(stateOnlyPosition);
    checkClean(audit("processor_zero_sample_state_without_time", [&]
    {
        processor.processBlock(zeroSamples, midi);
    }));
    check(transport.timeInSeconds.load() == 0.75 && transport.isPlaying.load()
              && transport.presentationEpoch.load() == initialEpoch + 2,
          "zero_sample_state_only_observation_lost_time");

    playHead.setPosition(juce::nullopt);
    checkClean(audit("processor_zero_sample_missing_position", [&]
    {
        processor.processBlock(zeroSamples, midi);
    }));
    check(transport.timeInSeconds.load() == 0.75 && transport.isPlaying.load()
              && transport.presentationEpoch.load() == initialEpoch + 2,
          "zero_sample_missing_position_changed_transport");
    playHead.setPosition(makePlayingPosition());
#endif

    for (int block = 0; block < 8; ++block)
    {
        buffer.clear();
        checkClean(audit("processor_normal_float_block", [&]
        {
            processor.processBlock(buffer, midi);
        }));
    }
#if !JucePlugin_Build_Standalone
    processor.setPlayHead(nullptr);
#endif
}

#if !JucePlugin_Build_Standalone
void runAraCallback(AraFixture& fixture)
{
    FixedPlayHead playHead;
    fixture.processor().setPlayHead(&playHead);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    buffer.clear();
    const auto firstBlock = audit("ara_processor_first_float_block", [&]
    {
        fixture.processor().processBlock(buffer, midi);
    });
    checkClean(firstBlock);
    check(fixture.hasPlaybackRegion(), "ara_playback_region_missing");
    check(hasAudibleOutput(buffer), "ara_processor_first_float_block_silent");
    check(std::abs(buffer.getSample(0, 0) - 0.25f) < 0.000001f,
          "ara_processor_first_float_block_pcm_mismatch");
    fixture.processor().setPlayHead(nullptr);
}
#endif
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    OpenTune::OpenTuneAudioProcessor processor;
    processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    FixedPlayHead playerPlayHead;
    OpenTune::OpenTuneAudioProcessor playerProcessor;
    juce::AudioProcessorPlayer player;
    player.setProcessor(&playerProcessor);
    FakeAudioDeviceLifecycle deviceLifecycle;
    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType(std::make_unique<FakeAudioDeviceType>(deviceLifecycle));
    manager.addAudioCallback(&player);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = "RealtimeAuditFake";
    setup.outputDeviceName = "RealtimeAuditFake";
    setup.sampleRate = sampleRate;
    setup.bufferSize = blockSize;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setRange(0, 2, true);
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setRange(0, 2, true);
    const auto openError = manager.initialise(2, 2, nullptr, false, {}, &setup);
    check(openError.isEmpty(), "audio_manager_fake_device_open_failed");
    check(playerProcessor.getSampleRate() == sampleRate,
          "player_processor_prepare_sample_rate_mismatch");
    installPlayableContent(playerProcessor, "player_processor_playable_content_setup_failed");
#if !JucePlugin_Build_Standalone
    playerProcessor.setPlayHead(&playerPlayHead);
#endif

#if !JucePlugin_Build_Standalone
    const auto* araFactory = createARAFactory();
    check(araFactory != nullptr, "ara_factory_unavailable");
    if (araFactory == nullptr)
        return 1;

    AraFixture araFixture(araFactory);
    const bool araReady = araFixture.initialize();
    check(araReady, "ara_fixture_initialization_failed");
#endif

    const WindowsHookReport hooks = OpenTune::RealtimeAuditTest::installWindowsHooks();
    OpenTune::RealtimeAuditTest::reportHookCoverage(hooks);
    check(hooks.supported && hooks.importsByApi[static_cast<size_t>(OpenTune::RealtimeAudit::Api::WinHeapAlloc)] > 0,
          "windows_heap_import_hook_missing");
    check(hooks.importsByApi[static_cast<size_t>(OpenTune::RealtimeAudit::Api::WinEnterCriticalSection)] > 0,
          "critical_section_import_hook_missing");
    check(hooks.importsByApi[static_cast<size_t>(OpenTune::RealtimeAudit::Api::WinWaitForSingleObject)] > 0,
          "wait_import_hook_missing");
    check(hooks.importsByApi[static_cast<size_t>(OpenTune::RealtimeAudit::Api::WinCreateFileW)] > 0,
          "file_import_hook_missing");

    runAllocationAndWaitSelfTest();
    runNestedScopeSelfTest();
    runProcessorCallbacks(processor);
    runManagerCallback(manager, playerProcessor, "audio_manager_player_first_callback");

    setup = manager.getAudioDeviceSetup();
    setup.sampleRate = 44100.0;
    setup.bufferSize = 128;
    const auto reconfigureError = manager.setAudioDeviceSetup(setup, false);
    check(reconfigureError.isEmpty(), "audio_manager_fake_device_reconfigure_failed");
    check(playerProcessor.getSampleRate() == setup.sampleRate,
          "player_processor_reconfigure_sample_rate_mismatch");
#if JucePlugin_Build_Standalone
    playerProcessor.play();
#endif
    runManagerCallback(manager, playerProcessor, "audio_manager_player_reconfigured_first_callback");

    const auto beforeCloseLifecycle = deviceLifecycle;
    manager.closeAudioDevice();
    check(deviceLifecycle.stops == beforeCloseLifecycle.stops + 1,
          "audio_manager_close_did_not_stop_fake_device");
    check(deviceLifecycle.closes == beforeCloseLifecycle.closes + 1,
          "audio_manager_close_did_not_close_fake_device");
    check(manager.getCurrentAudioDevice() == nullptr,
          "audio_manager_device_remained_open_after_close");

    manager.restartLastAudioDevice();
    check(deviceLifecycle.opens == beforeCloseLifecycle.opens + 1,
          "audio_manager_restart_did_not_reopen_fake_device");
    check(deviceLifecycle.starts == beforeCloseLifecycle.starts + 1,
          "audio_manager_restart_did_not_start_fake_device");
    check(deviceLifecycle.stops == beforeCloseLifecycle.stops + 1,
          "audio_manager_restart_changed_stop_count");
#if JucePlugin_Build_Standalone
    playerProcessor.play();
#endif
    runManagerCallback(manager, playerProcessor, "audio_manager_player_restarted_first_callback");

#if !JucePlugin_Build_Standalone
    if (araReady)
        runAraCallback(araFixture);
#endif

    manager.removeAudioCallback(&player);
    manager.closeAudioDevice();
    player.setProcessor(nullptr);
#if !JucePlugin_Build_Standalone
    playerProcessor.setPlayHead(nullptr);
#endif
    processor.releaseResources();

    if (failures == 0)
    {
        std::puts("Realtime callback audit passed.");
        return 0;
    }

    std::fprintf(stderr, "%d realtime audit failure(s).\n", failures);
    return 1;
}
