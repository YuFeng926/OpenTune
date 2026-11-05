// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"

#include <cstdint>
#include <functional>

namespace OpenTune {

class ControlMethodTable;

/** Registers import.start (capability import.v1): imports audio files as new clips, as a job of kind
    import (PROTOCOL.md section 12, methods/import.start.schema.json). */
void registerImportOps(ControlMethodTable& table);

/** How the import job asks for the pitch analysis of a clip it committed: by default
    OpenTuneAudioProcessor::requestContentRefresh(), as the editor does after its own import. */
using ControlImportAnalysis = std::function<bool(OpenTuneAudioProcessor& processor,
                                                 const OpenTuneAudioProcessor::ContentRefreshRequest& request)>;

/** Test seam for the headless tests, which have no pitch model: replaces the analysis request of every
    later import job, or restores the default with an empty function. Never used by OpenTune itself.
    Message thread. */
void controlSetImportAnalysisForTesting(ControlImportAnalysis analysis);

/** Number of times an import job copied the audio of a prepared file so that it could repeat the commit,
    which it does only while the clip id that OpenTune hands out next may be in use (after a project was
    opened). For the tests; never used by OpenTune itself. Message thread. */
std::uint64_t controlImportCommitCopiesForTesting();

} // namespace OpenTune
