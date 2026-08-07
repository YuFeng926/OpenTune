#include "GameNoteGenerator.h"
#include "../Utils/AppLogger.h"
#include "../Utils/PitchUtils.h"
#include "../Utils/SilentGapDetector.h"
#include "../Render/RenderChunkPlanner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace OpenTune {

// ============================================================================
// Construction
// ============================================================================

GameNoteGenerator::GameNoteGenerator(const std::string& modelDir, Ort::Env& env)
{
    // OpenTune builds with ORT_API_MANUAL_INIT (see CMakeLists.txt). Must be
    // called before any Ort::* call other than via the api object.  Idempotent.
    Ort::InitApi();

    auto resolve = [&modelDir](const char* fname) {
        return juce::File(juce::String(modelDir)).getChildFile(fname).getFullPathName().toStdString();
    };

    const std::string encoderPath   = resolve("encoder.onnx");
    const std::string segmenterPath = resolve("segmenter.onnx");
    const std::string estimatorPath = resolve("estimator.onnx");
    const std::string bd2durPath    = resolve("bd2dur.onnx");
    const std::string configPath    = resolve("config.json");

    for (const auto& p : { encoderPath, segmenterPath, estimatorPath, bd2durPath, configPath }) {
        if (!juce::File(juce::String(p)).existsAsFile()) {
            throw std::runtime_error("[GameNoteGenerator] Missing model file: " + p);
        }
    }

    loadConfig(configPath);

    Ort::SessionOptions opts;
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // CPU-only — CoreML EP silently swallows kernel errors on GAME's
    // softmax_nd graph in ORT 1.21 (process exits 0 mid-inference, no
    // crash report).  See research/p1_note_transcription_spike/reports.md.
    // Do NOT add CoreMLExecutionProvider here even if available.
    opts.DisableMemPattern();
    opts.DisableCpuMemArena();

    auto makeSession = [&env, &opts](const std::string& modelPath) {
#if JUCE_WINDOWS
        const juce::String jucePath(modelPath);
        auto widePath = jucePath.toWideCharPointer();
        return std::make_unique<Ort::Session>(env, widePath, opts);
#else
        return std::make_unique<Ort::Session>(env, modelPath.c_str(), opts);
#endif
    };

    try {
        encoder_   = makeSession(encoderPath);
        segmenter_ = makeSession(segmenterPath);
        estimator_ = makeSession(estimatorPath);
        bd2dur_    = makeSession(bd2durPath);
    } catch (const Ort::Exception& e) {
        throw std::runtime_error(std::string("[GameNoteGenerator] ONNX load failure: ") + e.what());
    }

    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

    AppLogger::info(juce::String("[NoteGen] backend=GAME-small loaded from ")
                    + juce::String(modelDir)
                    + " (sr=" + juce::String(nativeSampleRate_)
                    + ", timestep=" + juce::String(timestep_, 3)
                    + ", lang=" + juce::String(languageId_) + ")");
}

GameNoteGenerator::~GameNoteGenerator() = default;

void GameNoteGenerator::loadConfig(const std::string& configPath)
{
    juce::var parsed;
    auto raw = juce::File(juce::String(configPath)).loadFileAsString();
    auto result = juce::JSON::parse(raw, parsed);
    if (result.failed() || !parsed.isObject()) {
        throw std::runtime_error("[GameNoteGenerator] Failed to parse config.json: "
                                  + result.getErrorMessage().toStdString());
    }

    if (parsed.hasProperty("samplerate"))   nativeSampleRate_ = static_cast<int>(parsed["samplerate"]);
    if (parsed.hasProperty("timestep"))     timestep_         = static_cast<double>(parsed["timestep"]);
    if (parsed.hasProperty("embedding_dim")) embeddingDim_    = static_cast<int>(parsed["embedding_dim"]);
    if (parsed.hasProperty("loop"))         loopMode_         = static_cast<bool>(parsed["loop"]);
}

// ============================================================================
// Top-level dispatch
// ============================================================================

std::vector<Note> GameNoteGenerator::generate(const NoteGeneratorInput& input)
{
    if (input.audio.empty()) {
        return {};
    }

    if (input.sampleRate <= 0.0) {
        AppLogger::warn("[GameNoteGenerator] Invalid sampleRate=" + juce::String(input.sampleRate));
        return {};
    }

    const double durationSec = static_cast<double>(input.audio.size()) / input.sampleRate;

    // Short-clip fast path: no chunking, single inference.
    if (durationSec <= maxChunkSeconds_) {
        return runSingleChunk(input.audio.data(),
                              static_cast<int64_t>(input.audio.size()),
                              input.sampleRate);
    }

    // Long-clip path: reuse the existing silence-aware boundary builder.
    // Wrap the audio in a juce::AudioBuffer (single-channel, 44.1k assumed —
    // SilentGapDetector requires kRenderSampleRate == 44100).  If the caller's
    // audio is at a different SR, fall back to a single chunk and let the
    // encoder downsample internally; we don't try to retrofit silence-aware
    // chunking on non-native SR audio in this iteration.
    if (static_cast<int>(std::round(input.sampleRate)) != static_cast<int>(SilentGapDetector::kInternalSampleRate)) {
        AppLogger::warn("[GameNoteGenerator] long-clip non-native SR ("
                        + juce::String(input.sampleRate)
                        + " Hz != " + juce::String(SilentGapDetector::kInternalSampleRate)
                        + " Hz). Skipping silence-aware chunking; running as one chunk.");
        return runSingleChunk(input.audio.data(),
                              static_cast<int64_t>(input.audio.size()),
                              input.sampleRate);
    }

    juce::AudioBuffer<float> buf(1, static_cast<int>(input.audio.size()));
    std::memcpy(buf.getWritePointer(0), input.audio.data(), input.audio.size() * sizeof(float));

    const auto silentGaps = SilentGapDetector::detectAllGapsAdaptive(buf);

    // hopSize for boundary placement: align cuts to GAME's frame grid.
    const int hopSize = static_cast<int>(std::round(input.sampleRate * timestep_));

    const auto boundaries = RenderChunkPlanner::buildChunkBoundariesFromSilentGaps(
        static_cast<int64_t>(input.audio.size()), silentGaps, hopSize);

    if (boundaries.size() < 2) {
        // No useful boundaries — fall back to single chunk.
        return runSingleChunk(input.audio.data(),
                              static_cast<int64_t>(input.audio.size()),
                              input.sampleRate);
    }

    std::vector<std::vector<Note>> perChunk;
    std::vector<double>            chunkStartSeconds;
    perChunk.reserve(boundaries.size() - 1);
    chunkStartSeconds.reserve(boundaries.size() - 1);

    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const int64_t startSample = boundaries[i];
        const int64_t endSample   = boundaries[i + 1];
        const int64_t chunkLen    = endSample - startSample;
        if (chunkLen <= 0) continue;

        const double chunkStartSec = static_cast<double>(startSample) / input.sampleRate;
        chunkStartSeconds.push_back(chunkStartSec);
        perChunk.push_back(runSingleChunk(input.audio.data() + startSample,
                                          chunkLen,
                                          input.sampleRate));
    }

    return mergeChunkNotes(perChunk, chunkStartSeconds);
}

// ============================================================================
// Single-chunk inference (D3PM iterative segmenter)
// ============================================================================

namespace {

// Per the GAME ONNX spec (ONNX.md / Python prototype):
//   threshold, radius           → rank-0 scalar
//   duration, t, language       → rank-1, shape (1,)
// We model both with explicit shape vectors below.
inline std::vector<int64_t> kRank1OneShape() { return { 1 }; }
inline std::vector<int64_t> kRank0Shape()    { return {}; }

// Wrap a contiguous byte buffer as an ONNX bool tensor. ORT stores bool as
// 1 byte per element, so byte_count == element_count.
inline Ort::Value makeBoolTensor(Ort::MemoryInfo& mem,
                                 unsigned char*   data,
                                 size_t           elements,
                                 const int64_t*   shape,
                                 size_t           shapeLen)
{
    return Ort::Value::CreateTensor(mem, data, elements,
                                    shape, shapeLen,
                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
}

// Convert a presence/MIDI score pair into a Note. Mirrors the Python
// `_durations_to_notes` walk: cumulative start time = sum of preceding voiced
// durations only (unvoiced durations also advance the cursor).
//
// The Python code accumulates `cursor += d` for ALL maskN-valid durations
// regardless of presence, only emitting a Note when presence[i] is true.
void appendNote(std::vector<Note>& out, double start, double end,
                int midiInt, float continuousMidi)
{
    Note n;
    n.startTime     = start;
    n.endTime       = end;
    n.pitch         = PitchUtils::midiToFreq(static_cast<float>(midiInt));
    n.originalPitch = 440.0f * std::pow(2.0f, (continuousMidi - 69.0f) / 12.0f);
    n.isVoiced      = true;
    out.push_back(n);
}

} // namespace

std::vector<Note> GameNoteGenerator::runSingleChunk(const float* audio,
                                                    int64_t      numSamples,
                                                    double       sampleRate)
{
    if (audio == nullptr || numSamples <= 0) return {};

    // ----------------------------------------------------------------------
    // Optional resample to GAME native rate.  GAME-small expects 44.1k; if
    // the caller hands us something else we resample lightly.
    // ----------------------------------------------------------------------
    std::vector<float> nativeAudio;
    const float* audioPtr = audio;
    int64_t      audioLen = numSamples;

    if (static_cast<int>(std::round(sampleRate)) != nativeSampleRate_) {
        // Simple linear resample. Quality is fine for inference (model is
        // robust to mild aliasing; we are not generating audio).
        const double ratio = static_cast<double>(nativeSampleRate_) / sampleRate;
        const int64_t outLen = static_cast<int64_t>(std::round(static_cast<double>(numSamples) * ratio));
        nativeAudio.resize(static_cast<size_t>(outLen));
        for (int64_t i = 0; i < outLen; ++i) {
            const double srcF = static_cast<double>(i) / ratio;
            const int64_t srcLo = static_cast<int64_t>(std::floor(srcF));
            const int64_t srcHi = std::min(srcLo + 1, numSamples - 1);
            const double frac = srcF - static_cast<double>(srcLo);
            nativeAudio[static_cast<size_t>(i)] = static_cast<float>(
                (1.0 - frac) * audio[srcLo] + frac * audio[srcHi]);
        }
        audioPtr = nativeAudio.data();
        audioLen = outLen;
    }

    const float durationSeconds = static_cast<float>(audioLen) / static_cast<float>(nativeSampleRate_);

    // ----------------------------------------------------------------------
    // Encoder
    // ----------------------------------------------------------------------
    std::vector<Note> outNotes;

    try {
        const std::vector<int64_t> waveformShape = { 1, audioLen };
        auto waveformTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_, const_cast<float*>(audioPtr), static_cast<size_t>(audioLen),
            waveformShape.data(), waveformShape.size());

        float durationVal = durationSeconds;
        const auto durShape = kRank1OneShape();
        auto durationTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_, &durationVal, 1, durShape.data(), durShape.size());

        const char* encInputNames[]  = { "waveform", "duration" };
        const char* encOutputNames[] = { "x_seg", "x_est", "maskT" };

        std::array<Ort::Value, 2> encInputs = {
            std::move(waveformTensor), std::move(durationTensor)
        };

        auto encOut = encoder_->Run(
            runOptions_,
            encInputNames, encInputs.data(), encInputs.size(),
            encOutputNames, 3);

        // x_seg / x_est: [1, T, C] float
        const auto xSegInfo  = encOut[0].GetTensorTypeAndShapeInfo();
        const auto xEstInfo  = encOut[1].GetTensorTypeAndShapeInfo();
        const auto maskTInfo = encOut[2].GetTensorTypeAndShapeInfo();
        const auto xSegShape  = xSegInfo.GetShape();
        const auto xEstShape  = xEstInfo.GetShape();
        const auto maskTShape = maskTInfo.GetShape();

        if (maskTShape.size() != 2 || maskTShape[0] != 1) {
            throw std::runtime_error("[GameNoteGenerator] unexpected maskT shape");
        }
        const int64_t T = maskTShape[1];

        // Copy maskT into a stable owned buffer (we'll feed it to multiple
        // downstream sessions).
        std::vector<bool> maskTBuf(static_cast<size_t>(T));
        {
            const bool* src = encOut[2].GetTensorData<bool>();
            for (int64_t i = 0; i < T; ++i) maskTBuf[static_cast<size_t>(i)] = src[i];
        }

        // ------------------------------------------------------------------
        // known_boundaries: mark the first and last valid frame (Python
        // prototype's "no a-priori constraints — use duration" rule).
        // ------------------------------------------------------------------
        std::vector<unsigned char> knownBd(static_cast<size_t>(T), 0);
        {
            int64_t firstValid = -1, lastValid = -1;
            for (int64_t i = 0; i < T; ++i) {
                if (maskTBuf[static_cast<size_t>(i)]) {
                    if (firstValid < 0) firstValid = i;
                    lastValid = i;
                }
            }
            if (firstValid >= 0) {
                knownBd[static_cast<size_t>(firstValid)] = 1;
                knownBd[static_cast<size_t>(lastValid)]  = 1;
            }
        }

        // ------------------------------------------------------------------
        // D3PM iterative segmenter loop
        // ------------------------------------------------------------------
        // ts = linspace(d3pmT0, 0, d3pmSteps + 1)[:-1]  (excludes the final 0)
        std::vector<float> ts;
        if (loopMode_ && d3pmSteps_ > 0) {
            ts.reserve(static_cast<size_t>(d3pmSteps_));
            for (int i = 0; i < d3pmSteps_; ++i) {
                const float frac = static_cast<float>(i) / static_cast<float>(d3pmSteps_);
                ts.push_back(d3pmT0_ * (1.0f - frac));
            }
        } else {
            ts.push_back(0.0f);
        }

        std::vector<unsigned char> prevBd = knownBd; // start = known

        std::vector<int64_t> language = { static_cast<int64_t>(languageId_) };
        const std::vector<int64_t> languageShape = { 1 };

        // bool tensors for ORT need a contiguous bool array — we store as
        // unsigned char (1 byte) and reinterpret. That matches ORT's wire
        // layout (1 byte per bool element).
        const std::vector<int64_t> boolShape = { 1, T };

        // Persistent threshold/radius tensors — values don't change across the
        // D3PM loop. (Re-used per iteration via fresh CreateTensor calls
        // wrapping these stable storage locations.)
        float   segThresholdVal = segThreshold_;
        int64_t segRadiusVal    = static_cast<int64_t>(segRadius_);

        // maskT as bool
        std::vector<unsigned char> maskTBytes(static_cast<size_t>(T));
        for (int64_t i = 0; i < T; ++i) {
            maskTBytes[static_cast<size_t>(i)] = maskTBuf[static_cast<size_t>(i)] ? 1 : 0;
        }

        std::vector<unsigned char> currBd(static_cast<size_t>(T), 0);

        for (size_t step = 0; step < ts.size(); ++step) {
            float tVal = ts[step];

            // Build segmenter inputs.  Python uses input-name filter; we
            // pass the canonical 8 inputs and accept that the graph
            // signature on GAME-1.0.3-small includes them all.
            const std::vector<int64_t> tShape       = kRank1OneShape();
            const std::vector<int64_t> scalarShape  = kRank0Shape();

            auto xSegTensor = Ort::Value::CreateTensor<float>(
                *memoryInfo_, const_cast<float*>(encOut[0].GetTensorData<float>()),
                static_cast<size_t>(xSegInfo.GetElementCount()),
                xSegShape.data(), xSegShape.size());

            auto langTensor = Ort::Value::CreateTensor<int64_t>(
                *memoryInfo_, language.data(), language.size(),
                languageShape.data(), languageShape.size());

            auto knownTensor = makeBoolTensor(*memoryInfo_,
                knownBd.data(), knownBd.size(),
                boolShape.data(), boolShape.size());

            auto prevTensor = makeBoolTensor(*memoryInfo_,
                prevBd.data(), prevBd.size(),
                boolShape.data(), boolShape.size());

            auto tTensor = Ort::Value::CreateTensor<float>(
                *memoryInfo_, &tVal, 1, tShape.data(), tShape.size());

            auto maskTTensor = makeBoolTensor(*memoryInfo_,
                maskTBytes.data(), maskTBytes.size(),
                boolShape.data(), boolShape.size());

            auto thresholdTensor = Ort::Value::CreateTensor<float>(
                *memoryInfo_, &segThresholdVal, 1, scalarShape.data(), scalarShape.size());

            auto radiusTensor = Ort::Value::CreateTensor<int64_t>(
                *memoryInfo_, &segRadiusVal, 1, scalarShape.data(), scalarShape.size());

            const char* segInputNames[] = {
                "x_seg", "language", "known_boundaries", "prev_boundaries",
                "t", "maskT", "threshold", "radius"
            };
            const char* segOutputNames[] = { "boundaries" };

            std::array<Ort::Value, 8> segInputs = {
                std::move(xSegTensor),  std::move(langTensor),
                std::move(knownTensor), std::move(prevTensor),
                std::move(tTensor),     std::move(maskTTensor),
                std::move(thresholdTensor), std::move(radiusTensor)
            };

            auto segOut = segmenter_->Run(
                runOptions_,
                segInputNames, segInputs.data(), segInputs.size(),
                segOutputNames, 1);

            const bool* boundariesData = segOut[0].GetTensorData<bool>();
            for (int64_t i = 0; i < T; ++i) {
                currBd[static_cast<size_t>(i)] = boundariesData[i] ? 1 : 0;
            }
            prevBd = currBd;
        }

        // ------------------------------------------------------------------
        // bd2dur: boundaries → durations (seconds) + maskN
        // ------------------------------------------------------------------
        const std::vector<int64_t> bdShape = { 1, T };

        auto bdTensor = makeBoolTensor(*memoryInfo_,
            prevBd.data(), prevBd.size(),
            bdShape.data(), bdShape.size());

        auto bdMaskTTensor = makeBoolTensor(*memoryInfo_,
            maskTBytes.data(), maskTBytes.size(),
            bdShape.data(), bdShape.size());

        const char* bd2durInputNames[]  = { "boundaries", "maskT" };
        const char* bd2durOutputNames[] = { "durations", "maskN" };

        std::array<Ort::Value, 2> bd2durInputs = {
            std::move(bdTensor), std::move(bdMaskTTensor)
        };

        auto bd2durOut = bd2dur_->Run(
            runOptions_,
            bd2durInputNames, bd2durInputs.data(), bd2durInputs.size(),
            bd2durOutputNames, 2);

        const auto durationsInfo = bd2durOut[0].GetTensorTypeAndShapeInfo();
        const auto durationsShape = durationsInfo.GetShape();
        const auto maskNInfo = bd2durOut[1].GetTensorTypeAndShapeInfo();
        const auto maskNShape = maskNInfo.GetShape();
        if (durationsShape.size() != 2 || maskNShape.size() != 2 || durationsShape[1] != maskNShape[1]) {
            throw std::runtime_error("[GameNoteGenerator] bd2dur output shape mismatch");
        }
        const int64_t N = durationsShape[1];

        std::vector<float>          durations(static_cast<size_t>(N));
        std::vector<unsigned char>  maskN(static_cast<size_t>(N));
        std::memcpy(durations.data(), bd2durOut[0].GetTensorData<float>(),
                    static_cast<size_t>(N) * sizeof(float));
        {
            const bool* src = bd2durOut[1].GetTensorData<bool>();
            for (int64_t i = 0; i < N; ++i) maskN[static_cast<size_t>(i)] = src[i] ? 1 : 0;
        }

        // ------------------------------------------------------------------
        // Estimator: presence + scores (continuous MIDI)
        // ------------------------------------------------------------------
        auto xEstTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_, const_cast<float*>(encOut[1].GetTensorData<float>()),
            static_cast<size_t>(xEstInfo.GetElementCount()),
            xEstShape.data(), xEstShape.size());

        auto bdForEstTensor = makeBoolTensor(*memoryInfo_,
            prevBd.data(), prevBd.size(),
            boolShape.data(), boolShape.size());

        auto maskTForEstTensor = makeBoolTensor(*memoryInfo_,
            maskTBytes.data(), maskTBytes.size(),
            boolShape.data(), boolShape.size());

        const std::vector<int64_t> maskNShapeVec = { 1, N };
        auto maskNTensor = makeBoolTensor(*memoryInfo_,
            maskN.data(), maskN.size(),
            maskNShapeVec.data(), maskNShapeVec.size());

        float estThresholdVal = estThreshold_;
        const std::vector<int64_t> threshScalar = kRank0Shape();
        auto estThresholdTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo_, &estThresholdVal, 1, threshScalar.data(), threshScalar.size());

        const char* estInputNames[]  = { "x_est", "boundaries", "maskT", "maskN", "threshold" };
        const char* estOutputNames[] = { "presence", "scores" };

        std::array<Ort::Value, 5> estInputs = {
            std::move(xEstTensor),    std::move(bdForEstTensor),
            std::move(maskTForEstTensor), std::move(maskNTensor),
            std::move(estThresholdTensor)
        };

        auto estOut = estimator_->Run(
            runOptions_,
            estInputNames, estInputs.data(), estInputs.size(),
            estOutputNames, 2);

        const bool*  presenceData = estOut[0].GetTensorData<bool>();
        const float* scoresData   = estOut[1].GetTensorData<float>();

        // ------------------------------------------------------------------
        // Walk durations to produce notes (cursor accumulates through ALL
        // maskN-valid entries, but emits a Note only when presence is true).
        // ------------------------------------------------------------------
        double cursor = 0.0;
        outNotes.reserve(static_cast<size_t>(N));
        for (int64_t i = 0; i < N; ++i) {
            if (!maskN[static_cast<size_t>(i)]) continue;
            const double d = static_cast<double>(durations[static_cast<size_t>(i)]);
            if (d <= 0.0) continue;
            const double start = cursor;
            const double end   = cursor + d;
            if (presenceData[i]) {
                const float continuousMidi = scoresData[i];
                const int   midiInt        = static_cast<int>(std::round(continuousMidi));
                appendNote(outNotes, start, end, midiInt, continuousMidi);
            }
            cursor = end;
        }

    } catch (const Ort::Exception& e) {
        AppLogger::error(juce::String("[GameNoteGenerator] ORT error: ") + e.what());
        return {};
    } catch (const std::exception& e) {
        AppLogger::error(juce::String("[GameNoteGenerator] error: ") + e.what());
        return {};
    }

    return outNotes;
}

// ============================================================================
// Free function: per-chunk note stitching with seam dedup
// ============================================================================

std::vector<Note> mergeChunkNotes(const std::vector<std::vector<Note>>& perChunk,
                                  const std::vector<double>&            chunkStartSeconds,
                                  double                                seamToleranceSec)
{
    std::vector<Note> merged;
    if (perChunk.empty()) return merged;

    // Reserve a reasonable upper bound.
    size_t total = 0;
    for (const auto& c : perChunk) total += c.size();
    merged.reserve(total);

    auto append = [&](const Note& n, double offset) {
        Note shifted = n;
        shifted.startTime += offset;
        shifted.endTime   += offset;
        merged.push_back(shifted);
    };

    // First chunk: copy as-is with offset.
    if (!perChunk[0].empty()) {
        const double offset0 = chunkStartSeconds.empty() ? 0.0 : chunkStartSeconds[0];
        for (const auto& n : perChunk[0]) append(n, offset0);
    }

    // Subsequent chunks: drop notes within seamToleranceSec of seam if a
    // matching-MIDI note already lives in the prior chunk's tail.
    for (size_t c = 1; c < perChunk.size(); ++c) {
        const double offset = (c < chunkStartSeconds.size()) ? chunkStartSeconds[c] : 0.0;
        const double seamSec = (c <= chunkStartSeconds.size()) ? chunkStartSeconds[c] : 0.0;

        for (const auto& n : perChunk[c]) {
            const double absStart = n.startTime + offset;
            const int    midiCandidate = static_cast<int>(std::round(PitchUtils::freqToMidi(n.getAdjustedPitch())));

            bool drop = false;
            // Walk merged in reverse to find tail notes inside the seam window.
            for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
                if (seamSec - it->endTime > seamToleranceSec) break; // before the window
                if (std::abs(absStart - seamSec) > seamToleranceSec) break; // candidate not at seam
                if (static_cast<int>(std::round(PitchUtils::freqToMidi(it->getAdjustedPitch()))) == midiCandidate) {
                    drop = true;
                    break;
                }
            }

            if (!drop) append(n, offset);
        }
    }

    // Defensive: ensure monotonically non-overlapping notes.  Mirror the
    // tail-clamp logic from LegacyNoteGenerator::generate.
    std::sort(merged.begin(), merged.end(),
              [](const Note& a, const Note& b) { return a.startTime < b.startTime; });
    for (size_t k = 1; k < merged.size(); ++k) {
        if (merged[k - 1].endTime > merged[k].startTime) {
            merged[k - 1].endTime = merged[k].startTime;
        }
    }
    merged.erase(
        std::remove_if(merged.begin(), merged.end(),
                       [](const Note& n) { return n.endTime <= n.startTime; }),
        merged.end());

    return merged;
}

} // namespace OpenTune
