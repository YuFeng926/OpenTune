#include "CopyVocalBundle.h"
#include "ContentAnalysisState.h"
#include "../Services/ImportedClipF0Extraction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OpenTune::CopyVocalBundle {

namespace {

using Var = juce::var;

Result<Bundle> fail(const juce::String& message)
{
    return Result<Bundle>::failure(
        Error(ErrorCode::InvalidParameter, message.toStdString(), "CopyVocal bundle"));
}

bool isFiniteNumber(const Var& value)
{
    return value.isInt() || value.isInt64() || value.isDouble();
}

bool readNumber(const Var& object, const juce::Identifier& name, double& out,
                bool required, const juce::String& context, juce::String& error)
{
    const auto value = object.getProperty(name, {});
    if (value.isVoid() || value.isUndefined())
    {
        if (required)
        {
            error = context + ": missing required number '" + name.toString() + "'";
            return false;
        }
        return true;
    }
    if (!isFiniteNumber(value))
    {
        error = context + ": '" + name.toString() + "' must be a number";
        return false;
    }
    out = static_cast<double>(value);
    if (!std::isfinite(out))
    {
        error = context + ": '" + name.toString() + "' must be finite";
        return false;
    }
    return true;
}

bool readInt(const Var& object, const juce::Identifier& name, int& out,
             bool required, const juce::String& context, juce::String& error)
{
    double number = 0.0;
    if (!readNumber(object, name, number, required, context, error))
        return false;
    if (object.getProperty(name, {}).isVoid())
        return true;
    if (number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max()))
    {
        error = context + ": '" + name.toString() + "' is out of range";
        return false;
    }
    out = static_cast<int>(std::llround(number));
    return true;
}

bool readInt64(const Var& object, const juce::Identifier& name, int64_t& out,
               bool required, const juce::String& context, juce::String& error)
{
    double number = 0.0;
    if (!readNumber(object, name, number, required, context, error))
        return false;
    if (object.getProperty(name, {}).isVoid())
        return true;
    out = static_cast<int64_t>(std::llround(number));
    return true;
}

bool readBool(const Var& object, const juce::Identifier& name, bool& out,
              bool required, const juce::String& context, juce::String& error)
{
    const auto value = object.getProperty(name, {});
    if (value.isVoid() || value.isUndefined())
    {
        if (required)
        {
            error = context + ": missing required bool '" + name.toString() + "'";
            return false;
        }
        return true;
    }
    if (!value.isBool())
    {
        error = context + ": '" + name.toString() + "' must be a bool";
        return false;
    }
    out = static_cast<bool>(value);
    return true;
}

bool readString(const Var& object, const juce::Identifier& name, juce::String& out,
                bool required, const juce::String& context, juce::String& error)
{
    const auto value = object.getProperty(name, {});
    if (value.isVoid() || value.isUndefined())
    {
        if (required)
        {
            error = context + ": missing required string '" + name.toString() + "'";
            return false;
        }
        return true;
    }
    if (!value.isString())
    {
        error = context + ": '" + name.toString() + "' must be a string";
        return false;
    }
    out = value.toString();
    return true;
}

bool readNumberArray(const Var& object, const juce::Identifier& name,
                     std::vector<double>& out, bool required,
                     const juce::String& context, juce::String& error)
{
    const auto value = object.getProperty(name, {});
    if (value.isVoid() || value.isUndefined())
    {
        if (required)
        {
            error = context + ": missing required array '" + name.toString() + "'";
            return false;
        }
        return true;
    }
    auto* array = value.getArray();
    if (array == nullptr)
    {
        error = context + ": '" + name.toString() + "' must be an array";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(array->size()));
    for (const auto& item : *array)
    {
        if (!isFiniteNumber(item))
        {
            error = context + ": '" + name.toString() + "' contains a non-number";
            return false;
        }
        const double number = static_cast<double>(item);
        if (!std::isfinite(number))
        {
            error = context + ": '" + name.toString() + "' contains a non-finite value";
            return false;
        }
        out.push_back(number);
    }
    return true;
}

bool decodeBase64(const juce::String& encoded, juce::MemoryBlock& out)
{
    out.reset();
    if (encoded.isEmpty())
        return false;
    juce::MemoryOutputStream stream;
    if (!juce::Base64::convertFromBase64(stream, encoded))
        return false;
    out = stream.getMemoryBlock();
    return !out.isEmpty();
}

bool parsePitchTrack(const Var& object, PitchTrack& out, const juce::String& context,
                     juce::String& error)
{
    if (!object.isObject())
    {
        error = context + ": must be an object";
        return false;
    }
    if (!readInt(object, "hopSize", out.hopSize, true, context, error))
        return false;
    if (!readNumber(object, "sampleRate", out.sampleRate, true, context, error))
        return false;
    if (!readInt(object, "frameCount", out.frameCount, true, context, error))
        return false;
    if (out.hopSize != kF0HopSize || std::abs(out.sampleRate - kF0SampleRate) > 1e-6)
    {
        error = context + ": only hopSize=160 / sampleRate=16000 is supported by format v1";
        return false;
    }
    if (out.frameCount <= 1 || out.frameCount > 20'000'000)
    {
        error = context + ": frameCount is out of range";
        return false;
    }

    return true;
}

bool parsePitchChannel(const Var& channelObject, PitchTrack& track, const juce::String& context,
                       juce::String& error)
{
    if (!channelObject.isObject())
    {
        error = context + ": F0 channel must be an object";
        return false;
    }
    juce::String encoding;
    if (!readString(channelObject, "encoding", encoding, true, context, error))
        return false;
    if (encoding != "base64-f32le")
    {
        error = context + ": unsupported F0 encoding '" + encoding + "'";
        return false;
    }
    juce::String valuesBase64;
    juce::String voicedBase64;
    if (!readString(channelObject, "values", valuesBase64, true, context, error))
        return false;
    if (!readString(channelObject, "voiced", voicedBase64, true, context, error))
        return false;

    juce::MemoryBlock valuesBlock;
    juce::MemoryBlock voicedBlock;
    if (!decodeBase64(valuesBase64, valuesBlock))
    {
        error = context + ": 'values' is not valid base64";
        return false;
    }
    if (!decodeBase64(voicedBase64, voicedBlock))
    {
        error = context + ": 'voiced' is not valid base64";
        return false;
    }
    const auto expectedValueBytes = static_cast<size_t>(track.frameCount) * sizeof(float);
    if (valuesBlock.getSize() != expectedValueBytes)
    {
        error = context + ": 'values' byte count " + juce::String(valuesBlock.getSize())
            + " does not match frameCount*4=" + juce::String(expectedValueBytes);
        return false;
    }
    if (voicedBlock.getSize() != static_cast<size_t>(track.frameCount))
    {
        error = context + ": 'voiced' byte count " + juce::String(voicedBlock.getSize())
            + " does not match frameCount=" + juce::String(track.frameCount);
        return false;
    }

    track.f0Hz.resize(static_cast<size_t>(track.frameCount));
    std::memcpy(track.f0Hz.data(), valuesBlock.getData(), expectedValueBytes);
    track.voiced.assign(voicedBlock.getSize(), 0);
    std::memcpy(track.voiced.data(), voicedBlock.getData(), voicedBlock.getSize());

    for (int frame = 0; frame < track.frameCount; ++frame)
    {
        if (!std::isfinite(track.f0Hz[static_cast<size_t>(frame)])
            || track.f0Hz[static_cast<size_t>(frame)] < 0.0f)
        {
            error = context + ": F0 value at frame " + juce::String(frame) + " is invalid";
            return false;
        }
        track.voiced[static_cast<size_t>(frame)] =
            (track.voiced[static_cast<size_t>(frame)] != 0
             && track.f0Hz[static_cast<size_t>(frame)] > 0.0f) ? 1 : 0;
    }
    return true;
}

bool parseMedia(const Var& object, const juce::File& bundleDirectory, MediaRef& out,
                bool required, const juce::String& context, juce::String& error)
{
    if (object.isVoid() || object.isUndefined())
    {
        if (required)
        {
            error = context + ": missing required object";
            return false;
        }
        return true;
    }
    if (!object.isObject())
    {
        error = context + ": must be an object";
        return false;
    }
    juce::String path;
    if (!readString(object, "path", path, true, context, error))
        return false;
    out.file = juce::File::isAbsolutePath(path)
        ? juce::File(path)
        : bundleDirectory.getChildFile(path);
    if (!out.file.existsAsFile())
    {
        error = context + ": media file does not exist: " + out.file.getFullPathName();
        return false;
    }
    if (!readInt(object, "sampleRate", out.sampleRate, true, context, error))
        return false;
    if (!readInt(object, "channels", out.channels, true, context, error))
        return false;
    if (!readInt64(object, "sampleCount", out.sampleCount, true, context, error))
        return false;
    if (!readNumber(object, "durationSeconds", out.durationSeconds, true, context, error))
        return false;
    if (out.sampleRate <= 0 || out.channels <= 0 || out.channels > 2 || out.sampleCount <= 0)
    {
        error = context + ": invalid media shape (sampleRate/channels/sampleCount)";
        return false;
    }
    if (!readString(object, "sha256", out.sha256, false, context, error))
        return false;
    return true;
}

bool parseNotes(const Var& object, std::vector<NoteEvent>& out, const juce::String& context,
                juce::String& error)
{
    const auto value = object.getProperty("notes", {});
    if (value.isVoid() || value.isUndefined())
        return true;
    auto* array = value.getArray();
    if (array == nullptr)
    {
        error = context + ": 'notes' must be an array";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(array->size()));
    int index = 0;
    for (const auto& item : *array)
    {
        const auto itemContext = context + " note[" + juce::String(index++) + "]";
        if (!item.isObject())
        {
            error = itemContext + ": must be an object";
            return false;
        }
        NoteEvent note;
        double pitchHz = 0.0;
        double midi = 0.0;
        double confidence = 0.0;
        if (!readNumber(item, "start", note.startSeconds, true, itemContext, error)
            || !readNumber(item, "end", note.endSeconds, true, itemContext, error)
            || !readNumber(item, "pitchHz", pitchHz, true, itemContext, error)
            || !readNumber(item, "midi", midi, false, itemContext, error)
            || !readNumber(item, "confidence", confidence, false, itemContext, error)
            || !readString(item, "lyric", note.lyric, false, itemContext, error)
            || !readString(item, "source", note.source, false, itemContext, error))
        {
            return false;
        }
        note.pitchHz = static_cast<float>(pitchHz);
        note.midi = static_cast<float>(midi);
        note.confidence = static_cast<float>(confidence);
        if (note.endSeconds <= note.startSeconds || note.pitchHz <= 0.0f)
            continue;
        out.push_back(std::move(note));
    }
    std::sort(out.begin(), out.end(), [](const NoteEvent& a, const NoteEvent& b) {
        return a.startSeconds < b.startSeconds
            || (a.startSeconds == b.startSeconds && a.endSeconds < b.endSeconds);
    });
    return true;
}

bool parseRegions(const Var& object, std::vector<Region>& out, const juce::String& context,
                  juce::String& error)
{
    const auto value = object.getProperty("regions", {});
    if (value.isVoid() || value.isUndefined())
        return true;
    auto* array = value.getArray();
    if (array == nullptr)
    {
        error = context + ": 'regions' must be an array";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(array->size()));
    int index = 0;
    for (const auto& item : *array)
    {
        const auto itemContext = context + " region[" + juce::String(index++) + "]";
        if (!item.isObject())
        {
            error = itemContext + ": must be an object";
            return false;
        }
        Region region;
        if (!readNumber(item, "teacherStart", region.teacherStart, true, itemContext, error)
            || !readNumber(item, "teacherEnd", region.teacherEnd, true, itemContext, error)
            || !readNumber(item, "studentStart", region.studentStart, true, itemContext, error)
            || !readNumber(item, "studentEnd", region.studentEnd, true, itemContext, error)
            || !readNumber(item, "quality", region.quality, false, itemContext, error)
            || !readBool(item, "unsupported", region.unsupported, false, itemContext, error)
            || !readString(item, "status", region.status, false, itemContext, error)
            || !readString(item, "mode", region.mode, false, itemContext, error)
            || !readString(item, "reason", region.reason, false, itemContext, error)
            || !readNumberArray(item, "teacherAnchors", region.teacherAnchors, false, itemContext, error)
            || !readNumberArray(item, "studentAnchors", region.studentAnchors, false, itemContext, error))
        {
            return false;
        }
        double stretchRatio = 0.0;
        if (item.getProperty("stretchRatio", {}).isVoid() == false)
        {
            if (!readNumber(item, "stretchRatio", stretchRatio, false, itemContext, error))
                return false;
            region.hasStretchRatio = true;
            region.stretchRatio = stretchRatio;
        }
        if (region.teacherEnd < region.teacherStart)
            std::swap(region.teacherStart, region.teacherEnd);
        if (region.studentEnd < region.studentStart)
            std::swap(region.studentStart, region.studentEnd);
        if (region.teacherAnchors.size() != region.studentAnchors.size()
            || region.teacherAnchors.size() < 2)
        {
            region.teacherAnchors.clear();
            region.studentAnchors.clear();
        }
        else
        {
            bool valid = true;
            for (size_t anchor = 0; anchor < region.teacherAnchors.size(); ++anchor)
            {
                const double previousTeacher = anchor == 0 ? -1.0 : region.teacherAnchors[anchor - 1];
                const double previousStudent = anchor == 0 ? -1.0 : region.studentAnchors[anchor - 1];
                if (region.teacherAnchors[anchor] <= previousTeacher
                    || region.studentAnchors[anchor] <= previousStudent)
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
            {
                region.teacherAnchors.clear();
                region.studentAnchors.clear();
            }
        }
        out.push_back(std::move(region));
    }
    return true;
}

bool parseTimeGrid(const Var& object, std::vector<TimeHandleSpec>& out, const juce::String& context,
                   juce::String& error)
{
    const auto grid = object.getProperty("timeGrid", {});
    if (!grid.isObject())
    {
        error = context + ": missing 'timeGrid' object";
        return false;
    }
    const auto handlesValue = grid.getProperty("handles", {});
    auto* handles = handlesValue.getArray();
    if (handles == nullptr || handles->size() < 2)
    {
        error = context + ": 'timeGrid.handles' must contain at least two handles";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(handles->size()));
    int index = 0;
    for (const auto& item : *handles)
    {
        const auto itemContext = context + " timeGrid.handle[" + juce::String(index++) + "]";
        if (!item.isObject())
        {
            error = itemContext + ": must be an object";
            return false;
        }
        TimeHandleSpec handle;
        if (!readNumber(item, "sourceSeconds", handle.sourceSeconds, true, itemContext, error)
            || !readNumber(item, "outputSeconds", handle.outputSeconds, true, itemContext, error)
            || !readString(item, "kind", handle.kind, true, itemContext, error)
            || !readString(item, "confidence", handle.confidence, true, itemContext, error))
        {
            return false;
        }
        out.push_back(std::move(handle));
    }
    return true;
}

juce::var childObject(const Var& parent, const juce::Identifier& name)
{
    return parent.getProperty(name, {});
}

} // namespace

int expectedF0FrameCount(int64_t sampleCount, int hopSize, double f0SampleRate) noexcept
{
    if (sampleCount <= 0 || hopSize <= 0 || f0SampleRate <= 0.0)
        return 1;
    const auto f0Samples = static_cast<int64_t>(
        std::llround(static_cast<double>(sampleCount) * f0SampleRate / 44100.0));
    return static_cast<int>(f0Samples / hopSize + 1);
}

Result<Bundle> Bundle::loadFromFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return fail("bundle file does not exist: " + file.getFullPathName());

    const auto root = juce::JSON::parse(file);
    if (root.isVoid() || !root.isObject())
        return fail("bundle is not valid JSON object: " + file.getFullPathName());

    const juce::String context = "CopyVocal bundle";
    juce::String error;
    Bundle bundle;

    juce::String format;
    if (!readString(root, "format", format, true, context, error))
        return fail(error);
    if (format != kFormatName)
        return fail("unsupported bundle format '" + format + "' (expected " + kFormatName + ")");
    if (!readInt(root, "formatVersion", bundle.formatVersion, true, context, error))
        return fail(error);
    if (bundle.formatVersion <= 0 || bundle.formatVersion > kFormatVersion)
    {
        return fail("unsupported bundle formatVersion "
            + juce::String(bundle.formatVersion) + " (this OpenTune supports up to "
            + juce::String(kFormatVersion) + ")");
    }

    const auto project = childObject(root, "project");
    if (!project.isObject())
        return fail("missing 'project' object");
    if (!readString(project, "name", bundle.projectName, true, "project", error)
        || !readString(project, "language", bundle.language, false, "project", error))
        return fail(error);

    const auto clocks = childObject(root, "clocks");
    if (!clocks.isObject())
        return fail("missing 'clocks' object");
    if (!readNumber(clocks, "studentSeconds", bundle.studentSeconds, true, "clocks", error)
        || !readNumber(clocks, "teacherSeconds", bundle.teacherSeconds, true, "clocks", error)
        || !readNumber(clocks, "outputSeconds", bundle.outputSeconds, true, "clocks", error)
        || !readNumber(clocks, "projectSourceSeconds", bundle.projectSourceSeconds, true, "clocks", error)
        || !readBool(clocks, "timingChanged", bundle.timingChanged, true, "clocks", error)
        || !readString(clocks, "outputClock", bundle.outputClock, true, "clocks", error)
        || !readString(clocks, "pitchPolicy", bundle.pitchPolicy, false, "clocks", error)
        || !readNumber(clocks, "registerOffsetCents", bundle.registerOffsetCents, false, "clocks", error))
        return fail(error);

    if (bundle.outputSeconds <= 0.0 || bundle.projectSourceSeconds <= 0.0)
        return fail("clocks: output/projectSource duration must be positive");
    if (std::abs(bundle.outputSeconds - bundle.projectSourceSeconds) > 1e-6)
        return fail("clocks: projectSourceSeconds must equal outputSeconds");
    if (bundle.outputClock != "teacher" && bundle.outputClock != "student")
        return fail("clocks: outputClock must be 'teacher' or 'student'");

    const auto media = childObject(root, "media");
    if (!media.isObject())
        return fail("missing 'media' object");
    if (!parseMedia(childObject(media, "student"), file.getParentDirectory(),
                    bundle.studentMedia, true, "media.student", error))
        return fail(error);
    bundle.hasTeacherMedia = !childObject(media, "teacher").isVoid();
    if (bundle.hasTeacherMedia
        && !parseMedia(childObject(media, "teacher"), file.getParentDirectory(),
                       bundle.teacherMedia, false, "media.teacher", error))
        return fail(error);
    bundle.hasCorrectedMedia = !childObject(media, "corrected").isVoid();
    if (bundle.hasCorrectedMedia
        && !parseMedia(childObject(media, "corrected"), file.getParentDirectory(),
                       bundle.correctedMedia, false, "media.corrected", error))
        return fail(error);

    const auto f0 = childObject(root, "f0");
    if (!f0.isObject())
        return fail("missing 'f0' object");
    if (!parsePitchTrack(f0, bundle.originalF0, "f0", error)
        || !parsePitchChannel(childObject(f0, "original"), bundle.originalF0, "f0.original", error))
        return fail(error);
    bundle.targetF0.hopSize = bundle.originalF0.hopSize;
    bundle.targetF0.sampleRate = bundle.originalF0.sampleRate;
    bundle.targetF0.frameCount = bundle.originalF0.frameCount;
    if (!parsePitchChannel(childObject(f0, "target"), bundle.targetF0, "f0.target", error))
        return fail(error);

    const auto expectedFrames = static_cast<int>(std::llround(bundle.outputSeconds / kF0FrameStepSeconds)) + 1;
    if (std::abs(bundle.originalF0.frameCount - expectedFrames) > 2)
    {
        return fail("f0: frameCount " + juce::String(bundle.originalF0.frameCount)
            + " does not match project duration " + juce::String(bundle.outputSeconds, 9)
            + "s (expected about " + juce::String(expectedFrames) + ")");
    }

    if (!parseNotes(root, bundle.notes, "bundle", error)
        || !parseRegions(root, bundle.regions, "bundle", error)
        || !parseTimeGrid(root, bundle.timeGrid, "bundle", error))
        return fail(error);

    const auto diagnostics = childObject(root, "diagnostics");
    if (diagnostics.isObject())
    {
        const auto warnings = diagnostics.getProperty("warnings", {});
        if (auto* array = warnings.getArray())
            for (const auto& warning : *array)
                if (warning.isString())
                    bundle.warnings.add(warning.toString());
        if (!readInt(diagnostics, "unresolvedRegionCount", bundle.unresolvedRegionCount,
                     false, "diagnostics", error)
            || !readNumber(diagnostics, "unsupportedDurationSeconds",
                           bundle.unsupportedDurationSeconds, false, "diagnostics", error))
            return fail(error);
    }

    // Notes may be slightly outside the padded/trimmed project window; clamp
    // them instead of rejecting an otherwise usable bundle.
    for (auto& note : bundle.notes)
    {
        note.startSeconds = juce::jlimit(0.0, bundle.projectSourceSeconds, note.startSeconds);
        note.endSeconds = juce::jlimit(0.0, bundle.projectSourceSeconds, note.endSeconds);
    }
    bundle.notes.erase(
        std::remove_if(bundle.notes.begin(), bundle.notes.end(),
            [](const NoteEvent& note) { return note.endSeconds <= note.startSeconds; }),
        bundle.notes.end());

    return Result<Bundle>::success(std::move(bundle));
}

Result<std::shared_ptr<PitchCurve>> buildPitchCurve(const Bundle& bundle, int expectedFrames)
{
    const int frames = expectedFrames > 0 ? expectedFrames : bundle.originalF0.frameCount;
    if (frames <= 0)
        return Result<std::shared_ptr<PitchCurve>>::failure(
            Error(ErrorCode::InvalidParameter, "invalid F0 frame count", "CopyVocal bundle"));

    std::vector<float> original(static_cast<size_t>(frames), 0.0f);
    std::vector<float> target(static_cast<size_t>(frames), 0.0f);
    const int copyFrames = std::min(frames, bundle.originalF0.frameCount);
    if (copyFrames > 0)
    {
        std::copy_n(bundle.originalF0.f0Hz.begin(), copyFrames, original.begin());
        std::copy_n(bundle.targetF0.f0Hz.begin(), copyFrames, target.begin());
    }

    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(kF0HopSize);
    curve->setSampleRate(kF0SampleRate);
    curve->setOriginalF0(original);

    std::vector<PitchCorrectionSegment> segments;
    int frame = 0;
    while (frame < frames)
    {
        const bool voiced = target[static_cast<size_t>(frame)] > 0.0f
            && original[static_cast<size_t>(frame)] > 0.0f;
        if (!voiced)
        {
            ++frame;
            continue;
        }
        const int start = frame;
        while (frame < frames
               && target[static_cast<size_t>(frame)] > 0.0f
               && original[static_cast<size_t>(frame)] > 0.0f)
            ++frame;
        const int end = frame;
        PitchCorrectionSegment segment;
        segment.startFrame = start;
        segment.endFrame = end;
        segment.f0Data.assign(target.begin() + start, target.begin() + end);
        segment.baseF0Data = segment.f0Data;
        segment.source = PitchCorrectionSegment::Source::HandDraw;
        segment.parameterSnapshot = {};
        segments.push_back(std::move(segment));
    }
    if (!segments.empty())
        curve->replaceCorrectionSegments(segments);
    return Result<std::shared_ptr<PitchCurve>>::success(std::move(curve));
}

std::vector<Note> buildNotes(const Bundle& bundle)
{
    std::vector<Note> notes;
    notes.reserve(bundle.notes.size());
    for (const auto& event : bundle.notes)
    {
        Note note;
        note.startTime = juce::jlimit(0.0, bundle.projectSourceSeconds, event.startSeconds);
        note.endTime = juce::jlimit(0.0, bundle.projectSourceSeconds, event.endSeconds);
        if (note.endTime - note.startTime < 0.020)
            continue;
        note.pitch = event.pitchHz;
        const int frame = static_cast<int>(std::llround(note.startTime / kF0FrameStepSeconds));
        if (frame >= 0 && frame < bundle.originalF0.frameCount)
        {
            const float original = bundle.originalF0.f0Hz[static_cast<size_t>(frame)];
            note.originalPitch = original > 0.0f ? original : event.pitchHz;
        }
        else
        {
            note.originalPitch = event.pitchHz;
        }
        note.pitchOffset = 0.0f;
        note.isVoiced = true;
        note.dirty = true;
        notes.push_back(note);
    }
    return normalizeStoredNotes(notes);
}

namespace {

HandleKind handleKindFromString(const juce::String& value)
{
    if (value == "clipStart") return HandleKind::ClipStart;
    if (value == "clipEnd") return HandleKind::ClipEnd;
    if (value == "onsetVoiced") return HandleKind::OnsetVoiced;
    if (value == "onsetSibilant") return HandleKind::OnsetSibilant;
    if (value == "onsetSilence") return HandleKind::OnsetSilence;
    if (value == "userAdded") return HandleKind::UserAdded;
    return HandleKind::InternalOnset;
}

} // namespace

std::shared_ptr<const TimeGridSnapshot> buildTimeGrid(const Bundle& bundle)
{
    if (!bundle.timingChanged || bundle.outputClock != "teacher")
        return TimeGridSnapshot::makeIdentity(bundle.projectSourceSeconds);

    if (bundle.timeGrid.size() < 2)
        return TimeGridSnapshot::makeIdentity(bundle.projectSourceSeconds);

    std::vector<TimeHandle> handles;
    handles.reserve(bundle.timeGrid.size());
    uint64_t id = 1;
    for (const auto& spec : bundle.timeGrid)
    {
        TimeHandle handle;
        handle.id = id++;
        handle.kind = handleKindFromString(spec.kind);
        handle.source_seconds = spec.sourceSeconds;
        handle.output_seconds = spec.outputSeconds;
        handle.confidence = spec.confidence == "high" ? Confidence::High : Confidence::Default;
        if (handle.kind == HandleKind::ClipStart || handle.kind == HandleKind::ClipEnd)
            handle.confidence = Confidence::Default;
        handles.push_back(handle);
    }

    auto snapshot = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (snapshot == nullptr
        || std::abs(snapshot->totalDurationSeconds() - bundle.projectSourceSeconds) > 1e-6)
    {
        return TimeGridSnapshot::makeIdentity(bundle.projectSourceSeconds);
    }
    if (snapshot->isIdentity())
        return TimeGridSnapshot::makeIdentity(bundle.projectSourceSeconds);
    return snapshot;
}

bool hasEditablePitch(const Bundle& bundle) noexcept
{
    for (const auto value : bundle.targetF0.f0Hz)
        if (value > 0.0f)
            return true;
    return false;
}

Result<PreparedProject> buildProject(const Bundle& bundle,
                                     int64_t storedSamples,
                                     int numChannels,
                                     double sampleRate,
                                     const std::vector<SilentGap>& silentGaps,
                                     int projectFormatVersion,
                                     const juce::String& appVersion,
                                     const float* sourcePCM)
{
    if (storedSamples <= 0 || numChannels <= 0 || numChannels > 2 || sampleRate <= 0.0)
    {
        return Result<PreparedProject>::failure(
            Error(ErrorCode::InvalidAudioInput,
                  "invalid stored audio shape for CopyVocal bundle",
                  "CopyVocal bundle"));
    }
    if (std::abs(sampleRate - 44100.0) > 1e-6)
    {
        return Result<PreparedProject>::failure(
            Error(ErrorCode::InvalidAudioInput,
                  "CopyVocal student media must be imported at 44100 Hz",
                  "CopyVocal bundle"));
    }

    const double durationSeconds = static_cast<double>(storedSamples) / sampleRate;
    if (std::abs(durationSeconds - bundle.projectSourceSeconds) > 0.002)
    {
        return Result<PreparedProject>::failure(
            Error(ErrorCode::InvalidParameter,
                  "stored audio duration does not match the bundle's projectSourceSeconds",
                  "CopyVocal bundle"));
    }

    const int expectedFrames = expectedF0FrameCount(storedSamples);
    auto curveResult = buildPitchCurve(bundle, expectedFrames);
    if (!curveResult.ok())
        return Result<PreparedProject>::failure(curveResult.error());
    auto pitchCurve = std::move(curveResult).value();

    if (sourcePCM != nullptr)
    {
        // OpenDyne draws note blobs from per-frame original energy. Native
        // imports get this from F0 extraction; a bundle import must compute it
        // from the same student PCM or the roller shows no note blocks.
        const auto originalSnapshot = pitchCurve->getSnapshot();
        auto energy = computeFrameEnergy(
            sourcePCM,
            static_cast<int>(storedSamples),
            static_cast<int>(std::llround(sampleRate)),
            originalSnapshot->getOriginalF0(),
            static_cast<int>(kF0SampleRate),
            kF0HopSize);
        pitchCurve->setOriginalEnergy(energy);
    }

    auto notes = buildNotes(bundle);
    for (auto& note : notes)
    {
        note.startTime = juce::jlimit(0.0, durationSeconds, note.startTime);
        note.endTime = juce::jlimit(0.0, durationSeconds, note.endTime);
    }
    notes = normalizeStoredNotes(notes);

    auto timeGrid = buildTimeGrid(bundle);
    if (timeGrid == nullptr
        || std::abs(timeGrid->totalDurationSeconds() - durationSeconds) > 1e-6)
        timeGrid = TimeGridSnapshot::makeIdentity(durationSeconds);
    if (timeGrid == nullptr)
    {
        return Result<PreparedProject>::failure(
            Error(ErrorCode::InvalidParameter,
                  "cannot build a time grid for the CopyVocal bundle",
                  "CopyVocal bundle"));
    }

    const ContentKey contentKey{DomainKind::StandaloneClip, 1, 0};

    PreparedProject prepared;
    prepared.contentKey = contentKey;
    prepared.pitchCurve = std::move(pitchCurve);
    prepared.expectedF0Frames = expectedFrames;
    prepared.projectDurationSeconds = durationSeconds;

    auto& snapshot = prepared.snapshot;
    snapshot.header.projectFormatVersion = projectFormatVersion;
    snapshot.header.appVersion = appVersion;
    snapshot.header.projectName = bundle.projectName.isNotEmpty()
        ? bundle.projectName
        : juce::String("CopyVocal");
    snapshot.header.projectId = ProjectSnapshot::generateProjectId();
    snapshot.header.createdAt = ProjectSnapshot::generateTimestamp();

    snapshot.settings.bpm = 120.0;
    snapshot.settings.sampleRate = sampleRate;
    snapshot.settings.timeSignatureNumerator = 4;
    snapshot.settings.timeSignatureDenominator = 4;
    snapshot.settings.selectedTrackId = 0;
    snapshot.settings.selectedPlacementId = 1;
    snapshot.settings.visibleTrackCount = 1;
    snapshot.settings.activeTrackId = 0;

    ProjectSourceEntry sourceEntry;
    sourceEntry.sourceId = 1;
    sourceEntry.displayName = "CopyVocal Student";
    sourceEntry.originalImportPath = bundle.studentMedia.file.getFullPathName();
    sourceEntry.sampleRate = sampleRate;
    sourceEntry.numChannels = numChannels;
    sourceEntry.lengthSamples = storedSamples;
    sourceEntry.lengthSeconds = durationSeconds;
    sourceEntry.contentHash = bundle.studentMedia.sha256;
    sourceEntry.fileSizeBytes = bundle.studentMedia.file.getSize();
    snapshot.sources.push_back(std::move(sourceEntry));

    ProjectContentEntry contentEntry;
    contentEntry.contentKey = contentKey;
    contentEntry.sourceId = 1;
    contentEntry.sourceWindow.sourceId = 1;
    contentEntry.sourceWindow.sourceStartSeconds = 0.0;
    contentEntry.sourceWindow.sourceEndSeconds = durationSeconds;
    contentEntry.notes = notes;
    contentEntry.noteTopologyInitialized = true;
    contentEntry.originalF0State = static_cast<uint8_t>(OriginalF0State::Ready);
    contentEntry.timeGrid.handles.reserve(timeGrid->handles().size());
    for (const auto& handle : timeGrid->handles())
    {
        ProjectContentEntry::TimeGridEntry::HandleEntry entry;
        entry.id = handle.id;
        entry.kind = static_cast<uint8_t>(handle.kind);
        entry.sourceSeconds = handle.source_seconds;
        entry.outputSeconds = handle.output_seconds;
        entry.confidence = static_cast<uint8_t>(handle.confidence);
        contentEntry.timeGrid.handles.push_back(entry);
    }
    contentEntry.silentGaps.reserve(silentGaps.size());
    for (const auto& gap : silentGaps)
    {
        ProjectContentEntry::SilentGapEntry entry;
        entry.startSample = gap.startSample;
        entry.endSampleExclusive = gap.endSampleExclusive;
        entry.minLevel_dB = gap.minLevel_dB;
        contentEntry.silentGaps.push_back(entry);
    }
    snapshot.contents.push_back(std::move(contentEntry));

    ProjectTrackEntry trackEntry;
    trackEntry.trackId = 0;
    trackEntry.name = "Vocal";
    trackEntry.gain = 1.0f;
    trackEntry.mute = false;
    trackEntry.solo = false;
    trackEntry.colour = juce::Colour(0xff4a90d9);
    ProjectPlacementEntry placement;
    placement.placementId = 1;
    placement.contentKey = contentKey;
    placement.timelineStartSeconds = 0.0;
    placement.timelineDurationSeconds = durationSeconds;
    placement.clipGain = 1.0f;
    placement.name = snapshot.header.projectName;
    trackEntry.placements.push_back(std::move(placement));
    snapshot.tracks.push_back(std::move(trackEntry));

    return Result<PreparedProject>::success(std::move(prepared));
}

} // namespace OpenTune::CopyVocalBundle
