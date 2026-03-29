#include "RenderingManager.h"
#include "Utils/AppLogger.h"
#include <algorithm>

namespace OpenTune {

RenderingManager::RenderingManager()
{
}

RenderingManager::~RenderingManager() {
    shutdown();
}

void RenderingManager::initialize() {
    running_ = true;
    const int numWorkers = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    AppLogger::log("RenderingManager: initializing with " + juce::String(numWorkers) + " workers");
    for (int i = 0; i < numWorkers; ++i) {
        workers_.push_back(std::make_unique<Worker>(*this, "RenderWorker" + juce::String(i + 1)));
    }
}

void RenderingManager::shutdown() {
    running_ = false;
    for (auto& w : workers_) {
        if (w) w->notify();
    }
    for (auto& w : workers_) {
        if (w) w->stopThread(2000);
    }
    workers_.clear();
}

void RenderingManager::submit(std::shared_ptr<const ChunkInputs> inputs,
                              std::vector<float> f0,
                              std::shared_ptr<RenderCache> cache,
                              std::function<void(bool isError)> onDone) {
    if (!inputs || !cache || f0.empty()) {
        AppLogger::log("RenderTrace: RenderingManager::submit early-return inputs=" + juce::String(inputs ? "1" : "0")
            + " cache=" + juce::String(cache ? "1" : "0") + " f0.size=" + juce::String(static_cast<int>(f0.size())));
        if (onDone) onDone(true);
        return;
    }
    
    AppLogger::log("RenderTrace: RenderingManager::submit enqueuing task startSec=" + juce::String(inputs->startSeconds, 3)
        + " lengthSec=" + juce::String(inputs->lengthSeconds, 3)
        + " numFrames=" + juce::String(inputs->numFrames)
        + " f0.size=" + juce::String(static_cast<int>(f0.size())));
    
    RenderTask task;
    task.cache = cache;
    task.inputs = inputs;
    task.f0 = std::move(f0);
    task.onDone = std::move(onDone);
    
    queue_.try_enqueue(std::move(task));
    notifyWorkers();
}

bool RenderingManager::isRendering() const {
    return activeCount_.load() > 0;
}

RenderingManager::Worker::Worker(RenderingManager& owner, const juce::String& name)
    : juce::Thread(name), owner_(owner) {
    startThread(juce::Thread::Priority::normal);
}

void RenderingManager::Worker::run() {
    AppLogger::log("RenderTrace: RenderingManager::Worker started");
    while (!threadShouldExit()) {
        RenderTask task;
        if (owner_.queue_.try_dequeue(task)) {
            AppLogger::log("RenderTrace: RenderingManager::Worker dequeued task");
            owner_.activeCount_.fetch_add(1);
            owner_.process(task);
            owner_.activeCount_.fetch_sub(1);
            AppLogger::log("RenderTrace: RenderingManager::Worker processed task");
        } else {
            wait(50);
        }
    }
    AppLogger::log("RenderTrace: RenderingManager::Worker exiting");
}

void RenderingManager::process(RenderTask& task) {
    if (!task.cache || !task.inputs) {
        AppLogger::log("RenderTrace: RenderingManager::process early-return cache=" + juce::String(task.cache ? "1" : "0")
            + " inputs=" + juce::String(task.inputs ? "1" : "0"));
        if (task.onDone) task.onDone(true);
        return;
    }
    
    auto& inference = InferenceManager::getInstance();
    
    const auto& inputs = *task.inputs;
    // mel.size() / 128 = mel 帧数（每个帧 128 个 mel bins）
    const int melFrames = static_cast<int>(inputs.mel.size() / 128);
    if (melFrames <= 0) {
        AppLogger::log("RenderTrace: RenderingManager::process early-return melFrames="
            + juce::String(melFrames));
        if (task.onDone) task.onDone(true);
        return;
    }
    
    AppLogger::log("RenderTrace: RenderingManager::process synthesizing audio melFrames=" + juce::String(melFrames));
    
    std::vector<float> audio = inference.synthesizeAudioWithEnergy(
        task.f0, inputs.energy, inputs.mel.data());
    
    if (audio.empty()) {
        AppLogger::log("RenderTrace: RenderingManager::process synthesizing returned empty audio");
        if (task.onDone) task.onDone(true);
        return;
    }
    
    AppLogger::log("RenderTrace: RenderingManager::process got audio size=" + juce::String(static_cast<int>(audio.size())));
    
    const int64_t targetSamples = inputs.targetSamples;
    const int64_t outputSamples = static_cast<int64_t>(audio.size());
    
    if (targetSamples <= 0) {
        AppLogger::log("RenderTrace: RenderingManager::process invalid targetSamples=" + juce::String(static_cast<int>(targetSamples)));
        if (task.onDone) task.onDone(true);
        return;
    }
    
    if (targetSamples > outputSamples) {
        AppLogger::log("RenderTrace: RenderingManager::process targetSamples exceeds output outputSamples="
            + juce::String(static_cast<int>(outputSamples))
            + " targetSamples=" + juce::String(static_cast<int>(targetSamples)));
        if (task.onDone) task.onDone(true);
        return;
    }
    
    std::vector<float> cropped(audio.begin(), audio.begin() + targetSamples);
    
    const bool published = task.cache->addChunk(inputs.startSeconds, inputs.endSeconds, std::move(cropped), inputs.targetRevision);
    AppLogger::log("RenderTrace: RenderingManager::process added chunk to cache startSec=" + juce::String(inputs.startSeconds, 3)
        + " lengthSec=" + juce::String(inputs.lengthSeconds, 3)
        + " outputSamples=" + juce::String(static_cast<int>(outputSamples))
        + " croppedSamples=" + juce::String(static_cast<int>(targetSamples))
        + " targetRevision=" + juce::String(static_cast<juce::int64>(inputs.targetRevision))
        + " published=" + juce::String(published ? 1 : 0));
    
    if (published && inputs.targetSampleRate != static_cast<int>(RenderCache::kSampleRate)) {
        std::vector<float> audioForResample;
        audioForResample.assign(audio.begin(), audio.begin() + targetSamples);
        
        ResamplingManager resampler;
        std::vector<float> resampled = resampler.resample(
            audioForResample, 
            RenderCache::kSampleRate, 
            static_cast<double>(inputs.targetSampleRate));
        
        if (!resampled.empty()) {
            task.cache->addResampledChunk(
                inputs.startSeconds, 
                inputs.endSeconds, 
                inputs.targetSampleRate, 
                std::move(resampled), 
                inputs.targetRevision);
            AppLogger::log("RenderTrace: RenderingManager::process added resampled chunk rate=" 
                + juce::String(inputs.targetSampleRate) 
                + " size=" + juce::String(static_cast<int>(resampled.size())));
        }
    }
    
    if (task.onDone) task.onDone(false);
}

void RenderingManager::notifyWorkers() {
    for (auto& w : workers_) {
        if (w) w->notify();
    }
}

} // namespace OpenTune
