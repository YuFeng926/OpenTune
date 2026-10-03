#include "VocoderRenderScheduler.h"
#include "VocoderInferenceService.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

VocoderRenderScheduler::VocoderRenderScheduler() = default;

VocoderRenderScheduler::~VocoderRenderScheduler() {
    shutdown();
}

bool VocoderRenderScheduler::isJobPayloadWithinLimit(
    std::size_t f0Capacity, std::size_t uvCapacity, std::size_t conditioningCapacity) noexcept {
    constexpr auto maxFloats = kMaxJobPayloadBytes / sizeof(float);
    if (f0Capacity > maxFloats)
        return false;
    const auto remainingAfterF0 = maxFloats - f0Capacity;
    if (uvCapacity > remainingAfterF0)
        return false;
    return conditioningCapacity <= remainingAfterF0 - uvCapacity;
}

VocoderRenderScheduler::InferenceFailure VocoderRenderScheduler::classifyInferenceError(
    const Error& error)
{
    return {
        error.code == ErrorCode::OperationCancelled ? JobResult::Cancelled : JobResult::Failed,
        error.fullMessage()
    };
}

bool VocoderRenderScheduler::initialize(VocoderInferenceService* service) {
    if (!service) {
        AppLogger::error("[VocoderRenderScheduler] Service is null");
        return false;
    }

    service_ = service;
    acceptingJobs_.store(true, std::memory_order_release);
    
    try {
        worker_ = std::make_unique<std::thread>([this] { workerThread(); });
        AppLogger::info("[VocoderRenderScheduler] Initialized with serial worker thread");
        return true;
    } catch (const std::exception& e) {
        AppLogger::error("[VocoderRenderScheduler] Failed to create worker thread: " + juce::String(e.what()));
        acceptingJobs_.store(false, std::memory_order_release);
        return false;
    }
}

void VocoderRenderScheduler::shutdown() {
    if (!acceptingJobs_.load(std::memory_order_acquire) && !worker_)
        return;
    acceptingJobs_.store(false, std::memory_order_release);

    // Terminate the worker's in-flight synthesize() Run. Without this the
    // worker can sit inside an unbounded DML Run and join() below freezes
    // (REAPER unload deadlock). The scheduler is never reused after shutdown,
    // so the terminate flag is never cleared.
    runOptions_.SetTerminate();
    queueCV_.notify_one();
    
    if (worker_ && worker_->joinable()) {
        if (worker_->get_id() == std::this_thread::get_id()) {
            AppLogger::error("[VocoderRenderScheduler] self-join rejected; hard failure");
            std::terminate();
        }
        worker_->join();
    }
    
    std::deque<std::function<void()>> completions;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!jobQueue_.empty()) {
            auto job = std::move(jobQueue_.front());
            jobQueue_.pop_front();
            if (job.onComplete)
                completions.push_back([callback = std::move(job.onComplete)] {
                    try { callback(JobResult::Cancelled, "Scheduler shutdown", {}); }
                    catch (const std::exception& e) {
                        AppLogger::error("[VocoderRenderScheduler] shutdown completion threw: " + juce::String(e.what()));
                    }
                    catch (...) {
                        AppLogger::error("[VocoderRenderScheduler] shutdown completion threw unknown exception");
                    }
                });
        }
    }
    for (auto& completion : completions)
    {
        try { completion(); }
        catch (const std::exception& e) {
            AppLogger::error("[VocoderRenderScheduler] queued completion threw: " + juce::String(e.what()));
        }
        catch (...) {
            AppLogger::error("[VocoderRenderScheduler] queued completion threw unknown exception");
        }
    }
}

bool VocoderRenderScheduler::submit(Job job) {
    if (!isJobPayloadWithinLimit(job.f0.capacity(), job.uv.capacity(), job.conditioning.capacity()))
        return false;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!acceptingJobs_.load())
            return false;

        if (static_cast<int>(jobQueue_.size()) >= kMaxQueueDepth) {
            return false;
        }
        jobQueue_.push_back(std::move(job));
    }

    queueCV_.notify_one();
    return true;
}

void VocoderRenderScheduler::workerThread() {
    while (true) {
        Job job;
        bool shutdownRequested = false;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] { 
                return !jobQueue_.empty()
                    || !acceptingJobs_.load(std::memory_order_acquire);
            });
            
            shutdownRequested = !acceptingJobs_.load(std::memory_order_acquire);
            
            if (!jobQueue_.empty()) {
                job = std::move(jobQueue_.front());
                jobQueue_.pop_front();
            }
        }
        
        if (job.onComplete) {
            JobResult resultType = JobResult::Cancelled;
            juce::String resultError = "Scheduler shutdown";
            std::vector<float> resultAudio;

            if (!shutdownRequested) {
                try {
                    AppLogger::log("VocoderTrace: dequeued job f0_frames=" + juce::String(job.f0.size()));
                    AppLogger::log("VocoderTrace: run start");
                    if (service_) {
                        auto result = service_->synthesize(
                            job.f0,
                            job.uv,
                            job.conditioning.empty() ? nullptr : job.conditioning.data(), job.conditioning.size(),
                            runOptions_);
                        AppLogger::log("VocoderTrace: run end");
                        const bool accepting = acceptingJobs_.load(std::memory_order_acquire);
                        if (result.ok()) {
                            if (accepting) {
                                resultType = JobResult::Succeeded;
                                resultError.clear();
                                resultAudio = result.value();
                            } else {
                                resultType = JobResult::Cancelled;
                                resultError = "Scheduler shutdown discarded completed inference";
                            }
                        } else {
                            auto failure = classifyInferenceError(result.error());
                            resultType = failure.result;
                            resultError = std::move(failure.reason);
                        }
                    } else {
                        resultType = JobResult::Failed;
                        resultError = "Vocoder service not available";
                    }
                } catch (const std::exception& e) {
                    resultType = JobResult::Failed;
                    resultError = e.what();
                } catch (...) {
                    resultType = JobResult::Failed;
                    resultError = "Unknown vocoder exception";
                }
            }

            try {
                job.onComplete(resultType, resultError, resultAudio);
            } catch (const std::exception& e) {
                AppLogger::error("[VocoderRenderScheduler] job completion threw: " + juce::String(e.what()));
            } catch (...) {
                AppLogger::error("[VocoderRenderScheduler] job completion threw unknown exception");
            }
        }
        
        if (shutdownRequested) {
            return;
        }
    }
}

} // namespace OpenTune
