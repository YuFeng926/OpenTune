#pragma once

/**
 * LegacyNoteGenerator - DSP-based note generator (existing implementation,
 * formerly named `NoteGenerator`).
 *
 * Generates a note sequence from an F0 curve via:
 * - Pitch-transition thresholding (cents) for segmentation
 * - Unvoiced-gap bridging
 * - Optional scale snapping
 *
 * This is the byte-equivalent successor to the prior `NoteGenerator` class.
 * It is kept as a hidden fallback path; new imports default to GAME-small
 * via `GameNoteGenerator` (see Source/Inference/GameNoteGenerator.h).
 *
 * Implements `INoteGenerator` (`generate(const NoteGeneratorInput&)`) so the
 * import flow can dispatch polymorphically; the static API is preserved for
 * existing call sites (PianoRollCorrectionWorker, Standalone/PluginEditor).
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

    // INoteGenerator override — used by the import flow via std::unique_ptr<INoteGenerator>.
    // Reads input.f0/energy/hopSize/f0SampleRate/hostSampleRate/start/endFrame/params,
    // ignores input.audio and input.sampleRate.
    std::vector<Note> generate(const NoteGeneratorInput& input) override;

    // Static API — preserved for legacy callers (PianoRollCorrectionWorker, etc.)
    static std::vector<Note> generate(
        const float*               f0,
        int                        f0Count,
        const float*               energy,
        int                        startFrame,
        int                        endFrameExclusive,
        int                        hopSize,
        double                     f0SampleRate,
        double                     hostSampleRate,
        const NoteGeneratorParams& params = {});

    static std::vector<Note> generate(
        const std::vector<float>&  f0,
        const std::vector<float>&  energy,
        int                        hopSize,
        double                     f0SampleRate,
        double                     hostSampleRate,
        const NoteGeneratorParams& params = {});

    static bool validate(const std::vector<Note>& notes);

private:
    static float representativePitch(
        const float* pitches,
        const float* energyWeights,
        int          count,
        float        hopSizeTime);

    // Always rounds to the nearest chromatic semitone. ScaleSnap is no longer
    // applied here — see ScaleSnapConfig::applyToNotes for the post-generation
    // hook that AutoTune uses.
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
