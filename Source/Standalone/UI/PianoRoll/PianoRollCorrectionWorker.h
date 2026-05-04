#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/NoteGenerator.h"
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdint>

namespace OpenTune {

/**
 * 钢琴卷帘音高修正异步工作器
 * 在后台线程处理音高修正任务，支持音符范围修正、手动绘制修正和自动音调生成
 */
class PianoRollCorrectionWorker
{
public:
    /**
     * 异步修正请求结构体
     * 包含修正所需的所有参数和数据
     */
    struct AsyncCorrectionRequest
    {
        enum class Kind
        {
            ApplyNoteRange,
            AutoTuneGenerate
        };

        Kind kind = Kind::ApplyNoteRange;
        std::shared_ptr<PitchCurve> curve;
        std::vector<Note> notes;
        int startFrame = 0;
        int endFrameExclusive = 0;
        float retuneSpeed = 1.0f;
        float vibratoDepth = 0.0f;
        float vibratoRate = 5.0f;
        double audioSampleRate = 44100.0;
        uint64_t version = 0;
        int autoHopSize = 160;
        double autoF0SampleRate = 16000.0;
        int autoStartFrame = 0;
        int autoEndFrame = 0;
        NoteGeneratorParams autoGenParams;
        std::vector<float> autoOriginalF0Full;
        uint64_t materializationEpochSnapshot = 0;
        uint64_t materializationIdSnapshot = 0;

        enum class ErrorKind {
            None,
            InvalidRange,
            VersionMismatch,
            ExecutionError
        };

        bool success = false;
        std::string errorMessage;
        ErrorKind errorKind = ErrorKind::None;
    };

    using RequestPtr = std::shared_ptr<AsyncCorrectionRequest>;

    PianoRollCorrectionWorker();
    ~PianoRollCorrectionWorker();

    void enqueue(RequestPtr request);
    RequestPtr takeCompleted();
    void stop();

    static void executeRequest(AsyncCorrectionRequest& request);

private:
    void workerLoop();
    uint64_t getVersion() const { return version_.load(std::memory_order_acquire); }
    uint64_t incrementVersion() { return version_.fetch_add(1, std::memory_order_acq_rel) + 1; }

    std::shared_ptr<AsyncCorrectionRequest> pendingRequest_;
    std::mutex pendingRequestMutex_;
    std::condition_variable pendingRequestCv_;
    std::atomic<bool> stopFlag_{false};
    std::thread workerThread_;
    std::atomic<uint64_t> version_{0};
    RequestPtr completedRequest_;
    std::mutex completedRequestMutex_;
};

} // namespace OpenTune
