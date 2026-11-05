// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Contents with a synthetic pitch analysis, shared by the scenarios of the reads (ControlDispatcherTests.cpp,
// which defines the functions) and of the edits (ControlEditTests.cpp).

#include "Content/ContentKey.h"
#include "Control/ControlJson.h"
#include "Utils/DetectedKey.h"
#include "Utils/PitchShiftSettings.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

class OpenTuneAudioProcessor;

namespace ControlTest {

struct NoteSpec
{
    double startSec = 0.0;
    double endSec = 0.0;
    double targetMidi = 0.0;
};

enum class Analysis {
    NotRequested,
    Running,
    Failed,
    Ready
};

struct ClipSpec
{
    std::string name = "clip";
    int trackId = 0;
    double startTimelineSec = 0.0;
    double durationSec = 6.0;
    Analysis analysis = Analysis::Ready;
    std::vector<NoteSpec> notes;
    std::optional<DetectedKey> key;
    std::optional<PitchShiftSettings> pitchShift;
    /** Content times of the handles between the end points of the time grid (output equals content). */
    std::vector<double> gridHandlesContentSec;
};

struct Clip
{
    ContentKey key;
    std::uint64_t placementId = 0;
    std::string sourcePath;
    /** The handleIds of the time grid's inner handles, in order. */
    std::vector<std::string> handleIds;
};

/** The handles and tokens of one clip as project.get reports them. */
struct ClipHandles
{
    std::string contentId;
    std::string placementId;
    std::string geometryHash;
    JsonValue placement;
    JsonValue content;
    JsonValue tokens;
};

/** Imports a clip of a voiced tone the way the editor imports, and gives its content the requested analysis
    state; a Ready analysis is synthetic F0 committed as OpenTune commits a finished one (the notes and the key
    of the spec, the pitch analysis state last). */
Clip fixtureAddClip(OpenTuneAudioProcessor& processor, const ClipSpec& spec);
/** The handles of the clip that project.get reports on the track at the timeline start. */
std::optional<ClipHandles> fixtureHandlesOf(const JsonValue& project, int trackId, double startTimelineSec);
/** A handle prefix of an instance that is not this one. */
std::string fixtureOtherInstanceShort(const std::string& instanceShort);
/** The scale of a wire name; nullopt for an unknown name. */
std::optional<Scale> fixtureScaleOf(const std::string& name);
/** Saves the project and opens it again with the editor's two-phase flows; opening clears the content
    repository (a new content epoch) and OpenTune's undo history. */
bool fixtureSaveAndReopen(OpenTuneAudioProcessor& processor);
/** fixtureSaveAndReopen(), then gives every clip of the opened project a synthetic pitch analysis again (opening
    restarts the analysis of every clip, which cannot succeed without the models). The clips keep the ids they were
    saved with, while the repository's counter for new ids is back at 1. With renumberFrom not 0, the clip saved with
    that id comes back with the id renumberTo (any 64-bit value, as a project file can hold). Message thread. */
bool fixtureReopenWithAnalysis(OpenTuneAudioProcessor& processor, std::uint64_t renumberFrom = 0,
                               std::uint64_t renumberTo = 0);

} // namespace ControlTest
} // namespace OpenTune
