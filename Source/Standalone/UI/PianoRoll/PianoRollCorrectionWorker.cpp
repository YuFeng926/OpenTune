#include "PianoRollCorrectionWorker.h"
#include "../../../Utils/AppLogger.h"

namespace OpenTune {

// ============================================================================
// PianoRollCorrectionWorker - 音高修正异步处理工作器
// ============================================================================

PianoRollCorrectionWorker::PianoRollCorrectionWorker()
    : workerThread_([this] { workerLoop(); })
{
}

PianoRollCorrectionWorker::~PianoRollCorrectionWorker()
{
    stop();
}

void PianoRollCorrectionWorker::enqueue(RequestPtr request)
{
    if (!request) {
        AppLogger::warn("[PianoRollCorrectionWorker] Enqueue rejected: request is null");
        return;
    }

    if (request->kind == AsyncCorrectionRequest::Kind::ApplyNoteRange && !request->curve) {
        AppLogger::warn("[PianoRollCorrectionWorker] Enqueue rejected: note-based correction requires curve");
        return;
    }

    request->version = incrementVersion();

    RequestPtr oldReq;
    {
        std::lock_guard<std::mutex> lock(pendingRequestMutex_);
        oldReq = std::move(pendingRequest_);
        pendingRequest_ = std::move(request);
    }
    pendingRequestCv_.notify_one();

    if (oldReq) {
        oldReq->success = false;
        oldReq->errorKind = AsyncCorrectionRequest::ErrorKind::VersionMismatch;
        oldReq->errorMessage = "Superseded by newer correction request";

        {
            std::lock_guard<std::mutex> lock(completedRequestMutex_);
            completedRequest_ = oldReq;
        }
    }
}

PianoRollCorrectionWorker::RequestPtr PianoRollCorrectionWorker::takeCompleted()
{
    std::lock_guard<std::mutex> lock(completedRequestMutex_);
    auto completed = std::move(completedRequest_);
    completedRequest_.reset();
    return completed;
}

void PianoRollCorrectionWorker::stop()
{
    stopFlag_.store(true, std::memory_order_release);
    pendingRequestCv_.notify_one();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void PianoRollCorrectionWorker::executeRequest(AsyncCorrectionRequest& request)
{
    switch (request.kind)
    {
        case AsyncCorrectionRequest::Kind::ApplyNoteRange:
        {
            request.curve->applyCorrectionToRange(
                request.notes,
                request.startFrame,
                request.endFrameExclusive,
                request.retuneSpeed,
                request.vibratoDepth,
                request.vibratoRate,
                request.audioSampleRate
            );
            break;
        }

        case AsyncCorrectionRequest::Kind::AutoTuneGenerate:
        {
            request.notes = NoteGenerator::generate(
                request.autoOriginalF0Full.data(),
                static_cast<int>(request.autoOriginalF0Full.size()),
                nullptr,
                request.autoStartFrame,
                request.autoEndFrame + 1,
                request.autoHopSize,
                request.autoF0SampleRate,
                request.audioSampleRate,
                request.autoGenParams);

            NoteGenerator::validate(request.notes);
            break;
        }
    }
}

void PianoRollCorrectionWorker::workerLoop()
{
    while (!stopFlag_.load(std::memory_order_acquire))
    {
        RequestPtr reqSharedPtr;
        {
            std::unique_lock<std::mutex> lock(pendingRequestMutex_);
            pendingRequestCv_.wait(lock, [this]() {
                return stopFlag_.load(std::memory_order_acquire) || pendingRequest_ != nullptr;
            });
            if (stopFlag_.load(std::memory_order_acquire)) {
                break;
            }
            reqSharedPtr = std::move(pendingRequest_);
            pendingRequest_.reset();
        }

        // 创建共享所有权，确保请求处理结束后结果可由主线程拉取
        auto requestPtr = std::make_shared<AsyncCorrectionRequest>(std::move(*reqSharedPtr));

        auto publishResult = [this, &requestPtr](bool success) {
            requestPtr->success = success;
            std::lock_guard<std::mutex> lock(completedRequestMutex_);
            completedRequest_ = requestPtr;
        };

        // === 提前退出检查 ===

        // 无效范围检查
        if (requestPtr->kind == AsyncCorrectionRequest::Kind::AutoTuneGenerate) {
            if (requestPtr->autoEndFrame <= requestPtr->autoStartFrame) {
                AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: InvalidRange (autoEndFrame <= autoStartFrame)");
                requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::InvalidRange;
                requestPtr->errorMessage = "Invalid range: autoEndFrame <= autoStartFrame";
                publishResult(false);
                continue;
            }
        } else {
            if (requestPtr->endFrameExclusive <= requestPtr->startFrame) {
                AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: InvalidRange (endFrameExclusive <= startFrame)");
                requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::InvalidRange;
                requestPtr->errorMessage = "Invalid range: endFrameExclusive <= startFrame";
                publishResult(false);
                continue;
            }
        }

        // 版本不匹配检查
        if (requestPtr->version != getVersion()) {
            AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: VersionMismatch (request=" 
                + juce::String(requestPtr->version) + ", current=" + juce::String(getVersion()) + ")");
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::VersionMismatch;
            requestPtr->errorMessage = "Version mismatch: request version outdated";
            publishResult(false);
            continue;
        }

        // === 异常处理执行 ===
        try {
            executeRequest(*requestPtr);
            publishResult(true);
            
        } catch (const std::exception& e) {
            AppLogger::error("[PianoRollCorrectionWorker] Exception during execution: " + juce::String(e.what()));
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::ExecutionError;
            requestPtr->errorMessage = e.what();
            publishResult(false);
            
        } catch (...) {
            AppLogger::error("[PianoRollCorrectionWorker] Unknown exception during execution");
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::ExecutionError;
            requestPtr->errorMessage = "Unknown exception during execution";
            publishResult(false);
        }
    }
}

} // namespace OpenTune
