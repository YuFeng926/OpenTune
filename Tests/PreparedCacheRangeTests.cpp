// Standalone contract test: prepared render-cache range SRC must match the
// original full-sequence r8brain resampler for edited chunks, blank chunks,
// cropped canonical storage, and explicitly offset Stage 2 output.

#include "Inference/RenderCache.h"
#include "Inference/TimeStretchCache.h"
#include "Content/ContentKey.h"
#include "Utils/PlaybackSampleGeometry.h"
#include "CDSPResampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using OpenTune::NativePcmSource;
using OpenTune::RenderCache;
using OpenTune::TimeStretchCache;
using OpenTune::PlaybackSampleGeometry::projectToSample;

constexpr double kCanonicalRate = RenderCache::kSampleRate;
constexpr double kTolerance = 1e-6;

bool expect(bool value, const std::string& reason)
{
    if (!value)
        std::fprintf(stderr, "PreparedCacheRangeTests: FAIL %s\n", reason.c_str());
    return value;
}

std::string rateTag(double rate)
{
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%.17g", rate);
    return std::string("rate ") + text;
}

std::vector<float> makeSignal(int length, uint32_t seed)
{
    std::vector<float> pcm(static_cast<size_t>(length));
    uint32_t state = seed;
    for (int i = 0; i < length; ++i)
    {
        state = state * 1664525u + 1013904223u;
        const double noise = static_cast<double>(state) / 4294967296.0 - 0.5;
        pcm[static_cast<size_t>(i)] = static_cast<float>(
            0.30 * noise
            + 0.25 * std::sin(6.283185307179586 * i / 19.0 + static_cast<double>(seed % 11u))
            + 0.20 * std::sin(6.283185307179586 * i / 6.7));
    }
    return pcm;
}

std::shared_ptr<const juce::AudioBuffer<float>> toBuffer(const std::vector<float>& pcm)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, static_cast<int>(pcm.size()));
    std::copy(pcm.begin(), pcm.end(), buffer->getWritePointer(0));
    return buffer;
}

// The original zero-origin r8brain path on the complete continuous sequence.
std::vector<float> originalReference(const std::vector<float>& input, double targetRate)
{
    const int length = static_cast<int>(
        projectToSample(static_cast<double>(input.size()) * targetRate / kCanonicalRate));
    std::vector<float> output(static_cast<size_t>(length));
    r8b::CDSPResampler24 original(kCanonicalRate, targetRate, 4096);
    original.oneshot(input.data(), static_cast<int>(input.size()), output.data(), length);
    return output;
}

double maxError(const std::vector<float>& actual, const std::vector<float>& reference,
                int64_t referenceFirst)
{
    double largest = 0.0;
    for (size_t i = 0; i < actual.size(); ++i)
    {
        const int64_t index = referenceFirst + static_cast<int64_t>(i);
        const double expected = index >= 0 && index < static_cast<int64_t>(reference.size())
            ? static_cast<double>(reference[static_cast<size_t>(index)]) : 0.0;
        largest = std::max(largest, std::abs(static_cast<double>(actual[i]) - expected));
    }
    return largest;
}

double maxErrorInRange(const std::vector<float>& actual, const std::vector<float>& reference,
                       int64_t referenceFirst, int64_t begin, int64_t end)
{
    double largest = 0.0;
    const int64_t limit = std::min<int64_t>(end, static_cast<int64_t>(actual.size()));
    for (int64_t i = std::max<int64_t>(0, begin); i < limit; ++i)
    {
        const int64_t index = referenceFirst + i;
        const double expected = index >= 0 && index < static_cast<int64_t>(reference.size())
            ? static_cast<double>(reference[static_cast<size_t>(index)]) : 0.0;
        largest = std::max(largest,
            std::abs(static_cast<double>(actual[static_cast<size_t>(i)]) - expected));
    }
    return largest;
}

std::vector<float> overlayPrepared(const RenderCache& cache, int64_t readStartSample,
                                   int numSamples, double targetSampleRate,
                                   uint64_t contentRevision)
{
    std::vector<float> output(static_cast<size_t>(numSamples), 0.0f);
    juce::AudioBuffer<float> destination(1, numSamples);
    destination.clear();
    cache.overlayPreparedAudio(
        destination, 0, numSamples, readStartSample, targetSampleRate, contentRevision);
    if (numSamples > 0)
        std::copy_n(destination.getReadPointer(0), numSamples, output.begin());
    return output;
}

bool allZero(const std::vector<float>& samples)
{
    for (const float sample : samples)
        if (sample != 0.0f)
            return false;
    return true;
}

bool renderJobs(RenderCache& cache, const std::map<int64_t, std::vector<float>>& audio,
                const std::string& tag)
{
    const auto jobs = cache.getPendingJobs();
    bool ok = expect(jobs.size() == audio.size(), tag + ": pending job count mismatch");
    for (const auto& job : jobs)
    {
        const auto audioIt = audio.find(job.startSample);
        if (!expect(audioIt != audio.end(),
                    tag + ": missing canonical audio for pending chunk"))
        {
            ok = false;
            continue;
        }
        if (!expect(cache.claimPendingJob(job), tag + ": claimPendingJob failed"))
        {
            ok = false;
            continue;
        }
        std::vector<float> pcm = audioIt->second;
        if (!expect(pcm.size() == static_cast<size_t>(job.endSampleExclusive - job.startSample),
                    tag + ": canonical audio length mismatch"))
        {
            ok = false;
            continue;
        }
        const auto completed = cache.completeChunkRenderWithAudio(
            job.startSample, job.endSampleExclusive, std::move(pcm), job.targetRevision);
        ok = expect(completed == RenderCache::ChunkRenderResult::Published,
                    tag + ": completeChunkRenderWithAudio failed") && ok;
    }
    return ok;
}

bool verifyAdjacentEditedChunks(double targetRate)
{
    const std::string tag = rateTag(targetRate) + " adjacent edits";
    RenderCache cache;
    auto canonical = makeSignal(4352, 0x51u);
    std::fill(canonical.begin() + 512, canonical.begin() + 4096, 0.0f);
    const auto editFirst = makeSignal(256, 0xA1u);
    const auto editSecond = makeSignal(256, 0xB2u);
    const auto editFar = makeSignal(256, 0xD7u);

    NativePcmSource native;
    native.buffer = toBuffer(canonical);
    native.sampleRate = kCanonicalRate;
    native.firstSourceSample = 0;
    native.audioRevision = 1;
    cache.setCanonicalSource(toBuffer(canonical), 0, native);

    const std::vector<RenderCache::PlannedChunk> plan{{0, 256}, {256, 512}, {4096, 4352}};
    cache.reconcileFullPlanAndRequest(plan, 0, 512, 1);
    bool ok = renderJobs(cache,
                         {{0, editFirst}, {256, editSecond}, {4096, editFar}},
                         tag + " initial render");
    ok = expect(cache.prepareForPlaybackSampleRate(targetRate),
                tag + ": prepareForPlaybackSampleRate failed") && ok;
    const size_t bytesBeforeEdit =
        RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire);

    auto editedSequence = canonical;
    std::copy(editFirst.begin(), editFirst.end(), editedSequence.begin());
    std::copy(editSecond.begin(), editSecond.end(), editedSequence.begin() + 256);
    std::copy(editFar.begin(), editFar.end(), editedSequence.begin() + 4096);
    const auto reference = originalReference(editedSequence, targetRate);
    const int64_t preparedSeam = projectToSample(256.0 * targetRate / kCanonicalRate);
    const int64_t preparedEnd = projectToSample(512.0 * targetRate / kCanonicalRate);
    const int preparedLength = static_cast<int>(preparedEnd);
    const auto prepared = overlayPrepared(cache, 0, preparedLength, targetRate, 1);
    ok = expect(maxError(prepared, reference, 0) <= kTolerance,
                tag + ": edited chunks deviate from the original full-sequence reference") && ok;
    ok = expect(maxErrorInRange(prepared, reference, 0,
                                std::max<int64_t>(0, preparedSeam - 64),
                                std::min<int64_t>(preparedLength, preparedSeam + 64)) <= kTolerance,
                tag + ": edited chunk seam deviates from the original reference") && ok;

    // Mutate only the second chunk. The first chunk's real FIR support includes
    // that edit, so its prepared PCM must follow the updated full sequence. The
    // far chunk lies outside the support and must be reused without allocating.
    const auto changedSecond = makeSignal(256, 0xC3u);
    cache.reconcileFullPlanAndRequest(plan, 256, 512, 2);
    const auto pending = cache.getPendingJobs();
    ok = expect(pending.size() == 1 && pending.front().startSample == 256,
                tag + ": neighbor edit did not queue exactly the edited chunk") && ok;
    ok = renderJobs(cache, {{256, changedSecond}}, tag + " neighbor edit") && ok;

    auto updatedSequence = canonical;
    std::copy(editFirst.begin(), editFirst.end(), updatedSequence.begin());
    std::copy(changedSecond.begin(), changedSecond.end(), updatedSequence.begin() + 256);
    std::copy(editFar.begin(), editFar.end(), updatedSequence.begin() + 4096);
    const auto updatedReference = originalReference(updatedSequence, targetRate);
    const auto updatedFirst = overlayPrepared(cache, 0, static_cast<int>(preparedSeam), targetRate, 2);
    ok = expect(maxError(updatedFirst, updatedReference, 0) <= kTolerance,
                tag + ": unedited neighbor prepared chunk did not follow the updated full reference")
        && ok;

    const size_t expectedAfterNeighborEdit = bytesBeforeEdit + 256 * sizeof(float)
        + static_cast<size_t>(preparedSeam) * sizeof(float)
        + static_cast<size_t>(preparedEnd - preparedSeam) * sizeof(float);
    ok = expect(RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                    == expectedAfterNeighborEdit,
                tag + ": far prepared chunk was rebuilt instead of reused") && ok;
    return ok;
}

bool verifyBlankNeighborUsesRealCanonical(double targetRate)
{
    const std::string tag = rateTag(targetRate) + " blank neighbor";
    RenderCache cache;
    const auto canonical = makeSignal(512, 0x61u);
    const auto editFirst = makeSignal(256, 0xD4u);

    NativePcmSource native;
    native.buffer = toBuffer(canonical);
    native.sampleRate = kCanonicalRate;
    native.firstSourceSample = 0;
    native.audioRevision = 1;
    cache.setCanonicalSource(toBuffer(canonical), 0, native);

    const std::vector<RenderCache::PlannedChunk> plan{{0, 256}, {256, 512}};
    cache.reconcileFullPlanAndRequest(plan, 0, 512, 1);
    const auto jobs = cache.getPendingJobs();
    bool ok = expect(jobs.size() == 2, tag + ": expected two pending chunks");
    RenderCache::PendingJob firstJob{};
    RenderCache::PendingJob secondJob{};
    for (const auto& job : jobs)
    {
        if (job.startSample == 0)
            firstJob = job;
        else if (job.startSample == 256)
            secondJob = job;
    }
    ok = expect(firstJob.endSampleExclusive == 256 && secondJob.endSampleExclusive == 512,
                tag + ": pending chunk spans mismatch") && ok;
    ok = expect(cache.claimPendingJob(firstJob), tag + ": claim first chunk failed") && ok;
    ok = expect(cache.completeChunkRenderWithAudio(
                    0, 256, std::vector<float>(editFirst), firstJob.targetRevision)
                    == RenderCache::ChunkRenderResult::Published,
                tag + ": first chunk completion failed") && ok;
    ok = expect(cache.claimPendingJob(secondJob), tag + ": claim blank chunk failed") && ok;
    ok = expect(cache.markChunkAsBlank(256, secondJob.targetRevision)
                    == RenderCache::ChunkRenderResult::Published,
                tag + ": markChunkAsBlank failed") && ok;
    ok = expect(cache.prepareForPlaybackSampleRate(targetRate),
                tag + ": prepareForPlaybackSampleRate failed") && ok;

    // The blank chunk publishes no edit; the prepared chunk must still see the
    // real canonical PCM that the installed canonical context retains there.
    auto fullEdited = canonical;
    std::copy(editFirst.begin(), editFirst.end(), fullEdited.begin());
    const auto reference = originalReference(fullEdited, targetRate);
    const int64_t preparedSeam = projectToSample(256.0 * targetRate / kCanonicalRate);
    const int64_t preparedEnd = projectToSample(512.0 * targetRate / kCanonicalRate);
    const auto preparedFirst = overlayPrepared(cache, 0, static_cast<int>(preparedSeam), targetRate, 1);
    ok = expect(maxError(preparedFirst, reference, 0) <= kTolerance,
                tag + ": blank neighbor did not retain the real canonical context") && ok;
    const auto blankOutput = overlayPrepared(
        cache, preparedSeam, static_cast<int>(preparedEnd - preparedSeam), targetRate, 1);
    ok = expect(allZero(blankOutput), tag + ": blank chunk still produced prepared PCM") && ok;
    return ok;
}

bool verifyCroppedCanonicalNativeHalo(double targetRate)
{
    const std::string tag = rateTag(targetRate) + " native halo";
    RenderCache cache;
    constexpr int nativeLength = 2048;
    constexpr int64_t firstCanonicalSample = 1000;
    constexpr int localLength = 256;

    const auto original = makeSignal(nativeLength, 0x71u);
    const auto editLocal = makeSignal(localLength, 0xE5u);
    const std::vector<float> canonicalCrop(
        original.begin() + static_cast<std::ptrdiff_t>(firstCanonicalSample),
        original.begin() + static_cast<std::ptrdiff_t>(firstCanonicalSample + localLength));

    NativePcmSource native;
    native.buffer = toBuffer(original);
    native.sampleRate = kCanonicalRate;
    native.firstSourceSample = 0;
    native.audioRevision = 1;
    cache.setCanonicalSource(toBuffer(canonicalCrop), firstCanonicalSample, native);

    const std::vector<RenderCache::PlannedChunk> plan{{0, localLength}};
    cache.reconcileFullPlanAndRequest(plan, 0, localLength, 1);
    bool ok = renderJobs(cache, {{0, editLocal}}, tag + " render");
    ok = expect(cache.prepareForPlaybackSampleRate(targetRate),
                tag + ": prepareForPlaybackSampleRate failed") && ok;

    // The reference keeps the whole original source and overlays only the local
    // edited region. Zeroed crop boundaries would fail outside the crop.
    auto fullEdited = original;
    std::copy(editLocal.begin(), editLocal.end(),
              fullEdited.begin() + static_cast<std::ptrdiff_t>(firstCanonicalSample));
    const auto reference = originalReference(fullEdited, targetRate);
    const int64_t preparedBegin = projectToSample(
        static_cast<double>(firstCanonicalSample) * targetRate / kCanonicalRate);
    const int64_t preparedEnd = projectToSample(
        static_cast<double>(firstCanonicalSample + localLength) * targetRate / kCanonicalRate);
    const auto prepared = overlayPrepared(
        cache, preparedBegin, static_cast<int>(preparedEnd - preparedBegin), targetRate, 1);
    ok = expect(maxError(prepared, reference, preparedBegin) <= kTolerance,
                tag + ": cropped canonical boundary zeroed the retained native halo") && ok;
    return ok;
}

bool verifyTimeStretchExplicitOriginAndNegativeStart()
{
    TimeStretchCache cache;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 7001, 0};
    const uint32_t generation = cache.beginBuild(key);
    const auto audio = makeSignal(256, 0xF6u);
    cache.store(key, audio, 7, 9, kCanonicalRate, generation, 300);
    cache.prepareForPlaybackSampleRate(kCanonicalRate);

    bool ok = true;
    {
        juce::AudioBuffer<float> destination(1, 16);
        destination.clear();
        const int copied = cache.sliceCanonicalForOutputRange(key, 7, 9, 300, destination, 0, 16);
        ok = expect(copied == 16,
                    "TimeStretchCache explicit canonical origin was not served") && ok;
        ok = expect(std::equal(audio.begin(), audio.begin() + 16, destination.getReadPointer(0)),
                    "TimeStretchCache explicit canonical origin is not bit exact") && ok;
    }
    {
        juce::AudioBuffer<float> destination(1, 320);
        destination.clear();
        const int copied = cache.sliceForOutputRange(
            key, 7, 9, -10, destination, 0, 320, kCanonicalRate);
        ok = expect(copied == 320,
                    "TimeStretchCache negative read start was not clipped to its valid suffix") && ok;
        bool prefixZero = true;
        for (int i = 0; i < 310; ++i)
            prefixZero = prefixZero && destination.getSample(0, i) == 0.0f;
        ok = expect(prefixZero,
                    "TimeStretchCache negative read start overwrote the zero prefix") && ok;
        ok = expect(std::equal(audio.begin(), audio.begin() + 10,
                               destination.getReadPointer(0) + 310),
                    "TimeStretchCache negative read start did not preserve the suffix") && ok;
    }
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    for (const double targetRate : {48000.0, 48000.25})
    {
        ok = verifyAdjacentEditedChunks(targetRate) && ok;
        ok = verifyBlankNeighborUsesRealCanonical(targetRate) && ok;
        ok = verifyCroppedCanonicalNativeHalo(targetRate) && ok;
    }
    ok = verifyTimeStretchExplicitOriginAndNegativeStart() && ok;

    std::printf("PreparedCacheRangeTests: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
