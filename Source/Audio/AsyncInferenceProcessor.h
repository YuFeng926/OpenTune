#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include "../Utils/Note.h"
#include "../Inference/InferenceManager.h"

namespace OpenTune {

class AsyncInferenceProcessor : public juce::Thread
{
public:
    struct InferenceResult
    {
        bool success;
        juce::String errorMessage;
        std::vector<float> f0Data;
        std::vector<Note> notes;
    };

    using ProgressCallback = std::function<void(float, const juce::String&)>;
    using CompletionCallback = std::function<void(InferenceResult)>;
    using PartialResultCallback = std::function<void(const std::vector<float>&, const std::vector<float>&)>; // originalF0, confidences

    AsyncInferenceProcessor() : juce::Thread("InferenceThread")
    {
        validityToken_ = std::make_shared<std::atomic<bool>>(true);
    }

    ~AsyncInferenceProcessor() override
    {
        *validityToken_ = false;
        stopThread(2000);
    }

    void processAudio(const float* audioData, int numSamples, int sampleRate,
                      float baseProgress,
                      ProgressCallback progressCallback,
                      CompletionCallback completionCallback,
                      PartialResultCallback partialCallback = nullptr)
    {
        stopThread(2000);

        // Copy audio data to internal buffer for thread safety
        audioBuffer_.assign(audioData, audioData + numSamples);
        sampleRate_ = sampleRate;
        baseProgress_ = baseProgress;
        progressCallback_ = progressCallback;
        completionCallback_ = completionCallback;
        partialCallback_ = partialCallback;

        startThread();
    }

    void run() override
    {
        auto& manager = InferenceManager::getInstance();
        if (!manager.isInitialized())
        {
            notifyCompletion({ false, "InferenceManager not initialized", {}, {} });
            return;
        }

        float f0Start = baseProgress_;
        float f0End = 1.0f;

        updateProgress(f0Start, "Extracting F0...");

        // 分块 F0 处理
        std::vector<float> totalF0;
        // 根据时长预分配 F0 空间（hop size 512）
        int estimatedFrames = static_cast<int>(audioBuffer_.size()) / 512 + 100;
        totalF0.reserve(estimatedFrames);

        // 每块 10 秒
        int chunkSizeSamples = sampleRate_ * 10;
        int totalSamples = static_cast<int>(audioBuffer_.size());
        int processedSamples = 0;

        while (processedSamples < totalSamples)
        {
            // 检查取消请求
            if (threadShouldExit()) return;

            int currentChunkSize = std::min(chunkSizeSamples, totalSamples - processedSamples);
            
            // 计算当前块进度
            float chunkProgress = static_cast<float>(processedSamples) / static_cast<float>(totalSamples);
            float currentProgress = f0Start + chunkProgress * (f0End - f0Start);
            updateProgress(currentProgress, "Extracting F0 (" + juce::String(static_cast<int>(chunkProgress * 100)) + "%)...");

            std::vector<float> chunkF0 = manager.extractF0(
                audioBuffer_.data() + processedSamples,
                currentChunkSize,
                sampleRate_
            );
            
            // 追加块 F0 到总结果
            totalF0.insert(totalF0.end(), chunkF0.begin(), chunkF0.end());

            // 通知部分结果（使用默认置信度 1.0）
            std::vector<float> partialConfidences(totalF0.size(), 1.0f);
            notifyPartialResult(totalF0, partialConfidences);

            processedSamples += currentChunkSize;
        }
        
        if (totalF0.empty())
        {
             notifyCompletion({ false, "F0 extraction failed", {}, {} });
             return;
        }

        std::vector<Note> notes; // 当前版本暂不支持音符输出

        updateProgress(1.0f, "Analysis complete");
        notifyCompletion({ true, "", totalF0, notes });
    }

    void cancelProcess()
    {
        stopThread(2000);
    }

private:
    void updateProgress(float progress, const juce::String& status)
    {
        if (progressCallback_)
        {
            auto token = validityToken_;
            juce::MessageManager::callAsync([token, progress, status, callback = progressCallback_]()
            {
                if (token && *token && callback)
                {
                    callback(progress, status);
                }
            });
        }
    }

    void notifyPartialResult(const std::vector<float>& f0, const std::vector<float>& confidences)
    {
        if (partialCallback_)
        {
            auto token = validityToken_;
            // 使用 shared_ptr 避免在 UI 线程复制大对象
            auto f0Ptr = std::make_shared<std::vector<float>>(f0);
            auto confPtr = std::make_shared<std::vector<float>>(confidences);
            
            juce::MessageManager::callAsync([token, f0Ptr, confPtr, callback = partialCallback_]()
            {
                if (token && *token && callback)
                {
                    callback(*f0Ptr, *confPtr);
                }
            });
        }
    }

    void notifyCompletion(const InferenceResult& result)
    {
        if (completionCallback_)
        {
            auto token = validityToken_;
            juce::MessageManager::callAsync([token, result, callback = completionCallback_]()
            {
                if (token && *token && callback)
                {
                    callback(result);
                }
            });
        }
    }

    std::vector<float> audioBuffer_;
    int sampleRate_ = 44100;
    float baseProgress_ = 0.0f;
    ProgressCallback progressCallback_;
    CompletionCallback completionCallback_;
    PartialResultCallback partialCallback_;
    std::shared_ptr<std::atomic<bool>> validityToken_;
};

} // namespace OpenTune
