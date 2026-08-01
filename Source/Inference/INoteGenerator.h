#pragma once

/**
 * INoteGenerator - polymorphic note generator interface.
 *
 * Two backends implement this:
 *   - LegacyNoteGenerator (Source/Utils/LegacyNoteGenerator.{h,cpp}) — DSP on F0.
 *   - GameNoteGenerator   (Source/Inference/GameNoteGenerator.{h,cpp}) — GAME-small ONNX.
 *
 * Backends consume different fields of NoteGeneratorInput. Legacy reads the
 * frame-domain fields (f0/energy/hopSize/f0SampleRate); GAME reads audio + sampleRate
 * and ignores the rest. See research/p1_note_transcription_spike/reports.md
 * for the design rationale.
 */

#include <atomic>
#include <memory>
#include <vector>

#include "../Utils/Note.h"
#include "../Utils/NoteGeneratorTypes.h"

namespace OpenTune {

struct NoteGeneratorInput {
    // GAME consumes these
    std::vector<float> audio;
    double             sampleRate = 44100.0;

    // Legacy consumes these (frame domain)
    std::vector<float> f0;
    std::vector<float> energy;
    int                hopSize           = 0;
    double             f0SampleRate      = 0.0;
    int                startFrame        = 0;
    int                endFrameExclusive = 0;   // 0 → use f0.size()

    NoteGeneratorParams params;
};

class INoteGenerator {
public:
    virtual ~INoteGenerator() = default;
    virtual std::vector<Note> generate(const NoteGeneratorInput& input) = 0;

    /// Abort any in-flight ORT Run so the calling thread can be joined without
    /// waiting for unbounded inference time. Default: no-op (DSP backends).
    virtual void terminateRun() {}

    /// Bind a persistent abort flag (shared with the owner). The flag is
    /// checked before every ORT Run; once set, generate() fails fast.
    virtual void setAbortFlag(std::shared_ptr<std::atomic<bool>> flag) {}
};

} // namespace OpenTune
