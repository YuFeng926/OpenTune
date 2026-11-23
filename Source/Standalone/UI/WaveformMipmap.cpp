#include "WaveformMipmap.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

void WaveformMipmap::setAudioSource(std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    if (!buffer || buffer->getNumSamples() == 0)
    {
        clear();
        return;
    }
    
    audioBuffer_ = buffer;
    numSamples_ = buffer->getNumSamples();
    numChannels_ = buffer->getNumChannels();
    
    initializeLevels();
}

bool WaveformMipmap::isSourceChanged(std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept
{
    if (audioBuffer_.get() != buffer.get())
        return true;
    
    if (buffer)
    {
        if (numSamples_ != buffer->getNumSamples() || numChannels_ != buffer->getNumChannels())
            return true;
    }
    
    return false;
}

void WaveformMipmap::initializeLevels()
{
    for (int i = 0; i < kNumLevels; ++i)
    {
        const int64_t numPeaks = (numSamples_ + kSamplesPerPeak[i] - 1) / kSamplesPerPeak[i];
        levels_[i].peaks.assign(static_cast<std::size_t>(numPeaks), PeakSample());
        levels_[i].complete = false;
        levels_[i].buildProgress = 0;
    }

}

bool WaveformMipmap::buildIncremental(double timeBudgetMs)
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return false;
    
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;
    
    for (int level = kNumLevels - 1; level >= 0; --level)
    {
        if (levels_[level].complete)
            continue;
        
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
            break;
        
        buildLevelSlice(level, remain);
        
        if (levels_[level].complete)
        {
            levelCompleted = true;
            continue;
        }
        
        break;
    }
    
    // 返回值语义：是否有新 level 从 incomplete 转为 complete（可绘制事件）
    return levelCompleted;
}

bool WaveformMipmap::buildLevelSlice(int level, double timeBudgetMs)
{
    auto& lvl = levels_[level];
    
    if (lvl.complete)
        return false;
    
    if (!audioBuffer_ || numSamples_ <= 0)
        return false;
    
    const int samplesPerPeak = kSamplesPerPeak[level];
    const int64_t totalPeaks = static_cast<int64_t>(lvl.peaks.size());
    
    if (totalPeaks <= 0)
        return false;
    
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;
    
    const int batchSize = 256;
    
    while (lvl.buildProgress < totalPeaks)
    {
        const int64_t startIdx = lvl.buildProgress;
        const int64_t endIdx = std::min(startIdx + batchSize, totalPeaks);
        
        for (int64_t peakIdx = startIdx; peakIdx < endIdx; ++peakIdx)
        {
            const int64_t sampleStart = peakIdx * samplesPerPeak;
            const int64_t sampleEnd = std::min(sampleStart + samplesPerPeak, numSamples_);
            
            // 区块峰值与 PCM 边缘聚合共用 scanPcmPeak 的同一量化函数，
            // 保证区块缓存与真实 PCM 聚合可交换（逐位一致）。
            lvl.peaks[static_cast<std::size_t>(peakIdx)] = scanPcmPeak(sampleStart, sampleEnd);
        }
        
        lvl.buildProgress = endIdx;
        progressed = true;
        
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if ((nowMs - startMs) >= timeBudgetMs)
            break;
    }
    
    if (lvl.buildProgress >= totalPeaks)
        lvl.complete = true;

    return progressed;
}

bool WaveformMipmap::isComplete() const noexcept
{
    for (int i = 0; i < kNumLevels; ++i)
    {
        if (!levels_[i].complete)
            return false;
    }
    return true;
}

float WaveformMipmap::getBuildProgress() const noexcept
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return 0.0f;
    
    int64_t totalPeaks = 0;
    int64_t completedPeaks = 0;
    
    for (int i = 0; i < kNumLevels; ++i)
    {
        totalPeaks += static_cast<int64_t>(levels_[i].peaks.size());
        completedPeaks += levels_[i].buildProgress;
    }
    
    if (totalPeaks == 0)
        return 0.0f;
    
    return static_cast<float>(completedPeaks) / static_cast<float>(totalPeaks);
}

int WaveformMipmap::selectBestLevelIndex(double pixelsPerSecond) const
{
    const double secondsPerPixel = 1.0 / pixelsPerSecond;
    
    for (int i = kNumLevels - 1; i >= 0; --i)
    {
        const double secondsPerPeak = static_cast<double>(kSamplesPerPeak[i]) / kBaseSampleRate;
        
        if (secondsPerPeak <= secondsPerPixel * 2.0 && levels_[i].complete && !levels_[i].peaks.empty())
        {
            return i;
        }
    }
    
    for (int i = 0; i < kNumLevels; ++i)
    {
        if (levels_[i].complete && !levels_[i].peaks.empty())
            return i;
    }
    
    return -1;
}

std::optional<PeakSample> WaveformMipmap::queryExactPeak(int64_t startSample,
                                                         int64_t endSampleExclusive) const
{
    if (numSamples_ <= 0)
        return std::nullopt;

    // 与真实样本范围求交集；交集为空即空区间
    const int64_t s0 = std::max<int64_t>(startSample, 0);
    const int64_t s1 = std::min<int64_t>(endSampleExclusive, numSamples_);
    if (s1 <= s0)
        return std::nullopt;

    PeakSample accumulator;
    bool hasData = false;
    accumulateRange(s0, s1, kNumLevels - 1, accumulator, hasData);

    // s1 > s0 时最细层必然扫到真实 PCM，accumulator 一定取到数据
    return accumulator;
}

void WaveformMipmap::accumulateRange(int64_t startSample, int64_t endSampleExclusive, int level,
                                     PeakSample& accumulator, bool& hasData) const
{
    if (startSample >= endSampleExclusive)
        return;

    if (level < 0)
    {
        const PeakSample peak = scanPcmPeak(startSample, endSampleExclusive);
        if (!hasData)
        {
            accumulator = peak;
            hasData = true;
        }
        else
        {
            accumulator.merge(peak);
        }
        return;
    }

    const auto& lvl = levels_[level];
    const int64_t samplesPerPeak = kSamplesPerPeak[level];
    const int64_t alignedStart = ((startSample + samplesPerPeak - 1) / samplesPerPeak) * samplesPerPeak;
    const int64_t alignedEnd = (endSampleExclusive / samplesPerPeak) * samplesPerPeak;

    if (alignedStart >= alignedEnd)
    {
        // 区间内没有本层完整区块，整段交给更细层
        accumulateRange(startSample, endSampleExclusive, level - 1, accumulator, hasData);
        return;
    }

    // 头部未对齐段交给更细层
    accumulateRange(startSample, alignedStart, level - 1, accumulator, hasData);

    // 完整覆盖的区块直接复用现有缓存；未建区块（buildProgress 之外）停在这里
    int64_t block = alignedStart / samplesPerPeak;
    const int64_t blockEnd = alignedEnd / samplesPerPeak;
    while (block < blockEnd && block < static_cast<int64_t>(lvl.buildProgress))
    {
        const PeakSample& peak = lvl.peaks[static_cast<std::size_t>(block)];
        if (!hasData)
        {
            accumulator = peak;
            hasData = true;
        }
        else
        {
            accumulator.merge(peak);
        }
        ++block;
    }

    accumulateRange(block * samplesPerPeak, endSampleExclusive, level - 1, accumulator, hasData);
}

PeakSample WaveformMipmap::scanPcmPeak(int64_t startSample, int64_t endSampleExclusive) const
{
    float globalMin = 0.0f;
    float globalMax = 0.0f;
    bool hasData = false;

    for (int ch = 0; ch < numChannels_; ++ch)
    {
        const float* channelData = audioBuffer_->getReadPointer(ch);
        const auto range = juce::FloatVectorOperations::findMinAndMax(
            channelData + startSample, static_cast<int>(endSampleExclusive - startSample));
        if (!hasData)
        {
            globalMin = range.getStart();
            globalMax = range.getEnd();
            hasData = true;
        }
        else
        {
            globalMin = std::min(globalMin, range.getStart());
            globalMax = std::max(globalMax, range.getEnd());
        }
    }

    PeakSample peak;
    peak.setRange(globalMin, globalMax);
    return peak;
}

void WaveformMipmap::clear()
{
    audioBuffer_.reset();
    numSamples_ = 0;
    numChannels_ = 0;
    
    for (int i = 0; i < kNumLevels; ++i)
    {
        levels_[i].peaks.clear();
        levels_[i].complete = false;
        levels_[i].buildProgress = 0;
    }

}

WaveformMipmap& WaveformMipmapCache::getOrCreate(ContentKey contentKey)
{
    auto it = caches_.find(contentKey);
    if (it != caches_.end())
        return *it->second;
    
    auto inserted = caches_.emplace(contentKey, std::make_unique<WaveformMipmap>());
    allMipmapsComplete_ = false;
    return *inserted.first->second;
}

void WaveformMipmapCache::remove(ContentKey contentKey)
{
    caches_.erase(contentKey);
}

void WaveformMipmapCache::prune(const std::set<ContentKey>& alive)
{
    for (auto it = caches_.begin(); it != caches_.end();)
    {
        if (alive.find(it->first) == alive.end())
        {
            it = caches_.erase(it);
            continue;
        }
        ++it;
    }
}

void WaveformMipmapCache::clear()
{
    caches_.clear();
    allMipmapsComplete_ = true;
}

bool WaveformMipmapCache::setAudioSource(ContentKey contentKey,
                                        std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    if (!buffer || buffer->getNumSamples() == 0)
        return caches_.erase(contentKey) != 0;

    auto& mipmap = getOrCreate(contentKey);
    if (!mipmap.isSourceChanged(buffer))
        return false;
    mipmap.setAudioSource(std::move(buffer));
    allMipmapsComplete_ = false;
    return true;
}

bool WaveformMipmapCache::buildIncremental(double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0)
        return false;

    if (allMipmapsComplete_)
        return false;
    
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;
    bool allComplete = true;
    
    for (auto& kv : caches_)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
        {
            allComplete = false;
            break;
        }
        
        if (kv.second->buildIncremental(remain))
            levelCompleted = true;
        if (!kv.second->isComplete())
            allComplete = false;
    }
    
    allMipmapsComplete_ = allComplete;
    // 返回值语义：是否有新 level 完成（可绘制事件）
    return levelCompleted;
}

} // namespace OpenTune
