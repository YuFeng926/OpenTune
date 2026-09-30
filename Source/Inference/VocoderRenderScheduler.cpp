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
            return;
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
        while (!completionQueue_.empty())
        {
            completions.push_back(std::move(completionQueue_.front()));
            completionQueue_.pop_front();
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

std::size_t VocoderRenderScheduler::jobQueueDepth() const noexcept {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return jobQueue_.size();
}

std::size_t VocoderRenderScheduler::completionQueueDepth() const noexcept {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return completionQueue_.size();
}

bool VocoderRenderScheduler::isAcceptingJobs() const noexcept {
    return acceptingJobs_.load(std::memory_order_acquire);
}

bool VocoderRenderScheduler::isWorkerJoinable() const noexcept {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return worker_ != nullptr && worker_->joinable();
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
                try { completion(); }
                catch (const std::exception& e) {
                    AppLogger::error("[VocoderRenderScheduler] queued completion threw: " + juce::String(e.what()));
                }
                catch (...) {
                    AppLogger::error("[VocoderRenderScheduler] queued completion threw unknown exception");
                }
                continue;
            }

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
                        if (result.ok()
                            && acceptingJobs_.load(std::memory_order_acquire)) {
                            resultType = JobResult::Succeeded;
                            resultError.clear();
                            resultAudio = result.value();
                        } else {
                            resultType = acceptingJobs_.load(std::memory_order_acquire)
                                ? JobResult::Failed : JobResult::Cancelled;
                            resultError = juce::String(result.error().fullMessage());
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
