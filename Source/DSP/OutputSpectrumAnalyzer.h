#pragma once

/**
 * Standalone 最终输出频谱分析器。
 *
 * 音频线程只采集固定 PCM 窗口；FFT 与显示频谱计算在 UI 读取快照时执行。
 * 频谱横轴固定为 20 Hz–20 kHz 的 684 个对数采样点，与 EQ 图的对数轴一致。
 *
 * FFT 输出在每个显示频率点上做小数 bin 线性插值（非整数 bin 区间平均），
 * 正确处理 JUCE performRealOnlyForwardTransform 的 real-only 布局。
 */

#include "Utils/SpectrumDisplayData.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace OpenTune {

class OutputSpectrumAnalyzer
{
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHalfFft = kFftSize / 2;
    static constexpr int kMaxChannels = 2;

    OutputSpectrumAnalyzer() = default;
    OutputSpectrumAnalyzer(const OutputSpectrumAnalyzer&) = delete;
    OutputSpectrumAnalyzer& operator=(const OutputSpectrumAnalyzer&) = delete;

    void prepare(double sampleRate)
    {
        const std::lock_guard<std::mutex> guard(uiMutex_);
        sampleRate_ = sampleRate;
        fft_ = std::make_unique<juce::dsp::FFT>(11); // 2^11 = 2048

        constexpr float twoPi = 6.2831853071795864769f;
        for (int i = 0; i < kFftSize; ++i)
            hannWindow_[static_cast<size_t>(i)] =
                0.5f * (1.0f - std::cos(twoPi * static_cast<float>(i)
                                         / static_cast<float>(kFftSize)));

        // 预计算 684 个对数频率点：20 * pow(20000/20, i/683)
        constexpr float logMin = 1.30102999566f; // log10(20)
        constexpr float logMax = 4.30102999566f; // log10(20000)
        for (int i = 0; i < kSpectrumDisplayPoints; ++i)
        {
            const float norm = static_cast<float>(i)
                             / static_cast<float>(kSpectrumDisplayPoints - 1);
            displayFreqs_[static_cast<size_t>(i)] =
                std::pow(10.0f, logMin + (logMax - logMin) * norm);
        }

        resetState();
        prepared_ = true;
    }

    void reset()
    {
        const std::lock_guard<std::mutex> guard(uiMutex_);
        resetState();
    }

    void push(const juce::AudioBuffer<float>& buffer) noexcept
    {
        if (!prepared_)
            return;

        const int numSamples = buffer.getNumSamples();
        const int channelCount = std::min(buffer.getNumChannels(), kMaxChannels);
        if (numSamples <= 0 || channelCount <= 0)
            return;

        if (captureChannels_ != channelCount)
        {
            samplesCollected_ = 0;
            captureChannels_ = channelCount;
        }

        auto& capture = snapshots_[static_cast<size_t>(writerSnapshotIndex_)];
        if (samplesCollected_ == 0)
        {
            capture.hasWindow = false;
            capture.channelCount = channelCount;
        }

        for (int sample = 0; sample < numSamples; ++sample)
        {
            for (int channel = 0; channel < channelCount; ++channel)
                capture.pcm[static_cast<size_t>(channel)][static_cast<size_t>(samplesCollected_)] =
                    buffer.getSample(channel, sample);

            if (++samplesCollected_ == kFftSize)
            {
                capture.hasWindow = true;
                publishWindow();
                samplesCollected_ = 0;
                return;
            }
        }
    }

    void copySnapshot(SpectrumArray& spectrum,
                       SpectrumArray& peaks) const noexcept
    {
        const std::lock_guard<std::mutex> guard(uiMutex_);
        const int state = middleSnapshotState_.load(std::memory_order_acquire);
        if ((state & kSnapshotDirtyBit) != 0)
        {
            const int latest = middleSnapshotState_.exchange(readerSnapshotIndex_,
                                                              std::memory_order_acq_rel);
            readerSnapshotIndex_ = latest & kSnapshotIndexMask;
            auto& latestSnapshot = snapshots_[static_cast<size_t>(readerSnapshotIndex_)];
            if (latestSnapshot.hasWindow)
                analyzeWindow(latestSnapshot);
        }

        const auto& snapshot = snapshots_[static_cast<size_t>(readerSnapshotIndex_)];
        spectrum = snapshot.spectrum;
        peaks = snapshot.peaks;
    }

private:
    static_assert(std::atomic<int>::is_always_lock_free);

    // prepare/reset run after audio quiescence. The UI mutex excludes a concurrent
    // display read while FFT storage and the triple-buffer indices are reset.
    void resetState()
    {
        samplesCollected_ = 0;
        captureChannels_ = 0;
        for (auto& fftData : fftData_)
            fftData.fill(0.0f);
        framePower_.fill(0.0f);
        smoothedBins_.fill(0.0f);
        peakBins_.fill(0.0f);
        for (auto& snapshot : snapshots_)
            snapshot = {};
        writerSnapshotIndex_ = 1;
        readerSnapshotIndex_ = 0;
        middleSnapshotState_.store(2, std::memory_order_release);
    }

    static constexpr float kAttack = 0.34f;
    static constexpr float kRelease = 0.09f;
    static constexpr float kPeakDecay = 0.08f;
    static constexpr int kSnapshotIndexMask = 0x3;
    static constexpr int kSnapshotDirtyBit = 0x4;

    struct Snapshot
    {
        std::array<std::array<float, kFftSize>, kMaxChannels> pcm{};
        int channelCount = 0;
        bool hasWindow = false;
        SpectrumArray spectrum{};
        SpectrumArray peaks{};
    };

    void publishWindow() noexcept
    {
        const int previousMiddle = middleSnapshotState_.exchange(
            writerSnapshotIndex_ | kSnapshotDirtyBit,
            std::memory_order_acq_rel);
        writerSnapshotIndex_ = previousMiddle & kSnapshotIndexMask;
    }

    void analyzeWindow(Snapshot& snapshot) const noexcept
    {
        framePower_.fill(0.0f);

        for (int channel = 0; channel < snapshot.channelCount; ++channel)
        {
            auto& fftData = fftData_[static_cast<size_t>(channel)];
            const auto& pcm = snapshot.pcm[static_cast<size_t>(channel)];
            for (int i = 0; i < kFftSize; ++i)
                fftData[static_cast<size_t>(i)] =
                    pcm[static_cast<size_t>(i)] * hannWindow_[static_cast<size_t>(i)];
            std::memset(fftData.data() + kFftSize, 0, sizeof(float) * kFftSize);
            fft_->performRealOnlyForwardTransform(fftData.data());

            const float* data = fftData.data();
            const float sampleRateF = static_cast<float>(sampleRate_);

            // 对每个显示频率点，在 FFT 幅度谱上做小数 bin 线性插值
            for (int dp = 0; dp < kSpectrumDisplayPoints; ++dp)
            {
                const float binPos = displayFreqs_[static_cast<size_t>(dp)]
                                   * static_cast<float>(kFftSize) / sampleRateF;
                const int binLo = std::max(0, std::min(static_cast<int>(binPos), kHalfFft));
                const int binHi = std::min(binLo + 1, kHalfFft);
                const float frac = std::clamp(
                    binPos - static_cast<float>(binLo), 0.0f, 1.0f);

                // JUCE real-only FFT 布局：
                //   bin 0     → data[0]（纯实数）
                //   bin k > 0 → data[2k] + j*data[2k+1]
                //   Nyquist   → data[1]（纯实数）
                const float magLo = magFromBin(data, binLo);
                const float magHi = magFromBin(data, binHi);
                const float mag = magLo + frac * (magHi - magLo);

                framePower_[static_cast<size_t>(dp)] += mag * mag;
            }
        }

        const float channelScale = 1.0f / static_cast<float>(snapshot.channelCount);
        for (int dp = 0; dp < kSpectrumDisplayPoints; ++dp)
        {
            const float magnitude = std::sqrt(framePower_[static_cast<size_t>(dp)] * channelScale);
            const float level = (2.0f * magnitude) / (static_cast<float>(kFftSize) * 0.5f);
            const float db = 20.0f * std::log10(std::max(level, 1.0e-10f));
            const float scaled = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);

            float& smoothed = smoothedBins_[static_cast<size_t>(dp)];
            const float alpha = scaled > smoothed ? kAttack : kRelease;
            smoothed += alpha * (scaled - smoothed);

            float& peak = peakBins_[static_cast<size_t>(dp)];
            peak = std::max(peak - kPeakDecay, smoothed);
        }

        snapshot.spectrum = smoothedBins_;
        snapshot.peaks = peakBins_;
    }

    /// JUCE real-only FFT 幅度提取：正确处理 bin 0 和 Nyquist
    static float magFromBin(const float* data, int bin) noexcept
    {
        if (bin == 0)
            return std::abs(data[0]);
        if (bin == kHalfFft)
            return std::abs(data[1]);
        const float re = data[2 * bin];
        const float im = data[2 * bin + 1];
        return std::sqrt(re * re + im * im);
    }

    double sampleRate_ = 0.0;
    mutable std::mutex uiMutex_;
    bool prepared_ = false;
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::array<float, kFftSize> hannWindow_{};
    std::array<float, kSpectrumDisplayPoints> displayFreqs_{};

    mutable std::array<std::array<float, kFftSize * 2>, kMaxChannels> fftData_{};
    mutable std::array<float, kSpectrumDisplayPoints> framePower_{};
    mutable SpectrumArray smoothedBins_{};
    mutable SpectrumArray peakBins_{};
    int captureChannels_ = 0;
    int samplesCollected_ = 0;

    // SPSC triple buffer: producer and reader own separate PCM/output slots.
    mutable std::array<Snapshot, 3> snapshots_{};
    int writerSnapshotIndex_{1};
    mutable int readerSnapshotIndex_{0};
    mutable std::atomic<int> middleSnapshotState_{2};
};

} // namespace OpenTune
