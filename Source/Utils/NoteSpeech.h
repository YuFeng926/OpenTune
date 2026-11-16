#pragma once

// Spoken note pitch text for screen readers: "A4 +25 ct", "C sharp 4".
// Standard library only, so the format can be tested without JUCE.

#include <cmath>
#include <string>

namespace OpenTune::NoteSpeech {

// Nearest note and the deviation from it in cents (-50 to +49).
struct PitchParts
{
    int midi = 0;
    int cents = 0;
};

inline PitchParts splitMidi(float midi)
{
    // Round to whole cents first, then split; otherwise 68.996 would give "A4 -0 ct".
    const long totalCents = std::lround(static_cast<double>(midi) * 100.0);
    const long nearest = (totalCents + 50) / 100;
    return { static_cast<int>(nearest), static_cast<int>(totalCents - nearest * 100) };
}

// "A4" or "C sharp 4". The # sign is avoided: speech synthesizers read it differently
// ("hash", "number"), so the sharp is passed as the word sharpWord.
inline std::string noteName(int midi, const std::string& sharpWord)
{
    static const char* const letters[12] = { "C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B" };
    static const bool sharp[12] = { false, true, false, true, false, false, true, false, true, false, true, false };

    const int pitchClass = ((midi % 12) + 12) % 12;
    // Floor division for negative note numbers so the octave stays correct.
    const int octave = (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1;

    std::string text = letters[pitchClass];
    if (sharp[pitchClass])
        text += " " + sharpWord + " ";
    text += std::to_string(octave);
    return text;
}

// "A4 +25 ct"; with zero deviation just "A4".
inline std::string describeMidi(float midi, const std::string& sharpWord)
{
    const auto parts = splitMidi(midi);
    std::string text = noteName(parts.midi, sharpWord);
    if (parts.cents != 0)
        text += (parts.cents > 0 ? " +" : " ") + std::to_string(parts.cents) + " ct";
    return text;
}

} // namespace OpenTune::NoteSpeech
