/**
 * TimeGrid runtime tests — direct use of production TimeGridSnapshot and
 * ProjectPersistence. No mocks and no copied production algorithms.
 */
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "Utils/ProjectPersistence.h"
#include "Utils/TimeGrid.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

bool sameSeconds(double left, double right)
{
    return std::abs(left - right) < 1.0e-12;
}

std::vector<OpenTune::TimeHandle> makeValidHandles()
{
    using namespace OpenTune;
    return {
        {0x7fff000000000001ULL, 0.0, 0.0,
         HandleKind::ClipStart, Confidence::Default},
        {0x7fff000000000002ULL, 5.0, 4.0,
         HandleKind::InternalOnset, Confidence::High},
        {0x7fff000000000003ULL, 10.0, 10.0,
         HandleKind::ClipEnd, Confidence::Default}
    };
}

void expectEqSettingsMatch(const OpenTune::EqSettings& actual, const OpenTune::EqSettings& expected)
{
    expect(actual.active == expected.active, "round trip preserves the active flag");
    expect(std::abs(actual.lowCutFrequencyHz - expected.lowCutFrequencyHz) < 1.0e-3f,
           "round trip preserves lowCutFrequencyHz");
    expect(std::abs(actual.lowShelfFrequencyHz - expected.lowShelfFrequencyHz) < 1.0e-3f,
           "round trip preserves lowShelfFrequencyHz");
    expect(std::abs(actual.lowShelfGainDb - expected.lowShelfGainDb) < 1.0e-3f,
           "round trip preserves lowShelfGainDb");
    expect(std::abs(actual.peakFrequencyHz - expected.peakFrequencyHz) < 1.0e-3f,
           "round trip preserves peakFrequencyHz");
    expect(std::abs(actual.peakGainDb - expected.peakGainDb) < 1.0e-3f,
           "round trip preserves peakGainDb");
    expect(std::abs(actual.highShelfFrequencyHz - expected.highShelfFrequencyHz) < 1.0e-3f,
           "round trip preserves highShelfFrequencyHz");
    expect(std::abs(actual.highShelfGainDb - expected.highShelfGainDb) < 1.0e-3f,
           "round trip preserves highShelfGainDb");
    expect(std::abs(actual.highCutFrequencyHz - expected.highCutFrequencyHz) < 1.0e-3f,
           "round trip preserves highCutFrequencyHz");
}

void testSnapshotAndValidation()
{
    using namespace OpenTune;

    const auto handles = makeValidHandles();
    constexpr uint64_t revision = 0x7fff000000000011ULL;
    const auto snapshot = TimeGridSnapshot::makeFromHandles(handles, revision);

    expect(snapshot != nullptr, "makeFromHandles accepts a legal non-identity grid");
    if (snapshot == nullptr)
        return;

    const auto& restoredHandles = snapshot->handles();
    expect(snapshot->revision() == revision, "snapshot preserves its revision");
    expect(restoredHandles.front().isEndpoint(), "ClipStart endpoint is derived from HandleKind");
    expect(restoredHandles.back().isEndpoint(), "ClipEnd endpoint is derived from HandleKind");
    expect(!restoredHandles[1].isEndpoint(), "internal handle is not an endpoint");

    juce::String error;
    expect(TimeGridSnapshot::validate(handles, error) && error.isEmpty(),
           "validate accepts monotonic handles with the endpoint invariant");
    expect(sameSeconds(snapshot->totalDurationSeconds(), 10.0),
           "total duration equals the endpoint source duration");
    expect(sameSeconds(snapshot->tauForward(0.0), 0.0), "tauForward preserves ClipStart");
    expect(sameSeconds(snapshot->tauForward(5.0), 4.0), "tauForward preserves the internal anchor");
    expect(sameSeconds(snapshot->tauForward(10.0), 10.0), "tauForward preserves ClipEnd");
    expect(sameSeconds(snapshot->tauInverse(0.0), 0.0), "tauInverse preserves ClipStart");
    expect(sameSeconds(snapshot->tauInverse(4.0), 5.0), "tauInverse preserves the internal anchor");
    expect(sameSeconds(snapshot->tauInverse(10.0), 10.0), "tauInverse preserves ClipEnd");

    auto nonMonotonic = handles;
    nonMonotonic[1].source_seconds = 10.0;
    error.clear();
    expect(!TimeGridSnapshot::validate(nonMonotonic, error),
           "validate rejects non-monotonic source anchors");

    auto invalidDuration = handles;
    invalidDuration.back().output_seconds = 9.0;
    error.clear();
    expect(!TimeGridSnapshot::validate(invalidDuration, error),
           "validate rejects a broken endpoint-duration invariant");
}

void testProjectPersistenceRoundTrip()
{
    using namespace OpenTune;

    const auto handles = makeValidHandles();
    constexpr uint64_t revision = 0x7fff000000000021ULL;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 0x7fff000000000101ULL;
    content.contentKey.sourceWindowDiscriminator = 0x7fff000000000102ULL;
    content.sourceId = 0x7fff000000000103ULL;
    content.timeGrid.revision = revision;
    for (const auto& handle : handles) {
        ProjectContentEntry::TimeGridEntry::HandleEntry entry;
        entry.id = handle.id;
        entry.kind = static_cast<uint8_t>(handle.kind);
        entry.sourceSeconds = handle.source_seconds;
        entry.outputSeconds = handle.output_seconds;
        entry.confidence = static_cast<uint8_t>(handle.confidence);
        content.timeGrid.handles.push_back(entry);
    }

    ProjectSnapshot original;
    original.header.projectFormatVersion = ProjectPersistence::kCurrentProjectFormatVersion;
    original.header.appVersion = "runtime-test";
    original.contents.push_back(content);

    const ProjectPersistence persistence;
    const auto restoredResult = persistence.fromValueTree(persistence.toValueTree(original));
    expect(restoredResult.ok(), "ProjectPersistence restores its serialized TimeGrid");
    if (!restoredResult.ok())
        return;

    const auto& restored = restoredResult.value();
    expect(restored.contents.size() == 1, "round trip preserves one content entry");
    if (restored.contents.size() != 1)
        return;

    const auto& restoredGrid = restored.contents.front().timeGrid;
    expect(restoredGrid.revision == revision, "round trip preserves TimeGrid revision");
    expect(restoredGrid.handles.size() == handles.size(), "round trip preserves handle count");
    if (restoredGrid.handles.size() != handles.size())
        return;

    for (std::size_t index = 0; index < handles.size(); ++index) {
        const auto& source = handles[index];
        const auto& restoredHandle = restoredGrid.handles[index];
        expect(restoredHandle.id == source.id, "round trip preserves high uint64 handle id");
        expect(restoredHandle.kind == static_cast<uint8_t>(source.kind),
               "round trip preserves HandleKind");
        expect(restoredHandle.confidence == static_cast<uint8_t>(source.confidence),
               "round trip preserves Confidence");
        expect(sameSeconds(restoredHandle.sourceSeconds, source.source_seconds),
               "round trip preserves source seconds");
        expect(sameSeconds(restoredHandle.outputSeconds, source.output_seconds),
               "round trip preserves output seconds");
    }
}

void testLegacyVolumeEnvelopeMigration()
{
    using namespace OpenTune;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 42;
    Note note;
    note.startTime = 1.5;
    note.endTime = 2.5;
    note.outputGainDb = 4.0f;
    content.notes.push_back(note);

    ProjectSnapshot project;
    project.contents.push_back(content);
    ProjectPersistence persistence;
    auto tree = persistence.toValueTree(project);
    tree.setProperty(ProjectPersistence::kProjectFormatVersionAttr, 3, nullptr);

    auto contentTree = tree.getChildWithName("Contents").getChild(0);
    juce::ValueTree legacy("SibilantGainEnvelope");
    legacy.setProperty("pointCount", 3, nullptr);
    for (int i = 1; i <= 3; ++i) {
        juce::ValueTree point("Point");
        point.setProperty("time", static_cast<double>(i), nullptr);
        point.setProperty("gainDb", static_cast<float>(i), nullptr);
        legacy.addChild(point, -1, nullptr);
    }
    contentTree.addChild(legacy, -1, nullptr);

    const auto result = persistence.fromValueTree(tree);
    expect(result.ok(), "v3 project migrates to the unified VolumeEnvelope");
    if (!result.ok())
        return;
    const auto& migrated = result.value().contents.front().volumeEnvelope;
    expect(std::abs(migrated.evalAt(2.0) - 6.0f) < 1.0e-4f,
           "v3 migration preserves the middle sibilant event and adds note gain");
    expect(result.value().header.projectFormatVersion == 5,
           "loaded v3 project is promoted to project format v5");
}

void testNoteEqPersistenceRoundTrip()
{
    using namespace OpenTune;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 0x7fff000000000401ULL;
    content.contentKey.sourceWindowDiscriminator = 0x7fff000000000402ULL;
    content.sourceId = 0x7fff000000000403ULL;

    Note activeNote;
    activeNote.startTime = 0.5;
    activeNote.endTime = 1.5;
    EqSettings activeEq;
    activeEq.active = true;
    activeEq.lowCutFrequencyHz = 90.0f;
    activeEq.lowShelfFrequencyHz = 480.0f;
    activeEq.lowShelfGainDb = -3.5f;
    activeEq.peakFrequencyHz = 2500.0f;
    activeEq.peakGainDb = 2.0f;
    activeEq.highShelfFrequencyHz = 8200.0f;
    activeEq.highShelfGainDb = 1.5f;
    activeEq.highCutFrequencyHz = 13000.0f;
    activeNote.eq = activeEq;
    content.notes.push_back(activeNote);

    Note bypassedNote;   // active=false：保留参数但全局旁通
    bypassedNote.startTime = 2.0;
    bypassedNote.endTime = 3.0;
    EqSettings bypassedEq;
    bypassedEq.active = false;
    bypassedEq.lowCutFrequencyHz = 120.0f;
    bypassedEq.lowShelfFrequencyHz = 700.0f;
    bypassedEq.lowShelfGainDb = -6.0f;
    bypassedEq.peakFrequencyHz = 1500.0f;
    bypassedEq.peakGainDb = -2.5f;
    bypassedEq.highShelfFrequencyHz = 9000.0f;
    bypassedEq.highShelfGainDb = 0.5f;
    bypassedEq.highCutFrequencyHz = 16000.0f;
    bypassedNote.eq = bypassedEq;
    content.notes.push_back(bypassedNote);

    ProjectSnapshot project;
    project.header.projectFormatVersion = ProjectPersistence::kCurrentProjectFormatVersion;
    project.header.appVersion = "runtime-test";
    project.contents.push_back(content);

    const ProjectPersistence persistence;
    const auto result = persistence.fromValueTree(persistence.toValueTree(project));
    expect(result.ok(), "ProjectPersistence restores notes with EQ settings");
    if (!result.ok())
        return;

    const auto& restoredNotes = result.value().contents.front().notes;
    expect(restoredNotes.size() == 2, "round trip preserves both EQ notes");
    if (restoredNotes.size() != 2)
        return;

    const auto& restoredActive = restoredNotes[0].eq;
    expect(restoredActive.has_value(), "round trip preserves the active EQ value");
    if (!restoredActive.has_value())
        return;
    expectEqSettingsMatch(*restoredActive, activeEq);

    const auto& restoredBypassed = restoredNotes[1].eq;
    expect(restoredBypassed.has_value(),
           "active=false EQ keeps its parameters through round trip");
    if (!restoredBypassed.has_value())
        return;
    expectEqSettingsMatch(*restoredBypassed, bypassedEq);
}

void testLegacyProjectWithoutEqRestoresNullopt()
{
    using namespace OpenTune;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 0x7fff000000000411ULL;

    Note note;
    note.startTime = 0.0;
    note.endTime = 1.0;
    content.notes.push_back(note);

    ProjectSnapshot project;
    project.contents.push_back(content);
    ProjectPersistence persistence;
    auto tree = persistence.toValueTree(project);
    tree.setProperty(ProjectPersistence::kProjectFormatVersionAttr, 4, nullptr);

    const auto result = persistence.fromValueTree(tree);
    expect(result.ok(), "v4 project without EQ loads");
    if (!result.ok())
        return;

    expect(!result.value().contents.front().notes.front().eq.has_value(),
           "v4 project without EQ restores nullopt");
}

void testBandDraftEqFormatDroppedAsNullopt()
{
    using namespace OpenTune;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 0x7fff000000000421ULL;

    Note note;
    note.startTime = 0.0;
    note.endTime = 1.0;
    content.notes.push_back(note);

    ProjectSnapshot project;
    project.contents.push_back(content);
    ProjectPersistence persistence;
    auto tree = persistence.toValueTree(project);
    tree.setProperty(ProjectPersistence::kProjectFormatVersionAttr, 5, nullptr);

    // 历史错误 Band 草稿：EqSettings 仅含 active + BandN 子节点（gainDb/frequency），
    // 缺 9 字段 scalar schema，读端必须直接丢弃为 nullopt
    auto contentTree = tree.getChildWithName("Contents").getChild(0);
    auto noteTree = contentTree.getChildWithName("Notes").getChild(0);
    juce::ValueTree eqTree("EqSettings");
    eqTree.setProperty("active", 1, nullptr);
    for (int b = 0; b < 5; ++b) {
        juce::ValueTree band("Band" + juce::String(b));
        band.setProperty("gainDb", 0.0f, nullptr);
        band.setProperty("frequency", 1000.0f, nullptr);
        eqTree.addChild(band, -1, nullptr);
    }
    noteTree.addChild(eqTree, -1, nullptr);

    const auto result = persistence.fromValueTree(tree);
    expect(result.ok(), "project with Band draft EqSettings still loads");
    if (!result.ok())
        return;

    expect(!result.value().contents.front().notes.front().eq.has_value(),
           "Band draft EqSettings without the 9-field scalar schema is dropped to nullopt");
}

void testV4ProjectWithScalarEqRestoresNullopt()
{
    using namespace OpenTune;

    ProjectContentEntry content;
    content.contentKey.domainKind = DomainKind::StandaloneClip;
    content.contentKey.objectId = 0x7fff000000000431ULL;

    Note note;
    note.startTime = 0.0;
    note.endTime = 1.0;
    EqSettings eq;
    eq.active = true;
    eq.lowCutFrequencyHz = 90.0f;
    eq.lowShelfFrequencyHz = 480.0f;
    eq.lowShelfGainDb = -3.5f;
    eq.peakFrequencyHz = 2500.0f;
    eq.peakGainDb = 2.0f;
    eq.highShelfFrequencyHz = 8200.0f;
    eq.highShelfGainDb = 1.5f;
    eq.highCutFrequencyHz = 13000.0f;
    note.eq = eq;
    content.notes.push_back(note);

    ProjectSnapshot project;
    project.contents.push_back(content);
    ProjectPersistence persistence;
    // v5 写出完整 scalar EQ 后把根版本改 4：模拟"v4 文件却携带完整 EqSettings"的真实场景
    auto tree = persistence.toValueTree(project);
    tree.setProperty(ProjectPersistence::kProjectFormatVersionAttr, 4, nullptr);

    const auto result = persistence.fromValueTree(tree);
    expect(result.ok(), "v4 project carrying complete scalar EqSettings still loads");
    if (!result.ok())
        return;

    expect(!result.value().contents.front().notes.front().eq.has_value(),
           "v4 project carrying complete scalar EqSettings restores nullopt");
}

} // namespace

int main()
{
    testSnapshotAndValidation();
    testProjectPersistenceRoundTrip();
    testLegacyVolumeEnvelopeMigration();
    testNoteEqPersistenceRoundTrip();
    testLegacyProjectWithoutEqRestoresNullopt();
    testBandDraftEqFormatDroppedAsNullopt();
    testV4ProjectWithScalarEqRestoresNullopt();

    if (failures != 0) {
        std::cerr << failures << " TimeGrid runtime test(s) failed\n";
        return 1;
    }

    std::cout << "TimeGrid runtime tests passed\n";
    return 0;
}
