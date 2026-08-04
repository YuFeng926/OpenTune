#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cstdint>
#include <memory>
#include <map>
#include <set>
#include "Content/ContentKey.h"

namespace OpenTune {

struct PeakSample
{
    int8_t min;
    int8_t max;
    
    PeakSample() noexcept : min(0), max(0) {}
    
    void setRange(float minVal, float maxVal) noexcept
    {
        min = static_cast<int8_t>(juce::jlimit(-127, 127, juce::roundToInt(minVal * 127.0f)));
        max = static_cast<int8_t>(juce::jlimit(-127, 127, juce::roundToInt(maxVal * 127.0f)));
    }
    
    float getMin() const noexcept { return min / 127.0f; }
    float getMax() const noexcept { return max / 127.0f; }
    float getMagnitude() const noexcept { return static_cast<float>(juce::jmax(std::abs(min), std::abs(max))) / 127.0f; }
    bool isZero() const noexcept { return min == 0 && max == 0; }
};

class WaveformMipmap
{
public:
    static constexpr int kNumLevels = 6;
    static constexpr int kBaseSampleRate = 44100;
    
    static constexpr int kSamplesPerPeak[kNumLevels] = {
        32,     // Level 0: 每32采样一个峰值
        128,    // Level 1: 每128采样一个峰值
        512,    // Level 2: 每512采样一个峰值
        2048,   // Level 3: 每2048采样一个峰值
        8192,   // Level 4: 每8192采样一个峰值
        32768   // Level 5: 每32768采样一个峰值
    };
    
    struct Level
    {
        std::vector<PeakSample> peaks;
        int64_t numSamplesCovered = 0;
        bool complete = false;
        int64_t buildProgress = 0;
    };

    WaveformMipmap() = default;
    
    void setAudioSource(std::shared_ptr<const juce::AudioBuffer<float>> buffer);
    bool hasSource() const noexcept { return audioBuffer_ != nullptr; }
    bool isSourceChanged(std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept;
    
    int64_t getNumSamples() const noexcept { return numSamples_; }
    int getNumChannels() const noexcept { return numChannels_; }
    
    Level& getLevel(int level) { return levels_[level]; }
    const Level& getLevel(int level) const { return levels_[level]; }
    
    bool buildIncremental(double timeBudgetMs);
    bool isComplete() const noexcept;
    float getBuildProgress() const noexcept;
    
    /// 返回适合当前缩放的 complete 非空 level 索引；无可用 level 时返回 -1，
    /// 调用方必须以索引 >= 0 为前提，禁止访问伪就绪的 level 0。
    int selectBestLevelIndex(double pixelsPerSecond) const;
    
    void clear();
    
private:
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    int64_t numSamples_ = 0;
    int numChannels_ = 0;
    Level levels_[kNumLevels];
    
    void initializeLevels();
    bool buildLevelSlice(int level, double timeBudgetMs);
};

class WaveformMipmapCache
{
public:
    void setAudioSource(ContentKey contentKey, std::shared_ptr<const juce::AudioBuffer<float>> buffer);
    void remove(ContentKey contentKey);
    void prune(const std::set<ContentKey>& alive);
    void clear();
    
    bool buildIncremental(double timeBudgetMs);
    bool isComplete() const noexcept { return allMipmapsComplete_; }
    
    const WaveformMipmap* get(ContentKey contentKey) const
    {
        auto it = caches_.find(contentKey);
        return it != caches_.end() ? it->second.get() : nullptr;
    }

private:
    WaveformMipmap& getOrCreate(ContentKey contentKey);
    std::map<ContentKey, std::unique_ptr<WaveformMipmap>> caches_;
    bool allMipmapsComplete_ = true;
};

} // namespace OpenTune
