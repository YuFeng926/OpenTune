#include "Stage2TimeStretchRebuilder.h"

#include "ContentRenderService.h"
#include "PlaybackReadSource.h"

#include "../Inference/SoundTouchStretcher.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/AppLogger.h"
#include "../Utils/PlaybackAudioReader.h"

#include <algorithm>
#include <vector>

namespace OpenTune {

/**
 * Stage2 rebuild — consumes only the immutable snapshot/audio carried by the
 * request. Stage1 canonical PCM is read through a source-domain request that
 * carries only the audio and RenderCache needed by readCanonicalAudio.
 */
bool Stage2TimeStretchRebuilder::rebuild(ContentRenderService& crs,
                                          const Request& request)
{
    const auto contentKey = request.contentKey;
    if (!contentKey.isValid()) return false;

    if (!request.contentSnapshot
        || !request.audioBuffer
        || request.audioBuffer->getNumChannels() <= 0
        || request.audioBuffer->getNumSamples() <= 0
        || request.audioSampleRate <= 0.0) {
        AppLogger::warn("Stage2: invalid request inputs for contentKey objectId="
                        + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        return false;
    }

    const auto& snapshot = *request.contentSnapshot;

    // 同 revision 的合法 republish 会换 snapshot 指针，指针相等不是新鲜度判据。
    // 只比较 CRS 已发布 snapshot 的 revision；getPlaybackReadSource 成功即遵守
    // makePlaybackReadSource 合同（contentSnapshot/timeGrid 非空），不再重复设防。
    const auto isCurrentPublishedRequest = [&]() {
        PlaybackReadSource published;
        return crs.getPlaybackReadSource(contentKey, published)
            && published.contentSnapshot != nullptr
            && published.contentSnapshot->contentRevision == snapshot.contentRevision
            && published.contentSnapshot->timeGridRevision == snapshot.timeGridRevision
            && published.contentSnapshot->audioRevision == snapshot.audioRevision
            && published.audioBuffer == request.audioBuffer;
    };

    if (!isCurrentPublishedRequest())
        return false;

    // identity TimeGrid 不需要 Stage2；mutation 已在发布新 snapshot 时失效旧输出。
    if (snapshot.timeGrid->isIdentity()) {
        return true;
    }

    const double sampleRate = request.audioSampleRate;
    SoundTouchStretcher* stretcher = crs.getStretcher(contentKey, sampleRate, 1);
    if (stretcher == nullptr) return false;

    auto schedule = stretcher->buildTempoScheduleFromTimeGrid(*snapshot.timeGrid);
    stretcher->beginRebuild(schedule);

    const auto stage1RenderCache = crs.getRenderCache(contentKey);

    const int totalSamples = request.audioBuffer->getNumSamples();
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

        ::OpenTune::CanonicalReadRequest req(request.audioBuffer,
                                              stage1RenderCache,
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

    const uint64_t contentRev = snapshot.contentRevision;
    const uint64_t timeGridRev = snapshot.timeGridRevision;

    if (!isCurrentPublishedRequest())
        return false;

    crs.getTimeStretchCache().store(contentKey,
                                    std::move(output),
                                    contentRev,
                                    timeGridRev,
                                    sampleRate,
                                    buildGen);

    AppLogger::log("Stage2Worker: rebuilt objectId="
                   + juce::String(static_cast<juce::int64>(contentKey.objectId))
                   + " contentRev=" + juce::String(static_cast<juce::int64>(contentRev))
                   + " timeGridRev=" + juce::String(static_cast<juce::int64>(timeGridRev))
                   + " stage1InputSamples=" + juce::String(totalSamples)
                   + " stage2OutputSamples=" + juce::String(static_cast<int>(stretcher->expectedOutputSamples()))
                   + " (SoundTouch WSOLA, Stage 1 via readCanonicalAudio)");
    return true;
}

} // namespace OpenTune
