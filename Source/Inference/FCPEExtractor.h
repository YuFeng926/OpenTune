#pragma once

#include "IF0Extractor.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <vector>
#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace OpenTune {

class ResamplingManager;

class FCPEExtractor : public IF0Extractor {
public:
    FCPEExtractor(
        std::unique_ptr<Ort::Session> session,
        std::shared_ptr<ResamplingManager> resampler
    );
    ~FCPEExtractor() override;

    std::vector<float> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        Ort::RunOptions& runOptions,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr
    ) override;

    int getHopSize() const override { return HOP; }
    int getTargetSampleRate() const override { return SAMPLE_RATE; }
    F0ModelType getModelType() const override { return F0ModelType::FCPE; }
    std::string getName() const override { return "FCPE"; }
    size_t getModelSize() const override { return 43 * 1024 * 1024; }

    void setConfidenceThreshold(float threshold) override { confidenceThreshold_ = threshold; }
    void setF0Min(float minHz) override { f0Min_ = minHz; }
    void setF0Max(float maxHz) override { f0Max_ = maxHz; }
    float getConfidenceThreshold() const override { return confidenceThreshold_; }
    float getF0Min() const override { return f0Min_; }
    float getF0Max() const override { return f0Max_; }

    struct PreflightResult {
        bool success = false;
        std::string errorMessage;
        double audioDurationSec = 0.0;
    };

    PreflightResult preflightCheck(size_t audioLength, int sampleRate) const;

private:
    std::unique_ptr<Ort::Session> session_;
    std::shared_ptr<ResamplingManager> resampler_;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo_;

    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int N_FFT = 1024;
    static constexpr int WIN_SIZE = 1024;
    static constexpr int HOP = 160;
    static constexpr int N_MELS = 128;
    static constexpr float CLIP_VAL = 1e-5f;
    static constexpr int OUT_DIMS = 360;
    static constexpr float F0_MIN_DEFAULT = 32.7f;
    static constexpr float F0_MAX_DEFAULT = 1975.5f;
    static constexpr double kMaxAudioDurationSec = 600.0;

    struct MelBand {
        int startBin = 0;
        int endBin = 0;
        std::vector<float> weights;
    };

    std::vector<MelBand> melFilterbank_;
    std::vector<float> hannWindow_;
    std::vector<float> centTable_;

    juce::dsp::FFT forwardFFT_;

    float confidenceThreshold_ = 0.05f;
    float f0Min_ = F0_MIN_DEFAULT;
    float f0Max_ = F0_MAX_DEFAULT;

    void initMelFilterbank();
    void initHannWindow();
    void initCentTable();

    std::vector<std::vector<float>> extractMel(const std::vector<float>& audio);
    std::vector<float> decodeF0(const float* latent, int numFrames, float threshold);

    static float centToF0(float cent) {
        return 10.0f * std::pow(2.0f, cent / 1200.0f);
    }
    static float f0ToCent(float f0) {
        return 1200.0f * std::log2(f0 / 10.0f);
    }
};

} // namespace OpenTune
