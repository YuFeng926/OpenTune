#include "RealtimeAudit.h"

#if !defined(_WIN32)
    #error RealtimeVST3Tests requires the Windows IAT audit hooks.
#endif

#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../Source/PluginProcessor.h"
#include "../Source/Plugin/Capture/CaptureSession.h"
#include "../Source/DSP/ResamplingManager.h"
#include "../Source/Render/ContentRenderService.h"
#include "../Source/Utils/PlaybackSampleGeometry.h"
#include "../Source/Utils/TimeCoordinate.h"

extern "C" Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory();

namespace
{
using Steinberg::IPtr;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IAudioProcessor;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::IConnectionPoint;

constexpr double initialSampleRate = 48000.0;
constexpr double switchedSampleRate = 44100.0;
constexpr int initialBlockSize = 256;
constexpr int switchedBlockSize = 128;
constexpr int pcmSampleCount = 48000;

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
OpenTune::RealtimeAudit::Result audit(const char* label, Callback&& callback)
{
    OpenTune::RealtimeAudit::Scope scope(label);
    callback();
    return scope.finish();
}

void checkClean(const OpenTune::RealtimeAudit::Result& result)
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

private:
    const PositionInfo position_ = makePlayingPosition();
};

class FixedParamValueQueue final : public Steinberg::Vst::IParamValueQueue
{
public:
    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return parameterId_; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return hasPoint_ ? 1 : 0; }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index,
                                          Steinberg::int32& sampleOffset,
                                          Steinberg::Vst::ParamValue& value) override
    {
        if (index != 0 || ! hasPoint_)
            return Steinberg::kResultFalse;
        sampleOffset = sampleOffset_;
        value = value_;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override
    {
        ++addPointCalls_;
        sampleOffset_ = sampleOffset;
        value_ = value;
        hasPoint_ = true;
        index = 0;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** object) override
    {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, &Steinberg::FUnknown::iid)
            || Steinberg::FUnknownPrivate::iidEqual(iid, &Steinberg::Vst::IParamValueQueue::iid))
        {
            *object = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        *object = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++referenceCount_; }
    Steinberg::uint32 PLUGIN_API release() override { return --referenceCount_; }

    void set(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
    {
        parameterId_ = id;
        value_ = value;
        sampleOffset_ = 0;
        hasPoint_ = true;
    }

    Steinberg::uint32 addPointCalls() const noexcept { return addPointCalls_; }

private:
    Steinberg::Vst::ParamID parameterId_ = 0;
    Steinberg::Vst::ParamValue value_ = 0.0;
    Steinberg::int32 sampleOffset_ = 0;
    Steinberg::uint32 referenceCount_ = 1;
    Steinberg::uint32 addPointCalls_ = 0;
    bool hasPoint_ = false;
};

class FixedParameterChanges final : public Steinberg::Vst::IParameterChanges
{
public:
    Steinberg::int32 PLUGIN_API getParameterCount() override { return hasQueue_ ? 1 : 0; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override
    {
        return index == 0 && hasQueue_ ? &queue_ : nullptr;
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(
        const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override
    {
        ++addParameterDataCalls_;
        queue_.set(id, 0.0);
        hasQueue_ = true;
        index = 0;
        return &queue_;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** object) override
    {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, &Steinberg::FUnknown::iid)
            || Steinberg::FUnknownPrivate::iidEqual(iid, &Steinberg::Vst::IParameterChanges::iid))
        {
            *object = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        *object = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++referenceCount_; }
    Steinberg::uint32 PLUGIN_API release() override { return --referenceCount_; }

    void set(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
    {
        queue_.set(id, value);
        hasQueue_ = true;
    }

    Steinberg::uint32 addParameterDataCalls() const noexcept { return addParameterDataCalls_; }
    Steinberg::uint32 addPointCalls() const noexcept { return queue_.addPointCalls(); }

private:
    FixedParamValueQueue queue_;
    Steinberg::uint32 referenceCount_ = 1;
    Steinberg::uint32 addParameterDataCalls_ = 0;
    bool hasQueue_ = false;
};

template <typename String>
bool equalsAscii(const String& value, const char* expected)
{
    for (size_t index = 0; index < 128; ++index)
    {
        const auto expectedCharacter = static_cast<unsigned char>(expected[index]);
        if (static_cast<unsigned int>(value[index]) != expectedCharacter)
            return false;
        if (expectedCharacter == 0)
            return true;
    }
    return false;
}

class VST3Fixture final
{
public:
    ~VST3Fixture()
    {
        if (processing_)
            audioProcessor_->setProcessing(false);
        if (active_)
            component_->setActive(false);
        if (componentConnection_ && controllerConnection_)
        {
            controllerConnection_->disconnect(componentConnection_);
            componentConnection_->disconnect(controllerConnection_);
        }
        if (controllerInitialized_)
            controller_->terminate();
        if (componentInitialized_)
            component_->terminate();
    }

    bool initialize()
    {
        factory_ = IPtr<Steinberg::IPluginFactory>::adopt(GetPluginFactory());
        if (factory_ == nullptr)
            return false;

        Steinberg::PClassInfo componentInfo{};
        bool foundComponent = false;
        for (Steinberg::int32 i = 0; i < factory_->countClasses(); ++i)
        {
            if (factory_->getClassInfo(i, &componentInfo) == Steinberg::kResultOk
                && std::strcmp(componentInfo.category, kVstAudioEffectClass) == 0)
            {
                foundComponent = true;
                break;
            }
        }
        if (! foundComponent)
            return false;

        void* rawComponent = nullptr;
        if (factory_->createInstance(componentInfo.cid,
                                     Steinberg::getTUID<IComponent>(),
                                     &rawComponent) != Steinberg::kResultOk)
            return false;
        component_ = IPtr<IComponent>::adopt(static_cast<IComponent*>(rawComponent));

        Steinberg::TUID controllerClassId{};
        if (component_->getControllerClassId(controllerClassId) != Steinberg::kResultOk
            || component_->initialize(nullptr) != Steinberg::kResultOk)
            return false;
        componentInitialized_ = true;

        void* rawController = nullptr;
        if (factory_->createInstance(controllerClassId,
                                     Steinberg::getTUID<IEditController>(),
                                     &rawController) != Steinberg::kResultOk)
            return false;
        controller_ = IPtr<IEditController>::adopt(static_cast<IEditController*>(rawController));
        if (controller_->initialize(nullptr) != Steinberg::kResultOk)
            return false;
        controllerInitialized_ = true;

        componentConnection_ = Steinberg::FUnknownPtr<IConnectionPoint>(component_.get());
        controllerConnection_ = Steinberg::FUnknownPtr<IConnectionPoint>(controller_.get());
        if (! componentConnection_ || ! controllerConnection_
            || componentConnection_->connect(controllerConnection_) != Steinberg::kResultOk
            || controllerConnection_->connect(componentConnection_) != Steinberg::kResultOk)
            return false;

        audioProcessor_ = Steinberg::FUnknownPtr<IAudioProcessor>(component_.get());
        if (! audioProcessor_)
            return false;

        for (Steinberg::int32 i = 0; i < controller_->getParameterCount(); ++i)
        {
            Steinberg::Vst::ParameterInfo parameterInfo{};
            if (controller_->getParameterInfo(i, parameterInfo) != Steinberg::kResultOk)
                continue;
            if ((parameterInfo.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0)
            {
                bypassParameterId_ = parameterInfo.id;
                hasBypassParameter_ = true;
            }
            if (equalsAscii(parameterInfo.title, "EditVersion"))
            {
                editVersionParameterId_ = parameterInfo.id;
                hasEditVersionParameter_ = true;
            }
        }

        const auto inputBusCount = component_->getBusCount(Steinberg::Vst::kAudio,
                                                           Steinberg::Vst::kInput);
        const auto outputBusCount = component_->getBusCount(Steinberg::Vst::kAudio,
                                                            Steinberg::Vst::kOutput);
        if (inputBusCount != 1 || outputBusCount != 1)
            return false;

        Steinberg::Vst::SpeakerArrangement inputArrangement = Steinberg::Vst::SpeakerArr::kStereo;
        Steinberg::Vst::SpeakerArrangement outputArrangement = Steinberg::Vst::SpeakerArr::kStereo;
        if (audioProcessor_->setBusArrangements(&inputArrangement, 1,
                                                &outputArrangement, 1) != Steinberg::kResultOk)
            return false;

        if (component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput,
                                    0, true) != Steinberg::kResultOk
            || component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput,
                                      0, true) != Steinberg::kResultOk)
            return false;

        return configureProcessing(initialSampleRate, initialBlockSize)
            && setActive(true)
            && setProcessing(true);
    }

    bool configureProcessing(double sampleRate, int blockSize)
    {
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = blockSize;
        setup.sampleRate = sampleRate;
        return audioProcessor_->setupProcessing(setup) == Steinberg::kResultOk;
    }

    bool setActive(bool active)
    {
        if (component_->setActive(active) != Steinberg::kResultOk)
            return false;
        active_ = active;
        return true;
    }

    bool setProcessing(bool processing)
    {
        if (audioProcessor_->setProcessing(processing) != Steinberg::kResultOk)
            return false;
        processing_ = processing;
        return true;
    }

    IAudioProcessor* audioProcessor() noexcept { return audioProcessor_.getInterface(); }
    Steinberg::Vst::ParamID bypassParameterId() const noexcept { return bypassParameterId_; }
    bool hasBypassParameter() const noexcept { return hasBypassParameter_; }
    bool hasEditVersionParameter() const noexcept { return hasEditVersionParameter_; }
    bool setEditVersionNormalized(Steinberg::Vst::ParamValue value)
    {
        return controller_->setParamNormalized(editVersionParameterId_, value) == Steinberg::kResultOk;
    }

private:
    IPtr<Steinberg::IPluginFactory> factory_;
    IPtr<IComponent> component_;
    IPtr<IEditController> controller_;
    Steinberg::FUnknownPtr<IAudioProcessor> audioProcessor_;
    Steinberg::FUnknownPtr<IConnectionPoint> componentConnection_;
    Steinberg::FUnknownPtr<IConnectionPoint> controllerConnection_;
    Steinberg::Vst::ParamID bypassParameterId_ = 0;
    Steinberg::Vst::ParamID editVersionParameterId_ = 0;
    bool componentInitialized_ = false;
    bool controllerInitialized_ = false;
    bool hasBypassParameter_ = false;
    bool hasEditVersionParameter_ = false;
    bool active_ = false;
    bool processing_ = false;
};

bool processBlock(VST3Fixture& fixture, Steinberg::Vst::ProcessData& data,
                  const char* label)
{
    Steinberg::tresult processResult = Steinberg::kResultFalse;
    const auto result = audit(label, [&]
    {
        processResult = fixture.audioProcessor()->process(data);
    });
    checkClean(result);
    return processResult == Steinberg::kResultOk || processResult == Steinberg::kResultTrue;
}

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

void fillInput(std::array<float, initialBlockSize>& left,
               std::array<float, initialBlockSize>& right)
{
    for (int i = 0; i < initialBlockSize; ++i)
    {
        left[static_cast<size_t>(i)] = static_cast<float>(i) / initialBlockSize;
        right[static_cast<size_t>(i)] = -left[static_cast<size_t>(i)];
    }
}

void runVST3Processing(VST3Fixture& fixture)
{
    check(fixture.hasBypassParameter(), "vst3_bypass_parameter_missing");
    if (! fixture.hasBypassParameter())
        return;

    std::array<float, initialBlockSize> inputLeft{};
    std::array<float, initialBlockSize> inputRight{};
    std::array<float, initialBlockSize> outputLeft{};
    std::array<float, initialBlockSize> outputRight{};
    fillInput(inputLeft, inputRight);
    float* inputChannels[] = { inputLeft.data(), inputRight.data() };
    float* outputChannels[] = { outputLeft.data(), outputRight.data() };

    Steinberg::Vst::AudioBusBuffers inputBus{};
    inputBus.numChannels = 2;
    inputBus.channelBuffers32 = inputChannels;
    Steinberg::Vst::AudioBusBuffers outputBus{};
    outputBus.numChannels = 2;
    outputBus.channelBuffers32 = outputChannels;
    Steinberg::Vst::ProcessContext context{};
    context.state = Steinberg::Vst::ProcessContext::kPlaying
        | Steinberg::Vst::ProcessContext::kTempoValid
        | Steinberg::Vst::ProcessContext::kTimeSigValid;
    context.sampleRate = initialSampleRate;
    context.projectTimeSamples = 0;
    context.tempo = 120.0;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;

    check(fixture.hasEditVersionParameter(), "vst3_edit_version_parameter_missing");
    if (fixture.hasEditVersionParameter())
        check(fixture.setEditVersionNormalized(0.5), "vst3_edit_version_parameter_update_failed");

    FixedParameterChanges outputChanges;
    Steinberg::Vst::ProcessData data;
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = initialBlockSize;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputBus;
    data.outputs = &outputBus;
    data.processContext = &context;
    data.outputParameterChanges = &outputChanges;

    check(processBlock(fixture, data, "vst3_first_non_bypass_float_block"),
          "vst3_first_non_bypass_process_failed");
    check(std::equal(inputLeft.begin(), inputLeft.end(), outputLeft.begin())
              && std::equal(inputRight.begin(), inputRight.end(), outputRight.begin()),
          "vst3_first_non_bypass_block_did_not_pass_audio");

    Steinberg::Vst::ProcessData zeroSampleData;
    zeroSampleData.processMode = Steinberg::Vst::kRealtime;
    zeroSampleData.symbolicSampleSize = Steinberg::Vst::kSample32;
    zeroSampleData.processContext = &context;
    zeroSampleData.outputParameterChanges = &outputChanges;
    check(processBlock(fixture, zeroSampleData, "vst3_zero_sample_transport_context"),
          "vst3_zero_sample_transport_process_failed");

    check(fixture.setProcessing(false), "vst3_processing_stop_failed");
    check(fixture.setActive(false), "vst3_component_deactivation_failed");
    outputLeft.fill(0.75f);
    outputRight.fill(-0.75f);
    check(processBlock(fixture, data, "vst3_inactive_output_mute"),
          "vst3_inactive_process_failed");
    check(std::all_of(outputLeft.begin(), outputLeft.end(), [](float value) { return value == 0.0f; })
              && std::all_of(outputRight.begin(), outputRight.end(), [](float value) { return value == 0.0f; }),
          "vst3_inactive_outputs_not_silent");

    check(fixture.setActive(true) && fixture.setProcessing(true),
          "vst3_component_reactivation_failed");
    FixedParameterChanges bypassChanges;
    bypassChanges.set(fixture.bypassParameterId(), 1.0);
    data.inputParameterChanges = &bypassChanges;
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    check(processBlock(fixture, data, "vst3_bypass_parameter_process"),
          "vst3_bypass_parameter_process_failed");
    check(std::equal(inputLeft.begin(), inputLeft.end(), outputLeft.begin())
              && std::equal(inputRight.begin(), inputRight.end(), outputRight.begin()),
          "vst3_bypass_parameter_did_not_pass_audio");

    check(fixture.setProcessing(false), "vst3_processing_stop_before_rate_switch_failed");
    check(fixture.setActive(false), "vst3_deactivation_before_rate_switch_failed");
    check(fixture.configureProcessing(switchedSampleRate, switchedBlockSize)
              && fixture.setActive(true)
              && fixture.setProcessing(true),
          "vst3_rate_switch_setup_failed");
    context.sampleRate = switchedSampleRate;
    bypassChanges.set(fixture.bypassParameterId(), 0.0);
    data.inputParameterChanges = &bypassChanges;
    data.numSamples = switchedBlockSize;
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    check(processBlock(fixture, data, "vst3_rate_switch_first_block"),
          "vst3_rate_switch_first_block_failed");
    check(std::equal(inputLeft.begin(), inputLeft.begin() + switchedBlockSize,
                     outputLeft.begin())
              && std::equal(inputRight.begin(), inputRight.begin() + switchedBlockSize,
                            outputRight.begin()),
          "vst3_rate_switch_first_block_did_not_pass_audio");

    const auto outputDataCalls = outputChanges.addParameterDataCalls();
    const auto outputPointCalls = outputChanges.addPointCalls();
    if (outputDataCalls == 0 && outputPointCalls == 0)
        std::puts("VST3 outputParameterChanges: no processor-originated event is reachable; processBlock only reads EditVersion for diagnostics, and controller setParamNormalized does not change the component parameter.");
    else
    {
        check(outputDataCalls > 0 && outputPointCalls > 0,
              "vst3_output_parameter_queue_missing_point");
        std::puts("VST3 outputParameterChanges: processor called addParameterData and addPoint.");
    }
}

void runCaptureReplacement()
{
    FixedPlayHead playHead;
    OpenTune::OpenTuneAudioProcessor processor;
    processor.setPlayHead(&playHead);
    processor.setRateAndBufferSizeDetails(initialSampleRate, initialBlockSize);
    processor.prepareToPlay(initialSampleRate, initialBlockSize);

    auto* session = processor.getCaptureSession();
    check(session != nullptr, "vst3_capture_session_missing");
    if (session == nullptr)
        return;

    auto pcm = std::make_shared<juce::AudioBuffer<float>>(2, pcmSampleCount);
    for (int channel = 0; channel < pcm->getNumChannels(); ++channel)
        std::fill_n(pcm->getWritePointer(channel), static_cast<size_t>(pcmSampleCount), 0.25f);
    constexpr uint64_t segmentId = 9341;
    const auto injectedId = session->testInjectEditedSegment(
        0.0, 1.0, segmentId, pcm, initialSampleRate);
    check(publishCaptureTestPlaybackSource(
              processor, { OpenTune::DomainKind::RegularVST3Capture, injectedId, 0 }),
          "vst3_capture_playback_source_setup_failed");

    juce::AudioBuffer<float> buffer(2, initialBlockSize);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        std::fill_n(buffer.getWritePointer(channel), static_cast<size_t>(initialBlockSize), 0.125f);
    juce::MidiBuffer midi;
    const auto result = audit("vst3_capture_replacement_first_block", [&]
    {
        processor.processBlock(buffer, midi);
    });
    checkClean(result);
    check(std::abs(buffer.getSample(0, 0) - 0.25f) < 0.000001f
              && std::abs(buffer.getSample(1, 0) - 0.25f) < 0.000001f,
          "vst3_capture_replacement_pcm_mismatch");
    check(buffer.getMagnitude(0, 0, initialBlockSize) > 0.1f,
          "vst3_capture_replacement_output_silent");

    processor.setPlayHead(nullptr);
    processor.releaseResources();
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto hooks = OpenTune::RealtimeAuditTest::installWindowsHooks();
    OpenTune::RealtimeAuditTest::reportHookCoverage(hooks);
    check(hooks.supported, "windows_hooks_unavailable");

    VST3Fixture fixture;
    const bool fixtureReady = fixture.initialize();
    check(fixtureReady, "vst3_factory_fixture_initialization_failed");
    if (fixtureReady)
        runVST3Processing(fixture);
    runCaptureReplacement();

    if (failures == 0)
    {
        std::puts("VST3 realtime callback audit passed.");
        return 0;
    }

    std::fprintf(stderr, "%d VST3 realtime audit failure(s).\n", failures);
    return 1;
}
