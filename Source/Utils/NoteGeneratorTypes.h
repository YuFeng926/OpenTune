#pragma once

/**
 * Shared parameter types used by the note-generation pipeline:
 * LegacyNoteGenerator and the ScaleSnap consumers (AutoTune, Pitch Tool).
 *
 * Method bodies (e.g. ScaleSnapConfig::semitones, snapMidi,
 * quantizeMidiToActiveScale, applyToNotes) live in
 * Source/Utils/LegacyNoteGenerator.cpp.
 *
 * Design note: ScaleSnap is intentionally NOT a field of NoteGeneratorParams.
 * Note generation produces chromatic-quantized notes; scale snapping is a
 * separate AutoTune-correction concern, applied post-generation by
 * `ScaleSnapConfig::applyToNotes`. This keeps the two responsibilities
 * decoupled (per `import-note-generation` spec).
 */

#include <cstdint>
#include <optional>
#include <vector>

#include "Note.h"

namespace OpenTune {

using RootNote = int;

enum class ScaleMode {
    Chromatic,
    Major,
    Minor,
    HarmonicMinor,
    Dorian,
    Mixolydian,
    PentatonicMajor,
    PentatonicMinor
};

struct ScaleSnapConfig {
    RootNote  root  = 0;
    ScaleMode mode  = ScaleMode::Chromatic;

    static const int* semitones(ScaleMode mode, int& outCount) noexcept;
    float snapMidi(float midiNote) const noexcept;

    /// The single scale-projection entry point shared by AutoTune's
    /// `applyToNotes` and the Pitch Tool (note drag and double-click).
    ///
    /// Semantics:
    ///  - Chromatic: projects to the nearest semitone (`std::round`).
    ///    Chromatic no longer doubles as "disable snapping".
    ///  - Other modes: projects to the nearest tone of the current root/mode.
    ///
    /// `applyToNotes` (AUTO) and the Pitch Tool both call this method.
    float quantizeMidiToActiveScale(float midiNote) const noexcept;

    /// Applies scale snap in-place to a vector of notes. For each note, takes
    /// `originalPitch` (continuous Hz from the segmenter) — falling back to
    /// `pitch` if `originalPitch` is unset — converts to MIDI, runs it through
    /// `quantizeMidiToActiveScale`, and writes the resulting Hz back into
    /// `note.pitch`. `originalPitch` is left unchanged.
    ///
    /// This is the post-generation step that lets AutoTune (and only AutoTune)
    /// re-quantize generated notes to a musical scale; it is mathematically
    /// equivalent to the old in-segmenter `quantisePitch(hz, scaleSnap)`
    /// because both `snapMidi` and rounding project onto the same integer
    /// MIDI lattice, so order does not matter.
    ///
    /// Idempotent in Chromatic mode: generated notes are already
    /// half-tone-quantized, so `std::round` changes nothing.
    void applyToNotes(std::vector<Note>& notes) const;
};

struct NoteSegmentationPolicy {
    float transitionThresholdCents = 80.0f;
    float gapBridgeMs              = 10.0f;
    float minDurationMs            = 100.0f;
    float tailExtendMs             = 15.0f;
};

struct NoteGeneratorParams {
    NoteSegmentationPolicy policy;
    // NOTE: retuneSpeed / vibratoDepth / vibratoRate are intentionally still
    // here — they are baked into the generated `Note` struct and consumed by
    // downstream save/load + AutoTune correction paths. Decoupling those is a
    // separate cleanup; this change only removes the ScaleSnap conflation.
    float retuneSpeed  = -1.0f;
    float vibratoDepth = -1.0f;
    float vibratoRate  = -1.0f;
    float pitchDriftScale = 1.0f;
};

} // namespace OpenTune
