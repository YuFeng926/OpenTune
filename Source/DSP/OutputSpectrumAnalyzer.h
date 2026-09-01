#pragma once

/**
 * Standalone 最终输出频谱分析器。
 *
 * 音频线程只写固定数组并执行预分配的 FFT；UI 线程通过原子快照读取。
 * 频谱横轴固定为 20 Hz–20 kHz，与 EQ 图的对数轴一致。
 */

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

namespace OpenTune {

class OutputSpectrumAnalyzer
{
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kNumBins = 128;
    static constexpr int kHalfFft = kFftSize / 2;
    static constexpr int kHopSize = 256;
    static constexpr int kMaxChannels = 2;

    OutputSpectrumAnalyzer() = default;
    OutputSpectrumAnalyzer(const OutputSpectrumAnalyzer&) = delete;
    OutputSpectrumAnalyzer& operator=(const OutputSpectrumAnalyzer&) = delete;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        fft_ = std::make_unique<juce::dsp::FFT>(11);

        constexpr float twoPi = 6.2831853071795864769f;
        for (int i = 0; i < kFftSize; ++i)
            hannWindow_[static_cast<size_t>(i)] =
                0.5f * (1.0f - std::cos(twoPi * static_cast<float>(i)
                                         / static_cast<float>(kFftSize)));

        const float binHz = static_cast<float>(sampleRate) / static_cast<float>(kFftSize);
        const float logMin = std::log10(20.0f);
        const float logMax = std::log10(20000.0f);
        for (int i = 0; i <= kNumBins; ++i)
        {
            const float norm = static_cast<float>(i) / static_cast<float>(kNumBins);
            const float frequency = std::pow(10.0f, logMin + (logMax - logMin) * norm);
            logBinEdges_[static_cast<size_t>(i)] = std::clamp(
                static_cast<int>(std::round(frequency / binHz)), 0, kHalfFft);
        }

        reset();
        prepared_ = true;
    }

    void reset()
    {
        writePos_ = 0;
        samplesFilled_ = 0;
        samplesSinceAnalysis_ = 0;
        historyChannels_ = 0;
        for (auto& history : history_)
            history.fill(0.0f);
        for (auto& fftData : fftData_)
            fftData.fill(0.0f);
        framePower_.fill(0.0f);
        smoothedBins_.fill(0.0f);
        peakBins_.fill(0.0f);
        publishSnapshot();
    }

    void push(const juce::AudioBuffer<float>& buffer) noexcept
    {
        if (!prepared_ || fft_ == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int channelCount = std::min(buffer.getNumChannels(), kMaxChannels);
        if (numSamples <= 0 || channelCount <= 0)
            return;

        if (historyChannels_ != channelCount)
        {
            for (auto& history : history_)
                history.fill(0.0f);
            writePos_ = 0;
            samplesFilled_ = 0;
            samplesSinceAnalysis_ = 0;
            historyChannels_ = channelCount;
        }

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const bool historyWasFull = samplesFilled_ == kFftSize;
            for (int channel = 0; channel < channelCount; ++channel)
                history_[static_cast<size_t>(channel)][static_cast<size_t>(writePos_)] =
                    buffer.getSample(channel, sample);

            writePos_ = (writePos_ + 1) % kFftSize;
            samplesFilled_ = std::min(samplesFilled_ + 1, kFftSize);

            if (!historyWasFull)
            {
                if (samplesFilled_ == kFftSize)
                    samplesSinceAnalysis_ = 0;
                continue;
            }

            ++samplesSinceAnalysis_;
            if (samplesFilled_ == kFftSize && samplesSinceAnalysis_ >= kHopSize)
            {
                samplesSinceAnalysis_ -= kHopSize;
                analyzeFrame(writePos_);
            }
        }
    }

    void copySnapshot(std::array<float, kNumBins>& spectrum,
                       std::array<float, kNumBins>& peaks) const noexcept
    {
        const int state = middleSnapshotState_.load(std::memory_order_acquire);
        if ((state & kSnapshotDirtyBit) != 0)
        {
            const int latest = middleSnapshotState_.exchange(readerSnapshotIndex_,
                                                              std::memory_order_acq_rel);
            readerSnapshotIndex_ = latest & kSnapshotIndexMask;
        }

        const auto& snapshot = snapshots_[static_cast<size_t>(readerSnapshotIndex_)];
        spectrum = snapshot.spectrum;
        peaks = snapshot.peaks;
    }

private:
    static constexpr float kAttack = 0.34f;
    static constexpr float kRelease = 0.09f;
    static constexpr float kPeakDecay = 0.08f;
    static constexpr int kSnapshotIndexMask = 0x3;
    static constexpr int kSnapshotDirtyBit = 0x4;

    struct Snapshot
    {
        std::array<float, kNumBins> spectrum{};
        std::array<float, kNumBins> peaks{};
    };

    void publishSnapshot() noexcept
    {
        auto& target = snapshots_[static_cast<size_t>(writerSnapshotIndex_)];
        target.spectrum = smoothedBins_;
        target.peaks = peakBins_;

        const int previousMiddle = middleSnapshotState_.exchange(
            writerSnapshotIndex_ | kSnapshotDirtyBit,
            std::memory_order_acq_rel);
        writerSnapshotIndex_ = previousMiddle & kSnapshotIndexMask;
    }

    void analyzeFrame(int endWritePos) noexcept
    {
        framePower_.fill(0.0f);

        for (int channel = 0; channel < historyChannels_; ++channel)
        {
            auto& fftData = fftData_[static_cast<size_t>(channel)];
            const auto& history = history_[static_cast<size_t>(channel)];
            for (int i = 0; i < kFftSize; ++i)
            {
                const int source = (endWritePos + i) % kFftSize;
                fftData[static_cast<size_t>(i)] =
                    history[static_cast<size_t>(source)] * hannWindow_[static_cast<size_t>(i)];
            }
            std::memset(fftData.data() + kFftSize, 0, sizeof(float) * kFftSize);
            fft_->performRealOnlyForwardTransform(fftData.data());

            for (int bin = 0; bin < kNumBins; ++bin)
            {
                const int lo = logBinEdges_[static_cast<size_t>(bin)];
                const int hi = std::max(logBinEdges_[static_cast<size_t>(bin + 1)], lo + 1);
                float power = 0.0f;
                int count = 0;
                for (int fftBin = lo; fftBin < hi && fftBin < kHalfFft; ++fftBin)
                {
                    const float real = fftData[static_cast<size_t>(2 * fftBin)];
                    const float imag = fftData[static_cast<size_t>(2 * fftBin + 1)];
                    power += real * real + imag * imag;
                    ++count;
                }
                if (count > 0)
                    framePower_[static_cast<size_t>(bin)] += power / static_cast<float>(count);
            }
        }

        const float channelScale = 1.0f / static_cast<float>(historyChannels_);
        for (int bin = 0; bin < kNumBins; ++bin)
        {
            const float magnitude = std::sqrt(framePower_[static_cast<size_t>(bin)] * channelScale);
            const float level = (2.0f * magnitude) / (static_cast<float>(kFftSize) * 0.5f);
            const float db = 20.0f * std::log10(std::max(level, 1.0e-10f));
            const float scaled = std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);

            float& smoothed = smoothedBins_[static_cast<size_t>(bin)];
            const float alpha = scaled > smoothed ? kAttack : kRelease;
            smoothed += alpha * (scaled - smoothed);

            float& peak = peakBins_[static_cast<size_t>(bin)];
            peak = std::max(peak - kPeakDecay, smoothed);
        }

        publishSnapshot();
    }

    double sampleRate_ = 0.0;
    bool prepared_ = false;
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::array<float, kFftSize> hannWindow_{};
    std::array<int, kNumBins + 1> logBinEdges_{};

    std::array<std::array<float, kFftSize>, kMaxChannels> history_{};
    std::array<std::array<float, kFftSize * 2>, kMaxChannels> fftData_{};
    std::array<float, kNumBins> framePower_{};
    std::array<float, kNumBins> smoothedBins_{};
    std::array<float, kNumBins> peakBins_{};
    int historyChannels_ = 0;
    int writePos_ = 0;
    int samplesFilled_ = 0;
    int samplesSinceAnalysis_ = 0;

    // SPSC triple buffer: reader and writer each own one slot; the atomic
    // middle slot transfers ownership, so ordinary float arrays never race.
    std::array<Snapshot, 3> snapshots_{};
    int writerSnapshotIndex_{1};
    mutable int readerSnapshotIndex_{0};
    mutable std::atomic<int> middleSnapshotState_{2};
};

} // namespace OpenTune
