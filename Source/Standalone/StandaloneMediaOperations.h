// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "StandaloneArrangement.h"
#include "Content/EditableContentSnapshot.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
    struct ContentInput {
        ContentKey key;
        std::uint64_t contentRevision = 0;
        std::uint64_t notesRevision = 0;
        std::uint64_t pitchRevision = 0;
        std::uint64_t timeGridRevision = 0;
        OriginalF0State originalF0State = OriginalF0State::NotRequested;
        bool hasUsableOriginalF0 = false;
        bool available = false;
    };
    std::vector<ContentInput> contentInputs;
};

enum class ExportPlanStatus {
    Current,
    Changed,
    TargetGone,
    ContentUnavailable
};

/** Build and validate plans on the message thread. Current means the plan still matches
    the live target, mix, contents and the exact source/content inputs captured for playback. */
ExportPlan buildExportPlan(OpenTuneAudioProcessor& processor, const ExportTarget& target);
ExportPlanStatus validateExportPlan(OpenTuneAudioProcessor& processor, const ExportPlan& plan);
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
    DryStretched,
    Busy,
    TargetGone,
    NothingToExport,
    Changed,
    LoadedMedia,
    FileIdentity,
    DestinationUnavailable,
    TargetExists,
    WriteFailed
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

enum class ExportDestinationPolicy {
    FailIfExists,
    Rename,
    Overwrite
};

struct ExportDryContent {
    ContentKey key;
    const char* reason = "";
};

enum class ExportOperationState {
    NotStarted,
    Waiting,
    Writing,
    Succeeded,
    Failed,
    Cancelled
};

struct ExportOperationProgress {
    ExportOperationState state = ExportOperationState::NotStarted;
    ExportFailure failure = ExportFailure::None;
    int readyContents = 0;
    int totalContents = 0;
    std::vector<ExportDryContent> dryContents;
};

struct ExportWavInfo {
    int channelCount = 0;
    double sampleRateHz = 0.0;
    std::int64_t frameCount = 0;
    std::string sampleFormat;
};

/** Owns the message-thread export lifecycle and the worker that writes and commits its WAV. */
class ExportOperation {
public:
    ExportOperation(OpenTuneAudioProcessor& processor,
                    ExportTarget target,
                    juce::File destination,
                    ExportDestinationPolicy destinationPolicy,
                    bool allowDry,
                    Language language,
                    std::function<void()> busyChanged = {});
    ~ExportOperation();

    ExportOperation(const ExportOperation&) = delete;
    ExportOperation& operator=(const ExportOperation&) = delete;

    ExportFailure start();
    ExportOperationProgress advance(bool cancelRequested,
                                    std::function<bool()> beforeCommit = {});
    void setWakeCallback(std::function<void()> wake);

    const ExportPlan& plan() const noexcept;
    const ExportWavInfo& wavInfo() const noexcept;
    const juce::File& destination() const noexcept;
    const juce::String& error() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace OpenTune::StandaloneMediaOperations
