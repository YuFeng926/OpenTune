// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Control/ControlJson.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/DetectedKey.h"
#include "Utils/LocalizationManager.h"
#include "Utils/PitchShiftSettings.h"

namespace OpenTune {

/**
 * Wire forms of small OpenTune values that more than one read method reports. They read no project
 * state (controlProcessingJson only the process-wide tuning atomic), so they may run on any thread.
 */

/** The wire name of a scale. */
const char* controlScaleName(Scale scale);

/** The key of a content; null until the analysis determined one. */
JsonValue controlKeyJson(const DetectedKey& key);

/** The pitch shift of a content. */
JsonValue controlPitchShiftJson(const PitchShiftSettings& settings);

/** The language tag session.info and prefs.get report (en, zh, ja, ru, es). */
const char* controlLanguageTag(Language language);

/** The wire name of an editing scheme: openDyne for the notes-primary scheme, openTune otherwise. */
const char* controlEditingSchemeName(AudioEditingScheme::Scheme scheme);

/** The members of the processing object that only session.info reports: the state of the inference
    runtime, which the caller reads. */
struct ControlProcessingRuntime
{
    /** Wire name of the inference backend in use (cpu, directml, coreml). */
    const char* backend = "cpu";
    bool vocoderReady = false;
    bool vocoderReconfiguring = false;
};

/**
 * The processing object of session.info (with runtime) and prefs.get (runtime null): the processing
 * preferences of a copy of the preferences' state, and the A4 frequency of
 * TuningConfig::currentTuningHz() (an atomic, any thread).
 */
JsonValue controlProcessingJson(const SharedPreferencesState& preferences, const ControlProcessingRuntime* runtime);

} // namespace OpenTune
