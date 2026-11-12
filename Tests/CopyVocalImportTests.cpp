#include "../Source/Utils/ProjectSession.h"
#include "../Source/Utils/CopyVocalBundle.h"
#include "../Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace OpenTune {
// The test links the Standalone shared code but not the UI. createEditor() is
// never exercised here, so a null factory satisfies the linker without pulling
// the whole editor into the test binary.
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    return nullptr;
}
} // namespace OpenTune

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

juce::File writeWav(const juce::File& file, double seconds, double sampleRate, int channels)
{
    file.deleteFile();
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (stream == nullptr)
        return {};
    juce::WavAudioFormat format;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        format.createWriterFor(stream.get(), sampleRate,
                               static_cast<unsigned int>(channels), 24, {}, 0));
    if (writer == nullptr)
        return {};
    stream.release();
    const int samples = static_cast<int>(std::llround(seconds * sampleRate));
    juce::AudioBuffer<float> buffer(channels, samples);
    buffer.clear();
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* data = buffer.getWritePointer(channel);
        for (int index = 0; index < samples; ++index)
            data[index] = 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi
                                           * 220.0f * static_cast<float>(index) / static_cast<float>(sampleRate));
    }
    writer->writeFromAudioSampleBuffer(buffer, 0, samples);
    writer.reset();
    return file;
}

juce::String base64F32(const std::vector<float>& values)
{
    return juce::Base64::toBase64(values.data(), values.size() * sizeof(float));
}

juce::String base64U8(const std::vector<uint8_t>& values)
{
    return juce::Base64::toBase64(values.data(), values.size());
}

juce::String makeBundle(const juce::File& media, double outputSeconds)
{
    const int frames = static_cast<int>(std::llround(outputSeconds / 0.01)) + 1;
    std::vector<float> original(static_cast<size_t>(frames), 0.0f);
    std::vector<float> target(static_cast<size_t>(frames), 0.0f);
    std::vector<uint8_t> originalMask(static_cast<size_t>(frames), 0);
    std::vector<uint8_t> targetMask(static_cast<size_t>(frames), 0);
    for (int frame = 5; frame < frames; ++frame)
    {
        original[static_cast<size_t>(frame)] = 200.0f;
        target[static_cast<size_t>(frame)] = 220.0f;
        originalMask[static_cast<size_t>(frame)] = 1;
        targetMask[static_cast<size_t>(frame)] = 1;
    }

    const double middle = outputSeconds * 0.5;
    const auto escapedPath = media.getFullPathName().replace("\\", "\\\\");
    juce::String json = R"JSON({
  "format": "copyvocal.file",
  "formatVersion": 1,
  "producer": { "name": "CopyVocal", "version": "test", "createdAt": "2026-11-10T00:00:00Z", "jobStatus": "completed" },
  "project": { "name": "integration", "language": "en", "sampleRate": 44100 },
  "clocks": {
    "studentSeconds": 1.0, "teacherSeconds": __OUT__,
    "outputSeconds": __OUT__, "projectSourceSeconds": __OUT__,
    "outputClock": "teacher", "timingChanged": true,
    "pitchPolicy": "teacher_contour_student_register", "registerOffsetCents": 0.0
  },
  "media": { "student": { "role": "student", "path": "__MEDIA__",
      "sampleRate": 44100, "channels": 1, "sampleCount": 44100, "durationSeconds": 1.0 } },
  "f0": {
    "hopSize": 160, "sampleRate": 16000, "frameStepSeconds": 0.01, "frameCount": __FRAMES__,
    "original": { "encoding": "base64-f32le", "values": "__ORIG__", "voiced": "__ORIGMASK__" },
    "target": { "encoding": "base64-f32le", "values": "__TARGET__", "voiced": "__TARGETMASK__" }
  },
  "notes": [ { "start": 0.05, "end": 0.2, "pitchHz": 220.0, "midi": 57.0, "source": "teacher" } ],
  "regions": [ { "teacherStart": 0.05, "teacherEnd": 0.2, "studentStart": 0.05, "studentEnd": 0.2,
      "status": "matched", "mode": "synthesize", "quality": 1.0, "unsupported": false,
      "teacherAnchors": [0.05, 0.2], "studentAnchors": [0.05, 0.2] } ],
  "timeGrid": { "totalDurationSeconds": __OUT__, "handles": [
      { "sourceSeconds": 0.0, "outputSeconds": 0.0, "kind": "clipStart", "confidence": "default" },
      { "sourceSeconds": 0.05, "outputSeconds": 0.05, "kind": "onsetVoiced", "confidence": "high" },
      { "sourceSeconds": 0.2, "outputSeconds": 0.2, "kind": "internalOnset", "confidence": "high" },
      { "sourceSeconds": __OUT__, "outputSeconds": __OUT__, "kind": "clipEnd", "confidence": "default" } ] },
  "diagnostics": { "warnings": [], "unresolvedRegionCount": 0, "unsupportedDurationSeconds": 0.0 }
})JSON";
    json = json.replace("__MEDIA__", escapedPath);
    json = json.replace("__OUT__", juce::String(outputSeconds, 9));
    json = json.replace("__FRAMES__", juce::String(frames));
    json = json.replace("__ORIG__", base64F32(original));
    json = json.replace("__ORIGMASK__", base64U8(originalMask));
    json = json.replace("__TARGET__", base64F32(target));
    json = json.replace("__TARGETMASK__", base64U8(targetMask));
    return json;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("copyvocal-cvf-import", {});
    root.createDirectory();

    // Student media is 1.0 s but the corrected output is 0.6 s: the importer
    // must trim, and the F0 grid must follow the trimmed duration.
    auto audio = writeWav(root.getChildFile("student.wav"), 1.0, 44100.0, 1);
    check(audio.existsAsFile(), "test wav written");

    const auto bundleFile = root.getChildFile("bundle.cvf");
    bundleFile.replaceWithText(makeBundle(audio, 0.6));

    OpenTune::AppPreferences::StorageOptions options;
    options.applicationName = "OpenTuneCopyVocalImportTest";
    options.settingsDirectory = root.getChildFile("settings");
    OpenTune::AppPreferences preferences(options);
    preferences.load();

    OpenTune::OpenTuneAudioProcessor processor;
    OpenTune::ProjectSession session(processor, preferences);

    const auto prepared = session.prepareCopyVocalBundle(bundleFile, OpenTune::Language::English);
    check(prepared.ok(), "bundle prepares an OpenTune project");
    if (prepared.ok())
    {
        const auto& value = prepared.value();
        check(value.sources.size() == 1, "one prepared source");
        check(value.providedF0.size() == 1, "one provided F0 curve");
        check(value.snapshot.sources.size() == 1, "snapshot has one source");
        check(value.snapshot.contents.size() == 1, "snapshot has one content");
        check(value.snapshot.tracks.size() == 1, "snapshot has one track");
        check(std::abs(value.snapshot.sources.front().lengthSeconds - 0.6) < 0.001,
              "source trimmed to the bundle duration");
        check(std::abs(value.snapshot.contents.front().sourceWindow.sourceEndSeconds - 0.6) < 0.001,
              "content window matches the bundle duration");
        if (!value.sources.empty())
        {
            const int stored = value.sources.front().preparedImport.storedAudioBuffer.getNumSamples();
            check(std::abs(static_cast<double>(stored) / 44100.0 - 0.6) < 0.001,
                  "stored audio has the bundle duration");
        }
        if (!value.providedF0.empty() && value.providedF0.front().pitchCurve != nullptr)
        {
            const auto curve = value.providedF0.front().pitchCurve;
            const auto snapshot = curve->getSnapshot();
            const int expected = OpenTune::CopyVocalBundle::expectedF0FrameCount(
                static_cast<int64_t>(std::llround(0.6 * 44100.0)));
            check(static_cast<int>(snapshot->getOriginalF0().size()) == expected,
                  "provided original F0 matches the clip frame count");
            check(static_cast<int>(snapshot->getOriginalEnergy().size()) == expected,
                  "provided curve carries OpenDyne energy");
            bool hasEnergy = false;
            for (const float value : snapshot->getOriginalEnergy())
                hasEnergy = hasEnergy || value > 0.0f;
            check(hasEnergy, "imported audio produces non-zero note energy");
            check(!snapshot->getCorrectionSegments().empty(),
                  "provided target F0 becomes a correction segment");
            if (!snapshot->getCorrectionSegments().empty())
            {
                const auto& segment = snapshot->getCorrectionSegments().front();
                check(std::abs(segment.f0Data.front() - 220.0f) < 1e-3f,
                      "correction carries the bundle target pitch");
            }
        }
        check(!value.snapshot.contents.front().notes.empty(), "bundle notes survive import");
        if (!value.snapshot.contents.front().notes.empty())
        {
            const auto& note = value.snapshot.contents.front().notes.front();
            check(std::abs(note.pitch - 220.0f) < 1e-3f, "note target pitch");
        }
        check(std::abs(value.snapshot.tracks.front().placements.front().timelineDurationSeconds - 0.6)
                  < 0.001, "placement duration");
    }

    root.deleteRecursively();

    if (failures != 0)
    {
        std::printf("CopyVocal import tests failed: %d\n", failures);
        return 1;
    }
    std::puts("CopyVocal import tests passed.");
    return 0;
}
