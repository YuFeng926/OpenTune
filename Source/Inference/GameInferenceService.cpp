#include "GameInferenceService.h"
#include "GameExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AppLogger.h"
#include <onnxruntime_cxx_api.h>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace OpenTune {

// ============================================================================
// Helper: slice global knownDurations into chunk-local durations
// ============================================================================
namespace {

/**
 * Slice global VUV knownDurations to a chunk time window [chunkStart, chunkEnd).
 * Segments crossing chunk boundaries are split. Sum of result == chunkEnd - chunkStart.
 */
std::vector<float> sliceKnownDurations(
    const std::vector<float>& globalDurations,
    double chunkStartSec,
    double chunkEndSec)
{
    std::vector<float> result;
    double cursor = 0.0;

    for (size_t i = 0; i < globalDurations.size(); ++i) {
        const double segStart = cursor;
        const double segEnd = cursor + static_cast<double>(globalDurations[i]);
        cursor = segEnd;

        // Skip segments entirely before chunk
        if (segEnd <= chunkStartSec) continue;
        // Stop after chunk
        if (segStart >= chunkEndSec) break;

        // Clip to chunk window
        const double clippedStart = std::max(segStart, chunkStartSec);
        const double clippedEnd = std::min(segEnd, chunkEndSec);
        const double dur = clippedEnd - clippedStart;
        if (dur > 1e-6) {
            result.push_back(static_cast<float>(dur));
        }
    }

    // Ensure sum equals chunk duration
    if (result.empty()) {
        result.push_back(static_cast<float>(chunkEndSec - chunkStartSec));
    } else {
        double sum = 0.0;
        for (auto d : result) sum += d;
        const double target = chunkEndSec - chunkStartSec;
        if (std::abs(sum - target) > 1e-6) {
            result.back() += static_cast<float>(target - sum);
            if (result.back() < 1e-6f) result.back() = 1e-6f;
        }
    }

    return result;
}

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

class GameInferenceService::Impl {
public:
    Impl(std::shared_ptr<Ort::Env> env) : env_(std::move(env)) {
        resamplingManager_ = std::make_shared<ResamplingManager>();
    }

    ~Impl() { shutdown(); }

    bool initialize(const std::string& modelDir) {
        modelDir_ = modelDir;
        initialized_.store(true, std::memory_order_release);
        AppLogger::info("[GameInferenceService] Initialized with model dir: " + juce::String(modelDir));
        return true;
    }

    void shutdown() {
        initialized_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(extractorMutex_);
        extractor_.reset();
    }

    Result<std::vector<ReferenceNote>> extractReferenceNotes(
        const float* audio, size_t length, int sampleRate,
        const std::vector<float>& knownDurations,
        const GameConfig& config,
        std::function<void(float)> progressCallback)
    {
        if (!initialized_.load(std::memory_order_acquire)) {
            return Result<std::vector<ReferenceNote>>::failure(
                ErrorCode::NotInitialized, "GameInferenceService not initialized");
        }

        // Hold lock for entire extraction to prevent idle release mid-operation (C2 fix)
        std::lock_guard<std::mutex> lock(extractorMutex_);

        // Ensure extractor is loaded
        if (!extractor_) {
            extractor_ = std::make_unique<GameExtractor>(*env_);
            if (!extractor_->loadSessions(modelDir_)) {
                extractor_.reset();
                return Result<std::vector<ReferenceNote>>::failure(
                    ErrorCode::ModelLoadFailed, "Failed to load GAME sessions");
            }
        }

        try {
            // Resample to 44100Hz if needed
            std::vector<float> resampled;
            const float* audioPtr = audio;
            size_t audioLength = length;

            if (sampleRate != 44100) {
                if (sampleRate > 44100) {
                    resampled = resamplingManager_->downsampleForInference(audio, length, sampleRate, 44100);
                } else {
                    resampled = resamplingManager_->upsampleForHost(audio, length, sampleRate, 44100);
                }
                audioPtr = resampled.data();
                audioLength = resampled.size();
            }

            const double totalDuration = static_cast<double>(audioLength) / 44100.0;

            // Chunking: 60s windows + 5s overlap
            constexpr double kChunkDuration = 60.0;
            constexpr double kOverlap = 5.0;
            constexpr int kSR = 44100;

            std::vector<ReferenceNote> allNotes;

            if (totalDuration <= kChunkDuration + kOverlap) {
                // Single chunk — no splitting needed
                auto result = extractor_->extractChunk(audioPtr, audioLength, knownDurations, config, progressCallback);
                allNotes = std::move(result.notes);
            } else {
                // Multi-chunk
                const int chunkSamples = static_cast<int>(kChunkDuration * kSR);
                const int overlapSamples = static_cast<int>(kOverlap * kSR);
                const int stepSamples = chunkSamples - overlapSamples;
                const int numChunks = static_cast<int>(std::ceil(
                    static_cast<double>(static_cast<int>(audioLength) - overlapSamples) / stepSamples));

                for (int c = 0; c < numChunks; ++c) {
                    const size_t startSample = static_cast<size_t>(c) * stepSamples;
                    const size_t endSample = std::min(startSample + static_cast<size_t>(chunkSamples), audioLength);
                    const size_t chunkLen = endSample - startSample;
                    const double chunkStartSec = static_cast<double>(startSample) / kSR;
                    const double chunkEndSec = static_cast<double>(endSample) / kSR;

                    // C1 fix: properly slice VUV knownDurations to chunk-local window
                    auto chunkDurations = sliceKnownDurations(knownDurations, chunkStartSec, chunkEndSec);

                    auto result = extractor_->extractChunk(
                        audioPtr + startSample, chunkLen, chunkDurations, config, nullptr);

                    // Offset notes to global time
                    for (auto& note : result.notes) {
                        note.onset += chunkStartSec;
                        note.offset += chunkStartSec;
                    }

                    // I3 fix: keep only notes from the "owned" region of each chunk.
                    // First chunk owns [0, chunkEnd - overlap/2).
                    // Middle chunks own [chunkStart + overlap/2, chunkEnd - overlap/2).
                    // Last chunk owns [chunkStart + overlap/2, end).
                    const double ownedStart = (c == 0) ? 0.0 : chunkStartSec + kOverlap / 2.0;
                    const double ownedEnd = (c == numChunks - 1) ? totalDuration : chunkEndSec - kOverlap / 2.0;

                    for (auto& note : result.notes) {
                        if (note.onset >= ownedStart && note.onset < ownedEnd) {
                            allNotes.push_back(note);
                        }
                    }

                    if (progressCallback) {
                        progressCallback(static_cast<float>(c + 1) / static_cast<float>(numChunks));
                    }
                }
            }

            // Sort by onset
            std::sort(allNotes.begin(), allNotes.end(),
                [](const ReferenceNote& a, const ReferenceNote& b) { return a.onset < b.onset; });

            lastExtractionTimeMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_release);
            return Result<std::vector<ReferenceNote>>::success(std::move(allNotes));

        } catch (const std::exception& e) {
            AppLogger::error("[GameInferenceService] Extraction failed: " + juce::String(e.what()));
            return Result<std::vector<ReferenceNote>>::failure(
                ErrorCode::ModelInferenceFailed, std::string(e.what()));
        }
    }

    bool isInitialized() const { return initialized_.load(std::memory_order_acquire); }

    void releaseIdleModelIfNeeded() {
        // C3 fix: check initialized_ before attempting release
        if (!initialized_.load(std::memory_order_acquire)) return;
        const uint64_t lastTime = lastExtractionTimeMs_.load(std::memory_order_acquire);
        if (lastTime == 0) return;
        const uint64_t now = juce::Time::getMillisecondCounter();
        if (now - lastTime >= kModelRetentionMs) {
            AppLogger::info("[GameInferenceService] Releasing idle GAME model after 30s");
            std::lock_guard<std::mutex> lock(extractorMutex_);
            extractor_.reset();
            lastExtractionTimeMs_.store(0, std::memory_order_release);
        }
    }

private:
    std::shared_ptr<Ort::Env> env_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<GameExtractor> extractor_;
    std::string modelDir_;
    std::mutex extractorMutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> lastExtractionTimeMs_{0};
    static constexpr uint64_t kModelRetentionMs = 30000;
};

// Forwarding
GameInferenceService::GameInferenceService(std::shared_ptr<Ort::Env> env)
    : pImpl_(std::make_unique<Impl>(std::move(env))) {}
GameInferenceService::~GameInferenceService() = default;

bool GameInferenceService::initialize(const std::string& modelDir) { return pImpl_->initialize(modelDir); }
void GameInferenceService::shutdown() { pImpl_->shutdown(); }

Result<std::vector<ReferenceNote>> GameInferenceService::extractReferenceNotes(
    const float* audio, size_t length, int sampleRate,
    const std::vector<float>& knownDurations, const GameConfig& config,
    std::function<void(float)> progressCallback)
{
    return pImpl_->extractReferenceNotes(audio, length, sampleRate, knownDurations, config, progressCallback);
}

bool GameInferenceService::isInitialized() const { return pImpl_->isInitialized(); }
void GameInferenceService::releaseIdleModelIfNeeded() { if (pImpl_) pImpl_->releaseIdleModelIfNeeded(); }

} // namespace OpenTune
