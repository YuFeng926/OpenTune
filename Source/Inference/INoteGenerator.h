#pragma once

/**
 * INoteGenerator - polymorphic note generator interface.
 *
 * GameNoteGenerator (Source/Inference/GameNoteGenerator.{h,cpp}) implements
 * this — GAME-small ONNX. It consumes `audio` + `sampleRate` from
 * NoteGeneratorInput. See research/p1_note_transcription_spike/reports.md
 * for the design rationale.
 */

#include <atomic>
#include <memory>
#include <vector>

#include "../Utils/Note.h"

namespace OpenTune {

struct NoteGeneratorInput {
    std::vector<float> audio;
    double             sampleRate = 44100.0;
};

class INoteGenerator {
public:
    virtual ~INoteGenerator() = default;
    virtual std::vector<Note> generate(const NoteGeneratorInput& input) = 0;

    /// Abort any in-flight ORT Run so the calling thread can be joined without
    /// waiting for unbounded inference time. Default: no-op.
    virtual void terminateRun() {}

    /// Bind a persistent abort flag (shared with the owner). The flag is
    /// checked before every ORT Run; once set, generate() fails fast.
    virtual void setAbortFlag(std::shared_ptr<std::atomic<bool>> flag) {}
};

} // namespace OpenTune
