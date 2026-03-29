#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/NoteGenerator.h"
#include "Utils/UndoAction.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
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
            ManualPatch,
            AutoTuneGenerate
        };

        /**
         * 手动操作结构体
         * 用于描述单次手动修正操作
         */
        struct ManualOp
        {
            enum class Type
            {
                ClearRange,
                SetManualRange
            };

            Type type = Type::ClearRange;
            int startFrame = 0;
            int endFrameExclusive = 0;
            std::vector<float> f0Data;
            CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
            float retuneSpeed = -1.0f;
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
        std::vector<ManualOp> manualOps;
        uint64_t version = 0;
        bool releaseAutoTuneOnDiscard = false;
        std::function<void()> onApplied;
        std::function<void()> onRenderComplete;
        int autoHopSize = 160;
        double autoF0SampleRate = 16000.0;
        int autoStartFrame = 0;
        int autoEndFrame = 0;
        double autoStartTime = 0.0;
        double autoEndTime = 0.0;
        NoteGeneratorParams autoGenParams;
        std::vector<float> autoOriginalF0Full;
        uint64_t clipContextGenerationSnapshot = 0;
        int trackIdSnapshot = -1;
        uint64_t clipIdSnapshot = 0;

        std::vector<Note> autoNotesBefore;
        std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot> autoF0Before;

        enum class ErrorKind {
            None,
            InvalidRange,
            VersionMismatch,
            ClipContextMismatch,
            ExecutionError
        };

        bool success = false;
        std::string errorMessage;
        ErrorKind errorKind = ErrorKind::None;
    };

    using RequestPtr = std::shared_ptr<AsyncCorrectionRequest>;
    using ApplyCallback = std::function<void(RequestPtr)>;

    PianoRollCorrectionWorker();
    ~PianoRollCorrectionWorker();

    void enqueue(RequestPtr request);
    void stop();

    void setVersion(uint64_t version) { version_.store(version, std::memory_order_release); }
    uint64_t getVersion() const { return version_.load(std::memory_order_acquire); }
    uint64_t incrementVersion() { return version_.fetch_add(1, std::memory_order_acq_rel) + 1; }

    void setClipContextGeneration(uint64_t gen) { clipContextGeneration_.store(gen, std::memory_order_release); }
    uint64_t getClipContextGeneration() const { return clipContextGeneration_.load(std::memory_order_acquire); }

    void setClipContext(int trackId, uint64_t clipId);
    void getClipContext(int& trackId, uint64_t& clipId) const;

    void setIsAutoTuneProcessing(bool processing) { isAutoTuneProcessing_.store(processing, std::memory_order_release); }
    bool isAutoTuneProcessing() const { return isAutoTuneProcessing_.load(std::memory_order_acquire); }

    void setApplyCallback(ApplyCallback callback) { applyCallback_ = std::move(callback); }

    static void executeRequest(AsyncCorrectionRequest& request);

private:
    void workerLoop();

    std::shared_ptr<AsyncCorrectionRequest> pendingRequest_;
    std::mutex pendingRequestMutex_;
    std::atomic<bool> hasPendingRequest_{false};
    std::atomic<bool> stopFlag_{false};
    std::thread workerThread_;
    std::atomic<uint64_t> version_{0};
    std::atomic<uint64_t> clipContextGeneration_{0};
    std::atomic<int> currentTrackId_{-1};
    std::atomic<uint64_t> currentClipId_{0};
    std::atomic<bool> isAutoTuneProcessing_{false};
    ApplyCallback applyCallback_;
};

/**
 * RAII 回调守卫，确保在任何作用域退出时都会调用回调。
 * 
 * 用于保证即使发生提前返回或异常，回调也能被正确触发。
 */
class CallbackGuard {
public:
    using Callback = std::function<void(bool success)>;
    
    explicit CallbackGuard(Callback cb) 
        : callback_(std::move(cb)), 
          success_(false),
          invoked_(false) {}
    
    ~CallbackGuard() {
        if (!invoked_ && callback_) {
            invoked_ = true;
            callback_(success_);
        }
    }
    
    void success() {
        success_ = true;
        invoke();
    }
    
    void fail() {
        success_ = false;
        invoke();
    }
    
    void disarm() {
        invoked_ = true;
    }
    
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
    CallbackGuard(CallbackGuard&&) = delete;
    CallbackGuard& operator=(CallbackGuard&&) = delete;
    
private:
    void invoke() {
        if (!invoked_ && callback_) {
            invoked_ = true;
            callback_(success_);
        }
    }
    
    Callback callback_;
    bool success_;
    bool invoked_;
};

} // namespace OpenTune