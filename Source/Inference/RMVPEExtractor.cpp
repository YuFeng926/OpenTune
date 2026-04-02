#include "RMVPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AppLogger.h"
#include <cmath>

namespace OpenTune {

RMVPEExtractor::PreflightResult RMVPEExtractor::preflightCheck(size_t audioLength, int sampleRate) const {
    if (sampleRate <= 0) {
        return {false, "[RMVPE] Invalid sample rate in preflight"};
    }

    if (audioLength == 0) {
        return {false, "[RMVPE] Empty audio buffer in preflight"};
    }

    if (!session_) {
        return {false, "[RMVPE] ONNX session not initialized"};
    }

    return {true, {}};
}

RMVPEExtractor::RMVPEExtractor(
    std::unique_ptr<Ort::Session> session,
    std::shared_ptr<ResamplingManager> resampler)
    : session_(std::move(session))
    , resampler_(resampler)
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)
    );
}

RMVPEExtractor::~RMVPEExtractor() = default;

void RMVPEExtractor::fixOctaveErrors(std::vector<float>& f0) {
    if (f0.empty()) return;

    // Iterate through F0 curve to find sudden drops in continuous voiced segments
    for (size_t i = 1; i < f0.size(); ++i) {
        float prev = f0[i - 1];
        float curr = f0[i];

        // Check if we are in a continuous voiced segment
        if (prev > 0.0f && curr > 0.0f) {
            float ratio = curr / prev;

            if (ratio > 0.45f && ratio < 0.55f) {
                f0[i] *= 2.0f;
            }
        }
    }
}

void RMVPEExtractor::fillF0Gaps(std::vector<float>& f0, int maxGapFrames) {
    if (f0.empty() || maxGapFrames <= 0) return;
    
    struct VoicedSegment {
        int startFrame;
        int endFrame;
        float startF0;
        float endF0;
    };
    std::vector<VoicedSegment> segments;
    
    bool inVoiced = false;
    int segStart = 0;
    
    for (size_t i = 0; i < f0.size(); ++i) {
        if (f0[i] > 0.0f) {
            if (!inVoiced) {
                inVoiced = true;
                segStart = static_cast<int>(i);
            }
        } else {
            if (inVoiced) {
                VoicedSegment seg;
                seg.startFrame = segStart;
                seg.endFrame = static_cast<int>(i);
                seg.startF0 = f0[segStart];
                seg.endF0 = f0[i - 1];
                segments.push_back(seg);
                inVoiced = false;
            }
        }
    }
    
    if (inVoiced) {
        VoicedSegment seg;
        seg.startFrame = segStart;
        seg.endFrame = static_cast<int>(f0.size());
        seg.startF0 = f0[segStart];
        seg.endF0 = f0[f0.size() - 1];
        segments.push_back(seg);
    }
    
    for (size_t i = 1; i < segments.size(); ++i) {
        const auto& prev = segments[i - 1];
        const auto& curr = segments[i];
        
        int gapStart = prev.endFrame;
        int gapEnd = curr.startFrame;
        int gapFrames = gapEnd - gapStart;
        
        if (gapFrames > 0 && gapFrames <= maxGapFrames &&
            prev.endF0 > 0.0f && curr.startF0 > 0.0f) {
            
            float logStart = std::log2(std::max(prev.endF0, 1e-6f));
            float logEnd = std::log2(std::max(curr.startF0, 1e-6f));
            
            for (int j = 0; j < gapFrames; ++j) {
                float t = static_cast<float>(j + 1) / static_cast<float>(gapFrames + 1);
                float logInterp = logStart + (logEnd - logStart) * t;
                f0[gapStart + j] = std::pow(2.0f, logInterp);
            }
        }
    }
}

std::vector<float> RMVPEExtractor::extractF0(
    const float* audio,
    size_t length,
    int sampleRate,
    std::function<void(float)> progressCallback,
    std::function<void(const std::vector<float>&, int)> partialCallback)
{
    // ============================================================
    // Step 0: Contract Validation
    // ============================================================
    if (audio == nullptr) {
        AppLogger::error("[RMVPEExtractor] Contract violation: audio is null");
        throw std::invalid_argument("[RMVPEExtractor] audio cannot be null");
    }

    if (length == 0) {
        AppLogger::error("[RMVPEExtractor] Contract violation: length is zero");
        throw std::invalid_argument("[RMVPEExtractor] length cannot be zero");
    }

    if (sampleRate <= 0) {
        AppLogger::error("[RMVPEExtractor] Contract violation: sampleRate is " + juce::String(sampleRate));
        throw std::invalid_argument("[RMVPEExtractor] sampleRate must be positive");
    }

    if (!session_) {
        AppLogger::error("[RMVPEExtractor] Contract violation: session not initialized");
        throw std::logic_error("[RMVPEExtractor] session not initialized");
    }

    if (!resampler_) {
        AppLogger::error("[RMVPEExtractor] Contract violation: resampler not initialized");
        throw std::logic_error("[RMVPEExtractor] resampler not initialized");
    }

    // Step 1: Resource Preflight Check
    PreflightResult preflight = preflightCheck(length, sampleRate);
    if (!preflight.success) {
        throw std::runtime_error(preflight.errorMessage);
    }

    // Step 2: Resample to RMVPE native 16kHz domain if needed
    std::vector<float> audio16k;
    if (sampleRate != 16000) {
        audio16k = resampler_->downsampleForInference(audio, length, sampleRate, 16000);
    } else {
        audio16k.assign(audio, audio + length);
    }

    if (audio16k.empty()) {
        AppLogger::error("[RMVPEExtractor] Resampling failed");
        throw std::runtime_error("[RMVPEExtractor] Resampling failed");
    }

    // --- Pre-processing: High-pass Filter & Noise Gate ---

    // 1. High-pass Filter: 50Hz, 48dB/oct (8th order)
    auto coefficients = juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(
        50.0f, 16000.0, 8); // sampleRate is 16000

    float* channelData = audio16k.data();
    juce::dsp::AudioBlock<float> block(&channelData, 1, audio16k.size());
    juce::dsp::ProcessContextReplacing<float> context(block);

    for (auto& coefs : coefficients) {
        juce::dsp::IIR::Filter<float> filter;
        filter.coefficients = coefs;
        filter.reset();
        filter.process(context);
    }

    // 2. Noise Gate: -50dBFS threshold
    constexpr float kNoiseGateThreshold = 0.00316227766f; // -50dBFS = 10^(-50/20)
    for (auto& sample : audio16k) {
        if (std::abs(sample) < kNoiseGateThreshold) {
            sample = 0.0f;
        }
    }

    const size_t audioLength = audio16k.size();
    if (progressCallback) {
        progressCallback(0.1f);
    }

    // Step 3: Prepare single full-length input tensors in RMVPE native domain.
    std::vector<int64_t> waveformShape = {1, static_cast<int64_t>(audioLength)};
    auto waveformTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        audio16k.data(),
        audioLength,
        waveformShape.data(),
        waveformShape.size()
    );

    float threshold = confidenceThreshold_;
    std::vector<int64_t> thresholdShape = {1};
    auto thresholdTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_,
        &threshold,
        1,
        thresholdShape.data(),
        thresholdShape.size()
    );

    if (progressCallback) {
        progressCallback(0.3f);
    }

    // Step 4: Single-pass ONNX inference. No chunking, no stitching, no post-hoc length repair.
    const char* inputNames[] = {"waveform", "threshold"};
    const char* outputNames[] = {"f0", "uv"};
    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(waveformTensor));
    inputTensors.push_back(std::move(thresholdTensor));

    auto outputTensors = session_->Run(
        Ort::RunOptions{nullptr},
        inputNames, inputTensors.data(), 2,
        outputNames, 2
    );

    // Step 5: Extract F0 and UV from model output
    const float* f0Data = outputTensors[0].GetTensorData<float>();
    const float* uvData = outputTensors[1].GetTensorData<float>();
    const auto f0Shape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();
    const size_t f0Length = static_cast<size_t>(f0Shape[1]);

    if (progressCallback) {
        progressCallback(0.8f);
    }

    // Step 6: Build F0 vector with UV filtering
    std::vector<float> finalF0;
    finalF0.reserve(f0Length);

    for (size_t i = 0; i < f0Length; ++i) {
        float value = f0Data[i];
        if (enableUvCheck_ && uvData[i] < confidenceThreshold_) {
            value = 0.0f;
        }
        finalF0.push_back(value);
    }

    // Step 7: Fix octave errors (sudden drops of ~1 octave)
    fixOctaveErrors(finalF0);

    // Step 8: Fill small gaps between voiced segments (< 80ms)
    fillF0Gaps(finalF0, kMaxGapFramesDefault);

    if (partialCallback && !finalF0.empty()) {
        partialCallback(finalF0, 0);
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    AppLogger::debug("[RMVPEExtractor] Extracted native F0 frames: " + juce::String((int)finalF0.size()));

    return finalF0;
}

} // namespace OpenTune
