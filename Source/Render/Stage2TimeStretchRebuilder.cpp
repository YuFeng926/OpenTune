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
 * The original implementation was a private method on the processor; this helper takes its
 * dependencies (CRS, owner snapshot) explicitly as parameters so the same algorithm can be
 * invoked from any domain owner (processor non-ARA path or ARA2 document controller).
 *
 * Behavior must be byte-identical to the original:
 *   - guard: invalid key, no snapshot, no source, empty audio, stale revisions
 *   - identity time-grid → invalidate cache, return true
 *   - otherwise: SoundTouch WSOLA time-stretch, write into CRS TimeStretchCache
 *     using snapshot revisions as the cache identity tuple.
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

    if (request.pitchRevision < ownerSnap->pitchRevision
        || request.pitchShiftRevision < ownerSnap->pitchShiftRevision
        || request.timeGridRevision < ownerSnap->timeGridRevision) {
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

    const int totalSamples = stage1Source.audioBuffer->getNumSamples();
    constexpr int kBlock = 4096;

    // Avoid double-processing: Stage2 reads Stage1's PlaybackReadSource via
    // readPlaybackAudio, which short-circuits through the time-stretch cache when
    // timeGridIsIdentity is false and a cache is set. Null the cache pointer and
    // force the identity path so Stage2 always consumes Stage1's underlying PCM.
    stage1Source.timeStretchCache = nullptr;
    stage1Source.timeGridIsIdentity = true;

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

        PlaybackReadRequest req(stage1Source,
                                static_cast<double>(offset) / sampleRate,
                                sampleRate,
                                n);

        const int wrote = readPlaybackAudio(req, readBuf, 0);
        const int actuallyWrote = juce::jlimit(0, n, wrote);

        for (int i = actuallyWrote; i < n; ++i) {
            const int srcIdx = offset + i;
            readBuf.setSample(0, i,
                              (srcIdx < totalSamples)
                                  ? stage1Source.audioBuffer->getSample(0, srcIdx)
                                  : 0.0f);
        }
        const bool isLast = (offset + n) >= totalSamples;
        stretcher->push(readBuf.getReadPointer(0), static_cast<size_t>(n), isLast);
        drainAvailable();
    }
    drainAvailable();

    // SoundTouch WSOLA may emit ±N samples of endpoint drift vs. the locked-endpoint
    // expected length.  Truncate or zero-pad so the cache entry has the canonical size.
    const size_t expectedSamples = stretcher->expectedOutputSamples();
    if (output.size() > expectedSamples) {
        output.resize(expectedSamples);
    } else if (output.size() < expectedSamples) {
        output.resize(expectedSamples, 0.0f);
    }

    const uint64_t pitchRev = ownerSnap->pitchRevision;
    const uint64_t pitchShiftRev = ownerSnap->pitchShiftRevision;
    const uint64_t timeGridRev = ownerSnap->timeGridRevision;

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
                   + " (SoundTouch WSOLA, Stage 1 via readPlaybackAudio dry+vocoder-overlay)");
    return true;
}

} // namespace OpenTune
