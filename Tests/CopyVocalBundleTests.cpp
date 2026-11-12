#include "Utils/CopyVocalBundle.h"
#include "Utils/ContentAnalysisState.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <vector>

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

juce::String base64F32(const std::vector<float>& values)
{
    return juce::Base64::toBase64(values.data(), values.size() * sizeof(float));
}

juce::String base64U8(const std::vector<uint8_t>& values)
{
    return juce::Base64::toBase64(values.data(), values.size());
}

juce::String makeBundleJson(const juce::String& mediaPath, const juce::String& originalBase64,
                            const juce::String& originalMask, const juce::String& targetBase64,
                            const juce::String& targetMask)
{
    juce::String json = R"JSON({
  "format": "copyvocal.file",
  "formatVersion": 1,
  "producer": { "name": "CopyVocal", "version": "test", "createdAt": "2026-11-10T00:00:00Z", "jobStatus": "completed" },
  "project": { "name": "unit-test", "language": "zh", "sampleRate": 44100 },
  "clocks": {
    "studentSeconds": 0.05,
    "teacherSeconds": 0.03,
    "outputSeconds": 0.03,
    "projectSourceSeconds": 0.03,
    "outputClock": "teacher",
    "timingChanged": true,
    "pitchPolicy": "teacher_contour_student_register",
    "registerOffsetCents": 0.0
  },
  "media": {
    "student": {
      "role": "student",
      "path": "__MEDIA__",
      "sampleRate": 44100,
      "channels": 1,
      "sampleCount": 1323,
      "durationSeconds": 0.03
    }
  },
  "f0": {
    "hopSize": 160,
    "sampleRate": 16000,
    "frameStepSeconds": 0.01,
    "frameCount": 3,
    "original": { "encoding": "base64-f32le", "values": "__ORIG__", "voiced": "__ORIGMASK__" },
    "target": { "encoding": "base64-f32le", "values": "__TARGET__", "voiced": "__TARGETMASK__" }
  },
  "notes": [
    { "start": 0.0, "end": 0.02, "pitchHz": 120.0, "midi": 46.7, "lyric": "a", "source": "teacher" }
  ],
  "regions": [
    { "teacherStart": 0.0, "teacherEnd": 0.03, "studentStart": 0.0, "studentEnd": 0.03,
      "status": "matched", "mode": "synthesize", "quality": 1.0, "unsupported": false,
      "teacherAnchors": [0.0, 0.03], "studentAnchors": [0.0, 0.03] }
  ],
  "timeGrid": {
    "totalDurationSeconds": 0.03,
    "handles": [
      { "sourceSeconds": 0.0, "outputSeconds": 0.0, "kind": "clipStart", "confidence": "default" },
      { "sourceSeconds": 0.01, "outputSeconds": 0.005, "kind": "internalOnset", "confidence": "high" },
      { "sourceSeconds": 0.02, "outputSeconds": 0.025, "kind": "internalOnset", "confidence": "high" },
      { "sourceSeconds": 0.03, "outputSeconds": 0.03, "kind": "clipEnd", "confidence": "default" }
    ]
  },
  "diagnostics": { "warnings": ["unit"], "unresolvedRegionCount": 0, "unsupportedDurationSeconds": 0.0 }
})JSON";
    json = json.replace("__MEDIA__", mediaPath.replace("\\", "\\\\"));
    json = json.replace("__ORIG__", originalBase64);
    json = json.replace("__ORIGMASK__", originalMask);
    json = json.replace("__TARGET__", targetBase64);
    json = json.replace("__TARGETMASK__", targetMask);
    return json;
}

} // namespace

int main(int argc, char** argv)
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("copyvocal-cvf-test", ".wav");
    temp.replaceWithText("RIFF");
    const auto mediaPath = temp.getFullPathName();

    const std::vector<float> original{100.0f, 0.0f, 200.0f};
    const std::vector<uint8_t> originalMask{1, 0, 1};
    const std::vector<float> target{110.0f, 120.0f, 0.0f};
    const std::vector<uint8_t> targetMask{1, 1, 0};

    auto json = makeBundleJson(mediaPath, base64F32(original), base64U8(originalMask),
                               base64F32(target), base64U8(targetMask));
    const auto bundleFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getNonexistentChildFile("copyvocal-cvf-test", ".cvf");
    bundleFile.replaceWithText(json);

    const auto parsed = OpenTune::CopyVocalBundle::Bundle::loadFromFile(bundleFile);
    check(parsed.ok(), "valid bundle parses");
    if (parsed.ok())
    {
        const auto& bundle = parsed.value();
        check(bundle.formatVersion == 1, "format version");
        check(bundle.originalF0.f0Hz.size() == 3, "original F0 size");
        check(std::abs(bundle.originalF0.f0Hz[0] - 100.0f) < 1e-4f, "original F0 value");
        check(bundle.targetF0.f0Hz[2] == 0.0f, "target F0 zero");
        check(bundle.notes.size() == 1, "note parsed");
        check(bundle.timeGrid.size() == 4, "time grid parsed");

        const int expectedFrames = OpenTune::CopyVocalBundle::expectedF0FrameCount(
            static_cast<int64_t>(std::llround(bundle.projectSourceSeconds * 44100.0)));
        check(expectedFrames == 4 || expectedFrames == 3, "expected frame count is sane");

        const auto curveResult = OpenTune::CopyVocalBundle::buildPitchCurve(bundle, 3);
        check(curveResult.ok(), "pitch curve builds");
        if (curveResult.ok())
        {
            const auto curve = curveResult.value();
            const auto snapshot = curve->getSnapshot();
            check(snapshot->getOriginalF0().size() == 3, "curve original size");
            check(std::abs(snapshot->getOriginalF0()[2] - 200.0f) < 1e-4f, "curve original value");
            check(snapshot->getCorrectionSegments().size() == 1, "one voiced correction run");
            if (!snapshot->getCorrectionSegments().empty())
            {
                const auto& segment = snapshot->getCorrectionSegments().front();
                check(segment.startFrame == 0 && segment.endFrame == 1, "correction clips unvoiced frame");
                check(std::abs(segment.f0Data[0] - 110.0f) < 1e-4f, "correction target value");
            }
        }

        const auto notes = OpenTune::CopyVocalBundle::buildNotes(bundle);
        check(notes.size() == 1, "note builder keeps valid note");
        if (!notes.empty())
        {
            check(std::abs(notes.front().pitch - 120.0f) < 1e-4f, "note pitch");
            check(std::abs(notes.front().originalPitch - 100.0f) < 1e-4f, "note original pitch");
        }

        const auto grid = OpenTune::CopyVocalBundle::buildTimeGrid(bundle);
        check(grid != nullptr, "time grid builds");
        if (grid != nullptr)
        {
            check(std::abs(grid->totalDurationSeconds() - 0.03) < 1e-9, "time grid duration");
            check(grid->handles().size() == 4, "dense high-confidence handles accepted");
            check(std::abs(grid->tauForward(0.01) - 0.005) < 1e-9, "time grid mapping");
            check(!grid->isIdentity(), "time grid is non-identity");
        }

        const int64_t storedSamples = static_cast<int64_t>(std::llround(
            bundle.projectSourceSeconds * 44100.0));
        std::vector<float> pcm(static_cast<size_t>(storedSamples), 0.25f);
        const auto project = OpenTune::CopyVocalBundle::buildProject(
            bundle, storedSamples, 1, 44100.0, {}, 7, "test", pcm.data());
        check(project.ok(), "project snapshot builds");
        if (project.ok())
        {
            const auto& prepared = project.value();
            check(prepared.snapshot.sources.size() == 1, "one source");
            check(prepared.snapshot.contents.size() == 1, "one content");
            check(prepared.snapshot.tracks.size() == 1, "one track");
            check(prepared.snapshot.tracks.front().placements.size() == 1, "one placement");
            check(prepared.contentKey.isValid(), "content key valid");
            check(prepared.snapshot.tracks.front().placements.front().contentKey
                      == prepared.contentKey, "placement binds the content key");
            check(prepared.snapshot.contents.front().contentKey == prepared.contentKey,
                  "content entry binds the content key");
            check(prepared.snapshot.contents.front().sourceId == 1, "content uses the source");
            check(std::abs(prepared.snapshot.contents.front().sourceWindow.sourceEndSeconds - 0.03)
                      < 1e-6, "source window spans the project duration");
            check(prepared.snapshot.contents.front().originalF0State
                      == static_cast<uint8_t>(OpenTune::OriginalF0State::Ready),
                  "original F0 marked ready");
            check(prepared.snapshot.contents.front().noteTopologyInitialized,
                  "note topology initialized");
            check(prepared.snapshot.contents.front().timeGrid.handles.size() == 4,
                  "project persists the imported grid");
            check(prepared.pitchCurve != nullptr, "project carries the provided curve");
            if (prepared.pitchCurve != nullptr)
            {
                const auto curve = prepared.pitchCurve->getSnapshot();
                check(static_cast<int>(curve->getOriginalEnergy().size())
                          == prepared.expectedF0Frames,
                      "provided curve carries per-frame energy");
                check(!curve->getOriginalEnergy().empty()
                          && curve->getOriginalEnergy().front() > 0.0f,
                      "voiced frames get non-zero energy");
            }
        }

        const auto wrongLength = OpenTune::CopyVocalBundle::buildProject(
            bundle, storedSamples + 4410, 1, 44100.0, {}, 7, "test");
        check(!wrongLength.ok(), "duration mismatch is rejected");
    }

    auto badJson = json.replace("\"formatVersion\": 1", "\"formatVersion\": 99");
    const auto badFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("copyvocal-cvf-bad", ".cvf");
    badFile.replaceWithText(badJson);
    const auto bad = OpenTune::CopyVocalBundle::Bundle::loadFromFile(badFile);
    check(!bad.ok(), "future format version rejected");

    // Optional round-trip against a real CopyVocal export:
    //   OpenTuneCopyVocalBundleTests.exe path\to\opentune.cvf
    if (argc > 1)
    {
        const juce::File realFile(juce::String::fromUTF8(argv[1]));
        const auto real = OpenTune::CopyVocalBundle::Bundle::loadFromFile(realFile);
        check(real.ok(), "real CopyVocal bundle parses");
        if (real.ok())
        {
            const auto& bundle = real.value();
            std::printf("real bundle: notes=%d handles=%d frames=%d timingChanged=%d\n",
                        static_cast<int>(bundle.notes.size()),
                        static_cast<int>(bundle.timeGrid.size()),
                        bundle.originalF0.frameCount,
                        bundle.timingChanged ? 1 : 0);
            const auto expected = OpenTune::CopyVocalBundle::expectedF0FrameCount(
                static_cast<int64_t>(std::llround(bundle.projectSourceSeconds * 44100.0)));
            const auto curve = OpenTune::CopyVocalBundle::buildPitchCurve(bundle, expected);
            check(curve.ok(), "real bundle pitch curve builds");
            if (curve.ok())
            {
                const auto snapshot = curve.value()->getSnapshot();
                check(!snapshot->getOriginalF0().empty(), "real bundle has original F0");
                check(snapshot->getHopSize() == 160, "real bundle hop size");
                int correctionFrames = 0;
                for (const auto& segment : snapshot->getCorrectionSegments())
                    correctionFrames += segment.endFrame - segment.startFrame;
                std::printf("real bundle: correction frames=%d\n", correctionFrames);
                check(correctionFrames > 0, "real bundle target F0 becomes corrections");
            }
            const auto grid = OpenTune::CopyVocalBundle::buildTimeGrid(bundle);
            check(grid != nullptr, "real bundle time grid builds");
            if (grid != nullptr)
                std::printf("real bundle: grid handles=%d identity=%d\n",
                            static_cast<int>(grid->handles().size()),
                            grid->isIdentity() ? 1 : 0);
            const auto realNotes = OpenTune::CopyVocalBundle::buildNotes(bundle);
            check(realNotes.size() == bundle.notes.size(), "real bundle notes survive normalization");

            // The real file is the one users import: make sure materializing it
            // keeps the notes in the project snapshot (the piano roll reads
            // notes from there, not from the parsed bundle).
            const int64_t realSamples = static_cast<int64_t>(std::llround(
                bundle.projectSourceSeconds * 44100.0));
            std::vector<float> silence(static_cast<size_t>(realSamples), 0.0f);
            const auto realProject = OpenTune::CopyVocalBundle::buildProject(
                bundle, realSamples, 1, 44100.0, {}, 7, "test", silence.data());
            check(realProject.ok(), "real bundle project builds");
            if (realProject.ok())
            {
                const auto& content = realProject.value().snapshot.contents.front();
                check(content.notes.size() == realNotes.size(),
                      "real bundle notes reach the project snapshot");
                check(realProject.value().pitchCurve != nullptr
                          && !realProject.value().pitchCurve->getSnapshot()
                                  ->getOriginalEnergy().empty(),
                      "real bundle project carries energy");
            }
        }
    }

    badFile.deleteFile();
    bundleFile.deleteFile();
    temp.deleteFile();

    if (failures != 0)
    {
        std::printf("CopyVocalBundle tests failed: %d\n", failures);
        return 1;
    }
    std::puts("CopyVocalBundle tests passed.");
    return 0;
}
