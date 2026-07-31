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

} // namespace

int main()
{
    testSnapshotAndValidation();
    testProjectPersistenceRoundTrip();

    if (failures != 0) {
        std::cerr << failures << " TimeGrid runtime test(s) failed\n";
        return 1;
    }

    std::cout << "TimeGrid runtime tests passed\n";
    return 0;
}
