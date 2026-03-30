#pragma once

/**
 * 异步音频加载器
 * 
 * 在后台线程中异步加载音频文件，避免阻塞UI线程。
 * 支持进度回调和完成回调，可通过有效性令牌安全取消。
 */

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <atomic>
#include <memory>

namespace OpenTune {

/**
 * AsyncAudioLoader - 异步音频加载器类
 * 
 * 继承自juce::Thread，在独立线程中执行音频文件加载。
 */
class AsyncAudioLoader : public juce::Thread
{
public:
    /**
     * LoadResult - 加载结果结构
     */
    struct LoadResult
    {
        bool success;                       // 是否成功
        juce::String errorMessage;          // 错误信息
        juce::AudioBuffer<float> audioBuffer;   // 音频缓冲区
        double sampleRate;                  // 采样率
    };

    /**
     * 进度回调类型
     * @param progress 进度（0.0-1.0）
     * @param status 状态描述
     */
    using ProgressCallback = std::function<void(float, const juce::String&)>;
    
    /**
     * 完成回调类型
     * @param result 加载结果
     */
    using CompletionCallback = std::function<void(LoadResult)>;

    AsyncAudioLoader() : juce::Thread("AudioLoaderThread") 
    {
        validityToken_ = std::make_shared<std::atomic<bool>>(true);
    }

    ~AsyncAudioLoader() override
    {
        *validityToken_ = false;
        stopThread(2000);
    }

    /**
     * 异步加载音频文件
     * @param file 要加载的文件
     * @param progressCallback 进度回调
     * @param completionCallback 完成回调
     */
    void loadAudioFile(const juce::File& file,
                       ProgressCallback progressCallback,
                       CompletionCallback completionCallback)
    {
        stopThread(2000);

        fileToLoad_ = file;
        progressCallback_ = progressCallback;
        completionCallback_ = completionCallback;

        startThread();
    }

    /**
     * 线程执行函数
     */
    void run() override
    {
        updateProgress(0.0f, juce::String::fromUTF8(u8"正在打开文件..."));

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(fileToLoad_));

        if (reader == nullptr)
        {
            const auto fileExt = fileToLoad_.getFileExtension().toLowerCase();
            const auto supportedWildcard = formatManager.getWildcardForAllFormats().replaceCharacters("*", "");

            juce::String detail = juce::String::fromUTF8(u8"无法打开该音频文件。\n");
            detail += juce::String::fromUTF8(u8"文件：") + fileToLoad_.getFileName() + "\n";
            if (fileExt.isNotEmpty())
                detail += juce::String::fromUTF8(u8"扩展名：") + fileExt + "\n";

            if (supportedWildcard.isNotEmpty())
                detail += juce::String::fromUTF8(u8"当前支持：") + supportedWildcard;
            else
                detail += juce::String::fromUTF8(u8"当前环境未注册可用音频解码器。");

            notifyCompletion({ false, detail, {}, 0.0 });
            return;
        }

        updateProgress(0.1f, juce::String::fromUTF8(u8"正在读取音频数据..."));

        juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels),
                                        static_cast<int>(reader->lengthInSamples));

        if (reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true))
        {
            updateProgress(1.0f, juce::String::fromUTF8(u8"加载完成"));
            notifyCompletion({ true, "", buffer, reader->sampleRate });
        }
        else
        {
            notifyCompletion({ false, juce::String::fromUTF8(u8"读取音频数据失败，文件可能损坏或编码不受支持。"), {}, 0.0 });
        }
    }

    /**
     * 取消加载
     */
    void cancelLoad()
    {
        stopThread(2000);
    }

private:
    /**
     * 更新进度（线程安全）
     */
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

    /**
     * 通知完成（线程安全）
     */
    void notifyCompletion(const LoadResult& result)
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

    juce::File fileToLoad_;                             // 要加载的文件
    ProgressCallback progressCallback_;                 // 进度回调
    CompletionCallback completionCallback_;             // 完成回调
    std::shared_ptr<std::atomic<bool>> validityToken_;  // 有效性令牌（用于安全取消）
};

} // namespace OpenTune
