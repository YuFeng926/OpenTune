#include "SileroVadExtractor.h"
#include "../Utils/AppLogger.h"
#include "../DSP/ResamplingManager.h"

#include <onnxruntime_cxx_api.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

namespace OpenTune {

SileroVadExtractor::SileroVadExtractor() = default;

SileroVadExtractor::~SileroVadExtractor()
{
    session_.reset();
    env_.reset();
}

bool SileroVadExtractor::initialize(const std::string& modelPath)
{
    juce::File modelFile(modelPath);
    if (!modelFile.existsAsFile()) {
        AppLogger::log("[SileroVadExtractor] model file not found: " + juce::String(modelPath)
                       + " — VAD will be unavailable; PhonemeClassifier degrades to RMVPE-only V/U fusion");
        ready_.store(false, std::memory_order_release);
        return false;
    }

    try {
        // OpenTune builds with ORT_API_MANUAL_INIT (see CMakeLists.txt) — must
        // initialize the API table before any Ort::* construction.  Idempotent.
        Ort::InitApi();
        env_ = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OpenTuneSileroVad");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#if JUCE_WINDOWS
        const std::wstring wpath(modelPath.begin(), modelPath.end());
        session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), opts);
#else
        session_ = std::make_unique<Ort::Session>(*env_, modelPath.c_str(), opts);
#endif

        AppLogger::log("[SileroVadExtractor] initialized: " + juce::String(modelPath));
        ready_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        AppLogger::error("[SileroVadExtractor] initialize failed: " + juce::String(e.what()));
        session_.reset();
        env_.reset();
        ready_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        AppLogger::error("[SileroVadExtractor] initialize failed: unknown exception");
        session_.reset();
        env_.reset();
        ready_.store(false, std::memory_order_release);
        return false;
    }
}

std::vector<float> SileroVadExtractor::extractVad(const float* audio,
                                                   size_t length,
                                                   int sampleRate,
                                                   std::function<void(float)> progressCallback)
{
    if (!ready_.load(std::memory_order_acquire) || session_ == nullptr) {
        return {};
    }
    if (audio == nullptr || length == 0 || sampleRate <= 0) {
        return {};
    }

    // Step 1: resample to 16 kHz via existing r8brain wrapper.
    constexpr int kVadSr = 16000;
    constexpr int kChunkSamples = 512;     // 32 ms hop @ 16 kHz → 31.25 fps
    constexpr int kStateInner = 128;        // Silero v5 LSTM state dim
    std::vector<float> resampled;
    if (sampleRate != kVadSr) {
        ResamplingManager rm;
        resampled = rm.downsampleForInference(audio, length, sampleRate, kVadSr);
    } else {
        resampled.assign(audio, audio + length);
    }

    if (resampled.size() < static_cast<size_t>(kChunkSamples)) {
        return {};
    }

    // Step 2: run Silero in 512-sample chunks, maintaining LSTM state.
    // Confirmed Silero v5 I/O (from onnxruntime probe 2026-05-12):
    //   "input": [batch, samples=512] (float32)
    //   "state": [2, batch=1, 128]    (float32) — initial zeros
    //   "sr":    scalar               (int64)   — must be 16000
    // Outputs:
    //   "output": [batch, 1]          (float32) — VAD probability ∈ [0, 1]
    //   "stateN": [2, batch=1, 128]   (float32) — next LSTM state
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // LSTM state survives across chunks
        std::vector<float> state(2 * 1 * kStateInner, 0.0f);
        std::array<int64_t, 3> stateShape{2, 1, kStateInner};

        const std::array<int64_t, 2> audioShape{1, kChunkSamples};
        const int64_t sr = kVadSr;

        const char* inputNames[]  = {"input", "state", "sr"};
        const char* outputNames[] = {"output", "stateN"};

        const size_t numChunks = resampled.size() / kChunkSamples;
        std::vector<float> probs;
        probs.reserve(numChunks);

        for (size_t c = 0; c < numChunks; ++c) {
            const float* chunk = resampled.data() + c * kChunkSamples;

            std::array<Ort::Value, 3> inputs = {
                Ort::Value::CreateTensor<float>(memInfo,
                    const_cast<float*>(chunk), kChunkSamples,
                    audioShape.data(), audioShape.size()),
                Ort::Value::CreateTensor<float>(memInfo,
                    state.data(), state.size(),
                    stateShape.data(), stateShape.size()),
                Ort::Value::CreateTensor<int64_t>(memInfo,
                    const_cast<int64_t*>(&sr), 1,
                    nullptr, 0),
            };

            auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                          inputNames, inputs.data(), inputs.size(),
                                          outputNames, 2);

            // outputs[0]: probability (1, 1)
            const float* probData = outputs[0].GetTensorData<float>();
            probs.push_back(probData[0]);

            // outputs[1]: next state (2, 1, 128) → copy back into `state`
            const float* nextState = outputs[1].GetTensorData<float>();
            std::copy(nextState, nextState + state.size(), state.begin());

            if (progressCallback && (c % 64 == 0)) {
                progressCallback(static_cast<float>(c) / static_cast<float>(numChunks));
            }
        }
        if (progressCallback) progressCallback(1.0f);
        return probs;
    } catch (const std::exception& e) {
        AppLogger::error("[SileroVadExtractor] inference failed: " + juce::String(e.what()));
        return {};
    } catch (...) {
        AppLogger::error("[SileroVadExtractor] inference failed: unknown exception");
        return {};
    }
}

} // namespace OpenTune
