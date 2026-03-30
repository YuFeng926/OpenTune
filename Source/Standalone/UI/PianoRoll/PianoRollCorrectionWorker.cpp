#include "PianoRollCorrectionWorker.h"
#include "../../../Utils/AppLogger.h"

namespace OpenTune {

// ============================================================================
// PianoRollCorrectionWorker - 音高修正异步处理工作器
// ============================================================================

PianoRollCorrectionWorker::PianoRollCorrectionWorker()
    : workerThread_([this] { workerLoop(); })
{
    AppLogger::debug("[PianoRollCorrectionWorker] Worker thread started");
}

PianoRollCorrectionWorker::~PianoRollCorrectionWorker()
{
    AppLogger::debug("[PianoRollCorrectionWorker] Stopping worker thread...");
    stop();
    AppLogger::debug("[PianoRollCorrectionWorker] Worker thread stopped");
}

void PianoRollCorrectionWorker::enqueue(RequestPtr request)
{
    if (!request || !request->curve) {
        AppLogger::warn("[PianoRollCorrectionWorker] Enqueue rejected: request or curve is null");
        return;
    }

    AppLogger::debug("[PianoRollCorrectionWorker] Enqueuing request: kind=" + juce::String(static_cast<int>(request->kind))
        + ", autoStartFrame=" + juce::String(request->autoStartFrame)
        + ", autoEndFrame=" + juce::String(request->autoEndFrame)
        + ", startFrame=" + juce::String(request->startFrame)
        + ", endFrame=" + juce::String(request->endFrameExclusive)
        + ", releaseAutoTuneOnDiscard=" + juce::String(request->releaseAutoTuneOnDiscard ? "true" : "false"));

    request->version = incrementVersion();

    RequestPtr oldReq;
    {
        std::lock_guard<std::mutex> lock(pendingRequestMutex_);
        oldReq = std::move(pendingRequest_);
        pendingRequest_ = std::move(request);
        hasPendingRequest_.store(true, std::memory_order_release);
    }

    if (oldReq && oldReq->releaseAutoTuneOnDiscard) {
        AppLogger::debug("[PianoRollCorrectionWorker] Discarding previous AutoTune request");
        isAutoTuneProcessing_.store(false, std::memory_order_release);
    }
}

void PianoRollCorrectionWorker::stop()
{
    AppLogger::debug("[PianoRollCorrectionWorker] Stop signal received");
    stopFlag_.store(true, std::memory_order_release);
    if (workerThread_.joinable()) {
        workerThread_.join();
        AppLogger::debug("[PianoRollCorrectionWorker] Worker thread joined");
    }
}

void PianoRollCorrectionWorker::setClipContext(int trackId, uint64_t clipId)
{
    AppLogger::debug("[PianoRollCorrectionWorker] Setting clip context: trackId=" + juce::String(trackId)
        + ", clipId=" + juce::String(static_cast<int64_t>(clipId)));
    currentTrackId_.store(trackId, std::memory_order_release);
    currentClipId_.store(clipId, std::memory_order_release);
    clipContextGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

void PianoRollCorrectionWorker::getClipContext(int& trackId, uint64_t& clipId) const
{
    trackId = currentTrackId_.load(std::memory_order_acquire);
    clipId = currentClipId_.load(std::memory_order_acquire);
}

void PianoRollCorrectionWorker::executeRequest(AsyncCorrectionRequest& request)
{
    AppLogger::debug("[PianoRollCorrectionWorker] Executing request: kind=" + juce::String(static_cast<int>(request.kind)));

    switch (request.kind)
    {
        case AsyncCorrectionRequest::Kind::ApplyNoteRange:
        {
            AppLogger::debug("[PianoRollCorrectionWorker] ApplyNoteRange: startFrame=" + juce::String(request.startFrame)
                + ", endFrame=" + juce::String(request.endFrameExclusive)
                + ", noteCount=" + juce::String(static_cast<int>(request.notes.size()))
                + ", retuneSpeed=" + juce::String(request.retuneSpeed)
                + ", vibratoDepth=" + juce::String(request.vibratoDepth)
                + ", vibratoRate=" + juce::String(request.vibratoRate));
            
            PerfTimer timer("[PianoRollCorrectionWorker] ApplyNoteRange execution");
            request.curve->applyCorrectionToRange(
                request.notes,
                request.startFrame,
                request.endFrameExclusive,
                request.retuneSpeed,
                request.vibratoDepth,
                request.vibratoRate,
                request.audioSampleRate
            );
            AppLogger::debug("[PianoRollCorrectionWorker] ApplyNoteRange completed");
            break;
        }

        case AsyncCorrectionRequest::Kind::ManualPatch:
        {
            AppLogger::debug("[PianoRollCorrectionWorker] ManualPatch: opCount=" + juce::String(static_cast<int>(request.manualOps.size())));
            
            PerfTimer timer("[PianoRollCorrectionWorker] ManualPatch execution");
            for (const auto& op : request.manualOps)
            {
                if (op.endFrameExclusive <= op.startFrame) {
                    AppLogger::warn("[PianoRollCorrectionWorker] Skipping invalid ManualOp: startFrame=" 
                        + juce::String(op.startFrame) + ", endFrame=" + juce::String(op.endFrameExclusive));
                    continue;
                }

                AppLogger::debug("[PianoRollCorrectionWorker]   ManualOp: type=" + juce::String(static_cast<int>(op.type))
                    + ", startFrame=" + juce::String(op.startFrame)
                    + ", endFrame=" + juce::String(op.endFrameExclusive)
                    + ", f0DataSize=" + juce::String(static_cast<int>(op.f0Data.size()))
                    + ", source=" + juce::String(static_cast<int>(op.source)));

                if (op.type == AsyncCorrectionRequest::ManualOp::Type::ClearRange)
                {
                    request.curve->clearCorrectionRange(op.startFrame, op.endFrameExclusive);
                }
                else
                {
                    request.curve->setManualCorrectionRange(
                        op.startFrame, op.endFrameExclusive, op.f0Data, op.source, op.retuneSpeed);
                }
            }
            AppLogger::debug("[PianoRollCorrectionWorker] ManualPatch completed");
            break;
        }

        case AsyncCorrectionRequest::Kind::AutoTuneGenerate:
        {
            AppLogger::debug("[PianoRollCorrectionWorker] AutoTuneGenerate: autoStartFrame=" + juce::String(request.autoStartFrame)
                + ", autoEndFrame=" + juce::String(request.autoEndFrame)
                + ", autoOriginalF0Full.size=" + juce::String(static_cast<int>(request.autoOriginalF0Full.size()))
                + ", autoHopSize=" + juce::String(request.autoHopSize)
                + ", autoF0SampleRate=" + juce::String(request.autoF0SampleRate)
                + ", retuneSpeed=" + juce::String(request.retuneSpeed));
            
            PerfTimer timer("[PianoRollCorrectionWorker] AutoTuneGenerate execution");

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

            AppLogger::debug("[PianoRollCorrectionWorker] NoteGenerator::generate returned: noteCount=" 
                + juce::String(static_cast<int>(request.notes.size())));

            NoteGenerator::validate(request.notes);

            PerfTimer timer2("[PianoRollCorrectionWorker] applyCorrectionToRange (AutoTune)");
            request.curve->applyCorrectionToRange(
                request.notes,
                request.autoStartFrame,
                request.autoEndFrame + 1,
                request.retuneSpeed,
                request.vibratoDepth,
                request.vibratoRate,
                request.audioSampleRate
            );
            AppLogger::debug("[PianoRollCorrectionWorker] AutoTuneGenerate completed");
            break;
        }
    }
}

void PianoRollCorrectionWorker::workerLoop()
{
    AppLogger::debug("[PianoRollCorrectionWorker] Worker loop started");
    
    while (!stopFlag_.load(std::memory_order_acquire))
    {
        RequestPtr reqSharedPtr;
        {
            std::lock_guard<std::mutex> lock(pendingRequestMutex_);
            if (pendingRequest_) {
                reqSharedPtr = std::move(pendingRequest_);
                pendingRequest_.reset();
                hasPendingRequest_.store(false, std::memory_order_release);
            }
        }
        
        if (!reqSharedPtr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;  // 无请求，跳过循环
        }

        AppLogger::debug("[PianoRollCorrectionWorker] Worker loop got request: kind=" + juce::String(static_cast<int>(reqSharedPtr->kind))
            + ", version=" + juce::String(reqSharedPtr->version));

        // 创建共享所有权，确保请求在回调完成前一直有效
        auto requestPtr = std::make_shared<AsyncCorrectionRequest>(std::move(*reqSharedPtr));

        // RAII 守卫确保在任何退出路径都会触发回调
        CallbackGuard guard([this, requestPtr](bool success) {
            AppLogger::debug("[PianoRollCorrectionWorker] Callback invoked: success=" + juce::String(success ? "true" : "false")
                + ", errorKind=" + juce::String(static_cast<int>(requestPtr->errorKind))
                + ", errorMessage=" + juce::String(requestPtr->errorMessage.c_str()));
            if (!applyCallback_) return;
            requestPtr->success = success;
            
            juce::MessageManager::callAsync([this, cb = applyCallback_, requestPtr]() {
                if (stopFlag_.load(std::memory_order_acquire)) {
                    AppLogger::debug("[PianoRollCorrectionWorker] Skipping callback due to stop flag");
                    return;
                }
                AppLogger::debug("[PianoRollCorrectionWorker] Posting callback to MessageManager");
                isAutoTuneProcessing_.store(false, std::memory_order_release);
                if (requestPtr->onRenderComplete) {
                    requestPtr->onRenderComplete();
                }
                cb(requestPtr);
            });
        });

        // === 提前退出检查（守卫析构时会调用回调，参数为 false） ===

        // 无效范围检查
        if (requestPtr->kind == AsyncCorrectionRequest::Kind::AutoTuneGenerate) {
            if (requestPtr->autoEndFrame <= requestPtr->autoStartFrame) {
                AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: InvalidRange (autoEndFrame <= autoStartFrame)");
                requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::InvalidRange;
                requestPtr->errorMessage = "Invalid range: autoEndFrame <= autoStartFrame";
                continue;  // 守卫调用回调(false)
            }
        } else {
            if (requestPtr->endFrameExclusive <= requestPtr->startFrame) {
                AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: InvalidRange (endFrameExclusive <= startFrame)");
                requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::InvalidRange;
                requestPtr->errorMessage = "Invalid range: endFrameExclusive <= startFrame";
                continue;  // 守卫调用回调(false)
            }
        }

        // 版本不匹配检查
        if (requestPtr->version != getVersion()) {
            AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: VersionMismatch (request=" 
                + juce::String(requestPtr->version) + ", current=" + juce::String(getVersion()) + ")");
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::VersionMismatch;
            requestPtr->errorMessage = "Version mismatch: request version outdated";
            continue;  // 守卫调用回调(false)
        }

        // 片段上下文不匹配检查
        if (requestPtr->releaseAutoTuneOnDiscard && requestPtr->clipContextGenerationSnapshot != 0) {
            const uint64_t currentGeneration = getClipContextGeneration();
            int currentTrackId;
            uint64_t currentClipId;
            getClipContext(currentTrackId, currentClipId);

            if (requestPtr->clipContextGenerationSnapshot != currentGeneration ||
                requestPtr->trackIdSnapshot != currentTrackId ||
                requestPtr->clipIdSnapshot != currentClipId) {
                AppLogger::warn("[PianoRollCorrectionWorker] Request rejected: ClipContextMismatch "
                    "(expected: trackId=" + juce::String(requestPtr->trackIdSnapshot) 
                    + ", clipId=" + juce::String(static_cast<int64_t>(requestPtr->clipIdSnapshot))
                    + ", gen=" + juce::String(static_cast<int64_t>(requestPtr->clipContextGenerationSnapshot))
                    + ", actual: trackId=" + juce::String(currentTrackId)
                    + ", clipId=" + juce::String(static_cast<int64_t>(currentClipId))
                    + ", gen=" + juce::String(static_cast<int64_t>(currentGeneration)) + ")");
                requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::ClipContextMismatch;
                requestPtr->errorMessage = "Clip context mismatch: user switched clips";
                continue;  // 守卫调用回调(false)
            }
        }

        // === 异常处理执行 ===
        try {
            PerfTimer totalTimer("[PianoRollCorrectionWorker] Total request execution");
            executeRequest(*requestPtr);
            guard.success();  // 显式成功，立即调用回调(true)
            AppLogger::debug("[PianoRollCorrectionWorker] Request execution completed successfully");
            
        } catch (const std::exception& e) {
            AppLogger::error("[PianoRollCorrectionWorker] Exception during execution: " + juce::String(e.what()));
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::ExecutionError;
            requestPtr->errorMessage = e.what();
            // 守卫析构时会调用回调(false)
            
        } catch (...) {
            AppLogger::error("[PianoRollCorrectionWorker] Unknown exception during execution");
            requestPtr->errorKind = AsyncCorrectionRequest::ErrorKind::ExecutionError;
            requestPtr->errorMessage = "Unknown exception during execution";
            // 守卫析构时会调用回调(false)
        }
    }
    
    AppLogger::debug("[PianoRollCorrectionWorker] Worker loop exiting");
}

} // namespace OpenTune
