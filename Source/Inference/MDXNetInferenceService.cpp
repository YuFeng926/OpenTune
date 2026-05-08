#include "MDXNetInferenceService.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/CpuBudgetManager.h"
#include "../DSP/ResamplingManager.h"
#include <onnxruntime_cxx_api.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <unordered_map>
#include <algorithm>

namespace OpenTune {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {
    // juce::dsp::FFT requires order, not size
    constexpr int kFftOrder = 12;       // 2^12 = 4096
    constexpr int kFftSize   = 1 << kFftOrder;
    constexpr int kHop       = 1024;
    constexpr int kDimF      = 2048;    // trimmed freq bins for model

    // Hann window (periodic: true for ISTFT compatibility)
    std::vector<float> makeHannWindow(int size) {
        std::vector<float> w(size);
        for (int i = 0; i < size; ++i)
            w[i] = 0.5f * (1.0f - std::cos(2.0f * 3.141592653589793f * i / (float)size));
        return w;
    }

    // Resample mono/stereo audio to 44100 Hz, output as stereo
    std::vector<float> resample(const float* src, int64_t n, int sr, int ch, int targetSr = 44100) {
        // Convert to mono first
        std::vector<float> mono((size_t)n);
        if (ch == 1) {
            std::copy(src, src + n, mono.begin());
        } else {
            for (int64_t i = 0; i < n; ++i)
                mono[(size_t)i] = 0.5f * (src[(size_t)(i*2)] + src[(size_t)(i*2) + 1]);
        }

        // Resample mono if needed
        std::vector<float> resampled;
        if (sr == targetSr) {
            resampled = std::move(mono);
        } else {
            ResamplingManager rm;
            resampled = rm.downsampleForInference(mono.data(), mono.size(), sr, targetSr);
        }

        // Duplicate to stereo
        const int64_t outLen = (int64_t)resampled.size();
        std::vector<float> stereo((size_t)outLen * 2);
        for (int64_t i = 0; i < outLen; ++i) {
            stereo[(size_t)(i*2)]     = resampled[(size_t)i];
            stereo[(size_t)(i*2) + 1] = resampled[(size_t)i];
        }
        return stereo;
    }
} // anonymous namespace

// ============================================================================
// STFT Buffers (PIMPL to keep header lean)
// ============================================================================
struct MDXNetInferenceService::StftBuffers {
    std::vector<float> hannWindow;
    std::unique_ptr<juce::dsp::FFT> fft;
    
    // Per-frame FFT scratch
    std::vector<juce::dsp::Complex<float>> fftBuf;   // kFftSize complex
    
    StftBuffers() {
        hannWindow = makeHannWindow(kFftSize);
        fft = std::make_unique<juce::dsp::FFT>(kFftOrder);
        fftBuf.resize((size_t)kFftSize);
    }
};

// ============================================================================
// Construction / Destruction
// ============================================================================
MDXNetInferenceService::MDXNetInferenceService(std::shared_ptr<Ort::Env> env)
    : env_(std::move(env))
    , stft_(std::make_unique<StftBuffers>())
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
}

MDXNetInferenceService::~MDXNetInferenceService()
{
    shutdown();
}

bool MDXNetInferenceService::initialize(const std::string& modelsDir)
{
    try {
        std::string modelPath = modelsDir;
        if (!modelPath.empty() && modelPath.back() != '/' && modelPath.back() != '\\')
            modelPath += "/";
        modelPath += "mdxnet_kara.onnx";

        auto& detector = AccelerationDetector::getInstance();

        Ort::SessionOptions sessionOpts;
        sessionOpts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // MDX-NET is CPU-primary (like F0), but honor CPU budget config
        const bool gpuMode = false;  // CPU-only for MDX
        const auto budget = CpuBudgetManager::buildConfig(gpuMode);
        sessionOpts.SetIntraOpNumThreads(budget.onnxIntra);
        sessionOpts.SetInterOpNumThreads(budget.onnxInter);
        sessionOpts.SetExecutionMode(budget.onnxSequential ? ExecutionMode::ORT_SEQUENTIAL : ExecutionMode::ORT_PARALLEL);
        sessionOpts.DisableMemPattern();
        sessionOpts.DisableCpuMemArena();

#ifdef __APPLE__
        if (detector.getSelectedBackend() == AccelerationDetector::AccelBackend::CoreML) {
            try {
                std::unordered_map<std::string, std::string> coremlOpts;
                coremlOpts["ModelFormat"] = "MLProgram";
                coremlOpts["MLComputeUnits"] = "CPUAndGPU";
                sessionOpts.AppendExecutionProvider("CoreML", coremlOpts);
                AppLogger::info("[MDXNet] CoreML EP added (macOS)");
            } catch (...) {
                AppLogger::warn("[MDXNet] CoreML EP failed — falling back to CPU");
            }
        }
#endif

#ifdef _WIN32
        juce::File modelFile(modelPath);
        std::wstring wModelPath = modelFile.getFullPathName().toWideCharPointer();
        session_ = std::make_unique<Ort::Session>(*env_, wModelPath.c_str(), sessionOpts);
#else
        session_ = std::make_unique<Ort::Session>(*env_, modelPath.c_str(), sessionOpts);
#endif

        // Verify input shape
        auto inputInfo = session_->GetInputTypeInfo(0);
        auto tensorInfo = inputInfo.GetTensorTypeAndShapeInfo();
        auto shape = tensorInfo.GetShape();
        if (shape.size() != 4 || shape[1] != 4 || shape[2] != 2048 || shape[3] != 256) {
            AppLogger::error("[MDXNet] Unexpected model input shape");
            return false;
        }

        initialized_ = true;
        AppLogger::info("[MDXNet] Initialized: " + juce::String(modelPath)
            + " backend=" + juce::String(detector.getBackendName()));
        return true;
    } catch (const std::exception& e) {
        AppLogger::error("[MDXNet] Init failed: " + juce::String(e.what()));
        return false;
    }
}

void MDXNetInferenceService::shutdown()
{
    if (session_) {
        session_.reset();
    }
    initialized_ = false;
}

// ============================================================================
// Streaming helpers: compute one STFT frame and per-chunk ISTFT
// ============================================================================

// Compute one STFT frame from mono audio, pack into 4-channel model format
// outFrame: [4 * kDimF] = L_real, L_imag, R_real, R_imag (L==R for mono)
static void computeOneStftFrame(const float* monoAudio, int64_t sampleStart,
                                 const std::vector<float>& hannWindow,
                                 juce::dsp::FFT& fft,
                                 std::vector<juce::dsp::Complex<float>>& fftBuf,
                                 float* outFrame)
{
    for (int i = 0; i < kFftSize; ++i)
        fftBuf[(size_t)i] = { monoAudio[sampleStart + i] * hannWindow[(size_t)i], 0.0f };
    fft.perform(fftBuf.data(), fftBuf.data(), false);
    for (int bin = 0; bin < kDimF; ++bin) {
        float re = fftBuf[(size_t)bin].real();
        float im = fftBuf[(size_t)bin].imag();
        outFrame[0 * kDimF + bin] = re;
        outFrame[1 * kDimF + bin] = im;
        outFrame[2 * kDimF + bin] = re;
        outFrame[3 * kDimF + bin] = im;
    }
}

// ISTFT a single model chunk [4, kDimF, kDimT] → stereo time-domain [2, audioLen]
// audioLen = (kDimT - 1) * kHop + kFftSize (with center=True semantics)
static void istftOneChunk(const float* spectrum,   // [4, kDimF, kDimT] row-major
                           const std::vector<float>& hannWindow,
                           juce::dsp::FFT& fft,
                           std::vector<juce::dsp::Complex<float>>& fftBuf,
                           float* outStereo)        // [2 * audioLen]
{
    constexpr int kDT = 256;
    constexpr int64_t kAudioLen = (kDT - 1) * kHop + kFftSize;
    std::fill(outStereo, outStereo + kAudioLen * 2, 0.0f);

    for (int ch = 0; ch < 2; ++ch) {
        const int64_t chOff = ch * 2 * kDimF * kDT;  // channel base in spectrum
        std::vector<float> overlapAcc((size_t)kAudioLen, 0.0f);
        std::vector<float> overlapWeight((size_t)kAudioLen, 0.0f);

        for (int f = 0; f < kDT; ++f) {
            const float* reSrc = spectrum + chOff + (size_t)f;
            const float* imSrc = spectrum + chOff + kDimF * kDT + (size_t)f;

            // Fill complex spectrum (row-major: bin varies fastest in spectrum layout? 
            // No — in our layout, time varies fastest. spectrum[ch][real/imag][bin][frame]
            // Actually: spectrum is flat: [ch * dimF * frames + bin * frames + frame]
            // Here chOff = ch*2*kDimF*kDimT, re is at chOff, im at chOff + kDimF*kDimT
            for (int bin = 0; bin < kDimF; ++bin) {
                size_t binOff = (size_t)(bin * kDT);
                fftBuf[(size_t)bin] = { reSrc[binOff], imSrc[binOff] };
            }
            fftBuf[(size_t)kDimF] = { 0.0f, 0.0f };
            for (int bin = kDimF + 1; bin < kFftSize; ++bin) {
                int conjBin = kFftSize - bin;
                if (conjBin < kDimF) {
                    size_t cOff = (size_t)(conjBin * kDT);
                    fftBuf[(size_t)bin] = {  reSrc[cOff], -imSrc[cOff] };
                } else {
                    fftBuf[(size_t)bin] = { 0.0f, 0.0f };
                }
            }

            fft.perform(fftBuf.data(), fftBuf.data(), true);

            float scale = 1.0f / (float)kFftSize;
            int64_t sampleOffset = f * (int64_t)kHop;
            for (int i = 0; i < kFftSize; ++i) {
                int64_t idx = sampleOffset + i;
                if (idx >= 0 && idx < kAudioLen) {
                    float winVal = hannWindow[(size_t)i];
                    overlapAcc[(size_t)idx] += fftBuf[(size_t)i].real() * winVal * scale;
                    overlapWeight[(size_t)idx] += winVal * winVal;
                }
            }
        }

        for (int64_t i = 0; i < kAudioLen; ++i) {
            float w = overlapWeight[(size_t)i];
            if (w > 1e-6f)
                outStereo[(size_t)(i * 2) + ch] = overlapAcc[(size_t)i] / w;
        }
    }
}

// ============================================================================
// Main extraction pipeline (streaming)
// ============================================================================
std::vector<float> MDXNetInferenceService::extractVocals(
    const float* audio, int64_t numSamples, int sampleRate, int numChannels)
{
    if (!initialized_ || !session_ || numSamples <= 0) return {};

    // 1. Resample to 44100 Hz stereo (L==R for mono input)
    std::vector<float> stereoData = resample(audio, numSamples, sampleRate, numChannels, kTargetSampleRate);
    const int64_t totalStereoLen = (int64_t)stereoData.size() / 2;
    const int64_t nFrames = (totalStereoLen - kFftSize) / kHop + 1;
    if (nFrames <= 0) return {};

    // Extract mono from interleaved stereo (channels are identical after resample)
    std::vector<float> monoData((size_t)totalStereoLen);
    for (int64_t i = 0; i < totalStereoLen; ++i)
        monoData[(size_t)i] = stereoData[(size_t)(i * 2)];

    // 2. Model inference tensors (fixed size, reused)
    std::vector<float> inputTensor((size_t)(4 * kDimF * kDimT));
    std::vector<float> outputTensor((size_t)(4 * kDimF * kDimT));
    std::vector<int64_t> inputShape = { 1, 4, kDimF, kDimT };
    const char* inputNames[]  = { "input" };
    const char* outputNames[] = { "output" };

    // 3. Streaming STFT + chunk inference + time-domain overlap-add
    constexpr int64_t kChunkAudioLen = (kDimT - 1) * kHop + kFftSize;  // ISTFT output length for one chunk

    // Spectrum ring buffer: holds up to kDimT STFT frames
    // Layout: frame-major [frame * 4 * kDimF + ch * kDimF + bin]
    constexpr size_t kFrameStride = 4 * kDimF;
    std::vector<float> specRing(kFrameStride * kDimT, 0.0f);

    // Hann window for chunk boundary smoothing (per-frame weights, applied in time domain)
    std::vector<float> chunkWindow(kDimT);
    for (int t = 0; t < kDimT; ++t)
        chunkWindow[t] = 0.5f * (1.0f - std::cos(2.0f * 3.141592653589793f * t / (float)(kDimT - 1)));

    // Growing output buffers (stereo interleaved + overlap weights)
    std::vector<float> outputStereoAccum;
    std::vector<float> outputWeightAccum;

    // Per-chunk ISTFT buffer
    std::vector<float> istftBuf((size_t)(kChunkAudioLen * 2));

    int64_t ringFill = 0;   // frames currently in ring (front at index 0)
    int64_t frameIdx = 0;   // next frame to compute
    int64_t chunkIdx = 0;   // count of chunks processed

    // Helper: transpose frame-major ring → model-major inputTensor
    //   Ring layout:  [f][ch][bin]
    //   Model layout: [ch][bin][f]  (time is innermost per bin)
    auto copyRingToInput = [&]() {
        std::fill(inputTensor.begin(), inputTensor.end(), 0.0f);
        for (int f = 0; f < kDimT; ++f) {
            for (int ch = 0; ch < 4; ++ch) {
                for (int bin = 0; bin < kDimF; ++bin) {
                    inputTensor[(size_t)(ch * kDimF * kDimT + bin * kDimT + f)] =
                        specRing[(size_t)(f * kFrameStride + ch * kDimF + bin)];
                }
            }
        }
    };

    // Helper: process one chunk (run model, ISTFT, overlap-add)
    auto processChunk = [&]() {
        copyRingToInput();

        // ONNX inference
        try {
            auto inputTensorObj = Ort::Value::CreateTensor<float>(
                *memoryInfo_, inputTensor.data(), inputTensor.size(), inputShape.data(), inputShape.size());
            auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensorObj, 1,
                                         outputNames, 1);
            std::copy(outputs[0].GetTensorMutableData<float>(),
                      outputs[0].GetTensorMutableData<float>() + inputTensor.size(),
                      outputTensor.begin());
        } catch (const std::exception& e) {
            AppLogger::error("[MDXNet] Inference failed: " + juce::String(e.what()));
            return false;
        }

        // ISTFT the model output → time-domain stereo
        istftOneChunk(outputTensor.data(), stft_->hannWindow, *stft_->fft, stft_->fftBuf, istftBuf.data());

        // Extend output/weight buffers
        int64_t chunkTimeOffset = chunkIdx * kChunkStep * kHop;
        int64_t neededLen = chunkTimeOffset + kChunkAudioLen;
        if (neededLen * 2 > (int64_t)outputStereoAccum.size()) {
            outputStereoAccum.resize((size_t)(neededLen * 2), 0.0f);
            outputWeightAccum.resize((size_t)neededLen, 0.0f);
        }

        // Weighted overlap-add in time domain
        for (int64_t n = 0; n < kChunkAudioLen; ++n) {
            int frameInChunk = (int)(n / kHop);
            if (frameInChunk < 0) frameInChunk = 0;
            if (frameInChunk >= kDimT) frameInChunk = kDimT - 1;
            float win = chunkWindow[(size_t)frameInChunk];

            int64_t outSample = chunkTimeOffset + n;
            outputStereoAccum[(size_t)(outSample * 2)]     += istftBuf[(size_t)(n * 2)]     * win;
            outputStereoAccum[(size_t)(outSample * 2) + 1] += istftBuf[(size_t)(n * 2) + 1] * win;
            outputWeightAccum[(size_t)outSample]           += win;
        }
        return true;
    };

    while (frameIdx < nFrames) {
        // Compute one STFT frame into ring
        int64_t sampleStart = frameIdx * kHop;
        if (sampleStart + kFftSize > totalStereoLen) break;

        float* frameSlot = specRing.data() + (size_t)(ringFill) * kFrameStride;
        computeOneStftFrame(monoData.data(), sampleStart, stft_->hannWindow,
                            *stft_->fft, stft_->fftBuf, frameSlot);

        ringFill++;
        frameIdx++;

        if (ringFill >= kDimT) {
            if (!processChunk()) return {};
            // Shift ring: discard first kChunkStep frames
            size_t keepStart = (size_t)kChunkStep * kFrameStride;
            size_t keepCount = (size_t)(kDimT - kChunkStep) * kFrameStride;
            std::memmove(specRing.data(), specRing.data() + keepStart, keepCount * sizeof(float));
            ringFill -= kChunkStep;
            chunkIdx++;
        }
    }

    // Tail: process remaining frames (zero-padded to kDimT)
    if (ringFill > 0) {
        // Zero remaining slots beyond ringFill
        std::fill(specRing.begin() + (size_t)ringFill * kFrameStride,
                  specRing.begin() + kFrameStride * kDimT, 0.0f);
        if (!processChunk()) return {};
        chunkIdx++;
    }

    // 4. Normalize overlap-add weights
    for (size_t i = 0; i < outputWeightAccum.size(); ++i) {
        float w = outputWeightAccum[i];
        if (w > 1e-6f) {
            outputStereoAccum[i * 2]     /= w;
            outputStereoAccum[i * 2 + 1] /= w;
        }
    }

    if (chunkIdx <= 0) return {};

    // 5. Trim output to valid audio range (no center-padding: STFT is non-centered)
    int64_t validEnd = nFrames * kHop;  // last frame starts here, spans kFftSize
    validEnd = (std::min)(validEnd, totalStereoLen);
    if (validEnd <= 0) return {};

    // 6. Extract vocal stem (channels 2-3 = "Other" output in UVR convention) and return mono
    //    Model output: ch0-1 = instrumental, ch2-3 = vocals
    std::vector<float> mono((size_t)validEnd);
    for (int64_t i = 0; i < validEnd; ++i)
        mono[(size_t)i] = outputStereoAccum[(size_t)(i * 2) + 1];  // R channel = vocals

    return mono;
}

} // namespace OpenTune
