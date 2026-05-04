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
    queueCV_.notify_one();
    
    if (worker_ && worker_->joinable()) {
        worker_->join();
    }
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!jobQueue_.empty()) {
            auto job = std::move(jobQueue_.front());
            jobQueue_.pop_front();
            if (job.onComplete) {
                job.onComplete(false, "Scheduler shutdown", {});
            }
        }
    }
}

void VocoderRenderScheduler::submit(Job job) {
    std::function<void(bool, const juce::String&, const std::vector<float>&)> supersededCallback;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!acceptingJobs_.load()) return;

        // 同 chunkKey 替换
        bool replaced = false;
        if (job.chunkKey != 0) {
            for (auto& existing : jobQueue_) {
                if (existing.chunkKey == job.chunkKey) {
                    supersededCallback = std::move(existing.onComplete);
                    existing = std::move(job);
                    replaced = true;
                    break;
                }
            }
        }

        if (!replaced) {
            if (static_cast<int>(jobQueue_.size()) >= kMaxQueueDepth) {
                auto discarded = std::move(jobQueue_.front());
                jobQueue_.pop_front();
                if (discarded.onComplete) {
                    discarded.onComplete(false, "Queue overflow: job discarded", {});
                }
            }
            jobQueue_.push_back(std::move(job));
        }
    }

    // 旧 job 的 onComplete 在锁外执行，避免死锁
    if (supersededCallback) {
        supersededCallback(false, "Superseded by newer revision", {});
    }
    queueCV_.notify_one();
}

void VocoderRenderScheduler::workerThread() {
    while (true) {
        Job job;
        bool shutdownRequested = false;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] { 
                return !jobQueue_.empty() || !acceptingJobs_.load(std::memory_order_acquire);
            });
            
            shutdownRequested = !acceptingJobs_.load(std::memory_order_acquire);
            
            if (!jobQueue_.empty()) {
                job = std::move(jobQueue_.front());
                jobQueue_.pop_front();
            }
        }
        
        if (job.onComplete) {
            if (shutdownRequested) {
                job.onComplete(false, "Scheduler shutdown", {});
            } else {
                AppLogger::log("VocoderTrace: dequeued job f0_frames=" + juce::String(job.f0.size()));
                AppLogger::log("VocoderTrace: run start");
                
                if (service_) {
                    auto result = service_->synthesize(
                        job.f0,
                        job.mel.empty() ? nullptr : job.mel.data(), job.mel.size());
                    
                    AppLogger::log("VocoderTrace: run end");
                    
                    if (result.ok()) {
                        AppLogger::log("VocoderTrace: synthesis complete samples=" + juce::String(result.value().size()));
                        job.onComplete(true, "", result.value());
                    } else {
                        job.onComplete(false, juce::String(result.error().fullMessage()), {});
                    }
                } else {
                    AppLogger::log("VocoderTrace: run end");
                    job.onComplete(false, "Vocoder service not available", {});
                }
            }
        }
        
        if (shutdownRequested) {
            return;
        }
    }
}

} // namespace OpenTune
