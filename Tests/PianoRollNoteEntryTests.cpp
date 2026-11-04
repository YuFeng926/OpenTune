#include "Standalone/UI/PianoRollNoteEntry.h"

#include <cstdio>

using OpenTune::CenterableNote;
using OpenTune::noteCenterTimeNearest;
using OpenTune::noteIndexNearestTime;

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void testEmptyClip()
{
    check(noteIndexNearestTime(nullptr, 0, 1.0) == -1, "an empty clip has no note to center");
}

void testSkipsUnusableNotes()
{
    const CenterableNote notes[] = {
        { 0.2, 0.2, 440.0f },
        { 0.1, 0.4, 0.0f },
        { 1.5, 2.0, 220.0f },
    };
    check(noteIndexNearestTime(notes, 3, 0.0) == 2,
          "zero-length and unpitched notes are not candidates");
}

void testPlayheadInsideNote()
{
    const CenterableNote notes[] = {
        { 1.0, 1.4, 246.0f },
        { 3.0, 3.8, 330.0f },
        { 5.0, 5.5, 200.0f },
    };
    check(noteIndexNearestTime(notes, 3, 3.2) == 1, "a playhead inside a note selects that note");
    check(noteCenterTimeNearest(3.2, notes[1]) == 3.2, "an interior playhead is the centered time");
}

void testPlayheadBeforeAllNotesUsesTheFirst()
{
    const CenterableNote notes[] = {
        { 1.1, 1.4, 246.0f },
        { 2.0, 2.5, 200.0f },
    };
    check(noteIndexNearestTime(notes, 2, 0.0) == 0, "a playhead before the clip uses the nearest, earliest note");
    check(noteCenterTimeNearest(0.0, notes[0]) == 1.1, "the centered time is that note's start");
}

void testPlayheadBetweenNotesUsesTheCloserEdge()
{
    const CenterableNote notes[] = {
        { 1.0, 1.2, 246.0f },
        { 4.0, 4.5, 330.0f },
    };
    check(noteIndexNearestTime(notes, 2, 2.0) == 0, "closer to the first note's end than the next start");
    check(noteCenterTimeNearest(2.0, notes[0]) == 1.2, "the centered time is the near edge");
    check(noteIndexNearestTime(notes, 2, 3.0) == 1, "closer to the next note's start");
}

void testEqualDistanceKeepsTheEarlierNote()
{
    const CenterableNote notes[] = {
        { 1.0, 2.0, 246.0f },
        { 4.0, 5.0, 330.0f },
    };
    check(noteIndexNearestTime(notes, 2, 3.0) == 0, "a midpoint tie keeps the earlier note");
}

} // namespace

int main()
{
    testEmptyClip();
    testSkipsUnusableNotes();
    testPlayheadInsideNote();
    testPlayheadBeforeAllNotesUsesTheFirst();
    testPlayheadBetweenNotesUsesTheCloserEdge();
    testEqualDistanceKeepsTheEarlierNote();

    if (failures != 0) {
        std::fprintf(stderr, "%d piano roll note entry checks failed\n", failures);
        return 1;
    }

    std::printf("PianoRollNoteEntryTests passed\n");
    return 0;
}
