#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

namespace OpenTune {

class ContentRenderService;

/**
 * Stage2 time-stretch rebuilder — pure function of request-carried immutable inputs.
 *
 * Consumes request.contentSnapshot (timeGrid + revisions) and request.audioBuffer
 * (Stage1 canonical PCM). Reads Stage1 via source-domain readCanonicalAudio,
 * applies TimeGrid-based time stretch via SoundTouch, writes the result into the
 * CRS TimeStretchCache. Never queries the content owner for snapshot or playback
 * source — those are fixed when the Stage2 request is enqueued.
 */
struct Stage2TimeStretchRebuilder
{
    struct Request
    {
        ContentKey contentKey;
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        double audioSampleRate{0.0};
    };

    /**
     * Execute a Stage2 rebuild from request-carried snapshot/audio.
     *
     * @param crs     The ContentRenderService owning TimeStretchCache / RenderCache /
     *                StretcherPool.
     * @param request Immutable inputs. contentRevision / timeGridRevision are read
     *                directly from request.contentSnapshot.
     * @return        true on success (cache stored or identity-timegrid invalidated);
     *                false on missing/invalid request inputs.
     */
    static bool rebuild(ContentRenderService& crs, const Request& request);
};

} // namespace OpenTune
