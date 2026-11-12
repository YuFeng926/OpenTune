// Native PCM 播放回归：同率 alias 逐样本保真、publisher prepared 身份复用与
// 换率确定性、固定映射下的分块/seek/loop 地址、非零 firstSourceSample 与负
// 前缀裁剪、显式自动化内容时间锚点、以及 owner 窗口秒值跨快照/发布保持原值。

#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Render/PlaybackSourcePublisher.h"
#include "../Source/Utils/AutomationLane.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Utils/PlaybackSampleGeometry.h"
#include "../Source/Utils/TimeCoordinate.h"
#include "../Source/Utils/TimeGrid.h"
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {

namespace PSG = OpenTune::PlaybackSampleGeometry;

OpenTune::ContentKey makeKey(uint64_t objectId)
{
    return OpenTune::ContentKey{OpenTune::DomainKind::StandaloneClip, objectId, 0};
}

std::shared_ptr<juce::AudioBuffer<float>> makeStereoBuffer(int numSamples)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(2, numSamples);
    for (int s = 0; s < numSamples; ++s) {
        const auto index = static_cast<int64_t>(s);
        buffer->setSample(0, s, static_cast<float>((index * 37 + 11) % 997) / 997.0f - 0.5f);
        buffer->setSample(1, s, static_cast<float>((index * 53 + 7) % 991) / 991.0f - 0.5f);
    }
    return buffer;
}

std::shared_ptr<juce::AudioBuffer<float>> makeMonoRamp(int numSamples)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, numSamples);
    for (int s = 0; s < numSamples; ++s)
        buffer->setSample(0, s, static_cast<float>(s) * 0.001f - 0.5f);
    return buffer;
}

OpenTune::NativePcmSource makeNative(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                                     double sampleRate,
                                     int64_t firstSourceSample = 0)
{
    return OpenTune::NativePcmSource{std::move(buffer), sampleRate, firstSourceSample, 1};
}

std::shared_ptr<OpenTune::EditableContentSnapshot> makeIdentitySnapshot(double durationSeconds)
{
    auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    snapshot->timeGrid = OpenTune::TimeGridSnapshot::makeIdentity(durationSeconds);
    return snapshot;
}

OpenTune::TimeStretchCache& testTimeStretchCache()
{
    static OpenTune::TimeStretchCache cache;
    return cache;
}

OpenTune::PlaybackReadSource makeReadSource(
    const OpenTune::ContentKey& key,
    std::shared_ptr<const OpenTune::EditableContentSnapshot> snapshot,
    const OpenTune::NativePcmSource& native,
    OpenTune::PlaybackPreparedDry dry)
{
    auto source = OpenTune::makePlaybackReadSource(
        key, snapshot, native, snapshot->audioBuffer, snapshot->audioSampleRate,
        snapshot->firstCanonicalSample, nullptr, testTimeStretchCache());
    source.preparedDry = std::move(dry);
    return source;
}

// 同率准备必须 alias 原始 PCM，stereo 两声道逐样本位等价。
bool testSameRateNativePreparedAliasIsBitExactInStereo()
{
    const double rates[] = {44100.0, 48000.0, 96000.0};
    for (const double rate : rates) {
        auto buffer = makeStereoBuffer(4096);
        const auto native = makeNative(buffer, rate);
        const auto dry = OpenTune::PlaybackSourcePublisher::prepareDry(native, rate);
        if (dry.buffer != native.buffer
            || dry.sampleRate != rate
            || dry.firstPreparedSample != native.firstSourceSample
            || !(dry.nativeSource == native))
            return false;

        const auto source = makeReadSource(makeKey(1), makeIdentitySnapshot(4096.0 / rate),
                                           native, dry);
        juce::AudioBuffer<float> destination(2, 4096);
        const OpenTune::PlaybackReadRequest request(source, 0, rate, 4096, 0.0);
        if (OpenTune::readPlaybackAudio(request, destination, 0) != 4096)
            return false;

        bool channelsDiffer = false;
        for (int s = 0; s < 4096; ++s) {
            if (destination.getSample(0, s) != buffer->getSample(0, s)
                || destination.getSample(1, s) != buffer->getSample(1, s))
                return false;
            channelsDiffer = channelsDiffer
                || destination.getSample(0, s) != destination.getSample(1, s);
        }
        if (!channelsDiffer)
            return false;
    }
    return true;
}

// 同源切片共享 prepared 身份；换率后回到原率恢复 native alias，
// 再次准备同一目标率产生逐样本相同的 PCM。
bool testPublisherReusesPreparedIdentityAndRestoresItDeterministically()
{
    constexpr double nativeRate = 48000.0;
    OpenTune::PlaybackSourcePublisher publisher;
    publisher.setPlaybackSampleRate(nativeRate);

    auto buffer = makeStereoBuffer(2048);
    const auto native = makeNative(buffer, nativeRate);
    const auto keyA = makeKey(101);
    const auto keyB = makeKey(102);

    publisher.publish(keyA, makeReadSource(keyA, makeIdentitySnapshot(2048.0 / nativeRate),
                                           native, {}));
    publisher.publish(keyB, makeReadSource(keyB, makeIdentitySnapshot(2048.0 / nativeRate),
                                           native, {}));

    OpenTune::PlaybackReadSource nativeA;
    OpenTune::PlaybackReadSource nativeB;
    if (!publisher.get(keyA, nativeA) || !publisher.get(keyB, nativeB))
        return false;
    if (nativeA.preparedDry.buffer != native.buffer
        || nativeB.preparedDry.buffer != native.buffer
        || nativeA.preparedDry.buffer != nativeB.preparedDry.buffer
        || nativeA.preparedDry.firstPreparedSample != native.firstSourceSample
        || !(nativeA.preparedDry.nativeSource == native))
        return false;

    // 44.1 kHz 派生一次，记录 prepared PCM 作为后续同率准备的确定性参考。
    publisher.setPlaybackSampleRate(OpenTune::TimeCoordinate::kRenderSampleRate);
    OpenTune::PlaybackReadSource derivedA;
    OpenTune::PlaybackReadSource derivedB;
    if (!publisher.get(keyA, derivedA) || !publisher.get(keyB, derivedB))
        return false;
    if (!derivedA.preparedDry.buffer
        || derivedA.preparedDry.buffer == native.buffer
        || derivedA.preparedDry.sampleRate != OpenTune::TimeCoordinate::kRenderSampleRate
        || derivedA.preparedDry.buffer != derivedB.preparedDry.buffer)
        return false;

    std::vector<float> reference;
    reference.reserve(static_cast<size_t>(derivedA.preparedDry.buffer->getNumChannels())
                      * static_cast<size_t>(derivedA.preparedDry.buffer->getNumSamples()));
    for (int ch = 0; ch < derivedA.preparedDry.buffer->getNumChannels(); ++ch)
        for (int s = 0; s < derivedA.preparedDry.buffer->getNumSamples(); ++s)
            reference.push_back(derivedA.preparedDry.buffer->getSample(ch, s));

    // 回到原率：两份切片同时恢复 native alias。
    publisher.setPlaybackSampleRate(nativeRate);
    if (!publisher.get(keyA, nativeA) || !publisher.get(keyB, nativeB))
        return false;
    if (nativeA.preparedDry.buffer != native.buffer
        || nativeA.preparedDry.buffer != nativeB.preparedDry.buffer
        || nativeA.preparedDry.sampleRate != nativeRate)
        return false;

    // 再次切换到同一目标率：同样的输入身份产生同样的 prepared PCM。
    publisher.setPlaybackSampleRate(OpenTune::TimeCoordinate::kRenderSampleRate);
    if (!publisher.get(keyA, derivedA) || !publisher.get(keyB, derivedB))
        return false;
    const auto& rederived = *derivedA.preparedDry.buffer;
    if (derivedA.preparedDry.buffer != derivedB.preparedDry.buffer
        || static_cast<size_t>(rederived.getNumChannels())
               * static_cast<size_t>(rederived.getNumSamples()) != reference.size())
        return false;
    size_t index = 0;
    for (int ch = 0; ch < rederived.getNumChannels(); ++ch)
        for (int s = 0; s < rederived.getNumSamples(); ++s)
            if (rederived.getSample(ch, s) != reference[index++])
                return false;
    return true;
}

// 用固定映射把 host block 投影为 prepared 读取地址，并与 native 连续参考比较。
bool readHostBlock(const OpenTune::PlaybackReadSource& source,
                   const PSG::Placement& placement,
                   const OpenTune::NativePcmSource& native,
                   int64_t hostBegin,
                   int64_t hostEnd,
                   std::vector<float>& out)
{
    const auto overlap = PSG::intersect({hostBegin, hostEnd}, PSG::hostPlaybackSpan(placement));
    if (overlap.empty())
        return false;

    const int count = static_cast<int>(overlap.end - overlap.begin);
    const auto address = PSG::mapHostSampleToPrepared(
        placement, overlap.begin, source.preparedDry.firstPreparedSample);
    juce::AudioBuffer<float> destination(native.buffer->getNumChannels(), count);
    const OpenTune::PlaybackReadRequest request(
        source, address.preparedGlobalIndex, source.preparedDry.sampleRate, count,
        static_cast<double>(overlap.begin - placement.playbackBeginSample) / placement.playbackRate);
    if (OpenTune::readPlaybackAudio(request, destination, 0) != count)
        return false;

    out.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int64_t local = address.preparedGlobalIndex + i - native.firstSourceSample;
        if (local < 0 || local >= native.buffer->getNumSamples())
            return false;
        for (int channel = 0; channel < native.buffer->getNumChannels(); ++channel)
            if (destination.getSample(channel, i)
                != native.buffer->getSample(channel, static_cast<int>(local)))
                return false;
        out[static_cast<size_t>(i)] = destination.getSample(0, i);
    }
    return true;
}

bool verifyPartition(const OpenTune::PlaybackReadSource& source,
                     const PSG::Placement& placement,
                     const OpenTune::NativePcmSource& native,
                     const std::vector<int>& blockSizes)
{
    const auto host = PSG::hostPlaybackSpan(placement);
    int64_t cursor = host.begin;
    int64_t covered = 0;
    size_t blockIndex = 0;
    while (cursor < host.end) {
        const int64_t blockEnd = std::min(
            host.end, cursor + static_cast<int64_t>(blockSizes[blockIndex % blockSizes.size()]));
        std::vector<float> block;
        if (!readHostBlock(source, placement, native, cursor, blockEnd, block))
            return false;
        if (static_cast<int64_t>(block.size()) != blockEnd - cursor)
            return false;
        covered += static_cast<int64_t>(block.size());
        cursor = blockEnd;
        ++blockIndex;
    }
    return cursor == host.end && covered == host.end - host.begin;
}

// 128/512/混合块、seek 与 loop 都复用同一固定映射；覆盖已复现的 3456/13824。
bool testBlockSeekAndLoopAddressingNearKnownBoundaries()
{
    constexpr double rate = 48000.0;
    auto buffer = makeMonoRamp(32768);
    const auto native = makeNative(buffer, rate);
    const auto dry = OpenTune::PlaybackSourcePublisher::prepareDry(native, rate);
    const auto source = makeReadSource(makeKey(201), makeIdentitySnapshot(32768.0 / rate),
                                       native, dry);

    struct PlacementCase
    {
        int64_t startSample;
        int64_t endSample;
    };
    const PlacementCase cases[] = {{3456, 14336}, {13824, 14336}};

    for (const auto& item : cases) {
        const auto placement = PSG::makePlacement(
            rate,
            static_cast<double>(item.startSample) / rate,
            static_cast<double>(item.endSample) / rate,
            0.0);
        if (placement.playbackBeginSample != item.startSample
            || placement.playbackEndSample != item.endSample)
            return false;

        if (!verifyPartition(source, placement, native, {128})
            || !verifyPartition(source, placement, native, {512})
            || !verifyPartition(source, placement, native, {1, 64, 127, 128, 256, 512, 1024}))
            return false;

        const auto host = PSG::hostPlaybackSpan(placement);
        const int64_t seek = host.begin + (host.end - host.begin) / 3;
        std::vector<float> seeked;
        if (!readHostBlock(source, placement, native, seek, seek + 256, seeked)
            || seeked.size() != 256)
            return false;
        for (size_t i = 0; i < seeked.size(); ++i) {
            const int64_t local = seek + static_cast<int64_t>(i)
                - placement.offsetSamples - native.firstSourceSample;
            if (seeked[i] != buffer->getSample(0, static_cast<int>(local)))
                return false;
        }

        std::vector<float> firstLoop;
        std::vector<float> secondLoop;
        if (!readHostBlock(source, placement, native, host.begin, host.begin + 512, firstLoop)
            || !readHostBlock(source, placement, native, host.begin, host.begin + 512, secondLoop)
            || firstLoop != secondLoop)
            return false;

        std::vector<float> pastEnd;
        if (readHostBlock(source, placement, native, host.end, host.end + 128, pastEnd))
            return false;
    }
    return true;
}

// 非零 firstSourceSample 下，负读取起点保留有效后缀；纯前缀与末尾部分可读
// 分别返回 0 与实际写入 extent。
bool testNonzeroFirstSourceSampleAndNegativePrefixPreserveSuffix()
{
    constexpr double rate = 48000.0;
    constexpr int64_t firstSourceSample = 100;
    auto buffer = makeMonoRamp(1000);
    const auto native = makeNative(buffer, rate, firstSourceSample);
    const auto dry = OpenTune::PlaybackSourcePublisher::prepareDry(native, rate);
    if (dry.buffer != native.buffer || dry.firstPreparedSample != firstSourceSample)
        return false;

    const auto source = makeReadSource(
        makeKey(301),
        makeIdentitySnapshot(static_cast<double>(firstSourceSample + buffer->getNumSamples()) / rate),
        native, dry);

    juce::AudioBuffer<float> exact(1, 32);
    if (OpenTune::readPlaybackAudio(
            OpenTune::PlaybackReadRequest(source, firstSourceSample, rate, 32, 0.0), exact, 0) != 32)
        return false;
    for (int s = 0; s < 32; ++s)
        if (exact.getSample(0, s) != buffer->getSample(0, s))
            return false;

    // 读取 [36, 136)：前 64 个输出为零，后续 36 个样本有效。
    juce::AudioBuffer<float> prefix(1, 100);
    prefix.clear();
    if (OpenTune::readPlaybackAudio(
            OpenTune::PlaybackReadRequest(source, 36, rate, 100, 0.0), prefix, 0) != 100)
        return false;
    for (int s = 0; s < 64; ++s)
        if (prefix.getSample(0, s) != 0.0f)
            return false;
    for (int s = 0; s < 36; ++s)
        if (prefix.getSample(0, 64 + s) != buffer->getSample(0, s))
            return false;

    juce::AudioBuffer<float> untouched(1, 40);
    untouched.clear();
    if (OpenTune::readPlaybackAudio(
            OpenTune::PlaybackReadRequest(source, 36, rate, 40, 0.0), untouched, 0) != 0)
        return false;

    // 末尾 8 个样本可读，其余输出保持清零。
    juce::AudioBuffer<float> tail(1, 16);
    tail.clear();
    const int64_t tailStart = firstSourceSample + buffer->getNumSamples() - 8;
    if (OpenTune::readPlaybackAudio(
            OpenTune::PlaybackReadRequest(source, tailStart, rate, 16, 0.0), tail, 0) != 8)
        return false;
    for (int s = 0; s < 8; ++s)
        if (tail.getSample(0, s) != buffer->getSample(0, 992 + s))
            return false;
    for (int s = 8; s < 16; ++s)
        if (tail.getSample(0, s) != 0.0f)
            return false;
    return true;
}

// 自动化读取显式 outputStartSeconds 作为内容时间锚点；同一锚点在不同读取
// 全局地址上产生同一增益序列。
bool testExplicitAutomationAnchorIsIndependentOfReadAddress()
{
    constexpr double rate = 48000.0;
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 4096);
    for (int s = 0; s < 4096; ++s)
        buffer->setSample(0, s, 0.5f);
    const auto native = makeNative(buffer, rate);
    const auto dry = OpenTune::PlaybackSourcePublisher::prepareDry(native, rate);

    auto snapshot = makeIdentitySnapshot(1.0);
    snapshot->volumeEnvelope = OpenTune::AutomationLane::fromSnapshot(
        std::vector<OpenTune::AutomationPoint>{{0.0, 0.0f}, {1.0, -6.0f}});
    const auto source = makeReadSource(makeKey(401), snapshot, native, dry);

    const auto read = [&source, rate](int64_t startSample, double outputStartSeconds) {
        juce::AudioBuffer<float> destination(1, 8);
        const OpenTune::PlaybackReadRequest request(
            source, startSample, rate, 8, outputStartSeconds);
        std::vector<float> out;
        if (OpenTune::readPlaybackAudio(request, destination, 0) != 8)
            return out;
        out.resize(8);
        for (int s = 0; s < 8; ++s)
            out[static_cast<size_t>(s)] = destination.getSample(0, s);
        return out;
    };

    const auto anchoredZero = read(0, 0.25);
    const auto anchoredShiftedAddress = read(2048, 0.25);
    const auto anchoredLater = read(0, 0.5);
    if (anchoredZero.empty() || anchoredZero != anchoredShiftedAddress)
        return false;

    for (int s = 0; s < 8; ++s) {
        const double outputSeconds = 0.25 + static_cast<double>(s) / rate;
        const float gain = std::exp(
            snapshot->volumeEnvelope.evalAt(outputSeconds) * 0.11512925465f);
        if (std::abs(anchoredZero[static_cast<size_t>(s)] - 0.5f * gain) > 1.0e-6f)
            return false;
    }
    return anchoredZero[0] != anchoredLater[0];
}

// canonical 读取保持 content-local 下标，不应用 firstCanonicalSample。
bool testReadCanonicalAudioUsesLocalOffsetsUnchanged()
{
    auto canonical = std::make_shared<juce::AudioBuffer<float>>(1, 16);
    for (int s = 0; s < 16; ++s)
        canonical->setSample(0, s, static_cast<float>(s));

    juce::AudioBuffer<float> aligned(1, 8);
    if (OpenTune::readCanonicalAudio(
            OpenTune::CanonicalReadRequest(canonical, nullptr, 4, 8), aligned, 0) != 8)
        return false;
    for (int s = 0; s < 8; ++s)
        if (aligned.getSample(0, s) != canonical->getSample(0, 4 + s))
            return false;

    juce::AudioBuffer<float> prefix(1, 8);
    prefix.clear();
    if (OpenTune::readCanonicalAudio(
            OpenTune::CanonicalReadRequest(canonical, nullptr, -4, 8), prefix, 0) != 8)
        return false;
    for (int s = 0; s < 4; ++s)
        if (prefix.getSample(0, s) != 0.0f)
            return false;
    for (int s = 0; s < 4; ++s)
        if (prefix.getSample(0, 4 + s) != canonical->getSample(0, s))
            return false;

    juce::AudioBuffer<float> tail(1, 8);
    if (OpenTune::readCanonicalAudio(
            OpenTune::CanonicalReadRequest(canonical, nullptr, 14, 8), tail, 0) != 2)
        return false;
    return tail.getSample(0, 0) == canonical->getSample(0, 14)
        && tail.getSample(0, 1) == canonical->getSample(0, 15)
        && tail.getSample(0, 7) == 0.0f;
}

// 权威窗口秒值、native 元数据与 canonical 句柄跨 snapshot 和 publisher 原样保持；
// prepared 由 native 准备，首样本位置按目标率投影。
bool testSharedWindowAndNativeFieldsSurviveSnapshotAndPublication()
{
    OpenTune::ContentState state;
    state.sourceWindow = OpenTune::SourceWindow{
        17, "persistent-source-17", 0.0011, 1.2345678901234};
    auto canonical = std::make_shared<juce::AudioBuffer<float>>(1, 100);
    for (int s = 0; s < 100; ++s)
        canonical->setSample(0, s, static_cast<float>(s) * 0.01f);
    state.audioBuffer = canonical;
    state.sampleRate = 44100.0;
    state.audioRevision = 7;
    state.nativeSource = makeNative(makeStereoBuffer(256), 48000.0, 1234);
    state.firstCanonicalSample = 55;
    state.timeGrid = OpenTune::TimeGridSnapshot::makeIdentity(0.5);

    const auto snapshot = std::make_shared<const OpenTune::EditableContentSnapshot>(
        OpenTune::makeContentSnapshot(state));
    if (snapshot->sourceWindow.sourceId != 17
        || snapshot->sourceWindow.sourcePersistentId != "persistent-source-17"
        || snapshot->sourceWindow.sourceStartSeconds != 0.0011
        || snapshot->sourceWindow.sourceEndSeconds != 1.2345678901234
        || snapshot->sourceWindow.durationSeconds() != state.sourceWindow.durationSeconds())
        return false;
    if (!(snapshot->nativeSource == state.nativeSource)
        || snapshot->firstCanonicalSample != 55
        || snapshot->audioBuffer != canonical
        || snapshot->audioSampleRate != 44100.0
        || snapshot->timeGrid != state.timeGrid)
        return false;

    const auto restored = OpenTune::contentStateFromSnapshot(*snapshot);
    if (!(restored.nativeSource == state.nativeSource)
        || restored.firstCanonicalSample != 55
        || restored.sourceWindow.sourceStartSeconds != 0.0011
        || restored.sourceWindow.sourceEndSeconds != 1.2345678901234
        || restored.timeGrid != state.timeGrid)
        return false;

    const auto key = makeKey(501);
    OpenTune::PlaybackSourcePublisher publisher;
    publisher.setPlaybackSampleRate(44100.0);
    publisher.publish(key, makeReadSource(key, snapshot, state.nativeSource, {}));

    OpenTune::PlaybackReadSource published;
    if (!publisher.get(key, published) || !published.contentSnapshot)
        return false;
    if (published.contentSnapshot->sourceWindow.sourceStartSeconds != 0.0011
        || published.contentSnapshot->sourceWindow.sourceEndSeconds != 1.2345678901234
        || !(published.nativeSource == state.nativeSource)
        || published.canonicalBuffer != canonical
        || published.canonicalSampleRate != 44100.0
        || published.firstCanonicalSample != 55
        || published.contentSnapshot->timeGrid != state.timeGrid)
        return false;
    return published.preparedDry.sampleRate == 44100.0
        && published.preparedDry.buffer != state.nativeSource.buffer
        && published.preparedDry.buffer->getNumChannels() == 2
        && published.preparedDry.firstPreparedSample == 1134
        && published.preparedDry.nativeSource == state.nativeSource;
}

bool verifyMediaFile(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(file));
    if (!reader || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max())
        return false;
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(
        static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    if (!reader->read(pcm.get(), 0, pcm->getNumSamples(), 0, true, true))
        return false;
    const double rate = reader->sampleRate;
    const auto native = makeNative(pcm, rate);
    const auto source = makeReadSource(makeKey(601),
        makeIdentitySnapshot(static_cast<double>(pcm->getNumSamples()) / rate), native,
        OpenTune::PlaybackSourcePublisher::prepareDry(native, rate));
    const auto placement = PSG::makePlacement(rate, 0.0,
        static_cast<double>(pcm->getNumSamples()) / rate, 0.0);
    const bool passed = verifyPartition(source, placement, native, {128})
        && verifyPartition(source, placement, native, {512})
        && verifyPartition(source, placement, native, {1, 64, 127, 128, 256, 512, 1024});
    std::printf("Media PCM: rate=%.17g channels=%d frames=%d bit_exact=%s\n",
        rate, pcm->getNumChannels(), pcm->getNumSamples(), passed ? "PASS" : "FAIL");
    return passed;
}

} // namespace

#if JUCE_WINDOWS
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    if (!testSameRateNativePreparedAliasIsBitExactInStereo())
    {
        std::fputs("FAIL: same-rate native prepared alias bit equality\n", stderr);
        return 1;
    }

    if (!testPublisherReusesPreparedIdentityAndRestoresItDeterministically())
    {
        std::fputs("FAIL: publisher prepared identity and rate-change determinism\n", stderr);
        return 1;
    }

    if (!testBlockSeekAndLoopAddressingNearKnownBoundaries())
    {
        std::fputs("FAIL: fixed-placement block/seek/loop addressing\n", stderr);
        return 1;
    }

    if (!testNonzeroFirstSourceSampleAndNegativePrefixPreserveSuffix())
    {
        std::fputs("FAIL: nonzero firstSourceSample and negative prefix suffix\n", stderr);
        return 1;
    }

    if (!testExplicitAutomationAnchorIsIndependentOfReadAddress())
    {
        std::fputs("FAIL: explicit automation anchor independent of read address\n", stderr);
        return 1;
    }

    if (!testReadCanonicalAudioUsesLocalOffsetsUnchanged())
    {
        std::fputs("FAIL: readCanonicalAudio local offsets\n", stderr);
        return 1;
    }

    if (!testSharedWindowAndNativeFieldsSurviveSnapshotAndPublication())
    {
        std::fputs("FAIL: shared window/native fields across snapshot and publication\n", stderr);
        return 1;
    }

    if (argc > 1 && !verifyMediaFile(juce::File(juce::String(argv[1])))) {
        std::fputs("FAIL: media file read or bit-exact playback verification\n", stderr);
        return 1;
    }
    std::puts("NativePcmPlayback tests passed.");
    return 0;
}
