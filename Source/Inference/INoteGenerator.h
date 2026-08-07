#pragma once

/**
 * INoteGenerator - polymorphic note generator interface.
 *
 * GameNoteGenerator (Source/Inference/GameNoteGenerator.{h,cpp}) implements
 * this — GAME-small ONNX. It consumes `audio` + `sampleRate` from
 * NoteGeneratorInput. See research/p1_note_transcription_spike/reports.md
 * for the design rationale.
 */

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
};

} // namespace OpenTune
