// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Control/ControlJson.h"
#include "Inference/IF0Extractor.h"

#include <cstddef>
#include <string>
#include <vector>

namespace OpenTune {

/**
 * Protocol options shared with prefs.get and prefs.set, plus the process-wide vocoder snapshot used by session.info.
 *
 * Threading: model options on the message thread; the vocoder state is safe from any thread.
 */

/** The longest text of a processing option, in code points. */
constexpr std::size_t kControlOptionTextCodePoints = 64;
/** The most members of one array of processing.options. */
constexpr std::size_t kControlOptionCount = 64;

/** The vocoder state reported by session.info. */
struct ControlVocoderState
{
    /** A vocoder is loaded (session.info vocoderReady). */
    bool ready = false;
    /** The runtime is replacing the vocoder (session.info vocoderReconfiguring). */
    bool reconfiguring = false;
    /** The runtime's control worker has no queued command and none in progress. */
    bool controlIdle = true;
};

/**
 * processing.options of prefs.get: {f0Model, renderingPriority}, each an array of strings.
 *
 * - f0Model: the names of the pitch models that are available (ModelFactory::getAvailableF0Models()); empty without
 *   a model.
 * - renderingPriority: every value.
 *
 * A name that is not valid UTF-8 or longer than kControlOptionTextCodePoints can never be sent back, so it is left
 * out; an array has at most kControlOptionCount members. Reads the disk; message thread.
 */
JsonValue controlPrefsOptionsJson();

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

} // namespace OpenTune
