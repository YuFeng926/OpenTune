#pragma once

/**
 * LegacyNoteGenerator - DSP-based note generator.
 *
 * Generates a note sequence from an F0 curve via:
 * - Pitch-transition thresholding (cents) for segmentation
 * - Unvoiced-gap bridging
 * - Optional scale snapping
 */

#include <vector>

#include "Note.h"
#include "NoteGeneratorTypes.h"

namespace OpenTune {

class LegacyNoteGenerator {
public:
    static std::vector<Note> generate(
        const float*               f0,
        int                        f0Count,
        const float*               energy,
        int                        startFrame,
        int                        endFrameExclusive,
        int                        hopSize,
        double                     f0SampleRate,
        const NoteGeneratorParams& params = {});

    static bool validate(const std::vector<Note>& notes);

private:
    static float representativePitch(
        const float* pitches,
        const float* energyWeights,
        int          count,
        float        hopSizeTime);

    static float quantisePitch(float hz);

    static void commitNote(
        std::vector<Note>&         out,
        Note&                      current,
        std::vector<float>&        pitches,
        std::vector<float>&        energyBuf,
        float                      hopSizeTime,
        double                     endTime,
        double                     minNoteDuration,
        double                     tailExtendDuration);
};

} // namespace OpenTune
