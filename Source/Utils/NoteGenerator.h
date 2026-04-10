#pragma once

/**
 * NoteGenerator - 音符生成器
 * 
 * 从 F0（基频）曲线生成音符序列，支持：
 * - 自动分段（基于音高变化阈值）
 * - 静默段桥接
 * - 半音量化（chromatic semitone quantization）
 */

#include <vector>
#include <cstdint>

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
};

struct NoteSegmentationPolicy {
    float transitionThresholdCents = 80.0f;
    float gapBridgeMs              = 10.0f;
    float minDurationMs            = 100.0f;
    float tailExtendMs             = 15.0f;
};

struct NoteGeneratorParams {
    NoteSegmentationPolicy policy;
};

class NoteGenerator {
public:
    NoteGenerator() = delete;

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
