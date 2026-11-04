// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ImportOps.h"

#include "ClipIds.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Audio/AudioFormatRegistry.h"
#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "Control/ControlUiBridge.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"
#include "Utils/NoteGeneratorTypes.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace OpenTune {

namespace {

constexpr std::size_t kMaxItems = 64;
constexpr std::size_t kMaxPathCodePoints = 4096;
constexpr std::size_t kMaxKeyLength = 128;
/** The limits session.info reports for imports (SessionOps.cpp): prepareImport rejects more than 2
    channels, and the pitch analysis accepts at most 10 minutes. */
constexpr int kMaxChannels = 2;
constexpr double kMaxAnalysisSec = 600.0;
/** Upper bound of startTimelineSec that this build accepts (about 115 days of timeline). */
constexpr double kMaxTimelineSec = 1.0e7;
/** Samples read per block while decoding; the worker checks between blocks whether the job is gone. */
constexpr int kDecodeBlockSamples = 1 << 18;
/** Sample rate of OpenTune's stored audio; prepareImport resamples when the file differs by more than
    kResampleToleranceHz. */
constexpr double kStoredSampleRateHz = 44100.0;
constexpr double kResampleToleranceHz = 1.0;

ControlImportAnalysis& analysisOverride()
{
    static ControlImportAnalysis analysis;
    return analysis;
}

/** Message thread only. */
std::uint64_t& commitCopies()
{
    static std::uint64_t copies = 0;
    return copies;
}

/**
 * The arrangement's selection before a commit: the active track and the selected clip of the track that the commit
 * inserts into. OpenTuneAudioProcessor::commitPreparedImportAsPlacement() selects every clip it commits and makes its
 * track active (in PluginProcessor.cpp, and insertPlacement() selects the first clip of an empty track); that is
 * view state the request did not ask for unless it focuses the clip. The editor's heartbeat would then load the
 * selected clip into the piano roll, which in the openDyne scheme generates its notes
 * (PianoRollComponent::ensureOpenDyneNotesIfNeeded()), so an import with focus false would generate notes it does
 * not report. The import job restores the selection the commit found (see ImportJob::commit()). Message thread.
 */
struct CommitSelection
{
    int activeTrack = 0;
    std::uint64_t selectedPlacementId = 0;
    bool valid = false;
};

CommitSelection captureCommitSelection(const StandaloneArrangement* arrangement, int trackId)
{
    if (arrangement == nullptr)
        return {};
    return {arrangement->getActiveTrackId(), arrangement->getSelectedPlacementId(trackId), true};
}

void restoreCommitSelection(StandaloneArrangement* arrangement, int trackId, const CommitSelection& selection)
{
    if (arrangement == nullptr || !selection.valid)
        return;
    // selectPlacement() makes the track active as well, so the active track is set afterwards. The selection of
    // another track was not touched by the commit and is left as it is. A saved clip that is gone (it should not) is
    // cleared rather than left at the clip the commit selected.
    if (!arrangement->selectPlacement(trackId, selection.selectedPlacementId))
        arrangement->setSelectedPlacementIndex(trackId, -1);
    arrangement->setActiveTrack(selection.activeTrack);
}

enum class Layout {
    Single,
    Sequential,
    SeparateTracks
};

const char* layoutName(Layout layout)
{
    switch (layout) {
        case Layout::Single:
            return "single";
        case Layout::Sequential:
            return "sequential";
        case Layout::SeparateTracks:
            return "separateTracks";
    }
    return "single";
}

// ---- Params ---------------------------------------------------------------------------------------

struct ImportParams
{
    std::vector<std::string> paths;
    std::string idempotencyKey;
    Layout layout = Layout::Single;
    std::optional<int> trackId;
    bool newTrack = false;
    std::optional<double> startTimelineSec;
    bool generateNotes = false;
    bool focus = true;

    /** The params other than idempotencyKey after defaults, as one string: equal strings mean equal
        params (section 12.5). Numbers compare by value. */
    std::string digest() const
    {
        auto json = JsonValue::object();
        auto items = JsonValue::array();
        for (const auto& path : paths)
            items.append(JsonValue::string(path));
        json.appendMember("items", std::move(items));
        json.appendMember("layout", JsonValue::string(layoutName(layout)));
        json.appendMember("trackId", trackId ? JsonValue::integer(*trackId) : JsonValue());
        json.appendMember("newTrack", JsonValue::boolean(newTrack));
        json.appendMember("startTimelineSec", startTimelineSec ? controlJsonNumber(*startTimelineSec, 9) : JsonValue());
        json.appendMember("generateNotes", JsonValue::boolean(generateNotes));
        json.appendMember("focus", JsonValue::boolean(focus));
        return toJsonString(json);
    }
};

bool optionalBoolean(ControlParamReader& reader, std::string_view name, bool& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isBoolean())
        return reader.fail(std::string(name) + " must be a boolean");
    out = value->asBoolean();
    return true;
}

bool readItems(ControlParamReader& reader, std::vector<std::string>& out)
{
    const JsonValue* items = reader.member("items");
    if (items == nullptr)
        return reader.fail("items is required");
    if (!items->isArray() || items->size() < 1 || items->size() > kMaxItems)
        return reader.fail("items must be an array of 1 to 64 items");
    for (std::size_t i = 0; i < items->size(); ++i) {
        const auto& item = items->at(i);
        const JsonValue* path = item.isObject() ? item.find("path") : nullptr;
        if (path == nullptr || item.size() != 1)
            return reader.fail("every item must be an object with exactly the member path");
        if (!path->isString() || path->text().empty() || utf8CodePointCount(path->text()) > kMaxPathCodePoints)
            return reader.fail("path must be a string of 1 to 4096 characters");
        out.push_back(path->text());
    }
    return true;
}

bool readLayout(ControlParamReader& reader, Layout& out)
{
    const JsonValue* layout = reader.member("layout");
    if (layout == nullptr)
        return reader.fail("layout is required");
    for (const auto candidate : {Layout::Single, Layout::Sequential, Layout::SeparateTracks})
        if (layout->isString() && layout->text() == layoutName(candidate)) {
            out = candidate;
            return true;
        }
    return reader.fail("layout must be single, sequential or separateTracks");
}

bool readKey(ControlParamReader& reader, std::string& out)
{
    const JsonValue* key = reader.member("idempotencyKey");
    if (key == nullptr)
        return true;
    if (!key->isString() || key->text().empty() || key->text().size() > kMaxKeyLength)
        return reader.fail("idempotencyKey must be a string of 1 to 128 characters");
    for (const char c : key->text())
        if (c < 0x21 || c > 0x7e)
            return reader.fail("idempotencyKey must be printable ASCII without spaces");
    out = key->text();
    return true;
}

/** methods/import.start.schema.json $defs.params, on the connection thread. Paths get their checks of
    PROTOCOL.md section 20.1 on the message thread, as method-specific validation with their own reasons. */
bool parseImport(const JsonValue& params, ImportParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> trackId;
    const bool ok =
        reader.onlyMembers(
            {"items", "idempotencyKey", "layout", "trackId", "newTrack", "startTimelineSec", "generateNotes", "focus"})
        && readItems(reader, out.paths) && readLayout(reader, out.layout) && readKey(reader, out.idempotencyKey)
        && reader.optionalInteger("trackId", 0, std::numeric_limits<int>::max(), trackId)
        && optionalBoolean(reader, "newTrack", out.newTrack)
        && reader.optionalNumber("startTimelineSec", 0.0, kMaxTimelineSec, out.startTimelineSec)
        && optionalBoolean(reader, "generateNotes", out.generateNotes) && optionalBoolean(reader, "focus", out.focus);
    if (ok && out.layout == Layout::Single && out.paths.size() > 1)
        reader.fail("layout single imports exactly one item");
    if (ok && reader.member("trackId") != nullptr && reader.member("newTrack") != nullptr)
        reader.fail("trackId and newTrack cannot be given together");
    if (!reader.why().empty()) {
        why = reader.why();
        return false;
    }
    if (trackId)
        out.trackId = static_cast<int>(*trackId);
    return true;
}

/** Whether the extension of the path is one of the formats the import registers (AudioFormatRegistry,
    as the UI's file chooser and session.info import.extensions list them), compared without case. */
bool hasImportExtension(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    const auto separator = path.find_last_of("\\/");
    if (dot == std::string::npos || (separator != std::string::npos && dot < separator))
        return false;
    std::string extension = path.substr(dot);
    for (char& c : extension)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    if (extension == ".wav")
        return true; // the WAV reader is always registered
    for (const auto& pattern : juce::StringArray::fromTokens(AudioFormatRegistry::getImportWildcardFilter(), ";,", ""))
        if (pattern.trim().fromLastOccurrenceOf(".", true, false).toLowerCase().toStdString() == extension)
            return true;
    return false;
}

std::string labelSeconds(double seconds)
{
    return controlJsonNumber(seconds, 3).text() + "s";
}

// ---- One file, decoded and prepared on a thread of its own ------------------------------------------

/**
 * Decodes one file and prepares it for the commit on a thread of its own, as the editor does with
 * AsyncAudioLoader::run() and its background task: AudioFormatRegistry::registerImportFormats(), the
 * format's reader, then OpenTuneAudioProcessor::prepareImport(), which the editor also calls off the
 * message thread. The errors are classified for the item (methods/import.start.schema.json).
 *
 * The destructor marks the work abandoned and joins the thread; the thread checks the mark between blocks
 * of the decode and before prepareImport(), so destroying an unfinished file waits for at most one block or
 * one prepareImport().
 */
class PreparedFile
{
public:
    PreparedFile(OpenTuneAudioProcessor& processor, std::string path) : path_(std::move(path))
    {
        thread_ = std::thread([this, &processor] {
            run(processor);
            done_ = true;
        });
    }

    ~PreparedFile()
    {
        abandoned_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    PreparedFile(const PreparedFile&) = delete;
    PreparedFile& operator=(const PreparedFile&) = delete;

    bool done() const noexcept { return done_.load(); }

    // Valid once done() is true.
    std::optional<ControlJobError> error;
    OpenTuneAudioProcessor::PreparedImport prepared;
    int channelCount = 0;
    double sampleRateHz = 0.0;

private:
    static ControlJobError itemError(const char* kind, const char* reason, const char* detail)
    {
        return ControlJobError{kind, reason, detail, false};
    }

    void run(OpenTuneAudioProcessor& processor)
    {
        const juce::File file(juce::String::fromUTF8(path_.c_str()));
        if (!file.existsAsFile()) {
            error = itemError("NOT_FOUND", "file_not_found", "the file does not exist");
            return;
        }
        juce::AudioFormatManager formats;
        AudioFormatRegistry::registerImportFormats(formats);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr) {
            const std::unique_ptr<juce::FileInputStream> stream(file.createInputStream());
            error =
                stream != nullptr && stream->openedOk()
                    ? itemError("UNSUPPORTED", "unsupported_format", "OpenTune cannot decode the file's audio format")
                    : itemError("IO_ERROR", "read_failed", "the file could not be read");
            return;
        }
        const auto channels = static_cast<int>(reader->numChannels);
        const double rate = reader->sampleRate;
        const auto length = reader->lengthInSamples;
        if (channels > kMaxChannels) {
            error = itemError("UNSUPPORTED", "too_many_channels", "the file has more than 2 channels");
            return;
        }
        if (channels < 1 || !(rate > 0.0) || length <= 0) {
            error = itemError("UNSUPPORTED", "unsupported_format", "the file holds no audio OpenTune can use");
            return;
        }
        if (static_cast<double>(length) / rate > kMaxAnalysisSec
            || length > static_cast<juce::int64>(std::numeric_limits<int>::max())) {
            error = itemError("INVALID_ARGUMENT", "audio_too_long", "the audio is longer than import.maxAnalysisSec");
            return;
        }
        juce::AudioBuffer<float> audio(channels, static_cast<int>(length));
        for (juce::int64 start = 0; start < length; start += kDecodeBlockSamples) {
            if (abandoned_.load())
                return;
            const auto count = static_cast<int>(std::min<juce::int64>(kDecodeBlockSamples, length - start));
            if (!reader->read(&audio, static_cast<int>(start), count, start, true, true)) {
                error = itemError("IO_ERROR", "read_failed", "reading the audio data failed");
                return;
            }
        }
        if (abandoned_.load())
            return;
        channelCount = channels;
        sampleRateHz = rate;
        if (!processor.prepareImport(std::move(audio), rate, file.getFileName(), juce::String::fromUTF8(path_.c_str()),
                                     prepared))
            error = itemError("UNSUPPORTED", "unsupported_format", "OpenTune did not accept the audio");
    }

    std::string path_;
    std::atomic<bool> done_{false};
    std::atomic<bool> abandoned_{false};
    std::thread thread_;
};

// ---- The job --------------------------------------------------------------------------------------

struct ImportItem
{
    std::string path;
    bool imported = false;
    std::optional<ControlJobError> error;
    int trackId = -1;
    std::uint64_t placementId = 0;
    ContentKey contentKey;
    std::string placementHandle;
    std::string contentHandle;
    double startSec = 0.0;
    double durationSec = 0.0;
    int channelCount = 0;
    double sampleRateHz = 0.0;
    bool analysisRequested = false;
    /** "request" (generateNotes) or "focus" (openDyne scheme), empty when no generation was requested. */
    std::string notesBy;
    /** Set on the message thread by the processor's note generation callback. */
    std::shared_ptr<std::atomic<bool>> notesGenerated = std::make_shared<std::atomic<bool>>(false);
    bool notesMarkedDirty = false;
};

/**
 * The work of one import job, on the message thread. It decodes and commits the files one after another,
 * as the editor's import queue does (startPendingImport, processNextImportInQueue in
 * Source/Standalone/PluginEditor.cpp), then focuses the last clip and requests the pitch analysis of every
 * committed clip, and ends when every analysis has finished.
 *
 * - The commit waits while the editor is busy (UI priority). Cancellation stops before the next commit
 *   without waiting for the UI; clips committed before stay, and the result reports them.
 * - OpenTuneAudioProcessor::commitPreparedImportAsPlacement() is the editor's commit. After a project was
 *   opened, OpenTune gives a new clip an id that a clip of the project may already have, and the commit
 *   fails; each failed attempt uses up that id. The job therefore repeats the commit, at most once per
 *   clip whose id can still be handed out, so the import succeeds where the UI would fail. Only then does
 *   it commit a copy of the prepared audio, which a further attempt needs; otherwise (no project opened, or
 *   every clip id behind OpenTune's counter) it commits once, as the editor does, without a copy.
 * - OpenTune's import creates no undo entry and the editor does not mark the project as changed after it;
 *   the job marks it (PROTOCOL.md section 15.5).
 * - The result has no undo entry (ui_parity). While the instance reports arrangement.v1, its revert recipe deletes the
 *   committed clips with one arrangement.edit op delete, and carries requiresConsent true because deleting clips needs
 *   the end user's consent (PROTOCOL.md section 15.2 rule 4); the recipe lists only the clips that were committed, so
 *   a cancelled job names the clips it got through. Without arrangement.v1 no request can remove them: revert is null
 *   and the envelope adds IRREVERSIBLE. The recipe carries the arrangement revision right after the last commit, so
 *   that it fails with REVISION_CONFLICT where the clips were changed since (rule 3). sideEffects.notesGenerated
 *   has the same recipe for the focused clips whose notes were generated (rule 5).
 * - commitPreparedImportAsPlacement() selects every clip it commits and makes its track the active track, also with
 *   focus false. A selected clip is what the editor's heartbeat loads into the piano roll, and the openDyne scheme
 *   generates the notes of a content it shows, so the clip would get notes the request did not ask for. The job
 *   therefore restores the selection that the commit found (captureCommitSelection()), and focus alone decides the
 *   selection: with focus false it does not change, and with focus true the job selects the last clip after the
 *   commits (PROTOCOL.md section 21 rule 2). With focus true and several target tracks, only the track of the last
 *   clip ends up with its new clip selected; the other tracks keep the clip that was selected before, where the
 *   editor's import leaves each of them on its new clip. This is deliberate: the selection is decided by focus alone.
 * - The clip id collision after a project was opened is only logged: the protocol has no warning code
 *   for it, so the result carries none.
 *
 * Where it differs from the editor's import, it follows methods/import.start.schema.json:
 * - In the openDyne scheme the editor generates notes for every clip it commits; the job generates them
 *   only for the focused clip (without generateNotes).
 * - The editor's import into separate tracks places every file at 0 s on its track; the job appends
 *   each file after the last clip on its track (or places it at startTimelineSec).
 */
class ImportJob
{
public:
    ImportJob(ControlOpEnvironment environment, const ImportParams& params, int targetTrack,
              std::unique_ptr<ControlWriteEnvelope> envelope)
        : environment_(std::move(environment))
        , params_(params)
        , targetTrack_(targetTrack)
        , envelope_(std::move(envelope))
        , analysis_(analysisOverride())
    {
        for (const auto& path : params_.paths) {
            ImportItem item;
            item.path = path;
            items_.push_back(std::move(item));
        }
    }

    void step(ControlJobStep& step)
    {
        ControlOpContext context(environment_);
        if (phase_ == Phase::Import)
            stepImport(context, step);
        if (phase_ == Phase::Analysis && !step.finished())
            stepAnalysis(context, step);
    }

private:
    enum class Phase {
        Import,
        Analysis
    };

    int trackOf(std::size_t index) const
    {
        return params_.layout == Layout::SeparateTracks ? targetTrack_ + static_cast<int>(index) : targetTrack_;
    }

    /** Mirrors OpenTuneAudioProcessorEditor::computeTrackAppendStartSeconds (Source/Standalone/
        PluginEditor.cpp): the end of the last clip with audio on the track, or 0. */
    static double appendStart(ControlOpContext& context, int trackId)
    {
        auto& processor = context.processor();
        const auto* arrangement = processor.getStandaloneArrangement();
        const auto* repository = processor.getStandaloneContentRepository();
        double end = 0.0;
        const int count = arrangement != nullptr ? arrangement->getNumPlacements(trackId) : 0;
        for (int index = 0; index < count; ++index) {
            StandaloneArrangement::Placement placement;
            if (!arrangement->getPlacementByIndex(trackId, index, placement) || !placement.contentKey.isValid())
                continue;
            const auto* clip = repository != nullptr ? repository->findClip(placement.contentKey) : nullptr;
            if (clip == nullptr || clip->content().audioBuffer == nullptr)
                continue;
            end = std::max(end, placement.timelineEndSeconds());
        }
        return end;
    }

    void stepImport(ControlOpContext& context, ControlJobStep& step)
    {
        if (file_ != nullptr) {
            if (!file_->done())
                return;
            auto& item = items_[next_];
            if (file_->error) {
                item.error = file_->error;
            } else {
                // Cancellation commits nothing, so it does not wait for the UI.
                if (step.cancelRequested()) {
                    cancelled_ = true;
                    enterAnalysis(context);
                    return;
                }
                // UI priority: nothing is committed under the user's hands; the next tick tries again.
                if (controlCheckBusy(context).has_value())
                    return;
                commit(context, item, *file_);
            }
            file_.reset();
            ++next_;
        }
        const auto total = static_cast<int>(items_.size());
        step.setProgress(static_cast<double>(next_) / total, "importing", static_cast<int>(next_), total);
        if (next_ >= items_.size() || step.cancelRequested()) {
            cancelled_ = next_ < items_.size();
            enterAnalysis(context);
            return;
        }
        file_ = std::make_unique<PreparedFile>(context.processor(), items_[next_].path);
    }

    void commit(ControlOpContext& context, ImportItem& item, PreparedFile& file)
    {
        auto& processor = context.processor();
        const int trackId = trackOf(next_);
        double start = 0.0;
        if (params_.layout == Layout::Sequential) {
            if (!sequentialCursor_)
                sequentialCursor_ =
                    params_.startTimelineSec ? *params_.startTimelineSec : appendStart(context, trackId);
            start = *sequentialCursor_;
        } else {
            start = params_.startTimelineSec ? *params_.startTimelineSec : appendStart(context, trackId);
        }
        OpenTuneAudioProcessor::ImportPlacement placement;
        placement.trackId = trackId;
        placement.timelineStartSeconds = start;

        // OpenTuneAudioProcessor::commitPreparedImportAsPlacement(): the editor's commit. A failed attempt
        // releases what it created and uses up one clip id; see the class comment for why it is repeated.
        // The commit consumes the prepared audio, so an attempt that may be followed by another one commits
        // a copy. Attempts are repeated only while the clip id can be the cause of the failure: for a file
        // that prepareImport() accepted, at a placement that ImportPlacement::isValid() accepts, the commit
        // has no other reason to fail, and any other failure would repeat on every attempt.
        const std::size_t attempts = (placement.isValid() ? controlPossibleClipIdCollisions(context) : 0) + 1;
        OpenTuneAudioProcessor::CommittedPlacement committed;
        std::size_t attempt = 0;
        auto* arrangement = processor.getStandaloneArrangement();
        // The commit selects the clip and makes its track active; the selection the job found is put back below,
        // so that the editor is not shown a clip the request did not focus (see the class comment).
        const auto selectionBefore = captureCommitSelection(arrangement, trackId);
        while (attempt < attempts && !committed.isValid()) {
            ++attempt;
            OpenTuneAudioProcessor::PreparedImport prepared;
            if (attempt < attempts) {
                prepared = file.prepared;
                ++commitCopies();
            } else {
                prepared = std::move(file.prepared);
            }
            committed = processor.commitPreparedImportAsPlacement(std::move(prepared), placement);
        }
        // On every path, a failed commit included: OpenTune does not touch the selection when it fails, so the
        // restore is then a no-op, and it keeps the selection from depending on that.
        restoreCommitSelection(arrangement, trackId, selectionBefore);
        if (!committed.isValid()) {
            item.error = ControlJobError{"INTERNAL", "", "OpenTune did not accept the clip", false};
            return;
        }
        controlRecordCommittedClipId(context, committed.contentKey);
        if (attempt > 1)
            AppLogger::logNoThrow("control API: import.start: the commit of a clip was repeated "
                                  + juce::String(static_cast<int>(attempt - 1))
                                  + " time(s) because the clip id was in use (project opened before)");

        StandaloneArrangement::Placement committedPlacement;
        const double duration =
            arrangement != nullptr && arrangement->getPlacementById(trackId, committed.placementId, committedPlacement)
                ? committedPlacement.durationSeconds
                : 0.0;
        item.imported = true;
        item.trackId = trackId;
        item.placementId = committed.placementId;
        item.contentKey = committed.contentKey;
        item.placementHandle = controlPlacementId(context, committed.placementId);
        item.contentHandle = controlContentId(context, committed.contentKey);
        item.startSec = start;
        item.durationSec = duration;
        item.channelCount = file.channelCount;
        item.sampleRateHz = file.sampleRateHz;
        if (sequentialCursor_)
            sequentialCursor_ = start + duration;
        // The arrangement right after this commit: the revert recipe expects it (see the class comment).
        arrangementRevision_ = controlArrangementRevision(context);

        const std::string label =
            "Track " + std::to_string(trackId + 1) + " " + labelSeconds(start) + "-" + labelSeconds(start + duration);
        auto placementRef = JsonValue::object();
        placementRef.appendMember("placementId", JsonValue::string(item.placementHandle));
        if (attempt > 1) {
            // The editor's import would have failed here: the result tells the client that the job committed the
            // clip again (registry/warnings.json IMPORT_COMMIT_REPEATED).
            auto data = JsonValue::object();
            data.appendMember("repeatCount", JsonValue::integer(static_cast<std::int64_t>(attempt - 1)));
            envelope_->addWarning("IMPORT_COMMIT_REPEATED", placementRef, std::move(data));
        }
        envelope_->addCreated("placement", std::move(placementRef), JsonValue::string(label));
        auto contentRef = JsonValue::object();
        contentRef.appendMember("contentId", JsonValue::string(item.contentHandle));
        envelope_->addCreated("content", std::move(contentRef), JsonValue::string("Content of " + label));
        envelope_->touchPlacement(committed.placementId);
        envelope_->touchContent(item.contentHandle);

        controlMarkDirty(context);
        if (auto* bridge = context.bridge()) {
            bridge->onStructureChanged();
            // As the editor's import into separate tracks: hidden tracks that now hold a clip are shown.
            const auto visible = bridge->visibleTrackCount();
            if (visible && trackId >= *visible && bridge->revealTracks(trackId + 1))
                for (int revealed = *visible; revealed <= trackId; ++revealed)
                    revealedTracks_.push_back(revealed);
        }
    }

    /** After the last commit (or cancellation): focus the last clip, request every analysis. A cancelled
        job, which does not wait for the UI, focuses no clip while the UI is busy, so that the view does
        not change under the user's hands. */
    void enterAnalysis(ControlOpContext& context)
    {
        phase_ = Phase::Analysis;
        file_.reset();
        auto& processor = context.processor();
        ImportItem* focus = nullptr;
        if (params_.focus && !(cancelled_ && controlCheckBusy(context).has_value()))
            for (auto it = items_.rbegin(); it != items_.rend() && focus == nullptr; ++it)
                if (it->imported)
                    focus = &*it;
        auto* bridge = context.bridge();
        if (focus != nullptr) {
            // As the editor after its import (applyPlacementSelectionContext): the clip becomes the
            // selected clip of the active track, and the editor shows it.
            if (auto* arrangement = processor.getStandaloneArrangement())
                arrangement->selectPlacement(focus->trackId, focus->placementId);
            if (bridge != nullptr)
                bridge->focusPlacement(focus->trackId, focus->placementId);
        }
        // In the openDyne scheme notes are generated for the focused clip once its analysis finished, as
        // import.start specifies (PROTOCOL.md section 21 rule 2), with the parameters the editor would use
        // for AUTO. The editor's own import generates them for every clip it commits; see the class comment.
        const bool openDyne = processor.getAppPreferences().getState().shared.audioEditingScheme
                              == AudioEditingScheme::Scheme::NotesPrimary;
        NoteGeneratorParams editorParams;
        const bool haveEditorParams = bridge != nullptr && bridge->currentNoteGenerationParams(editorParams);
        for (auto& item : items_) {
            if (!item.imported)
                continue;
            OpenTuneAudioProcessor::ContentRefreshRequest request;
            request.contentKey = item.contentKey;
            if (params_.generateNotes) {
                // Explicit generation never inherits the editor's knobs: OpenTune's default parameters.
                item.notesBy = "request";
                request.noteGenerationParams = NoteGeneratorParams{};
            } else if (&item == focus && openDyne) {
                item.notesBy = "focus";
                request.noteGenerationParams = haveEditorParams ? editorParams : NoteGeneratorParams{};
            }
            if (!item.notesBy.empty()) {
                request.generateNotesWholeContentOnReady = true;
                auto generated = item.notesGenerated;
                request.onNotesGenerated = [generated] { generated->store(true); };
            }
            // OpenTuneAudioProcessor::requestContentRefresh(): the editor's analysis request after an
            // import, which also runs the requested note generation once the analysis is ready.
            item.analysisRequested =
                analysis_ ? analysis_(processor, request) : processor.requestContentRefresh(request);
        }
    }

    std::optional<OriginalF0State> analysisState(ControlOpContext& context, const ImportItem& item) const
    {
        const auto snapshot = controlLiveSnapshot(context, item.contentKey);
        if (snapshot == nullptr)
            return std::nullopt;
        return snapshot->originalF0State;
    }

    void markGeneratedNotesDirty(ControlOpContext& context)
    {
        for (auto& item : items_)
            if (item.notesGenerated->load() && !item.notesMarkedDirty) {
                // The editor's callback of the import's note generation marks the project as changed.
                controlMarkDirty(context);
                item.notesMarkedDirty = true;
            }
    }

    void stepAnalysis(ControlOpContext& context, ControlJobStep& step)
    {
        markGeneratedNotesDirty(context);
        if (cancelled_ || step.cancelRequested()) {
            finish(context, step, true);
            return;
        }
        int imported = 0;
        int analysed = 0;
        for (const auto& item : items_) {
            if (!item.imported)
                continue;
            ++imported;
            const auto state = analysisState(context, item);
            if (!state || !item.analysisRequested || *state == OriginalF0State::Ready
                || *state == OriginalF0State::Failed)
                ++analysed;
        }
        step.setProgress(imported > 0 ? static_cast<double>(analysed) / imported : 1.0, "analysing", analysed,
                         imported);
        if (analysed == imported)
            finish(context, step, false);
    }

    JsonValue itemJson(ControlOpContext& context, const ImportItem& item) const
    {
        auto json = JsonValue::object();
        json.appendMember("path", JsonValue::string(item.path));
        json.appendMember("imported", JsonValue::boolean(item.imported));
        std::optional<ControlJobError> error = item.error;
        JsonValue f0State;
        if (item.imported) {
            if (const auto state = analysisState(context, item)) {
                f0State = JsonValue::string(controlF0StateName(*state));
                if (*state == OriginalF0State::Failed && !error)
                    error =
                        ControlJobError{"ANALYSIS_FAILED", "f0_failed",
                                        "the pitch analysis of the clip failed; the clip stays in the project", true};
            }
        }
        const auto handleOrNull = [&item](const std::string& handle) {
            return item.imported ? JsonValue::string(handle) : JsonValue();
        };
        json.appendMember("placementId", handleOrNull(item.placementHandle));
        json.appendMember("contentId", handleOrNull(item.contentHandle));
        json.appendMember("trackId", item.imported ? JsonValue::integer(item.trackId) : JsonValue());
        json.appendMember("startTimelineSec", item.imported ? controlJsonSeconds(item.startSec) : JsonValue());
        json.appendMember("durationSec", item.imported ? controlJsonSeconds(item.durationSec) : JsonValue());
        json.appendMember("channelCount", item.imported ? JsonValue::integer(item.channelCount) : JsonValue());
        json.appendMember("originalSampleRateHz",
                          item.imported ? controlJsonNumber(item.sampleRateHz, 3) : JsonValue());
        json.appendMember("resampled", item.imported
                                           ? JsonValue::boolean(std::abs(item.sampleRateHz - kStoredSampleRateHz)
                                                                > kResampleToleranceHz)
                                           : JsonValue());
        json.appendMember("f0State", std::move(f0State));
        const bool generated = item.notesGenerated->load();
        json.appendMember("notesGenerated", JsonValue::boolean(generated));
        json.appendMember("notesGeneratedBy", generated ? JsonValue::string(item.notesBy) : JsonValue());
        json.appendMember("error", error ? controlErrorObjectJson(*error) : JsonValue());
        return json;
    }

    /** The revert recipe that deletes the given clips: one arrangement.edit op delete at the arrangement as the last
        commit left it, with requiresConsent (the end user's consent to delete clips is the client's to add). Null
        without arrangement.v1 (PROTOCOL.md section 15.2 rule 2). At most kMaxItems clips, which one op delete accepts.
     */
    JsonValue deleteClipsRecipe(ControlOpContext& context, const std::vector<std::string>& placementHandles) const
    {
        if (placementHandles.empty() || !controlReportsCapability(context, "arrangement.v1"))
            return JsonValue();
        auto ids = JsonValue::array();
        for (const auto& handle : placementHandles)
            ids.append(JsonValue::string(handle));
        auto op = JsonValue::object();
        op.appendMember("op", JsonValue::string("delete"));
        op.appendMember("placementIds", std::move(ids));
        auto ops = JsonValue::array();
        ops.append(std::move(op));
        auto params = JsonValue::object();
        params.appendMember("expectedArrangementRevision", JsonValue::string(arrangementRevision_));
        params.appendMember("ops", std::move(ops));
        return controlRevertRecipe("arrangement.edit", std::move(params), true);
    }

    void finish(ControlOpContext& context, ControlJobStep& step, bool cancelled)
    {
        markGeneratedNotesDirty(context);
        auto items = JsonValue::array();
        int imported = 0;
        // Files that could not be imported; items that a cancellation left out are neither.
        int failed = 0;
        auto focusContents = JsonValue::array();
        // The clips of the result: every committed clip, and those whose notes the focus generated.
        std::vector<std::string> committedClips;
        std::vector<std::string> focusClips;
        for (const auto& item : items_) {
            items.append(itemJson(context, item));
            if (item.imported) {
                ++imported;
                committedClips.push_back(item.placementHandle);
            } else if (item.error) {
                ++failed;
            }
            if (item.notesGenerated->load() && item.notesBy == "focus") {
                focusContents.append(JsonValue::string(item.contentHandle));
                focusClips.push_back(item.placementHandle);
            }
        }
        for (const int trackId : revealedTracks_) {
            auto subject = JsonValue::object();
            subject.appendMember("trackId", JsonValue::integer(trackId));
            envelope_->addWarning("TRACK_AUTO_REVEALED", std::move(subject));
        }
        if (!revealedTracks_.empty()) {
            // View state: no revert recipe and no IRREVERSIBLE for it (section 15.2 rule 6).
            auto trackIds = JsonValue::array();
            for (const int trackId : revealedTracks_)
                trackIds.append(JsonValue::integer(trackId));
            auto effect = JsonValue::object();
            effect.appendMember("trackIds", std::move(trackIds));
            effect.appendMember("revert", JsonValue());
            envelope_->addSideEffect("tracksRevealed", std::move(effect));
        }
        for (std::size_t i = 0; i < focusContents.size(); ++i) {
            auto subject = JsonValue::object();
            subject.appendMember("contentId", focusContents.at(i));
            envelope_->addWarning("SELECTION_GENERATED_NOTES", std::move(subject));
        }
        if (focusContents.size() != 0) {
            // Deleting the clips is the only way back: the notes have no undo entry. Null without arrangement.v1.
            auto effect = JsonValue::object();
            effect.appendMember("contentIds", std::move(focusContents));
            effect.appendMember("revert", deleteClipsRecipe(context, focusClips));
            envelope_->addSideEffect("notesGenerated", std::move(effect));
        }
        // As in the UI there is no undo entry. The revert recipe deletes the committed clips with arrangement.edit
        // (null without arrangement.v1, then the envelope adds IRREVERSIBLE). Without a committed clip the import
        // changed nothing: no_op, also if the user changed the arrangement while the job ran.
        if (imported > 0)
            envelope_->setNotUndoable("ui_parity", deleteClipsRecipe(context, committedClips));
        else
            envelope_->setNotUndoable("no_op");
        envelope_->limitDiffToTouchedContents();
        auto result = envelope_->committed(context);
        result.appendMember("items", std::move(items));
        auto counts = JsonValue::object();
        counts.appendMember("requested", JsonValue::integer(static_cast<std::int64_t>(items_.size())));
        counts.appendMember("imported", JsonValue::integer(imported));
        counts.appendMember("failed", JsonValue::integer(failed));
        result.appendMember("counts", std::move(counts));
        if (cancelled)
            step.cancel(std::move(result), imported > 0);
        else
            step.succeed(std::move(result));
    }

    ControlOpEnvironment environment_;
    ImportParams params_;
    int targetTrack_;
    std::unique_ptr<ControlWriteEnvelope> envelope_;
    ControlImportAnalysis analysis_;
    std::vector<ImportItem> items_;
    std::size_t next_ = 0;
    std::unique_ptr<PreparedFile> file_;
    std::optional<double> sequentialCursor_;
    /** The arrangement revision right after the last commit. */
    std::string arrangementRevision_;
    std::vector<int> revealedTracks_;
    Phase phase_ = Phase::Import;
    bool cancelled_ = false;
};

// ---- import.start ---------------------------------------------------------------------------------

Result<JsonValue> runImportStart(ControlOpContext& context, const ImportParams& params)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    const auto digest = params.digest();

    // Section 12.5: a retained import job started with this key answers before any check of state.
    if (!params.idempotencyKey.empty()) {
        std::string existingDigest;
        const auto existing = jobs->findByKey("import", params.idempotencyKey, existingDigest);
        if (!existing.empty()) {
            if (existingDigest != digest)
                return controlOpError("INVALID_ARGUMENT", "idempotency_key_reused",
                                      "the idempotencyKey names a retained import job with other params");
            ControlWriteEnvelope envelope(context);
            return envelope.handedToJob(context, jobs->jobRef(existing));
        }
    }

    // Handles, then busy state, then the method's own checks (PROTOCOL.md section 13.1 rule 4).
    auto& processor = context.processor();
    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");
    const int trackCount = arrangement->getNumTracks();
    if (params.trackId && *params.trackId >= trackCount)
        return controlOpError("NOT_FOUND", "unknown_handle", "trackId names no track slot");
    if (auto busy = controlCheckBusy(context))
        return *busy;
    for (const auto& path : params.paths) {
        // Section 20.1: the raw text, before any file-system access.
        const auto reason = controlCheckAbsolutePath(path, controlNativePathStyle());
        if (!reason.empty())
            return controlOpError("INVALID_ARGUMENT", reason, "a path is not an absolute path in the accepted form");
        if (!hasImportExtension(path))
            return controlOpError("INVALID_ARGUMENT", "path_bad_extension",
                                  "a path does not end in an extension of session.info import.extensions");
    }

    int target = 0;
    if (params.trackId) {
        target = *params.trackId;
    } else if (params.newTrack) {
        const auto placements = controlLivePlacements(context);
        target = -1;
        for (int trackId = 0; trackId < trackCount && target < 0; ++trackId)
            if (std::none_of(placements.begin(), placements.end(),
                             [trackId](const ControlPlacementRef& p) { return p.trackId == trackId; }))
                target = trackId;
        if (target < 0)
            return controlOpError("INVALID_ARGUMENT", "track_capacity", "every track holds clips");
    } else {
        // As the editor's import: the active track, track 0 when none is active.
        const int active = arrangement->getActiveTrackId();
        target = active >= 0 && active < trackCount ? active : 0;
    }
    if (params.layout == Layout::SeparateTracks && target + static_cast<int>(params.paths.size()) > trackCount)
        return controlOpError("INVALID_ARGUMENT", "track_capacity",
                              "layout separateTracks needs one track per item from the target track on");

    // The envelope of the job's result starts here: its revisionDiff and dirty flag cover the whole job.
    auto jobEnvelope = std::make_unique<ControlWriteEnvelope>(context);
    auto job = std::make_shared<ImportJob>(context.environment(), params, target, std::move(jobEnvelope));
    const auto started =
        jobs->start("import", true, [job](ControlJobStep& step) { job->step(step); }, params.idempotencyKey, digest);
    if (started.jobId.empty())
        return started.refusal == "job_limit"
                   ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                   : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
    ControlWriteEnvelope envelope(context);
    return envelope.handedToJob(context, jobs->jobRef(started.jobId));
}

} // namespace

void registerImportOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<ImportParams>("import.start", parseImport, runImportStart));
}

void controlSetImportAnalysisForTesting(ControlImportAnalysis analysis)
{
    analysisOverride() = std::move(analysis);
}

std::uint64_t controlImportCommitCopiesForTesting()
{
    return commitCopies();
}

} // namespace OpenTune
