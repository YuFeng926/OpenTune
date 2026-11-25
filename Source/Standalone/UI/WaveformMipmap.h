#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cstdint>
#include <memory>
#include <map>
#include <set>
#include <optional>
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

    // int8 域 min/max 合并。所有峰值聚合（多通道、缓存区块、PCM 边缘）都走同一
    // 量化函数 setRange 与本合并，保证不同数据来源聚合出的结果逐位一致。
    void merge(const PeakSample& other) noexcept
    {
        if (other.min < min) min = other.min;
        if (other.max > max) max = other.max;
    }
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
        // 已建区块位图：区块可按任意顺序构建（预热窗口优先、左右跳窗），
        // 位图是已建集合的真实记录，也是窗口通道枚举未建块的索引。
        std::vector<uint64_t> builtMask;
        int64_t builtCount = 0;
        // 顺序构建游标：默认通道（左→右补齐）单调推进。
        int64_t buildProgress = 0;

        bool isBuilt(int64_t block) const noexcept
        {
            if (block < 0 || block >= static_cast<int64_t>(peaks.size()))
                return false;
            return (builtMask[static_cast<std::size_t>(block >> 6)]
                    >> (static_cast<unsigned>(block) & 63u)) & 1u;
        }

        void markBuilt(int64_t block) noexcept
        {
            auto& word = builtMask[static_cast<std::size_t>(block >> 6)];
            const uint64_t bit = 1ull << (static_cast<unsigned>(block) & 63u);
            if ((word & bit) == 0u)
            {
                word |= bit;
                ++builtCount;
            }
        }

        bool fullyBuilt() const noexcept
        {
            return !peaks.empty() && builtCount == static_cast<int64_t>(peaks.size());
        }
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
    /// 只跑预热窗口通道（各窗口内块，建完即止）：缓存层用它让所有欠债 key
    /// 的窗口先轮到，再交默认回填。
    bool buildPreheatWindows(double timeBudgetMs);
    bool isComplete() const noexcept;
    float getBuildProgress() const noexcept;

    /// 置「预热欠债」：构建顺序优先级窗口（样本域，含端点），levels [0, levelCount)
    /// 内覆盖该窗口的区块先于其余区块构建；窗口在显示层级全部建完即自动还清。
    /// 同一内容的多个 placement 各自置独立窗口（左跳右跳互不影响）。
    /// levelCount == 0（视口采样间距小于最小区块）时查询路径本就不消费任何区块，无欠债。
    void requestPreheat(int64_t startSample, int64_t endSampleExclusive, int levelCount);
    bool hasPreheatDebt() const noexcept { return !preheat_.empty(); }
    /// 清除预热欠债（离屏/换源/重新置债前调用，避免旧窗口持续消耗提升预算）。
    void clearPreheat() noexcept { preheat_.clear(); }

    /// 返回适合当前缩放的 complete 非空 level 索引；无可用 level 时返回 -1，
    /// 调用方必须以索引 >= 0 为前提，禁止访问伪就绪的 level 0。
    int selectBestLevelIndex(double pixelsPerSecond) const;

    /// 精确查询真实 PCM 在样本区间 [startSample, endSampleExclusive) 上的峰值。
    /// 样本索引属于 setAudioSource 传入 buffer 的域，时间→样本换算由调用方按
    /// 实际 buffer sampleRate 与 PianoRollTimeMap 完成。
    /// 空区间约定：区间先与 [0, getNumSamples()) 求交集，交集为空
    /// （endSampleExclusive <= startSample，或区间与音频不重叠）返回 std::nullopt；
    /// 有效交集返回该区间 int8 域 min/max，静音也是数据（min == max == 0 的有效值）。
    /// 完整覆盖的缓存区块直接复用，边缘与未建区块逐级下沉到真实 PCM 聚合；
    /// 缓存层级与构建进度只影响查询路径，不影响返回值。
    std::optional<PeakSample> queryExactPeak(int64_t startSample, int64_t endSampleExclusive) const;

    void clear();

private:
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    int64_t numSamples_ = 0;
    int numChannels_ = 0;
    Level levels_[kNumLevels];

    // 预热欠债窗口列表：可见采样范围 + 当前显示消费的层级数。
    // 同一内容的多个 placement（同一 modification 复用多个 region）各自持窗，
    // 任一窗口建完即从列表移除，列表空 = 无欠债。
    struct PreheatWindow
    {
        int64_t startSample = 0;
        int64_t endSampleExclusive = 0;
        int levelCount = 0;
    };
    std::vector<PreheatWindow> preheat_;

    void initializeLevels();
    // 区块构建原语：[startIdx, endIdx) 内未建块逐个扫描 PCM 并置位（位图跳过已建）。
    void buildBlockRange(int level, int64_t startIdx, int64_t endIdx);
    // 默认通道：左→右顺序回填剩余区块。
    void buildLevelSlice(int level, double timeBudgetMs);
    // 预热窗口通道：按位图单调推进枚举各窗口内未建块（左右跳窗、断点续建），
    // 全部窗口内建完即止，不做全层回填。
    void buildPreheatWindowSlice(int level, double timeBudgetMs);
    // 窗口通道 sweep：粗→细只扫窗口层；outOfBudget 置位表示预算耗尽。
    bool sweepPreheatWindows(double startMs, double timeBudgetMs, bool& outOfBudget);
    // 预热窗口在 level 的区块范围（clamp 到 [0, totalPeaks)）。
    void preheatBlockRange(int level, const PreheatWindow& window,
                           int64_t& blockStart, int64_t& blockEnd) const noexcept;
    bool isPreheatCompleteWindow(const PreheatWindow& window) const;
    // 从列表移除已建完的窗口（无条件，还清契约的一部分）；列表空 = 无欠债。
    void clearCompletedPreheatWindows();

    // queryExactPeak 的内部递归：按 level 从粗到细消费完整区块，
    // 未对齐/未建部分下沉更细层，level < 0 时直接扫描真实 PCM。
    void accumulateRange(int64_t startSample, int64_t endSampleExclusive, int level,
                         PeakSample& accumulator, bool& hasData) const;
    // 多通道真实 PCM 扫描 + setRange 量化；区间必须非空。
    PeakSample scanPcmPeak(int64_t startSample, int64_t endSampleExclusive) const;
};

class WaveformMipmapCache
{
public:
    bool setAudioSource(ContentKey contentKey, std::shared_ptr<const juce::AudioBuffer<float>> buffer);
    void remove(ContentKey contentKey);
    void prune(const std::set<ContentKey>& alive);
    void clear();

    bool buildIncremental(double timeBudgetMs);
    bool isComplete() const noexcept { return allMipmapsComplete_; }

    /// 按 key 置预热欠债（窗口为该 key 音频的样本域）；mipmap 不存在或无源时不置债。
    void requestPreheat(ContentKey contentKey, int64_t startSample, int64_t endSampleExclusive,
                        int levelCount);
    bool hasPreheatDebt() const noexcept;
    /// 清除全部 key 的预热欠债。
    void clearPreheatDebt() noexcept;

    // hasPreheatDebt 的 mipmap 扫描计数：稳态（allComplete）走 O(1) 快速路径不进循环。
    // 供欠债快速路径契约测试断言，无其他消费者。
    mutable uint64_t debtScanCounter_ = 0;

    const WaveformMipmap* get(ContentKey contentKey) const
    {
        auto it = caches_.find(contentKey);
        return it != caches_.end() ? it->second.get() : nullptr;
    }

private:
    WaveformMipmap& getOrCreate(ContentKey contentKey);
    /// 全部 mipmap 构建完成时置位（空表也算完成）；移除/构建/换源后统一刷新，
    /// 移除最后一个未完成 key 立即进入稳态。
    void recomputeAllComplete();
    std::map<ContentKey, std::unique_ptr<WaveformMipmap>> caches_;
    bool allMipmapsComplete_ = true;
};

} // namespace OpenTune
