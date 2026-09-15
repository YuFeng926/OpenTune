#include "../Source/Utils/PianoRollVisualPreferences.h"
#include "../Source/Utils/NoteGeneratorTypes.h"
#include <cstdio>

using namespace OpenTune;
static int failures = 0;
static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static void testPhysicalKeyMap()
{
    const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    for (int pc = 0; pc < 12; ++pc)
        check(isPhysicalBlackKey(60 + pc) == black[pc], "physical black/white pitch-class map");
    check(isPhysicalBlackKey(-11), "negative MIDI pitch classes are normalized");
}

static void testRoles()
{
    const auto major = buildPitchClassMask(ScaleMode::Major, 2);
    for (int pc = 0; pc < 12; ++pc) {
        check(classifyPitchRow(true, false, 60 + pc, !major[pc])
              == (isPhysicalBlackKey(60 + pc) ? PitchRowVisualRole::BlackKey : PitchRowVisualRole::WhiteKey),
              "keyboard on + assist off uses physical key map");
        check(classifyPitchRow(false, false, 60 + pc, major[pc]) == PitchRowVisualRole::WhiteKey,
              "keyboard off + assist off keeps every lane bright");
        check(classifyPitchRow(true, true, 60 + pc, major[pc])
              == (major[pc] ? PitchRowVisualRole::WhiteKey : PitchRowVisualRole::BlackKey),
              "keyboard on + assist follows inScale");
        check(classifyPitchRow(false, true, 60 + pc, major[pc])
              == (major[pc] ? PitchRowVisualRole::WhiteKey : PitchRowVisualRole::BlackKey),
              "keyboard off + assist follows inScale");
    }

    const auto chromatic = buildPitchClassMask(ScaleMode::Chromatic, 0);
    for (int pc = 0; pc < 12; ++pc)
        check(classifyPitchRow(true, true, 60 + pc, chromatic[pc]) == PitchRowVisualRole::WhiteKey,
              "chromatic scale is all bright");
}

static void testVisibility()
{
    check(shouldShowPianoKeys(true, false), "keyboard visible outside TimeTool");
    check(!shouldShowPianoKeys(true, true), "TimeTool hides keyboard");
    check(!shouldShowPianoKeys(false, false), "disabled keyboard is hidden");
    check(!shouldShowPianoKeys(false, true), "disabled keyboard stays hidden in TimeTool");
}

static void testEqualSpacing()
{
    constexpr float height = 25.0f;
    check(laneBackgroundYOffset(PianoGridStyle::PianoLanes, height) == 0.0f,
          "PianoLanes standard pitch position");
    constexpr float laneTop = 100.0f;
    constexpr float standardNotePosition = laneTop + height * 0.5f;
    check(laneTop + laneBackgroundYOffset(PianoGridStyle::EqualSpacing, height)
              == standardNotePosition,
          "EqualSpacing offset places standard pitch on the grid line");
}

int main()
{
    testPhysicalKeyMap();
    testRoles();
    testVisibility();
    testEqualSpacing();
    if (failures == 0) { std::puts("All PitchLaneVisualPolicy tests passed."); return 0; }
    std::fprintf(stderr, "%d test(s) FAILED.\n", failures);
    return 1;
}
