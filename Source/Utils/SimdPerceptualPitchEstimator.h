#pragma once

#include "SimdAccelerator.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace OpenTune {

class SimdPerceptualPitchEstimator {
public:
    /**
     * @brief Calculates the Perceptual Intentional Pitch (PIP) using VNC, SSA, and Energy Weighting.
     * 
     * @param f0 Pointer to the F0 buffer.
     * @param energy Pointer to the Energy (RMS) buffer.
     * @param numSamples Number of frames to process.
     * @param hopSizeTime Time duration of one hop/frame in seconds (e.g., 0.01s).
     * @return float The estimated perceptual pitch.
     */
    static float estimatePIP(const float* f0, 
                             const float* energy, 
                             int numSamples, 
                             float hopSizeTime) 
    {
        // --- 1. Exception Handling & Input Validation ---
        if (numSamples <= 0) return 0.0f;
        if (f0 == nullptr || energy == nullptr) 
            return 0.0f; // Return 0 instead of throwing to be safe in real-time context
        
        if (hopSizeTime <= 0.0f) 
            return 0.0f;

        // Use JUCE HeapBlock for stack-like performance with heap size
        juce::HeapBlock<float> vncBuffer(numSamples);
        juce::HeapBlock<float> weightBuffer(numSamples);
        
        // Zero initialize buffers
        juce::FloatVectorOperations::clear(vncBuffer.getData(), numSamples);
        juce::FloatVectorOperations::clear(weightBuffer.getData(), numSamples);

        // --- 2. Pre-compute VNC (Vibrato-Neutral Center) ---
        // Filter window: at least 150ms to cover vibrato cycle
        const int vncWin = std::max(1, static_cast<int>(0.150f / hopSizeTime)); 
        computeMovingAverage(f0, vncBuffer.getData(), numSamples, vncWin);

        // --- 3. Calculate Fusion Weights (SSA * Energy) ---
        // SSA: Stable-State Analysis (Tukey-like window)
        const float edgePercent = 0.15f; 
        const int rampLen = static_cast<int>(numSamples * edgePercent); 

        // Initialize weight buffer
        prepareWeightBuffer(weightBuffer.getData(), energy, numSamples, rampLen);

        // --- 4. Core Acceleration: Dot Product ---
        // Formula: Sum(VNC * Weight) / Sum(Weight)
        
        // Denominator: Sum(Weight)
        float totalWeight = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            totalWeight += weightBuffer[i];
        }
        
        if (totalWeight < 1e-7f) {
            // Fallback: simple average of F0 if weights are too low (silence)
            // But if it's silence, pitch doesn't matter much. Return median of raw F0.
            std::vector<float> sortedF0(f0, f0 + numSamples);
            std::sort(sortedF0.begin(), sortedF0.end());
            return sortedF0[numSamples / 2];
        }

        // Numerator: Sum(VNC * Weight) — single fused dot product (Apple Accelerate vDSP_dotpr on macOS).
        const float weightedF0Sum = SimdAccelerator::getInstance().dotProduct(
            vncBuffer.getData(), weightBuffer.getData(), static_cast<size_t>(numSamples));

        return weightedF0Sum / totalWeight;
    }

private:
    // Compute Moving Average (Sliding Window)
    static void computeMovingAverage(const float* src, float* dest, int n, int win) {
        if (n == 0) return;
        
        double sum = 0;
        int actualWin = std::min(win, n);
        
        // Initial window
        for (int i = 0; i < actualWin; ++i) {
            sum += src[i];
        }

        // We need a centered window moving average
        // For index i, window is [i - win/2, i + win/2]
        int halfWin = actualWin / 2;

        // Naive implementation for correctness first (O(N*Win) is bad, O(N) is good)
        // Sliding window O(N):
        
        // Pre-fill sum for the first center point
        // But we need to handle boundaries carefully.
        // Let's use a simpler approach for boundaries: clamp indices.
        
        for (int i = 0; i < n; ++i) {
            int start = std::max(0, i - halfWin);
            int end = std::min(n - 1, i + halfWin);
            int count = end - start + 1;
            
            double localSum = 0.0;
            for (int j = start; j <= end; ++j) {
                localSum += src[j];
            }
            dest[i] = static_cast<float>(localSum / count);
        }
        
        // Note: The O(N) optimized version is tricky with variable boundary conditions.
        // Given N is small (one note length, maybe 50-500 frames), O(N*Win) is acceptable 
        // and safer for now. win is ~15 frames (150ms / 10ms). 15 * 500 = 7500 ops. Very fast.
    }

    // Prepare Weight Vector (SSA * Energy)
    static void prepareWeightBuffer(float* weights, const float* energy, int n, int ramp) {
        // Step 1: Copy Energy to weights
        // Energy is assumed to be linear magnitude or similar positive weight?
        // User said: "Extract RMS... before computing E(t), suggest log transform (dB) to prevent dominance"
        // BUT user code example passes 'energy' directly.
        // Let's assume 'energy' passed in is already the suitable weight E(t).
        juce::FloatVectorOperations::copy(weights, energy, n);

        // Step 2: Apply SSA (Stable-State Analysis) Ramp
        // Tukey window edges
        for (int i = 0; i < ramp; ++i) {
            // 0 to 1 cosine fade in
            // i=0 -> cos(-pi) = -1 -> w = 0
            // i=ramp -> cos(0) = 1 -> w = 1
            float w = 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * (static_cast<float>(i) - ramp) / ramp));
            
            weights[i] *= w;
            
            if (n - 1 - i >= 0) {
                weights[n - 1 - i] *= w;
            }
        }
        
        // Step 3: Ensure non-negative
        for (int i = 0; i < n; ++i) {
            if (weights[i] < 0.0f) weights[i] = 0.0f;
        }
    }
};

} // namespace OpenTune
