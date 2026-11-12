#pragma once

#include <juce_core/juce_core.h>

#include "Error.h"
#include "Note.h"
#include "PitchCurve.h"
#include "ProjectModel.h"
#include "SilentGapDetector.h"
#include "TimeGrid.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune::CopyVocalBundle {

inline constexpr const char* kFormatName = "copyvocal.file";
inline constexpr int kFormatVersion = 1;

/** Canonical file extension, without the dot. */
inline constexpr const char* kFileExtension = "cvf";

inline constexpr int kF0HopSize = 160;
inline constexpr double kF0SampleRate = 16000.0;
inline constexpr double kF0FrameStepSeconds = 0.01;

struct MediaRef
{
    juce::File file;
    int sampleRate{0};
    int channels{0};
    int64_t sampleCount{0};
    double durationSeconds{0.0};
    juce::String sha256;

    bool isValid() const noexcept
    {
        return file != juce::File() && sampleRate > 0 && channels > 0 && sampleCount > 0;
    }
};

struct PitchTrack
{
    int hopSize{kF0HopSize};
    double sampleRate{kF0SampleRate};
    int frameCount{0};
    std::vector<float> f0Hz;
    std::vector<uint8_t> voiced;

    double timeAtFrame(int frame) const noexcept
    {
        return sampleRate > 0.0
            ? static_cast<double>(frame) * static_cast<double>(hopSize) / sampleRate
            : 0.0;
    }
};

struct NoteEvent
{
    double startSeconds{0.0};
    double endSeconds{0.0};
    float pitchHz{0.0f};
    float midi{0.0f};
    float confidence{0.0f};
    juce::String lyric;
    juce::String source;
};

struct Region
{
    double teacherStart{0.0};
    double teacherEnd{0.0};
    double studentStart{0.0};
    double studentEnd{0.0};
    double quality{0.0};
    bool unsupported{false};
    juce::String status;
    juce::String mode;
    juce::String reason;
    std::vector<double> teacherAnchors;
    std::vector<double> studentAnchors;
    bool hasStretchRatio{false};
    double stretchRatio{0.0};
};

struct TimeHandleSpec
{
    double sourceSeconds{0.0};
    double outputSeconds{0.0};
    juce::String kind;
    juce::String confidence;
};

struct Bundle
{
    int formatVersion{0};

    juce::String projectName;
    juce::String language;

    double studentSeconds{0.0};
    double teacherSeconds{0.0};
    double outputSeconds{0.0};
    double projectSourceSeconds{0.0};
    bool timingChanged{false};
    juce::String outputClock;
    juce::String pitchPolicy;
    double registerOffsetCents{0.0};

    MediaRef studentMedia;
    MediaRef teacherMedia;
    MediaRef correctedMedia;
    bool hasTeacherMedia{false};
    bool hasCorrectedMedia{false};

    PitchTrack originalF0;
    PitchTrack targetF0;

    std::vector<NoteEvent> notes;
    std::vector<Region> regions;
    std::vector<TimeHandleSpec> timeGrid;

    juce::StringArray warnings;
    int unresolvedRegionCount{0};
    double unsupportedDurationSeconds{0.0};

    /** Reads, validates and decodes a .cvf file. */
    static Result<Bundle> loadFromFile(const juce::File& file);
};

/** Number of 10 ms F0 frames OpenTune expects for a canonical clip length. */
int expectedF0FrameCount(int64_t sampleCount, int hopSize = kF0HopSize,
                         double f0SampleRate = kF0SampleRate) noexcept;

/// Maps original F0 to the original layer and voiced-original target F0 to HandDraw segments;
/// clips or zero-pads minor (<=2) frame-count differences to match `expectedFrames`.
Result<std::shared_ptr<PitchCurve>> buildPitchCurve(const Bundle& bundle, int expectedFrames);

/** Builds normalized, non-overlapping OpenTune notes in project source seconds. */
std::vector<Note> buildNotes(const Bundle& bundle);

/// Builds the source->output time grid, falling back to an identity grid when
/// outputClock is "student", no internal anchors exist, or validation fails.
std::shared_ptr<const TimeGridSnapshot> buildTimeGrid(const Bundle& bundle);

bool hasEditablePitch(const Bundle& bundle) noexcept;

/// Materializes a bundle into a ProjectSnapshot without a processor (`storedSamples` at 44.1 kHz
/// post-fit). Optional mono `sourcePCM` computes per-frame energy required to display note blobs.
struct PreparedProject
{
    ProjectSnapshot snapshot;
    ContentKey contentKey;
    std::shared_ptr<PitchCurve> pitchCurve;
    int expectedF0Frames{0};
    double projectDurationSeconds{0.0};
};

Result<PreparedProject> buildProject(const Bundle& bundle,
                                     int64_t storedSamples,
                                     int numChannels,
                                     double sampleRate,
                                     const std::vector<SilentGap>& silentGaps,
                                     int projectFormatVersion,
                                     const juce::String& appVersion,
                                     const float* sourcePCM = nullptr);

} // namespace OpenTune::CopyVocalBundle
