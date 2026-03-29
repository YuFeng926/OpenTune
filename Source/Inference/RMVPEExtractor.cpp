#include "RMVPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/GpuDetector.h"
#include "../Utils/AppLogger.h"
#include <cmath>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace OpenTune {

size_t RMVPEExtractor::getAvailableSystemMemoryMB() {
#ifdef _WIN32
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<size_t>(memStatus.ullAvailPhys / (1024 * 1024));
    }
#endif
    return 2048;
}

size_t RMVPEExtractor::estimateMemoryRequiredMB(size_t audioSamples16k) const {
    size_t inputBytes = audioSamples16k * sizeof(float);
    
    size_t numFrames = (audioSamples16k + hopSize_ - 1) / hopSize_;
    size_t outputBytes = numFrames * sizeof(float) * 2;
    
    size_t intermediateBytes = static_cast<size_t>(inputBytes * kMemoryOverheadFactor);
    
    size_t modelBytes = kModelMemoryMB * 1024 * 1024;
    
    size_t totalBytes = inputBytes + outputBytes + intermediateBytes + modelBytes;
    
    return totalBytes / (1024 * 1024);
}

RMVPEExtractor::PreflightResult RMVPEExtractor::preflightCheck(size_t audioLength, int sampleRate) const {
    PreflightResult result;

    if (sampleRate <= 0) {
        result.errorMessage = "[RMVPE] Invalid sample rate in preflight";
        result.errorCategory = "SYSTEM";
        result.success = false;
        AppLogger::error(result.errorMessage);
        return result;
    }

    if (audioLength == 0) {
        result.errorMessage = "[RMVPE] Empty audio buffer in preflight";
        result.errorCategory = "SYSTEM";
        result.success = false;
        AppLogger::error(result.errorMessage);
        return result;
    }
    
    // Calculate duration at native sample rate, then convert to 16kHz samples
    double durationSec = static_cast<double>(audioLength) / sampleRate;
    result.audioDurationSec = durationSec;
    
    // Resampling ratio for 16kHz target
    double resampleRatio = 16000.0 / sampleRate;
    size_t audioSamples16k = static_cast<size_t>(audioLength * resampleRatio);
    
    // ============================================================
    // 1. Duration Gate Check
    // ============================================================
    if (durationSec > kMaxAudioDurationSec) {
        std::ostringstream oss;
        oss << "[RMVPE] Duration gate exceeded: " << std::fixed << std::setprecision(1) 
            << durationSec << "s > " << kMaxAudioDurationSec << "s limit. "
            << "Reduce audio length or use shorter segments.";
        result.errorMessage = oss.str();
        result.errorCategory = "DURATION";
        result.success = false;
        
        AppLogger::error(result.errorMessage);
        AppLogger::error("[RMVPE] Preflight FAIL - Duration: " + juce::String(durationSec, 1) 
                  + "s, Limit: " + juce::String(kMaxAudioDurationSec) + "s");
        return result;
    }
    
    // ============================================================
    // 2. Memory Budget Estimation
    // ============================================================
    size_t requiredMB = estimateMemoryRequiredMB(audioSamples16k);
    result.estimatedMemoryMB = requiredMB;
    
    // Get available memory based on execution backend
    auto& gpuDetector = GpuDetector::getInstance();
    bool useGpu = (gpuDetector.getSelectedBackend() == GpuDetector::GpuBackend::DirectML);
    
    if (useGpu) {
        // GPU mode: Use GPU memory limit from GpuDetector
        size_t gpuMemLimitBytes = gpuDetector.getRecommendedGpuMemoryLimit();
        size_t availableMB = gpuMemLimitBytes / (1024 * 1024);
        result.availableMemoryMB = availableMB;
        
        // Check if GPU memory is sufficient
        if (availableMB < kMinGpuMemoryMB) {
            std::ostringstream oss;
            oss << "[RMVPE] GPU memory insufficient: " << availableMB << "MB < " 
                << kMinGpuMemoryMB << "MB minimum. "
                << "GPU: " << gpuDetector.getSelectedGpu().name;
            result.errorMessage = oss.str();
            result.errorCategory = "MEMORY";
            result.success = false;
            
            AppLogger::error(result.errorMessage);
            AppLogger::error("[RMVPE] Preflight FAIL - GPU Memory: " + juce::String((int)availableMB) 
                      + "MB available, " + juce::String((int)requiredMB) + "MB estimated");
            return result;
        }
        
        // For GPU, we also check against the recommended limit
        // ONNX Runtime with DirectML may need to allocate temp buffers
        size_t effectiveLimitMB = (availableMB > kMinReservedMemoryMB)
            ? (availableMB - kMinReservedMemoryMB)
            : 0;
        if (requiredMB > effectiveLimitMB) {
            std::ostringstream oss;
            oss << "[RMVPE] Memory budget exceeded: estimated " << requiredMB 
                << "MB > " << effectiveLimitMB << "MB available on GPU. "
                << "Audio duration: " << std::fixed << std::setprecision(1) << durationSec << "s. "
                << "GPU: " << gpuDetector.getSelectedGpu().name;
            result.errorMessage = oss.str();
            result.errorCategory = "MEMORY";
            result.success = false;
            
            AppLogger::error(result.errorMessage);
            AppLogger::error("[RMVPE] Preflight FAIL - Memory Budget: " + juce::String((int)requiredMB) 
                      + "MB required, " + juce::String((int)effectiveLimitMB) + "MB available (GPU)");
            return result;
        }
    } else {
        // CPU mode: Use system memory
        size_t systemMemMB = getAvailableSystemMemoryMB();
        size_t availableMB = (systemMemMB > kMinReservedMemoryMB) 
            ? (systemMemMB - kMinReservedMemoryMB) : 0;
        result.availableMemoryMB = availableMB;
        
        if (requiredMB > availableMB) {
            std::ostringstream oss;
            oss << "[RMVPE] Memory budget exceeded: estimated " << requiredMB 
                << "MB > " << availableMB << "MB available (CPU mode). "
                << "Audio duration: " << std::fixed << std::setprecision(1) << durationSec << "s. "
                << "System memory: " << systemMemMB << "MB total available.";
            result.errorMessage = oss.str();
            result.errorCategory = "MEMORY";
            result.success = false;
            
            AppLogger::error(result.errorMessage);
            AppLogger::error("[RMVPE] Preflight FAIL - Memory Budget: " + juce::String((int)requiredMB) 
                      + "MB required, " + juce::String((int)availableMB) + "MB available (CPU)");
            return result;
        }
    }
    
    // ============================================================
    // 3. Model Session Check
    // ============================================================
    if (!session_) {
        result.errorMessage = "[RMVPE] ONNX session not initialized";
        result.errorCategory = "MODEL";
        result.success = false;
        
        AppLogger::error(result.errorMessage);
        return result;
    }
    
    // ============================================================
    // Preflight Passed
    // ============================================================
    result.success = true;
    
    AppLogger::debug("[RMVPE] Preflight PASS - Duration: " + juce::String(durationSec, 1) 
              + "s, Estimated memory: " + juce::String((int)requiredMB) + "MB, "
              + "Available: " + juce::String((int)result.availableMemoryMB) + "MB ("
              + juce::String(useGpu ? "GPU" : "CPU") + ")");
    
    return result;
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

    // Initialize FFT
    forwardFFT_ = std::make_unique<juce::dsp::FFT>(fftOrder_);

    // Initialize Hann window
    hannWindow_.resize(fftSize_);
    for (int i = 0; i < fftSize_; ++i) {
        hannWindow_[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (fftSize_ - 1)));
    }
}

RMVPEExtractor::~RMVPEExtractor() = default;

std::vector<float> RMVPEExtractor::computeSTFTFrame(const float* audioData, size_t totalSamples, int startSample) {
    const int numFFTBins = fftSize_ / 2 + 1;
    std::vector<float> magnitude(numFFTBins, 0.0f);

    // Prepare windowed frame
    std::vector<float> frame(fftSize_ * 2, 0.0f);  // Complex FFT needs double size

    for (int i = 0; i < fftSize_; ++i) {
        int sampleIdx = startSample + i;
        if (sampleIdx >= 0 && sampleIdx < static_cast<int>(totalSamples)) {
            frame[i] = audioData[sampleIdx] * hannWindow_[i];
        }
    }

    // Perform FFT
    forwardFFT_->performRealOnlyForwardTransform(frame.data());

    // Compute magnitude
    for (int i = 0; i < numFFTBins; ++i) {
        float real = frame[i * 2];
        float imag = frame[i * 2 + 1];
        magnitude[i] = std::sqrt(real * real + imag * imag + 1e-9f);
    }

    return magnitude;
}

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
    // Step 0: Preflight Resource Check (Fail-Fast)
    // ============================================================
    // Check resources BEFORE any processing to fail fast and cleanly
    PreflightResult preflight = preflightCheck(length, sampleRate);
    if (!preflight.success) {
        // Structured error already logged in preflightCheck
        // Return empty vector - all-or-nothing semantics
        return {};
    }

    if (!session_ || !resampler_) {
        AppLogger::error("[RMVPEExtractor] Not initialized");
        return {};
    }

    try {
        // Step 1: Resample to RMVPE native 16kHz domain if needed
        std::vector<float> audio16k;
        if (sampleRate != 16000) {
            audio16k = resampler_->downsampleForInference(audio, length, sampleRate, 16000);
        } else {
            audio16k.assign(audio, audio + length);
        }

        if (audio16k.empty()) {
            AppLogger::error("[RMVPEExtractor] Resampling failed");
            return {};
        }

        // --- Pre-processing: High-pass Filter & Noise Gate ---
        
        // 1. High-pass Filter: 50Hz, 48dB/oct (8th order)
        try {
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
        } catch (const std::exception& e) {
            AppLogger::warn("[RMVPEExtractor] Failed to apply high-pass filter: " + juce::String(e.what()));
        } catch (...) {
            AppLogger::warn("[RMVPEExtractor] Failed to apply high-pass filter (unknown error)");
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

        // Step 2: Prepare single full-length input tensors in RMVPE native domain.
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

        // Step 3: Single-pass ONNX inference. No chunking, no stitching, no post-hoc length repair.
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

        if (progressCallback) {
            progressCallback(0.8f);
        }

        // Step 4: Extract model-native F0 output directly.
        const float* f0Data = outputTensors[0].GetTensorData<float>();
        const auto f0Shape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();
        const size_t f0Length = (f0Shape.size() >= 2 && f0Shape[1] > 0)
            ? static_cast<size_t>(f0Shape[1])
            : 0;

        const float* uvData = outputTensors[1].GetTensorData<float>();
        std::vector<float> finalF0;
        finalF0.reserve(f0Length);

        for (size_t i = 0; i < f0Length; ++i) {
            float value = f0Data[i];
            if (enableUvCheck_ && uvData[i] < confidenceThreshold_) {
                value = 0.0f;
            }
            finalF0.push_back(value);
        }

        // Step 5: Fix octave errors (sudden drops of ~1 octave)
        fixOctaveErrors(finalF0);

        // Step 6: Fill small gaps between voiced segments (< 80ms)
        fillF0Gaps(finalF0, kMaxGapFramesDefault);

        if (partialCallback && !finalF0.empty()) {
            partialCallback(finalF0, 0);
        }

        if (progressCallback) {
            progressCallback(1.0f);
        }

        AppLogger::debug("[RMVPEExtractor] Extracted native F0 frames: " + juce::String((int)finalF0.size()));

        return finalF0;

    } catch (const Ort::Exception& e) {
        AppLogger::error("[RMVPEExtractor] ONNX error: " + juce::String(e.what()));
        return {};
    } catch (const std::exception& e) {
        AppLogger::error("[RMVPEExtractor] Error: " + juce::String(e.what()));
        return {};
    }
}

} // namespace OpenTune
