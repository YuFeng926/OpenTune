#include "VocoderDomain.h"
#include "VocoderInferenceService.h"
#include "VocoderRenderScheduler.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

class VocoderDomain::Impl {
public:
    Impl() : inferenceService_(std::make_unique<VocoderInferenceService>()) {}
    
    bool initialize(const std::string& modelDir) {
        if (!inferenceService_->initialize(modelDir)) {
            AppLogger::error("[VocoderDomain] Failed to initialize inference service");
            return false;
        }
        
        scheduler_ = std::make_unique<VocoderRenderScheduler>();
        if (!scheduler_->initialize(inferenceService_.get())) {
            AppLogger::error("[VocoderDomain] Failed to initialize scheduler");
            scheduler_.reset();
            inferenceService_->shutdown();
            return false;
        }
        
        AppLogger::info("[VocoderDomain] Initialized successfully");
        return true;
    }
    
    void shutdown() {
        if (scheduler_) {
            scheduler_->shutdown();
            scheduler_.reset();
        }
        if (inferenceService_) {
            inferenceService_->shutdown();
        }
        AppLogger::info("[VocoderDomain] Shutdown complete");
    }
    
    void submit(Job job) {
        if (scheduler_) {
            VocoderRenderScheduler::Job schedulerJob;
            schedulerJob.f0 = std::move(job.f0);
            schedulerJob.energy = std::move(job.energy);
            schedulerJob.mel = std::move(job.mel);
            schedulerJob.onComplete = std::move(job.onComplete);
            scheduler_->submit(std::move(schedulerJob));
        }
    }
    
    int getQueueDepth() const {
        return scheduler_ ? scheduler_->getQueueDepth() : 0;
    }
    
    bool isRunning() const {
        return scheduler_ && scheduler_->isRunning();
    }
    
    bool isInitialized() const {
        return inferenceService_ && inferenceService_->isInitialized();
    }
    
    int getVocoderHopSize() const {
        return inferenceService_ ? inferenceService_->getVocoderHopSize() : 0;
    }
    
    int getMelBins() const {
        return inferenceService_ ? inferenceService_->getMelBins() : 0;
    }
    
private:
    std::unique_ptr<VocoderInferenceService> inferenceService_;
    std::unique_ptr<VocoderRenderScheduler> scheduler_;
};

VocoderDomain::VocoderDomain() : pImpl_(std::make_unique<Impl>()) {}

VocoderDomain::~VocoderDomain() {
    if (pImpl_) {
        pImpl_->shutdown();
    }
}

bool VocoderDomain::initialize(const std::string& modelDir) {
    return pImpl_->initialize(modelDir);
}

void VocoderDomain::shutdown() {
    pImpl_->shutdown();
}

void VocoderDomain::submit(Job job) {
    pImpl_->submit(std::move(job));
}

int VocoderDomain::getQueueDepth() const {
    return pImpl_->getQueueDepth();
}

bool VocoderDomain::isRunning() const {
    return pImpl_->isRunning();
}

bool VocoderDomain::isInitialized() const {
    return pImpl_->isInitialized();
}

int VocoderDomain::getVocoderHopSize() const {
    return pImpl_->getVocoderHopSize();
}

int VocoderDomain::getMelBins() const {
    return pImpl_->getMelBins();
}

} // namespace OpenTune
