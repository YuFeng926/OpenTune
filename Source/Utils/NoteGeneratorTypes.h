#pragma once

/**
 * Shared parameter types used by the note-generation pipeline:
 * LegacyNoteGenerator and the ScaleSnap consumers (AutoTune, Pitch Tool).
 *
 * ScaleSnapConfig::semitones is inlined here (pure standard library) so that
 * both LegacyNoteGenerator.cpp and ScaleUiMapping.h can call it without
 * a separate translation-unit definition. buildPitchClassMask is a pure
 * helper (no JUCE) used by ScaleAssist rendering and tests.
 *
 * snapMidi, quantizeMidiToActiveScale, and applyToNotes bodies remain in
 * Source/Utils/LegacyNoteGenerator.cpp.
 *
 * Design note: ScaleSnap is intentionally NOT a field of NoteGeneratorParams.
 * Note generation produces chromatic-quantized notes; scale snapping is a
 * separate AutoTune-correction concern, applied post-generation by
 * `ScaleSnapConfig::applyToNotes`. This keeps the two responsibilities
 * decoupled (per `import-note-generation` spec).
 */

#include <array>
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

    static const int* semitones(ScaleMode mode, int& outCount) noexcept {
        static constexpr int kMajor[]            = {0, 2, 4, 5, 7, 9, 11};
        static constexpr int kMinor[]            = {0, 2, 3, 5, 7, 8, 10};
        static constexpr int kChromatic[]        = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        static constexpr int kHarmonicMinor[]    = {0, 2, 3, 5, 7, 8, 11};
        static constexpr int kDorian[]           = {0, 2, 3, 5, 7, 9, 10};
        static constexpr int kMixolydian[]       = {0, 2, 4, 5, 7, 9, 10};
        static constexpr int kPentatonicMajor[]  = {0, 2, 4, 7, 9};
        static constexpr int kPentatonicMinor[]  = {0, 3, 5, 7, 10};
        switch (mode) {
            case ScaleMode::Major:           outCount = 7;  return kMajor;
            case ScaleMode::Minor:           outCount = 7;  return kMinor;
            case ScaleMode::HarmonicMinor:   outCount = 7;  return kHarmonicMinor;
            case ScaleMode::Dorian:          outCount = 7;  return kDorian;
            case ScaleMode::Mixolydian:      outCount = 7;  return kMixolydian;
            case ScaleMode::PentatonicMajor: outCount = 5;  return kPentatonicMajor;
            case ScaleMode::PentatonicMinor: outCount = 5;  return kPentatonicMinor;
            case ScaleMode::Chromatic:
            default:                         outCount = 12; return kChromatic;
        }
    }

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
    // 与 UI 旋钮默认值一致（PitchControlConfig::kDefaultNoteSplitCents），
    // 三形态（Standalone/VST3/ARA）共享同一默认策略，不依赖 editor 初始化调用。
    float transitionThresholdCents = PitchControlConfig::kDefaultNoteSplitCents;
    float gapBridgeMs              = 10.0f;
    float minDurationMs            = 20.0f;
    float tailExtendMs             = 15.0f;
};

struct NoteGeneratorParams {
    NoteSegmentationPolicy policy;
    float retuneSpeed  = PitchControlConfig::kDefaultRetuneSpeedNormalized;
    float vibratoDepth = PitchControlConfig::kDefaultVibratoDepth;
    float vibratoRate  = PitchControlConfig::kDefaultVibratoRateHz;
    float pitchDriftScale = 1.0f;
};

/**
 * 构建 12 pitch-class mask：true 表示该 pitch class 在当前 ScaleMode + root内。
 * Chromatic 返回全部 true。
 * 纯标准库实现（无 JUCE），供 ScaleAssist 渲染、TimelineLayerComposer 和测试共享。
 */
inline std::array<bool, 12> buildPitchClassMask(ScaleMode mode, int rootNote) noexcept {
    std::array<bool, 12> result{};
    if (mode == ScaleMode::Chromatic) {
        result.fill(true);
        return result;
    }
    result.fill(false);
    const int rootPc = ((rootNote % 12) + 12) % 12;
    int count = 0;
    const int* intervals = ScaleSnapConfig::semitones(mode, count);
    for (int i = 0; i < count; ++i)
        result[static_cast<std::size_t>((rootPc + intervals[i]) % 12)] = true;
    return result;
}

} // namespace OpenTune
