#include "VocoderDomain.h"
#include "VocoderInferenceService.h"
#include "VocoderRenderScheduler.h"
#include "../Utils/AppLogger.h"

namespace OpenTune {

VocoderDomain::VocoderDomain(std::shared_ptr<Ort::Env> env)
    : inferenceService_(std::make_unique<VocoderInferenceService>(std::move(env))) {}

VocoderDomain::~VocoderDomain() {
    shutdown();
}

bool VocoderDomain::initialize(const std::string& modelDir) {
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

void VocoderDomain::shutdown() {
    if (scheduler_) {
        scheduler_->shutdown();
        scheduler_.reset();
    }
    if (inferenceService_) {
        inferenceService_->shutdown();
    }
}

void VocoderDomain::submit(Job job) {
    if (!scheduler_) return;

    VocoderRenderScheduler::Job schedulerJob;
    schedulerJob.f0 = std::move(job.f0);
    schedulerJob.mel = std::move(job.mel);
    schedulerJob.chunkKey = job.chunkKey;
    schedulerJob.onComplete = std::move(job.onComplete);
    scheduler_->submit(std::move(schedulerJob));
}

int VocoderDomain::getVocoderHopSize() const {
    return inferenceService_ ? inferenceService_->getVocoderHopSize() : 0;
}

int VocoderDomain::getMelBins() const {
    return inferenceService_ ? inferenceService_->getMelBins() : 0;
}

} // namespace OpenTune
