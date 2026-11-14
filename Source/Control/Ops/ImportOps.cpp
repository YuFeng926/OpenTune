// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ImportOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Audio/AudioFormatRegistry.h"
#include "Content/EditableContentSnapshot.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Standalone/StandaloneMediaOperations.h"
#include "Utils/TimeCoordinate.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
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
        params. Numbers compare by value. */
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

/** Parses import.start params on the connection thread. Paths are checked on the message thread with
    method-specific validation and errors. */
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
 * message thread. Errors are classified for the item.
 *
 * The destructor marks the work abandoned and joins the thread; the thread checks the mark between blocks
 * of the decode and before prepareImport(), so destroying an unfinished file waits for at most one block or
 * one prepareImport().
 */
class PreparedFile
{
public:
    PreparedFile(std::string path, std::function<void()> wake)
        : path_(std::move(path))
        , wake_(std::move(wake))
    {
        thread_ = std::thread([this] {
            run();
            done_ = true;
            wake_();
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

    void run()
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
        if (!OpenTuneAudioProcessor::prepareImport(std::move(audio), rate, file.getFileName(),
                                                   juce::String::fromUTF8(path_.c_str()), prepared))
            error = itemError("UNSUPPORTED", "unsupported_format", "OpenTune did not accept the audio");
    }

    std::string path_;
    std::function<void()> wake_;
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
    /** Set on the message thread by the content completion event. */
    std::shared_ptr<std::atomic<bool>> notesGenerated = std::make_shared<std::atomic<bool>>(false);
};

/** API job coordination around the shared import placement, commit and content-refresh operations. */
class ImportJob
{
public:
    ImportJob(ControlOpEnvironment environment, const ImportParams& params, int targetTrack,
              std::unique_ptr<ControlWriteEnvelope> envelope, std::function<void()> wake)
        : environment_(std::move(environment))
        , params_(params)
        , targetTrack_(targetTrack)
        , envelope_(std::move(envelope))
        , wake_(std::move(wake))
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
                    enterAnalysis();
                    return;
                }
                // UI priority: nothing is committed under the user's hands; the next tick tries again.
                if (controlCheckBusy(context).has_value()) {
                    if (!watchingModal_)
                        if (auto* bridge = context.bridge()) {
                            const auto busy = bridge->queryBusy().value_or(ControlUiBridge::BusyState{});
                            if (busy.modalOpen) {
                                bridge->watchBusyState(wake_);
                                watchingModal_ = true;
                            }
                        }
                    return;
                }
                commit(context, item, *file_);
            }
            file_.reset();
            ++next_;
        }
        const auto total = static_cast<int>(items_.size());
        step.setProgress(static_cast<double>(next_) / total, "importing", static_cast<int>(next_), total);
        if (next_ >= items_.size() || step.cancelRequested()) {
            cancelled_ = next_ < items_.size();
            enterAnalysis();
            return;
        }
        file_ = std::make_unique<PreparedFile>(items_[next_].path, wake_);
    }

    void commit(ControlOpContext& context, ImportItem& item, PreparedFile& file)
    {
        StandaloneMediaOperations::ImportPlacementRequest layoutRequest;
        layoutRequest.layout = params_.layout == Layout::Sequential
                                   ? StandaloneMediaOperations::ImportLayout::Sequential
                               : params_.layout == Layout::SeparateTracks
                                   ? StandaloneMediaOperations::ImportLayout::SeparateTracks
                                   : StandaloneMediaOperations::ImportLayout::Single;
        layoutRequest.firstTrackId = targetTrack_;
        layoutRequest.itemIndex = next_;
        layoutRequest.startTimelineSec = params_.startTimelineSec;
        layoutRequest.sequentialCursorSec = sequentialCursor_;
        auto* bridge = context.bridge();
        if (bridge == nullptr) {
            item.error = ControlJobError{"INTERNAL", "editor_unavailable", "the Standalone editor is unavailable", false};
            return;
        }
        auto generated = item.notesGenerated;
        const auto result = bridge->commitImportedAudio(
            std::move(file.prepared), layoutRequest, params_.focus, params_.generateNotes,
            [generated, wake = wake_] {
                generated->store(true);
                wake();
            });
        if (!result.isValid()) {
            item.error = ControlJobError{"INTERNAL", "", "OpenTune did not accept the clip", false};
            return;
        }
        const auto& committed = result.committed;
        const double duration = result.durationSeconds;
        item.imported = true;
        item.trackId = result.placement.trackId;
        item.placementId = committed.placementId;
        item.contentKey = committed.contentKey;
        item.placementHandle = controlPlacementId(context, committed.placementId);
        item.contentHandle = controlContentId(context, committed.contentKey);
        item.startSec = result.placement.timelineStartSeconds;
        item.durationSec = duration;
        item.channelCount = file.channelCount;
        item.sampleRateHz = file.sampleRateHz;
        item.analysisRequested = result.analysisRequested;
        if (result.notesGenerationSource == StandaloneMediaOperations::NotesGenerationSource::ExplicitRequest)
            item.notesBy = "request";
        else if (result.notesGenerationSource == StandaloneMediaOperations::NotesGenerationSource::EditingScheme)
            item.notesBy = "focus";
        revealedTracks_.insert(revealedTracks_.end(), result.revealedTracks.begin(), result.revealedTracks.end());
        if (params_.layout == Layout::Sequential)
            sequentialCursor_ = result.placement.timelineStartSeconds + duration;
        // The arrangement right after this commit: the revert recipe expects it.
        arrangementRevision_ = controlArrangementRevision(context);

        const std::string label = "Track " + std::to_string(item.trackId + 1) + " "
                                  + labelSeconds(item.startSec) + "-"
                                  + labelSeconds(item.startSec + item.durationSec);
        auto placementRef = JsonValue::object();
        placementRef.appendMember("placementId", JsonValue::string(item.placementHandle));
        envelope_->addCreated("placement", std::move(placementRef), JsonValue::string(label));
        auto contentRef = JsonValue::object();
        contentRef.appendMember("contentId", JsonValue::string(item.contentHandle));
        envelope_->addCreated("content", std::move(contentRef), JsonValue::string("Content of " + label));
        envelope_->touchPlacement(committed.placementId);
        envelope_->touchContent(item.contentHandle);

    }

    /** After the last commit or cancellation, request analysis for every committed content. */
    void enterAnalysis()
    {
        phase_ = Phase::Analysis;
        file_.reset();
    }

    std::optional<OriginalF0State> analysisState(ControlOpContext& context, const ImportItem& item) const
    {
        const auto snapshot = controlLiveSnapshot(context, item.contentKey);
        if (snapshot == nullptr)
            return std::nullopt;
        return snapshot->originalF0State;
    }

    void stepAnalysis(ControlOpContext& context, ControlJobStep& step)
    {
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
            ? JsonValue::boolean(item.sampleRateHz != TimeCoordinate::kRenderSampleRate)
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
        without arrangement.v1. At most kMaxItems clips, which one op delete accepts.
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
            // View state: no revert recipe and no IRREVERSIBLE warning.
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
    std::function<void()> wake_;
    std::vector<ImportItem> items_;
    std::size_t next_ = 0;
    std::unique_ptr<PreparedFile> file_;
    std::optional<double> sequentialCursor_;
    /** The arrangement revision right after the last commit. */
    std::string arrangementRevision_;
    std::vector<int> revealedTracks_;
    Phase phase_ = Phase::Import;
    bool cancelled_ = false;
    bool watchingModal_ = false;
};

// ---- import.start ---------------------------------------------------------------------------------

Result<JsonValue> runImportStart(ControlOpContext& context, const ImportParams& params)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    const auto digest = params.digest();

    // A retained import job started with this key answers before any check of state.
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

    // Handles, then busy state, then the method's own checks.
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
        // Check the raw path text before any file-system access.
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
    auto job = std::make_shared<ImportJob>(context.environment(), params, target, std::move(jobEnvelope),
                                           jobs->wakeCallback());
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

} // namespace OpenTune
