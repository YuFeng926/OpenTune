// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Control/ControlJson.h"
#include "Inference/IF0Extractor.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * What prefs.get and prefs.set need from the models on disk and from the inference runtime: the lists that the
 * preferences window offers (processing.options of prefs.get), the state of the vocoder that a prefsApply job waits
 * for, and the processor functions the window calls when the vocoder weight, the rendering priority or the pitch
 * model changes.
 *
 * Every function below uses the real OpenTune function unless a test replaced the part with
 * controlSetPrefsBackendForTesting(). Threading: message thread (prefs.get, prefs.set and its job).
 */

/** The longest text of processing.options (prefs.get.schema.json), in code points. */
constexpr std::size_t kControlOptionTextCodePoints = 64;
/** The most members of one array of processing.options. */
constexpr std::size_t kControlOptionCount = 64;

/** The vocoder as the prefsApply job sees it. */
struct ControlVocoderState
{
    /** A vocoder is loaded (session.info vocoderReady). */
    bool ready = false;
    /** The runtime is replacing the vocoder (session.info vocoderReconfiguring). */
    bool reconfiguring = false;
    /** The runtime's control worker has no queued command and none in progress. */
    bool controlIdle = true;
};

/** The parts of the models and the inference runtime that prefs.get and prefs.set reach. An empty member means the
    real OpenTune function. Used only by tests. */
struct ControlPrefsBackend
{
    /** The models directory: the real one is ModelPathResolver::getModelsDirectory(). */
    std::function<std::string()> modelsDirectory;
    /** All pitch models, available or not: the real list is ModelFactory::getAvailableF0Models(). */
    std::function<std::vector<F0ModelInfo>()> f0Models;
    /** The vocoder of the process-wide render runtime. */
    std::function<ControlVocoderState()> vocoderState;
    /** OpenTuneAudioProcessor::setVocoderModelWeight(): the asynchronous switch of the vocoder weight. */
    std::function<void(OpenTuneAudioProcessor&, const std::string&)> setVocoderWeight;
    /** OpenTuneAudioProcessor::resetInferenceBackend(): the asynchronous reset of the inference backend. */
    std::function<void(OpenTuneAudioProcessor&, bool)> resetInferenceBackend;
    /** OpenTuneAudioProcessor::setF0ModelType(): true when the processor switched the pitch model. */
    std::function<bool(OpenTuneAudioProcessor&, F0ModelType)> setF0Model;
    /** The inference backend in use by its wire name (cpu, directml, coreml); a pointer to a string literal. The real
        one is the selection of AccelerationDetector. */
    std::function<const char*()> inferenceBackend;
};

/**
 * processing.options of prefs.get (methods/prefs.get.schema.json): {vocoderWeight, f0Model, renderingPriority}, each an
 * array of strings.
 *
 * - vocoderWeight: the file names that the preferences window lists, in its order (ModelFactory::
 *   getAvailableVocoderWeights() of the models directory); empty without a model.
 * - f0Model: the names of the pitch models that are available (ModelFactory::getAvailableF0Models()); empty without
 *   a model.
 * - renderingPriority: every value.
 *
 * A name that is not valid UTF-8 or longer than kControlOptionTextCodePoints can never be sent back, so it is left
 * out; an array has at most kControlOptionCount members. Reads the disk; message thread.
 */
JsonValue controlPrefsOptionsJson();

/** The file names of processing.options.vocoderWeight (see controlPrefsOptionsJson()). */
std::vector<std::string> controlVocoderWeightOptions();

/** Every pitch model OpenTune knows, available or not, with the name that the wire uses. */
std::vector<F0ModelInfo> controlF0Models();

/** The names of processing.options.f0Model: the pitch models that are available. */
std::vector<std::string> controlF0ModelOptions();

/** The wire name of a pitch model type: its name in controlF0Models(), "fcpe" for a type the list lacks. */
std::string controlF0ModelName(F0ModelType type);

/** The state of the process-wide vocoder. Any thread. */
ControlVocoderState controlVocoderState();

/** The inference backend that OpenTune's acceleration detector has chosen at this moment, by its wire name: cpu,
    directml or coreml. cpu also while nothing has been detected yet. It is what session.info reports as
    processing.backend and what a prefsApply job reads once, when it ends, to tell whether a request for gpuFirst fell
    back to the CPU (warning BACKEND_FALLBACK). Any thread. */
const char* controlInferenceBackendName();

/** The call the preferences window makes when the vocoder weight changes: processor.setVocoderModelWeight(). The
    switch runs on the runtime's control worker; the call returns at once. */
void controlSetVocoderWeight(OpenTuneAudioProcessor& processor, const std::string& weight);

/** The call the preferences window makes when the rendering priority changes: processor.resetInferenceBackend(). The
    reset runs on the runtime's control worker; the call returns at once. */
void controlResetInferenceBackend(OpenTuneAudioProcessor& processor, bool forceCpu);

/** The call the preferences window makes when the pitch model changes: processor.setF0ModelType(); false when the
    processor could not switch. */
bool controlSetF0Model(OpenTuneAudioProcessor& processor, F0ModelType type);

/** Test seam: replaces parts of the backend; a default-constructed backend restores the real functions. Any thread. */
void controlSetPrefsBackendForTesting(ControlPrefsBackend backend);

} // namespace OpenTune
