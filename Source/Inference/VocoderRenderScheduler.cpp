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
            jobQueue_.pop();
            if (job.onComplete) {
                job.onComplete(false, "Scheduler shutdown", {});
            }
        }
    }
}

void VocoderRenderScheduler::submit(Job job) {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        jobQueue_.push(std::move(job));
    }
    queueCV_.notify_one();
}

int VocoderRenderScheduler::getQueueDepth() const {
    std::unique_lock<std::mutex> lock(queueMutex_);
    return static_cast<int>(jobQueue_.size());
}

bool VocoderRenderScheduler::isRunning() const {
    return acceptingJobs_.load(std::memory_order_acquire);
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
                jobQueue_.pop();
            }
        }
        
        if (job.onComplete) {
            if (shutdownRequested) {
                job.onComplete(false, "Scheduler shutdown", {});
            } else {
                AppLogger::log("VocoderTrace: dequeued job f0_frames=" + juce::String(job.f0.size()));
                AppLogger::log("VocoderTrace: run start");
                
                if (service_) {
                    auto result = service_->synthesizeAudioWithEnergy(
                        job.f0, job.energy, 
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
