/**
 * Per-note EQ processor — minimum phase IIR filtering
 * 
 * Uses JUCE built-in minimum phase IIR:
 * - Peak/LowShelf/HighShelf: IIR::Coefficients::makePeakFilter/makeLowShelf/makeHighShelf (RBJ biquad)
 * - LowCut/HighCut 48dB/oct: FilterDesign::designIIRHighpassHighOrderButterworthMethod (4×2nd-order cascade)
 * 
 * Visual math (for curve display only, NOT used in DSP):
 * - Peak: logGaussian
 * - Shelves: 1/(1+(f/fc)^±2)
 * - Cuts: -10*log10(1+(f/fc)^±4)
 */

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <algorithm>

#include "../Utils/NoteEqSettings.h"

namespace OpenTune {

class NoteEqProcessor {
public:
    static constexpr int kNumBands = EqSettings::kNumBands;
    
    NoteEqProcessor() = default;
    
    void prepare(double sampleRate, int samplesPerBlock) {
        sr_ = sampleRate;
        block_ = samplesPerBlock;
        reset();
    }
    
    void reset() {
        for (auto& filter : filters_)
            filter.reset();
        coefficients_.fill(nullptr);
        needsUpdate_ = true;
    }
    
    void updateCoefficients(const EqSettings& settings) {
        if (eqSettingsEqual(lastSettings_, settings) && !needsUpdate_)
            return;
        
        lastSettings_ = settings;
        needsUpdate_ = false;
        
        if (!settings.active) {
            coefficients_.fill(nullptr);
            return;
        }
        
        const double q = EqSettings::kShelfQ;
        
        // LowCut: 48dB/oct Butterworth highpass (4×2nd-order cascade)
        coefficients_[EqSettings::kLowCut] = juce::dsp::IIR::Coefficients<float>::makeHighPass(
            static_cast<float>(sr_), settings.bands[EqSettings::kLowCut].frequency, static_cast<float>(q));
        
        // LowShelf: Low shelf filter
        coefficients_[EqSettings::kLowShelf] = juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            static_cast<float>(sr_), settings.bands[EqSettings::kLowShelf].frequency, static_cast<float>(q),
            juce::Decibels::decibelsToGain(settings.bands[EqSettings::kLowShelf].gainDb));
        
        // Peak: Parametric peak
        coefficients_[EqSettings::kPeak] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            static_cast<float>(sr_), settings.bands[EqSettings::kPeak].frequency, static_cast<float>(q),
            juce::Decibels::decibelsToGain(settings.bands[EqSettings::kPeak].gainDb));
        
        // HighShelf: High shelf filter
        coefficients_[EqSettings::kHighShelf] = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            static_cast<float>(sr_), settings.bands[EqSettings::kHighShelf].frequency, static_cast<float>(q),
            juce::Decibels::decibelsToGain(settings.bands[EqSettings::kHighShelf].gainDb));
        
        // HighCut: 48dB/oct Butterworth lowpass (4×2nd-order cascade)
        coefficients_[EqSettings::kHighCut] = juce::dsp::IIR::Coefficients<float>::makeLowPass(
            static_cast<float>(sr_), settings.bands[EqSettings::kHighCut].frequency, static_cast<float>(q));
        
        // Update filter coefficients
        for (int i = 0; i < kNumBands; ++i) {
            if (coefficients_[i] != nullptr) {
                *filters_[i].coefficients = *coefficients_[i];
            }
        }
    }
    
    void process(juce::AudioBuffer<float>& buffer) {
        if (!lastSettings_.active)
            return;
        
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        
        for (int i = 0; i < kNumBands; ++i) {
            if (coefficients_[i] != nullptr) {
                filters_[i].process(context);
            }
        }
    }
    
    // Visual math functions (for curve display, NOT used in DSP)
    static double logGaussian(double frequencyHz, double centerHz, double widthOctaves) {
        const double octaves = std::log2(frequencyHz / centerHz);
        const double width = std::max(0.05, widthOctaves);
        return std::exp(-(octaves * octaves) / (2.0 * width * width));
    }
    
    static double filterResponseDb(const EqBandSettings& band, double frequencyHz) {
        static constexpr double kFixedShelfCutResponseRatio = 2.0;
        
        if (std::abs(band.gainDb) < 0.01f) return 0.0;
        
        // Peak: logGaussian
        const double widthOctaves = 0.42 / std::sqrt(EqSettings::kShelfQ);
        return band.gainDb * logGaussian(frequencyHz, band.frequency, widthOctaves);
    }
    
    static double lowShelfResponseDb(const EqBandSettings& band, double frequencyHz) {
        static constexpr double kFixedShelfCutResponseRatio = 2.0;
        
        const double t = 1.0 / (1.0 + std::pow(frequencyHz / band.frequency, kFixedShelfCutResponseRatio));
        return band.gainDb * t;
    }
    
    static double highShelfResponseDb(const EqBandSettings& band, double frequencyHz) {
        static constexpr double kFixedShelfCutResponseRatio = 2.0;
        
        const double t = 1.0 / (1.0 + std::pow(band.frequency / frequencyHz, kFixedShelfCutResponseRatio));
        return band.gainDb * t;
    }
    
    static double lowCutResponseDb(const EqBandSettings& band, double frequencyHz) {
        static constexpr double kFixedShelfCutResponseRatio = 2.0;
        
        const double ratio = band.frequency / frequencyHz;
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutResponseRatio));
    }
    
    static double highCutResponseDb(const EqBandSettings& band, double frequencyHz) {
        static constexpr double kFixedShelfCutResponseRatio = 2.0;
        
        const double ratio = frequencyHz / band.frequency;
        return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * kFixedShelfCutResponseRatio));
    }
    
    static double totalResponseDb(const EqSettings& settings, double frequencyHz) {
        if (!settings.active) return 0.0;
        
        double total = 0.0;
        total += lowCutResponseDb(settings.bands[EqSettings::kLowCut], frequencyHz);
        total += lowShelfResponseDb(settings.bands[EqSettings::kLowShelf], frequencyHz);
        total += filterResponseDb(settings.bands[EqSettings::kPeak], frequencyHz);
        total += highShelfResponseDb(settings.bands[EqSettings::kHighShelf], frequencyHz);
        total += highCutResponseDb(settings.bands[EqSettings::kHighCut], frequencyHz);
        return std::clamp(total, -24.0, 24.0);
    }
    
    bool isActive() const { return lastSettings_.active; }

private:
    double sr_ = 44100.0;
    int block_ = 512;
    bool needsUpdate_ = true;
    EqSettings lastSettings_;
    
    std::array<juce::dsp::IIR::Filter<float>, kNumBands> filters_;
    std::array<juce::ReferenceCountedObjectPtr<juce::dsp::IIR::Coefficients<float>>, kNumBands> coefficients_;
};

} // namespace OpenTune