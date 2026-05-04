#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "Utils/LockFreeQueue.h"
#include "Utils/PitchCurve.h"
#include "Utils/Note.h"
#include "Utils/PitchUtils.h"
#include "ARA/OpenTuneDocumentController.h"
#include "ARA/VST3AraSession.h"
#include "MaterializationStore.h"
#include "StandaloneArrangement.h"
#include "PluginProcessor.h"
#include "Inference/RenderCache.h"
#include "Inference/VocoderRenderScheduler.h"
#include "Inference/F0InferenceService.h"
#include "Editor/AutoRenderOverlayComponent.h"
#include "Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h"
#include "Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h"
#include "Standalone/UI/PianoRollComponent.h"
#include "Standalone/UI/PlayheadOverlayComponent.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {
int64_t mapPlaybackTimeToSourceSample(double playbackTimeSeconds,
                                      double playbackStartSeconds,
                                      double playbackEndSeconds,
                                      double sourceStartSeconds,
                                      double sourceEndSeconds,
                                      double sourceSampleRate,
                                      int64_t sourceTotalSamples);

int computeSourceSamplesForHostBlock(int hostSamples,
                                     double hostSampleRate,
                                     double sourceSampleRate);

#if defined(OPENTUNE_TEST_BUILD)
struct VST3AraSessionTestProbe {
    static void seedSinglePublishedRegion(VST3AraSession& session,
                                          juce::ARAAudioSource* audioSource,
                                          juce::ARAPlaybackRegion* playbackRegion,
                                          uint64_t materializationId)
    {
        const std::lock_guard<std::mutex> lock(session.stateMutex_);

        auto& sourceSlot = session.sources_[audioSource];
        sourceSlot.audioSource = audioSource;
        sourceSlot.sourceId = 1;
        sourceSlot.sampleRate = 44100.0;
        sourceSlot.numChannels = 1;
        sourceSlot.numSamples = 128;
        sourceSlot.contentRevision = 1;
        sourceSlot.hydratedContentRevision = 0;

        auto& regionSlot = session.regions_[playbackRegion];
        regionSlot.identity.audioSource = audioSource;
        regionSlot.identity.playbackRegion = playbackRegion;
        regionSlot.appliedProjection.sourceId = sourceSlot.sourceId;
        regionSlot.appliedProjection.materializationId = materializationId;
        regionSlot.appliedProjection.appliedMaterializationRevision = sourceSlot.contentRevision;
        regionSlot.appliedProjection.appliedProjectionRevision = 1;
        regionSlot.appliedProjection.appliedSourceWindow = SourceWindow{sourceSlot.sourceId, 0.0, 1.0};
        regionSlot.appliedProjection.playbackStartSeconds = 0.0;
        regionSlot.appliedProjection.appliedRegionIdentity = regionSlot.identity;
        regionSlot.playbackStartSeconds = 0.0;
        regionSlot.playbackEndSeconds = 1.0;
        regionSlot.sourceWindow = SourceWindow{sourceSlot.sourceId, 0.0, 1.0};
        regionSlot.materializationDurationSeconds = 1.0;
        regionSlot.projectionRevision = 1;

        session.preferredRegion_ = regionSlot.identity;
        session.pendingSnapshotPublication_ = true;
        session.publishSnapshotLocked();
    }
};
#endif

struct PianoRollComponentTestProbe {
    static bool hasPendingVisualInvalidation(const PianoRollComponent& pianoRoll)
    {
        return pianoRoll.pendingVisualInvalidation_.hasWork();
    }

    static double lastVisualFlushMs(const PianoRollComponent& pianoRoll)
    {
        return pianoRoll.lastVisualFlushMs_;
    }

    static juce::Rectangle<int> getTimelineViewportBounds(const PianoRollComponent& pianoRoll)
    {
        return pianoRoll.getTimelineViewportBounds();
    }

    static PianoRollRenderer::RenderContext buildRenderContext(const PianoRollComponent& pianoRoll)
    {
        return pianoRoll.buildRenderContext();
    }

    static MaterializationTimelineProjection getMaterializationProjection(const PianoRollComponent& pianoRoll)
    {
        return pianoRoll.materializationProjection_;
    }

    static juce::Rectangle<int> getNoteBounds(const PianoRollComponent& pianoRoll, const Note& note)
    {
        return pianoRoll.getNoteBounds(note);
    }

    static int timeToX(const PianoRollComponent& pianoRoll, double seconds)
    {
        return pianoRoll.timeToX(seconds);
    }
};

}

using namespace OpenTune;

extern std::atomic<bool> gHasTestFailure;

void logPass(const char* testName);
void logFail(const char* testName, const char* detail);
void logSection(const char* section);

bool approxEqual(float a, float b, float tol = 1e-5f);
bool approxEqual(double a, double b, double tol = 1e-9);

MaterializationStore::CreateMaterializationRequest makeTestClipRequest();
bool seedPublishedIdleChunk(RenderCache& cache,
                             double startSeconds,
                             double endSeconds,
                             std::vector<float> audio);

void runCoreBehaviorSuite();
void runProcessorBehaviorSuite();
void runUiBehaviorSuite();
void runArchitectureBehaviorSuite();
void runUndoManagerSuite();
void runMemoryOptimizationSuite();

