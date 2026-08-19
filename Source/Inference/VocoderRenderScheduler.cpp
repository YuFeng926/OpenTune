#include "VocoderRenderScheduler.h"
#include "VocoderInferenceService.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

VocoderRenderScheduler::VocoderRenderScheduler() = default;

VocoderRenderScheduler::~VocoderRenderScheduler() {
    shutdown();
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
    acceptingJobs_.store(false, std::memory_order_release);

    // Terminate the worker's in-flight synthesize() Run. Without this the
    // worker can sit inside an unbounded DML Run and join() below freezes
    // (REAPER unload deadlock). The scheduler is never reused after shutdown,
    // so the terminate flag is never cleared.
    runOptions_.SetTerminate();
    queueCV_.notify_one();
    
    if (worker_ && worker_->joinable()) {
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
                    callback(JobResult::Cancelled, "Scheduler shutdown", {});
                });
        }
        while (!completionQueue_.empty())
        {
            completions.push_back(std::move(completionQueue_.front()));
            completionQueue_.pop_front();
        }
    }
    for (auto& completion : completions)
        completion();
}

bool VocoderRenderScheduler::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!acceptingJobs_.load())
            return false;

        if (static_cast<int>(jobQueue_.size()) >= kMaxQueueDepth) {
            auto discarded = std::move(jobQueue_.front());
            jobQueue_.pop_front();
            if (discarded.onComplete)
                completionQueue_.push_back([callback = std::move(discarded.onComplete)] {
                    callback(JobResult::Cancelled, "Queue overflow: job discarded", {});
                });
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
                    || !completionQueue_.empty()
                    || !acceptingJobs_.load(std::memory_order_acquire);
            });
            
            shutdownRequested = !acceptingJobs_.load(std::memory_order_acquire);
            
            if (!completionQueue_.empty()) {
                auto completion = std::move(completionQueue_.front());
                completionQueue_.pop_front();
                lock.unlock();
                completion();
                continue;
            }

            if (!jobQueue_.empty()) {
                job = std::move(jobQueue_.front());
                jobQueue_.pop_front();
            }
        }
        
        if (job.onComplete) {
            if (shutdownRequested) {
                job.onComplete(JobResult::Cancelled, "Scheduler shutdown", {});
            } else {
                AppLogger::log("VocoderTrace: dequeued job f0_frames=" + juce::String(job.f0.size()));
                AppLogger::log("VocoderTrace: run start");
                
                if (service_) {
                    auto result = service_->synthesize(
                        job.f0,
                        job.mel.empty() ? nullptr : job.mel.data(), job.mel.size(),
                        runOptions_);
                    
                    AppLogger::log("VocoderTrace: run end");
                    
                    if (result.ok()) {
                        AppLogger::log("VocoderTrace: synthesis complete samples=" + juce::String(result.value().size()));
                        job.onComplete(JobResult::Succeeded, "", result.value());
                    } else {
                        const auto resultType = acceptingJobs_.load(std::memory_order_acquire)
                            ? JobResult::Failed
                            : JobResult::Cancelled;
                        job.onComplete(resultType, juce::String(result.error().fullMessage()), {});
                    }
                } else {
                    AppLogger::log("VocoderTrace: run end");
                    job.onComplete(JobResult::Failed, "Vocoder service not available", {});
                }
            }
        }
        
        if (shutdownRequested) {
            return;
        }
    }
}

} // namespace OpenTune
