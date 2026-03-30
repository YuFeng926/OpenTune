#pragma once

/**
 * RenderingManager.h - AI 渲染任务执行器
 * 
 * 职责：接收任务，执行推理，写入缓存
 * 去重由 PluginProcessor 的 ChunkTaskKey 机制负责
 * 
 * 时间坐标：秒，音频固定 44.1kHz
 */

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include "../Utils/LockFreeQueue.h"
#include "RenderCache.h"
#include "InferenceManager.h"
#include "../DSP/ResamplingManager.h"

namespace OpenTune {

class RenderingManager {
public:
    struct ChunkInputs {
        double startSeconds{0.0};
        double lengthSeconds{0.0};
        double endSeconds{0.0};
        int64_t targetSamples{0};
        int numFrames{0};
        std::vector<float> mel;
        std::vector<float> energy;
        uint64_t targetRevision{0};
        int targetSampleRate{44100};
    };

    struct RenderTask {
        std::shared_ptr<RenderCache> cache;
        std::shared_ptr<const ChunkInputs> inputs;
        std::vector<float> f0;
        std::function<void(bool isError)> onDone;
    };

    RenderingManager();
    ~RenderingManager();

    void initialize();
    void shutdown();

    void submit(std::shared_ptr<const ChunkInputs> inputs,
               std::vector<float> f0,
               std::shared_ptr<RenderCache> cache,
               std::function<void(bool isError)> onDone);

    bool isRendering() const;

private:
    class Worker : public juce::Thread {
    public:
        Worker(RenderingManager& owner, const juce::String& name);
        void run() override;
    private:
        RenderingManager& owner_;
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    LockFreeQueue<RenderTask> queue_{4096};
    std::atomic<bool> running_{false};
    std::atomic<int> activeCount_{0};

    void process(RenderTask& task);
    void notifyWorkers();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderingManager)
};

} // namespace OpenTune
