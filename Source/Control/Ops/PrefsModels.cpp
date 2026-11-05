// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PrefsModels.h"

#include "PluginProcessor.h"
#include "Inference/ModelFactory.h"
#include "Runtime/ProcessRenderRuntime.h"
#include "Utils/AccelerationDetector.h"
#include "Utils/ModelPathResolver.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace OpenTune {

namespace {

std::mutex& backendMutex()
{
    static std::mutex mutex;
    return mutex;
}

ControlPrefsBackend& backendStorage()
{
    static ControlPrefsBackend backend;
    return backend;
}

/** A copy of the current backend: the functions run outside the lock. */
ControlPrefsBackend currentBackend()
{
    const std::lock_guard<std::mutex> lock(backendMutex());
    return backendStorage();
}

std::string modelsDirectory()
{
    const auto backend = currentBackend();
    // ModelPathResolver::getModelsDirectory(): the directory the preferences window scans for models.
    return backend.modelsDirectory ? backend.modelsDirectory() : ModelPathResolver::getModelsDirectory();
}

/** Keeps the names that the wire can carry and cuts the list at kControlOptionCount. */
std::vector<std::string> limitedNames(const std::vector<std::string>& names)
{
    std::vector<std::string> out;
    for (const auto& name : names) {
        if (out.size() >= kControlOptionCount)
            break;
        if (name.empty() || !isValidUtf8(name) || utf8CodePointCount(name) > kControlOptionTextCodePoints)
            continue;
        out.push_back(name);
    }
    return out;
}

JsonValue textArray(const std::vector<std::string>& names)
{
    auto array = JsonValue::array();
    for (const auto& name : names)
        array.append(JsonValue::string(name));
    return array;
}

} // namespace

std::vector<std::string> controlVocoderWeightOptions()
{
    // ModelFactory::getAvailableVocoderWeights(): the built-in weights of the models directory and the files of its
    // vocoder_weights folder, which is what the preferences window lists (SharedAudioPage).
    std::vector<std::string> names;
    for (const auto& weight : ModelFactory::getAvailableVocoderWeights(modelsDirectory()))
        names.push_back(weight.fileName);
    return limitedNames(names);
}

std::vector<F0ModelInfo> controlF0Models()
{
    const auto backend = currentBackend();
    if (backend.f0Models)
        return backend.f0Models();
    // ModelFactory::getAvailableF0Models(): the pitch models, each with whether its file exists.
    return ModelFactory::getAvailableF0Models(modelsDirectory());
}

std::vector<std::string> controlF0ModelOptions()
{
    std::vector<std::string> names;
    for (const auto& model : controlF0Models())
        if (model.isAvailable)
            names.push_back(model.name);
    return limitedNames(names);
}

std::string controlF0ModelName(F0ModelType type)
{
    for (const auto& model : controlF0Models())
        if (model.type == type)
            return model.name;
    return "fcpe"; // F0ModelType has FCPE only
}

JsonValue controlPrefsOptionsJson()
{
    auto options = JsonValue::object();
    options.appendMember("vocoderWeight", textArray(controlVocoderWeightOptions()));
    options.appendMember("f0Model", textArray(controlF0ModelOptions()));
    options.appendMember("renderingPriority", textArray({"gpuFirst", "cpuFirst"}));
    return options;
}

ControlVocoderState controlVocoderState()
{
    const auto backend = currentBackend();
    if (backend.vocoderState)
        return backend.vocoderState();
    // ProcessRenderRuntime: the process-wide vocoder and its control worker, polled without a hook. The worker's
    // queue and active transaction are read first: a command is queued before the vocoder is flagged as
    // reconfiguring, and the active transaction ends only after the completion ran on the message thread, so a worker
    // that is idle has finished every command queued before this call, and the two flags read after it are the state
    // that the commands left (read the other way round, a reconfiguration that ends between the reads could pair a
    // flag from before it with an idle worker after it).
    auto& render = ProcessRenderRuntime::getInstance();
    ControlVocoderState state;
    state.controlIdle = render.controlQueueDepth() == 0 && !render.hasActiveTransaction();
    state.reconfiguring = render.isVocoderReconfiguring();
    state.ready = render.isVocoderReady();
    return state;
}

const char* controlInferenceBackendName()
{
    const auto backend = currentBackend();
    if (backend.inferenceBackend)
        return backend.inferenceBackend();
    // AccelerationDetector::getSelection(): the inference backend in use (CPU until one was detected).
    switch (AccelerationDetector::getInstance().getSelection().backend) {
        case AccelerationDetector::AccelBackend::DirectML:
            return "directml";
        case AccelerationDetector::AccelBackend::CoreML:
            return "coreml";
        case AccelerationDetector::AccelBackend::CPU:
            break;
    }
    return "cpu";
}

void controlSetVocoderWeight(OpenTuneAudioProcessor& processor, const std::string& weight)
{
    const auto backend = currentBackend();
    if (backend.setVocoderWeight) {
        backend.setVocoderWeight(processor, weight);
        return;
    }
    // OpenTuneAudioProcessor::setVocoderModelWeight(): posts the switch to the runtime's control worker; its
    // completion clears the render caches and queues a render of every clip.
    processor.setVocoderModelWeight(weight);
}

void controlResetInferenceBackend(OpenTuneAudioProcessor& processor, bool forceCpu)
{
    const auto backend = currentBackend();
    if (backend.resetInferenceBackend) {
        backend.resetInferenceBackend(processor, forceCpu);
        return;
    }
    // OpenTuneAudioProcessor::resetInferenceBackend(): posts the reset to the runtime's control worker; nothing
    // clears a render cache.
    processor.resetInferenceBackend(forceCpu);
}

bool controlSetF0Model(OpenTuneAudioProcessor& processor, F0ModelType type)
{
    const auto backend = currentBackend();
    if (backend.setF0Model)
        return backend.setF0Model(processor, type);
    // OpenTuneAudioProcessor::setF0ModelType(): switches the process-wide pitch model; false when it cannot.
    return processor.setF0ModelType(type);
}

void controlSetPrefsBackendForTesting(ControlPrefsBackend backend)
{
    const std::lock_guard<std::mutex> lock(backendMutex());
    backendStorage() = std::move(backend);
}

} // namespace OpenTune
