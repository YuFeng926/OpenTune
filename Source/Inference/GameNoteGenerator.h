#pragma once

/**
 * GameNoteGenerator — ONNX-backed note transcriber using the GAME-small model.
 *
 * Wraps the OpenVPI GAME (Generative Adaptive MIDI Extractor) D3PM pipeline
 * (5 ONNX files: encoder / segmenter / estimator / bd2dur / dur2bd; we only
 * use 4 of them for transcription-from-scratch — dur2bd is for the alternate
 * path where the caller already has durations). Long clips are chunked at
 * silence midpoints via the existing
 * `RenderChunkPlanner::buildChunkBoundariesFromSilentGaps` infrastructure;
 * per-chunk notes are stitched together by `mergeChunkNotes`.
 *
 * Provider: CPU only.  ORT 1.21's CoreML EP silently swallows kernel errors
 * on GAME's softmax_nd graph (process exits 0 mid-inference, no crash report).
 * See research/p1_note_transcription_spike/reports.md.
 *
 * Implements `INoteGenerator`. Reads only `input.audio` and `input.sampleRate`
 * from `NoteGeneratorInput`.
 *
 * 生命周期合同（进程级共享）：本类是进程级共享单例（ProcessF0Runtime 持有，
 * 进程寿命，owner 实例卸载不销毁）。实例析构从不 join/终止本生成器的推理：
 * terminateRun / setAbortFlag 已被删除——owner 关闭后，进行中的 GAME 推理允许
 * 在后台完成，调用方通过 completion gate 丢弃结果。
 *
 * Design rationale: see openspec/changes/add-game-note-generator/design.md (D1–D7).
 */

#include "INoteGenerator.h"

#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenTune {

class GameNoteGenerator : public INoteGenerator {
public:
    /// @param modelDir  Directory containing encoder.onnx / segmenter.onnx /
    ///                  estimator.onnx / bd2dur.onnx / config.json.
    /// @param env       Shared ORT environment (typically owned by ModelFactory).
    /// Throws std::runtime_error if any model file is missing or fails to load.
    GameNoteGenerator(const std::string& modelDir, Ort::Env& env);
    ~GameNoteGenerator() override;

    GameNoteGenerator(const GameNoteGenerator&)            = delete;
    GameNoteGenerator& operator=(const GameNoteGenerator&) = delete;

    /// INoteGenerator override.
    std::vector<Note> generate(const NoteGeneratorInput& input) override;

    // ---- Hyperparameters (defaults match the spike's GAME-small settings) ----

    void   setSegThreshold(float t)      { segThreshold_ = t; }
    void   setSegRadius(int r)           { segRadius_ = r; }
    void   setEstThreshold(float t)      { estThreshold_ = t; }
    void   setD3pmSteps(int steps)       { d3pmSteps_ = steps; }
    void   setD3pmT0(float t)            { d3pmT0_ = t; }
    void   setLanguageId(int id)         { languageId_ = id; }

    /// Audio longer than this (in seconds) is chunked at silence midpoints.
    /// Default 45.0 s — derived from the spike's memory-safe window for
    /// GAME's O(T²) attention on a 5-min clip (see design.md D3).
    void   setMaxChunkSeconds(double s)  { maxChunkSeconds_ = s; }

    /// Native sample rate from config.json (44100 for GAME-small).
    int    nativeSampleRate() const noexcept { return nativeSampleRate_; }

    /// Native timestep in seconds (0.01 s = 100 fps for GAME-small).
    double nativeTimestep() const noexcept { return timestep_; }

private:
    /// Run one full encoder→segmenter(D3PM)→bd2dur→estimator pipeline on a
    /// single audio chunk. Returned notes have `start`/`end` relative to the
    /// chunk's first sample.
    std::vector<Note> runSingleChunk(const float* audio,
                                     int64_t      numSamples,
                                     double       sampleRate);

    void loadConfig(const std::string& configPath);

    std::unique_ptr<Ort::Session>     encoder_;
    std::unique_ptr<Ort::Session>     segmenter_;
    std::unique_ptr<Ort::Session>     estimator_;
    std::unique_ptr<Ort::Session>     bd2dur_;
    std::unique_ptr<Ort::MemoryInfo>  memoryInfo_;

    // 共享 RunOptions：进程级共享单例只被生成器自身使用（无 terminateRun /
    // SetTerminate 调用方），串行推理由 ProcessF0Runtime::gameMutex_ 保证。
    Ort::RunOptions runOptions_;

    // From config.json
    int    nativeSampleRate_ = 44100;
    double timestep_         = 0.01;
    int    embeddingDim_     = 128;
    bool   loopMode_         = true;

    // D3PM hyperparams
    float  segThreshold_     = 0.2f;
    int    segRadius_        = 2;
    float  estThreshold_     = 0.2f;
    int    d3pmSteps_        = 8;
    float  d3pmT0_           = 0.95f;
    int    languageId_       = 0;

    // Chunking threshold (variable-length silence-aware below this; chunk above)
    double maxChunkSeconds_  = 45.0;
};

/// Stitch per-chunk notes into a single timeline. Adds `chunkStartSeconds[i]`
/// to every note in `perChunk[i]`, concatenates in time order, then drops any
/// note in chunk i+1 whose `start` is within `seamToleranceSec` of the seam
/// from chunk i AND has the same MIDI as a chunk-i tail note (the chunk-i
/// version wins because its onset is more reliable). Default tolerance 50 ms.
std::vector<Note> mergeChunkNotes(const std::vector<std::vector<Note>>& perChunk,
                                  const std::vector<double>&            chunkStartSeconds,
                                  double                                seamToleranceSec = 0.05);

} // namespace OpenTune
