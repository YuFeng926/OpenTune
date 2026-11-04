#include "ExternalEditSession.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstring>

namespace OpenTune {

namespace {

constexpr const char* kEditFileName = "edit.xml";
constexpr const char* kCurveFileName = "curve.bin";
constexpr const char* kAudioFileName = "audio.wav";
constexpr const char* kPendingFileName = "external-edit-current.txt";

juce::File editFile(const juce::File& directory)
{
    return directory.getChildFile(kEditFileName);
}

juce::File curveFile(const juce::File& directory)
{
    return directory.getChildFile(kCurveFileName);
}

juce::File pendingPointerFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("OpenTune")
        .getChildFile(kPendingFileName);
}

void writeNoteElement(juce::XmlElement& parent, const Note& note)
{
    auto* element = parent.createNewChildElement("Note");
    element->setAttribute("start", note.startTime);
    element->setAttribute("end", note.endTime);
    element->setAttribute("pitch", note.pitch);
    element->setAttribute("originalPitch", note.originalPitch);
    element->setAttribute("pitchOffset", note.pitchOffset);
    element->setAttribute("retuneSpeed", note.retuneSpeed);
    element->setAttribute("pitchDriftScale", note.pitchDriftScale);
    element->setAttribute("vibratoDepth", note.vibratoDepth);
    element->setAttribute("vibratoRate", note.vibratoRate);
    element->setAttribute("outputGainDb", note.outputGainDb);
    element->setAttribute("isVoiced", note.isVoiced ? 1 : 0);
    if (!note.eq.has_value())
        return;

    auto* eq = element->createNewChildElement("EqSettings");
    eq->setAttribute("active", note.eq->active ? 1 : 0);
    for (const auto& filter : note.eq->filters) {
        auto* filterElement = eq->createNewChildElement("Filter");
        filterElement->setAttribute("type", static_cast<int>(filter.type));
        filterElement->setAttribute("frequencyHz", filter.frequencyHz);
        filterElement->setAttribute("gainDb", filter.gainDb);
        filterElement->setAttribute("q", filter.q);
        filterElement->setAttribute("slot", filter.paletteSlot);
        filterElement->setAttribute("bypassed", filter.bypassed ? 1 : 0);
    }
}

std::vector<Note> readNotes(const juce::XmlElement& parent)
{
    std::vector<Note> notes;
    for (auto* element = parent.getChildByName("Note"); element != nullptr; element = element->getNextElementWithTagName("Note")) {
        Note note;
        note.startTime = element->getDoubleAttribute("start");
        note.endTime = element->getDoubleAttribute("end");
        note.pitch = static_cast<float>(element->getDoubleAttribute("pitch"));
        note.originalPitch = static_cast<float>(element->getDoubleAttribute("originalPitch"));
        note.pitchOffset = static_cast<float>(element->getDoubleAttribute("pitchOffset"));
        note.retuneSpeed = static_cast<float>(element->getDoubleAttribute("retuneSpeed"));
        note.pitchDriftScale = static_cast<float>(element->getDoubleAttribute("pitchDriftScale", 1.0));
        note.vibratoDepth = static_cast<float>(element->getDoubleAttribute("vibratoDepth"));
        note.vibratoRate = static_cast<float>(element->getDoubleAttribute("vibratoRate"));
        note.outputGainDb = static_cast<float>(element->getDoubleAttribute("outputGainDb"));
        note.isVoiced = element->getIntAttribute("isVoiced", 1) != 0;
        if (note.endTime <= note.startTime)
            continue;
        if (auto* eqElement = element->getChildByName("EqSettings")) {
            EqSettings eq;
            eq.filters.clear();
            eq.active = eqElement->getIntAttribute("active", 1) != 0;
            for (auto* filterElement = eqElement->getChildByName("Filter");
                 filterElement != nullptr;
                 filterElement = filterElement->getNextElementWithTagName("Filter")) {
                EqFilter filter;
                filter.type = static_cast<EqFilterType>(filterElement->getIntAttribute("type"));
                filter.frequencyHz = static_cast<float>(filterElement->getDoubleAttribute("frequencyHz"));
                filter.gainDb = static_cast<float>(filterElement->getDoubleAttribute("gainDb"));
                filter.q = static_cast<float>(filterElement->getDoubleAttribute("q", 2.0));
                filter.paletteSlot = filterElement->getIntAttribute("slot");
                filter.bypassed = filterElement->getIntAttribute("bypassed") != 0;
                eq.filters.push_back(filter);
            }
            note.eq = std::move(eq);
        }
        notes.push_back(std::move(note));
    }
    return notes;
}

bool writeFloats(juce::OutputStream& out, const std::vector<float>& values)
{
    if (!out.writeInt(static_cast<int>(values.size())))
        return false;
    for (const float value : values)
        if (!out.writeFloat(value))
            return false;
    return true;
}

bool readFloats(juce::InputStream& in, std::vector<float>& values)
{
    if (in.getNumBytesRemaining() < 4)
        return false;
    const int count = in.readInt();
    if (count < 0 || static_cast<juce::int64>(count) * 4 > in.getNumBytesRemaining())
        return false;
    values.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        values[static_cast<size_t>(i)] = in.readFloat();
    return true;
}

bool writeCurve(const juce::File& file, const PitchCurveSnapshot& snapshot)
{
    juce::FileOutputStream stream(file);
    if (!stream.openedOk())
        return false;
    stream.setPosition(0);
    stream.truncate();
    if (!stream.write("OTC1", 4))
        return false;
    if (!stream.writeInt(snapshot.getHopSize()))
        return false;
    if (!stream.writeDouble(snapshot.getSampleRate()))
        return false;
    if (!writeFloats(stream, snapshot.getOriginalF0()))
        return false;
    if (!writeFloats(stream, snapshot.getOriginalEnergy()))
        return false;

    const auto& erased = snapshot.getErasedRanges();
    if (!stream.writeInt(static_cast<int>(erased.size())))
        return false;
    for (const auto& range : erased) {
        if (!stream.writeInt(range.startFrame) || !stream.writeInt(range.endFrameExclusive))
            return false;
    }

    const auto& segments = snapshot.getCorrectionSegments();
    if (!stream.writeInt(static_cast<int>(segments.size())))
        return false;
    for (const auto& segment : segments) {
        if (!stream.writeInt(segment.startFrame) || !stream.writeInt(segment.endFrame))
            return false;
        if (!stream.writeInt(static_cast<int>(segment.source)))
            return false;
        if (!stream.writeFloat(segment.parameterSnapshot.retuneSpeed)
            || !stream.writeFloat(segment.parameterSnapshot.pitchDriftScale)
            || !stream.writeFloat(segment.parameterSnapshot.vibratoDepth)
            || !stream.writeFloat(segment.parameterSnapshot.vibratoRate))
            return false;
        if (!writeFloats(stream, segment.f0Data) || !writeFloats(stream, segment.baseF0Data))
            return false;
    }
    return stream.getStatus().wasOk();
}

std::shared_ptr<PitchCurve> readCurve(const juce::File& file)
{
    juce::FileInputStream stream(file);
    if (!stream.openedOk())
        return nullptr;
    char magic[4] = {};
    if (stream.read(magic, 4) != 4 || std::memcmp(magic, "OTC1", 4) != 0)
        return nullptr;

    if (stream.getNumBytesRemaining() < 12)
        return nullptr;
    const int hopSize = stream.readInt();
    const double sampleRate = stream.readDouble();
    if (hopSize <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return nullptr;

    std::vector<float> originalF0;
    std::vector<float> energy;
    if (!readFloats(stream, originalF0) || !readFloats(stream, energy))
        return nullptr;

    if (stream.getNumBytesRemaining() < 4)
        return nullptr;
    const int erasedCount = stream.readInt();
    if (erasedCount < 0
        || static_cast<juce::int64>(erasedCount) * 8 > stream.getNumBytesRemaining())
        return nullptr;
    std::vector<F0FrameRange> erased(static_cast<size_t>(erasedCount));
    for (int i = 0; i < erasedCount; ++i) {
        erased[static_cast<size_t>(i)].startFrame = stream.readInt();
        erased[static_cast<size_t>(i)].endFrameExclusive = stream.readInt();
    }

    if (stream.getNumBytesRemaining() < 4)
        return nullptr;
    const int segmentCount = stream.readInt();
    if (segmentCount < 0
        || static_cast<juce::int64>(segmentCount) * 36 > stream.getNumBytesRemaining())
        return nullptr;
    std::vector<PitchCorrectionSegment> segments(static_cast<size_t>(segmentCount));
    for (int i = 0; i < segmentCount; ++i) {
        auto& segment = segments[static_cast<size_t>(i)];
        if (stream.getNumBytesRemaining() < 28)
            return nullptr;
        segment.startFrame = stream.readInt();
        segment.endFrame = stream.readInt();
        segment.source = static_cast<PitchCorrectionSegment::Source>(stream.readInt());
        segment.parameterSnapshot.retuneSpeed = stream.readFloat();
        segment.parameterSnapshot.pitchDriftScale = stream.readFloat();
        segment.parameterSnapshot.vibratoDepth = stream.readFloat();
        segment.parameterSnapshot.vibratoRate = stream.readFloat();
        if (!readFloats(stream, segment.f0Data) || !readFloats(stream, segment.baseF0Data))
            return nullptr;
    }
    if (stream.getNumBytesRemaining() != 0 || originalF0.size() != energy.size())
        return nullptr;

    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(hopSize);
    curve->setSampleRate(sampleRate);
    curve->setOriginalF0(originalF0);
    curve->setOriginalEnergy(energy);
    curve->replaceErasedRangesInRange(0, static_cast<int>(originalF0.size()), erased);
    curve->replaceCorrectionSegments(std::move(segments));
    return curve;
}

bool writeAudio(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(sampleRate)
        .withNumChannels(buffer.getNumChannels())
        .withBitsPerSample(24);
    auto writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
        return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

std::unique_ptr<juce::XmlElement> buildEditXml(const ContentKey& sourceKey,
                                              const EditableContentSnapshot& snapshot,
                                              bool returned)
{
    auto xml = std::make_unique<juce::XmlElement>("ExternalEdit");
    xml->setAttribute("status", returned ? "returned" : "editing");
    xml->setAttribute("domain", static_cast<int>(sourceKey.domainKind));
    xml->setAttribute("objectId", juce::String(static_cast<juce::int64>(sourceKey.objectId)));
    xml->setAttribute("semitone", snapshot.pitchShiftSettings.semitone);
    xml->setAttribute("cents", snapshot.pitchShiftSettings.cents);
    for (const auto& note : snapshot.notes)
        writeNoteElement(*xml, note);
    for (const auto& point : snapshot.volumeEnvelope.points()) {
        auto* element = xml->createNewChildElement("EnvelopePoint");
        element->setAttribute("time", point.timeSeconds);
        element->setAttribute("gainDb", point.gainDb);
    }
    return xml;
}

bool writeEditFiles(const juce::File& directory,
                    const ContentKey& sourceKey,
                    const EditableContentSnapshot& snapshot,
                    bool returned,
                    bool includeAudio)
{
    if (!directory.createDirectory())
        return false;
    if (includeAudio) {
        if (snapshot.audioBuffer == nullptr || snapshot.audioBuffer->getNumSamples() <= 0
            || snapshot.audioSampleRate <= 0.0)
            return false;
        if (!writeAudio(ExternalEditSession::audioFile(directory), *snapshot.audioBuffer, snapshot.audioSampleRate))
            return false;
    }
    if (snapshot.pitchCurve != nullptr) {
        if (!writeCurve(curveFile(directory), *snapshot.pitchCurve))
            return false;
    } else {
        auto staleCurve = curveFile(directory);
        if (staleCurve.existsAsFile() && !staleCurve.deleteFile())
            return false;
    }
    const auto xml = buildEditXml(sourceKey, snapshot, returned);
    return xml != nullptr && xml->writeTo(editFile(directory));
}

std::optional<ExternalEditDocument> readDocumentFromXml(const juce::File& directory,
                                                        const juce::XmlElement& xml)
{
    if (!xml.hasTagName("ExternalEdit"))
        return std::nullopt;

    ExternalEditDocument document;
    document.returned = xml.getStringAttribute("status") == "returned";
    document.sourceKey.domainKind = static_cast<DomainKind>(xml.getIntAttribute("domain"));
    document.sourceKey.objectId = static_cast<uint64_t>(xml.getStringAttribute("objectId").getLargeIntValue());
    document.notes = readNotes(xml);
    document.pitchShift.semitone = xml.getIntAttribute("semitone");
    document.pitchShift.cents = xml.getIntAttribute("cents");
    std::vector<AutomationPoint> points;
    for (auto* element = xml.getChildByName("EnvelopePoint");
         element != nullptr;
         element = element->getNextElementWithTagName("EnvelopePoint")) {
        AutomationPoint point;
        point.timeSeconds = element->getDoubleAttribute("time");
        point.gainDb = static_cast<float>(element->getDoubleAttribute("gainDb"));
        points.push_back(point);
    }
    document.envelope = AutomationLane::fromSnapshot(points);
    if (curveFile(directory).existsAsFile()) {
        document.pitchCurve = readCurve(curveFile(directory));
        if (document.pitchCurve == nullptr)
            return std::nullopt;
    }
    return document;
}

} // namespace

juce::File ExternalEditSession::audioFile(const juce::File& directory)
{
    return directory.getChildFile(kAudioFileName);
}

juce::File ExternalEditSession::createSessionDirectory()
{
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("OpenTune")
        .getChildFile("external-edit")
        .getChildFile(juce::Uuid().toDashedString());
    directory.createDirectory();
    return directory;
}

bool ExternalEditSession::writeSession(const juce::File& directory,
                                       const ContentKey& sourceKey,
                                       const EditableContentSnapshot& snapshot)
{
    return writeEditFiles(directory, sourceKey, snapshot, false, true);
}

bool ExternalEditSession::writeReturn(const juce::File& directory,
                                      const EditableContentSnapshot& snapshot)
{
    auto existing = juce::XmlDocument::parse(editFile(directory));
    ContentKey sourceKey;
    if (existing != nullptr) {
        sourceKey.domainKind = static_cast<DomainKind>(existing->getIntAttribute("domain"));
        sourceKey.objectId = static_cast<uint64_t>(existing->getStringAttribute("objectId").getLargeIntValue());
    }
    return writeEditFiles(directory, sourceKey, snapshot, true, false);
}

std::optional<ExternalEditDocument> ExternalEditSession::readDocument(const juce::File& directory)
{
    const auto xml = juce::XmlDocument::parse(editFile(directory));
    return xml != nullptr ? readDocumentFromXml(directory, *xml) : std::nullopt;
}

ExternalEditCheckResult ExternalEditSession::checkReturned(const juce::File& directory)
{
    const auto xml = juce::XmlDocument::parse(editFile(directory));
    if (xml == nullptr || !xml->hasTagName("ExternalEdit")
        || xml->getStringAttribute("status") != "returned")
        return { ExternalCurveCheckResult::Pending, {} };

    auto document = readDocumentFromXml(directory, *xml);
    if (!document.has_value()) {
        if (curveFile(directory).existsAsFile())
            return { ExternalCurveCheckResult::CorruptCurve, {} };
        return { ExternalCurveCheckResult::Pending, {} };
    }
    return { ExternalCurveCheckResult::Ok, std::move(*document) };
}

juce::File ExternalEditSession::findStandaloneExecutable()
{
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("OPENTUNE_STANDALONE", {});
    if (overridePath.isNotEmpty()) {
        const juce::File overrideFile(overridePath);
        if (overrideFile.exists())
            return overrideFile;
    }

#if JUCE_MAC
    const juce::File systemApp("/Applications/OpenTune.app");
    if (systemApp.exists())
        return systemApp;
    const auto userApp = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Applications/OpenTune.app");
    if (userApp.exists())
        return userApp;
#endif

    auto directory = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int depth = 0; depth < 8 && directory.exists(); ++depth) {
#if JUCE_MAC
        const auto bundle = directory.getChildFile("OpenTune.app");
        if (bundle.exists())
            return bundle;
        const auto binary = directory.getChildFile("OpenTune");
        if (binary.existsAsFile())
            return binary;
#elif JUCE_WINDOWS
        const auto binary = directory.getChildFile("OpenTune.exe");
        if (binary.existsAsFile())
            return binary;
#else
        const auto binary = directory.getChildFile("OpenTune");
        if (binary.existsAsFile())
            return binary;
#endif
        directory = directory.getParentDirectory();
    }
    return {};
}

bool ExternalEditSession::launchStandalone(const juce::File& directory)
{
    pendingPointerFile().getParentDirectory().createDirectory();
    pendingPointerFile().replaceWithText(directory.getFullPathName());

    const auto standalone = findStandaloneExecutable();
    if (!standalone.exists())
        return false;

    const juce::String parameters = juce::String(kExternalEditArgument) + " "
        + directory.getFullPathName().quoted();
    return juce::Process::openDocument(standalone.getFullPathName(), parameters);
}

juce::File ExternalEditSession::sessionDirectoryFromCommandLine()
{
    juce::StringArray arguments;
    if (auto* app = juce::JUCEApplicationBase::getInstance())
        arguments = app->getCommandLineParameterArray();

    std::vector<std::string> storage;
    storage.reserve(static_cast<size_t>(arguments.size()) + 1);
    storage.emplace_back("OpenTune");
    for (const auto& argument : arguments)
        storage.emplace_back(argument.toStdString());

    std::vector<const char*> argv;
    argv.reserve(storage.size());
    for (const auto& argument : storage)
        argv.push_back(argument.c_str());

    const auto launch = parseExternalEditLaunch(static_cast<int>(argv.size()), argv.data());
    if (launch.active)
        return juce::File(launch.sessionDirectory);

    const auto pending = pendingPointerFile();
    if (!pending.existsAsFile())
        return {};
    const juce::File directory(pending.loadFileAsString().trim());
    if (directory.getChildFile(kEditFileName).existsAsFile())
        return directory;
    return {};
}

} // namespace OpenTune
