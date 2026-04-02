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
    
    if (!isSourceChanged(buffer))
        return;
    
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
        levels_[i].numSamplesCovered = numSamples_;
        levels_[i].complete = false;
        levels_[i].buildProgress = 0;
    }
}

bool WaveformMipmap::buildIncremental(double timeBudgetMs)
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return false;
    
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;
    
    for (int level = 0; level < kNumLevels; ++level)
    {
        if (levels_[level].complete)
            continue;
        
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
            break;
        
        if (buildLevelSlice(level, remain))
            progressed = true;
        
        if (levels_[level].complete)
            continue;
        
        break;
    }
    
    return progressed;
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
            const int64_t numToProcess = sampleEnd - sampleStart;
            
            if (numToProcess <= 0)
            {
                lvl.peaks[static_cast<std::size_t>(peakIdx)] = PeakSample();
                continue;
            }
            
            float globalMin = 0.0f;
            float globalMax = 0.0f;
            bool hasData = false;
            
            for (int ch = 0; ch < numChannels_; ++ch)
            {
                const float* channelData = audioBuffer_->getReadPointer(ch);
                
                auto range = juce::FloatVectorOperations::findMinAndMax(
                    channelData + sampleStart, static_cast<int>(numToProcess));
                
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
            
            lvl.peaks[static_cast<std::size_t>(peakIdx)].setRange(globalMin, globalMax);
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

const WaveformMipmap::Level& WaveformMipmap::selectBestLevel(double pixelsPerSecond) const
{
    const int idx = selectBestLevelIndex(pixelsPerSecond);
    return levels_[idx];
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
    
    return 0;
}

void WaveformMipmap::clear()
{
    audioBuffer_.reset();
    numSamples_ = 0;
    numChannels_ = 0;
    
    for (int i = 0; i < kNumLevels; ++i)
    {
        levels_[i].peaks.clear();
        levels_[i].numSamplesCovered = 0;
        levels_[i].complete = false;
        levels_[i].buildProgress = 0;
    }
}

WaveformMipmap& WaveformMipmapCache::getOrCreate(uint64_t clipId)
{
    auto it = caches_.find(clipId);
    if (it != caches_.end())
        return *it->second;
    
    auto inserted = caches_.emplace(clipId, std::make_unique<WaveformMipmap>());
    return *inserted.first->second;
}

void WaveformMipmapCache::remove(uint64_t clipId)
{
    caches_.erase(clipId);
}

void WaveformMipmapCache::prune(const std::unordered_set<uint64_t>& alive)
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
}

bool WaveformMipmapCache::buildIncremental(double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0)
        return false;
    
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;
    
    for (auto& kv : caches_)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
            break;
        
        if (kv.second->buildIncremental(remain))
            progressed = true;
    }
    
    return progressed;
}

} // namespace OpenTune
