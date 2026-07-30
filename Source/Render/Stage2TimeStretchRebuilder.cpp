#include "Stage2TimeStretchRebuilder.h"

#include "ContentRenderService.h"
#include "PlaybackReadSource.h"

#include "../Inference/SoundTouchStretcher.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/AppLogger.h"
#include "../Utils/PlaybackAudioReader.h"
#include "../Utils/TimeCoordinate.h"

#include <algorithm>
#include <vector>

namespace OpenTune {

/**
 * Pure Stage2 rebuild — extracted from OpenTuneAudioProcessor::runStage2RebuildForContentKey.
 *
 * Reads Stage1 PlaybackReadSource via CanonicalReadRequest/readCanonicalAudio,
 * applies TimeGrid-based time stretch via SoundTouch, writes result into CRS TimeStretchCache.
 */
bool Stage2TimeStretchRebuilder::rebuild(ContentRenderService& crs,
                                          const Request& request,
                                          std::shared_ptr<const EditableContentSnapshot> ownerSnap)
{
    const auto contentKey = request.contentKey;
    if (!contentKey.isValid()) return false;

    if (!ownerSnap) {
        AppLogger::warn("Stage2: no owner snapshot for contentKey domain="
                        + juce::String(static_cast<int>(contentKey.domainKind))
                        + " objectId=" + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        return false;
    }

    PlaybackReadSource stage1Source;
    if (!crs.getPlaybackReadSource(contentKey, stage1Source)) {
        AppLogger::warn("Stage2: no CRS playback source for contentKey objectId="
                        + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        return false;
    }

    if (stage1Source.audioBuffer == nullptr
        || stage1Source.audioBuffer->getNumChannels() <= 0
        || stage1Source.audioBuffer->getNumSamples() <= 0) {
        AppLogger::warn("Stage2: playback source has no valid audio buffer for objectId="
                        + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        return false;
    }

    if (request.pitchRevision != ownerSnap->pitchRevision
        || request.pitchShiftRevision != ownerSnap->pitchShiftRevision
        || request.timeGridRevision != ownerSnap->timeGridRevision) {
        return false;
    }

    if (ownerSnap->timeGrid == nullptr || ownerSnap->timeGrid->isIdentity()) {
        crs.getTimeStretchCache().invalidate(contentKey);
        return true;
    }

    constexpr double sampleRate = TimeCoordinate::kRenderSampleRate;
    SoundTouchStretcher* stretcher = crs.getStretcher(contentKey, sampleRate, 1);
    if (stretcher == nullptr) return false;

    auto schedule = stretcher->buildTempoScheduleFromTimeGrid(*ownerSnap->timeGrid);
    stretcher->beginRebuild(schedule);

    // Stage2 must read Stage1 raw PCM, never its own cached TimeStretch output.
    stage1Source.timeStretchCache = nullptr;
    stage1Source.timeGridIsIdentity = true;

    const int totalSamples = stage1Source.audioBuffer->getNumSamples();
    constexpr int kBlock = 4096;

    juce::AudioBuffer<float> readBuf(1, kBlock);

    const uint32_t buildGen = crs.getTimeStretchCache().beginBuild(contentKey);

    std::vector<float> output;
    output.reserve(static_cast<size_t>(totalSamples + kBlock));
    std::vector<float> retrieveBuf(static_cast<size_t>(kBlock));

    auto drainAvailable = [&]() {
        while (true) {
            const size_t avail = stretcher->available();
            if (avail == 0) break;
            const size_t want = std::min<size_t>(avail, retrieveBuf.size());
            const size_t got = stretcher->pull(retrieveBuf.data(), want);
            if (got == 0) break;
            output.insert(output.end(),
                          retrieveBuf.begin(),
                          retrieveBuf.begin() + static_cast<std::ptrdiff_t>(got));
        }
    };

    for (int offset = 0; offset < totalSamples; offset += kBlock) {
        const int n = std::min(kBlock, totalSamples - offset);
        readBuf.clear(0, 0, n);

        ::OpenTune::CanonicalReadRequest req(stage1Source,
                                 static_cast<int64_t>(offset),
                                 n);

        const int wrote = readCanonicalAudio(req, readBuf, 0);

        // Read insufficient → rebuild failure (no manual dry fallback)
        if (wrote < n) {
            AppLogger::warn("Stage2: canonical read insufficient at offset="
                            + juce::String(offset) + " wanted=" + juce::String(n)
                            + " got=" + juce::String(wrote));
            return false;
        }

        const bool isLast = (offset + n) >= totalSamples;
        stretcher->push(readBuf.getReadPointer(0), static_cast<size_t>(n), isLast);
        drainAvailable();
    }
    drainAvailable();

    // SoundTouch WSOLA may emit ±N samples of endpoint drift vs. the endpoint-invariant
    // expected length.  Truncate or zero-pad so the cache entry has the canonical size.
    const size_t expectedSamples = stretcher->expectedOutputSamples();
    if (output.size() > expectedSamples) {
        output.resize(expectedSamples);
    } else if (output.size() < expectedSamples) {
        output.resize(expectedSamples, 0.0f);
    }

    const uint64_t pitchRev = request.pitchRevision;
    const uint64_t pitchShiftRev = request.pitchShiftRevision;
    const uint64_t timeGridRev = request.timeGridRevision;

    crs.getTimeStretchCache().store(contentKey,
                                    std::move(output),
                                    pitchRev,
                                    pitchShiftRev,
                                    timeGridRev,
                                    sampleRate,
                                    buildGen);

    AppLogger::log("Stage2Worker: rebuilt objectId="
                   + juce::String(static_cast<juce::int64>(contentKey.objectId))
                   + " timeGridRev=" + juce::String(static_cast<juce::int64>(timeGridRev))
                   + " stage1InputSamples=" + juce::String(totalSamples)
                   + " stage2OutputSamples=" + juce::String(static_cast<int>(stretcher->expectedOutputSamples()))
                   + " (SoundTouch WSOLA, Stage 1 via readCanonicalAudio)");
    return true;
}

} // namespace OpenTune
