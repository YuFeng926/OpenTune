// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "StandaloneArrangement.h"

#include <memory>
#include <optional>
#include <vector>

namespace OpenTune::StandaloneMediaOperations {

enum class ImportLayout {
    Single,
    Sequential,
    SeparateTracks
};

enum class NotesGenerationSource {
    None,
    ExplicitRequest,
    EditingScheme
};

struct ImportPlacementRequest {
    ImportLayout layout = ImportLayout::Single;
    int firstTrackId = 0;
    std::size_t itemIndex = 0;
    std::optional<double> startTimelineSec;
    std::optional<double> sequentialCursorSec;
};

double appendStartSeconds(const OpenTuneAudioProcessor& processor, int trackId);
OpenTuneAudioProcessor::ImportPlacement planImportPlacement(
    const OpenTuneAudioProcessor& processor, const ImportPlacementRequest& request);

struct ImportCommitResult {
    OpenTuneAudioProcessor::CommittedPlacement committed;
    OpenTuneAudioProcessor::ImportPlacement placement;
    double durationSeconds = 0.0;
    bool analysisRequested = false;
    NotesGenerationSource notesGenerationSource = NotesGenerationSource::None;
    std::vector<int> revealedTracks;

    bool isValid() const noexcept { return committed.isValid(); }
};

enum class ExportKind {
    Clip,
    Track,
    Master
};

struct ExportTarget {
    ExportKind kind = ExportKind::Master;
    int trackId = 0;
    std::uint64_t placementId = 0;
};

struct ExportClip {
    int trackId = 0;
    std::uint64_t placementId = 0;
    float trackVolume = 1.0f;
    StandaloneArrangement::PlaybackPlacement playback;
};

struct ExportPlan {
    ExportTarget target;
    bool targetGone = false;
    int clipTrackId = 0;
    double clipStartSec = 0.0;
    double outputEndSec = 0.0;
    std::vector<ExportClip> clips;
    std::vector<ContentKey> contents;
};

ExportPlan buildExportPlan(OpenTuneAudioProcessor& processor, const ExportTarget& target);
bool writeExportWav(const ExportPlan& plan,
                    const juce::File& file,
                    Language language,
                    juce::String& error);

enum class ExportFailure {
    None,
    ContentUnavailable,
    AnalysisFailed,
    RenderChunkFailed,
    RenderUnavailable,
    StretchFailed,
    DryStretched
};

struct ExportReadiness {
    enum class State {
        Wait,
        Ready,
        Dry,
        Fail
    };
    State state = State::Wait;
    const char* dryReason = "";
    ExportFailure failure = ExportFailure::None;
};

class ExportReadinessTracker {
public:
    explicit ExportReadinessTracker(bool allowDry);
    ~ExportReadinessTracker();

    ExportReadinessTracker(const ExportReadinessTracker&) = delete;
    ExportReadinessTracker& operator=(const ExportReadinessTracker&) = delete;

    ExportReadiness classify(OpenTuneAudioProcessor& processor, ContentKey key);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace OpenTune::StandaloneMediaOperations
