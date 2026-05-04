/**
 * 音频源存储（SourceStore）
 *
 * 管理原始音频源的身份与生命周期。每个 Source 代表一段导入的原始音频数据，
 * 包含音频缓冲区、采样率等元信息。Source 不持有任何编辑状态——编辑后的
 * 结果由 MaterializationStore 管理。
 *
 * 线程安全：内部使用 ReadWriteLock，读写均可跨线程调用。
 * 生命周期：支持 retire/revive 软删除，用于 Undo 系统的延迟回收。
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace OpenTune {

class SourceStore {
public:
    struct CreateSourceRequest {
        juce::String displayName;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        double sampleRate{0.0};
    };

    struct SourceSnapshot {
        uint64_t sourceId{0};
        juce::String displayName;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        double sampleRate{0.0};
        int numChannels{0};
        int64_t numSamples{0};
    };

    SourceStore();
    ~SourceStore();

    SourceStore(const SourceStore&) = delete;
    SourceStore& operator=(const SourceStore&) = delete;

    uint64_t createSource(CreateSourceRequest request, uint64_t forcedSourceId = 0); // 创建新 Source 并返回分配的 sourceId；forcedSourceId 非零时使用指定 ID（用于 Undo 恢复）
    void clear(); // 清空所有 Source（含已 retire 的）
    bool deleteSource(uint64_t sourceId); // 物理删除指定 Source
    bool containsSource(uint64_t sourceId) const;        // active only
    bool getSnapshot(uint64_t sourceId, SourceSnapshot& out) const; // 获取指定 Source 的只读快照
    bool getAudioBuffer(uint64_t sourceId, std::shared_ptr<const juce::AudioBuffer<float>>& out) const; // 获取指定 Source 的音频缓冲区共享指针

    // 软删除/恢复接口，供 UndoAction 和垃圾回收使用
    bool retireSource(uint64_t id);
    bool reviveSource(uint64_t id);
    bool isRetired(uint64_t id) const;
    bool physicallyDeleteIfReclaimable(uint64_t id);
    std::vector<uint64_t> getRetiredSourceIds() const;

private:
    // 内部存储条目
    struct SourceEntry {
        uint64_t sourceId{0};
        juce::String displayName;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        double sampleRate{0.0};
        int numChannels{0};
        int64_t numSamples{0};
        bool isRetired_{false};
    };

    juce::ReadWriteLock lock_;
    std::map<uint64_t, SourceEntry> sources_;
    std::atomic<uint64_t> nextSourceId_{1};
};

} // namespace OpenTune
