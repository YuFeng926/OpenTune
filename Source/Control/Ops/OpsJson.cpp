// SPDX-License-Identifier: AGPL-3.0-or-later
#include "OpsJson.h"

#include "OpsSupport.h"

#include "Utils/TuningConfig.h"

namespace OpenTune {

const char* controlScaleName(Scale scale)
{
    switch (scale) {
        case Scale::Major:
            return "major";
        case Scale::Minor:
            return "minor";
        case Scale::Chromatic:
            return "chromatic";
        case Scale::HarmonicMinor:
            return "harmonicMinor";
        case Scale::Dorian:
            return "dorian";
        case Scale::Mixolydian:
            return "mixolydian";
        case Scale::PentatonicMajor:
            return "pentatonicMajor";
        case Scale::PentatonicMinor:
            return "pentatonicMinor";
    }
    return "major";
}

JsonValue controlKeyJson(const DetectedKey& key)
{
    if (key.origin == Origin::Unset)
        return JsonValue();
    auto json = JsonValue::object();
    json.appendMember("root", JsonValue::integer(static_cast<int>(key.root)));
    json.appendMember("scale", JsonValue::string(controlScaleName(key.scale)));
    json.appendMember("origin", JsonValue::string(key.origin == Origin::Manual ? "manual" : "automatic"));
    json.appendMember("confidence", key.origin == Origin::Manual ? JsonValue() : controlJsonNumber(key.confidence, 4));
    return json;
}

JsonValue controlPitchShiftJson(const PitchShiftSettings& settings)
{
    auto json = JsonValue::object();
    json.appendMember("semitones", JsonValue::integer(settings.semitone));
    json.appendMember("cents", JsonValue::integer(settings.cents));
    return json;
}

const char* controlLanguageTag(Language language)
{
    switch (language) {
        case Language::English:
            return "en";
        case Language::Chinese:
            return "zh";
        case Language::Japanese:
            return "ja";
        case Language::Russian:
            return "ru";
        case Language::Spanish:
            return "es";
        case Language::Count:
            break;
    }
    return "en";
}

const char* controlEditingSchemeName(AudioEditingScheme::Scheme scheme)
{
    return scheme == AudioEditingScheme::Scheme::NotesPrimary ? "openDyne" : "openTune";
}

JsonValue controlProcessingJson(const SharedPreferencesState& preferences, const ControlProcessingRuntime* runtime)
{
    auto json = JsonValue::object();
    // TuningConfig::currentTuningHz(): the A4 frequency every *Midi value of this API refers to.
    json.appendMember("tuningHz", controlJsonNumber(TuningConfig::currentTuningHz(), 3));
    json.appendMember("editingScheme", JsonValue::string(controlEditingSchemeName(preferences.audioEditingScheme)));
    // The "hybrid" (light pitch correction) switch of the processing preferences.
    json.appendMember("hybridMode", JsonValue::boolean(preferences.lightPitchCorrectionEnabled));
    json.appendMember("f0Model", JsonValue::string("fcpe")); // F0ModelType has FCPE only
    json.appendMember(
        "renderingPriority",
        JsonValue::string(preferences.renderingPriority == RenderingPriority::CpuFirst ? "cpuFirst" : "gpuFirst"));
    if (runtime != nullptr) {
        json.appendMember("backend", JsonValue::string(runtime->backend));
        json.appendMember("vocoderReady", JsonValue::boolean(runtime->vocoderReady));
        json.appendMember("vocoderReconfiguring", JsonValue::boolean(runtime->vocoderReconfiguring));
    }
    json.appendMember("experimentalFeatures", JsonValue::boolean(preferences.experimentalFeaturesEnabled));
    return json;
}

} // namespace OpenTune
