#include "RMVPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/SimdAccelerator.h"
#include <cmath>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace OpenTune {

size_t RMVPEExtractor::getAvailableSystemMemoryMB() {
#ifdef _WIN32
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<size_t>(memStatus.ullAvailPhys / (1024 * 1024));
    }
#elif defined(__APPLE__)
    uint64_t memSize = 0;
    size_t len = sizeof(memSize);
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(mib, 2, &memSize, &len, nullptr, 0) == 0 && memSize > 0) {
        return static_cast<size_t>(memSize / (1024 * 1024));
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
    if (sampleRate <= 0) {
        return {false, "[RMVPE] Invalid sample rate in preflight"};
    }

    if (audioLength == 0) {
        return {false, "[RMVPE] Empty audio buffer in preflight"};
    }
    
    PreflightResult result;
    
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
    auto& gpuDetector = AccelerationDetector::getInstance();
    const auto backend = gpuDetector.getSelectedBackend();
    // DirectML uses dedicated GPU VRAM; CoreML uses unified system memory (macOS UMA)
    bool useGpuVram = (backend == AccelerationDetector::AccelBackend::DirectML);
    
    if (useGpuVram) {
        // DirectML mode: Use GPU VRAM limit from AccelerationDetector
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
        // CPU / CoreML mode: Use system memory
        // CoreML on macOS uses unified memory architecture (UMA), same pool as CPU
        size_t systemMemMB = getAvailableSystemMemoryMB();
        size_t availableMB = (systemMemMB > kMinReservedMemoryMB) 
            ? (systemMemMB - kMinReservedMemoryMB) : 0;
        result.availableMemoryMB = availableMB;
        
        if (requiredMB > availableMB) {
            std::ostringstream oss;
            oss << "[RMVPE] Memory budget exceeded: estimated " << requiredMB 
                << "MB > " << availableMB << "MB available (" << gpuDetector.getBackendName() << " mode). "
                << "Audio duration: " << std::fixed << std::setprecision(1) << durationSec << "s. "
                << "System memory: " << systemMemMB << "MB total available.";
            result.errorMessage = oss.str();
            result.errorCategory = "MEMORY";
            result.success = false;
            
            AppLogger::error(result.errorMessage);
            AppLogger::error("[RMVPE] Preflight FAIL - Memory Budget: " + juce::String((int)requiredMB) 
                      + "MB required, " + juce::String((int)availableMB) + "MB available (" 
                      + juce::String(gpuDetector.getBackendName()) + ")");
            return result;
        }
    }
    
    // ============================================================
    // 3. Model Session Check
    // ============================================================
    if (!session_) {
        return {false, "[RMVPE] ONNX session not initialized"};
    }
    
    // ============================================================
    // Preflight Passed
    // ============================================================
    result.success = true;
    
    AppLogger::info("[RMVPE] Preflight PASS - Duration: " + juce::String(durationSec, 1) 
              + "s, Estimated memory: " + juce::String((int)requiredMB) + "MB, "
              + "Available: " + juce::String((int)result.availableMemoryMB) + "MB ("
              + juce::String(gpuDetector.getBackendName()) + ")");
    
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

    // Compute magnitude via SimdAccelerator (uses vDSP_vdist on macOS, scalar on others)
    auto& simd = SimdAccelerator::getInstance();
    simd.complexMagnitude(magnitude.data(), frame.data(), static_cast<size_t>(numFFTBins));

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
