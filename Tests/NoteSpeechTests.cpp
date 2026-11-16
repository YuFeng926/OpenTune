// Tests for the spoken note pitch text (Utils/NoteSpeech.h). Standard library only.
#include "Utils/NoteSpeech.h"

#include <cstdio>

namespace {

int failures = 0;

void expectText(float midi, const char* expected)
{
    const auto text = OpenTune::NoteSpeech::describeMidi(midi, "sharp");
    if (text != expected) {
        std::printf("FAIL: midi %.4f -> \"%s\", expected \"%s\"\n", midi, text.c_str(), expected);
        ++failures;
    }
}

} // namespace

int main()
{
    expectText(69.0f, "A4");
    expectText(69.25f, "A4 +25 ct");
    expectText(68.75f, "A4 -25 ct");
    expectText(68.996f, "A4");          // rounding to cents must not give "-0 ct"
    expectText(60.0f, "C4");
    expectText(61.0f, "C sharp 4");
    expectText(71.5f, "C5 -50 ct");     // exactly halfway rounds up to the higher note
    expectText(71.49f, "B4 +49 ct");
    expectText(59.0f, "B3");
    expectText(21.0f, "A0");

    if (failures == 0)
        std::printf("NoteSpeech tests passed\n");
    return failures == 0 ? 0 : 1;
}
