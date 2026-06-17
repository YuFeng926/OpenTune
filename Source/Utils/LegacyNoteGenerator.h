#pragma once

/**
 * LegacyNoteGenerator - DSP-based note generator.
 *
 * Generates a note sequence from an F0 curve via:
 * - Pitch-transition thresholding (cents) for segmentation
 * - Unvoiced-gap bridging
 * - Optional scale snapping
 *
 * Implements INoteGenerator so frame-domain note generation can share the same
 * dispatch shape as GAME without mixing source-audio sample rate into F0 time.
 */

#include <vector>
#include <cstdint>
#include <optional>

#include "Note.h"
#include "NoteGeneratorTypes.h"
#include "../Inference/INoteGenerator.h"

namespace OpenTune {

class LegacyNoteGenerator : public INoteGenerator {
public:
    LegacyNoteGenerator() = default;

    std::vector<Note> generate(const NoteGeneratorInput& input) override;

    static std::vector<Note> generate(
        const float*               f0,
        int                        f0Count,
        const float*               energy,
        int                        startFrame,
        int                        endFrameExclusive,
        int                        hopSize,
        double                     f0SampleRate,
        const NoteGeneratorParams& params = {});

    static std::vector<Note> generate(
        const std::vector<float>&  f0,
        const std::vector<float>&  energy,
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
        double                     tailExtendDuration,
        const NoteGeneratorParams& params);
};

} // namespace OpenTune
