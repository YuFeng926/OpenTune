#include "OutputGainEnvelope.h"
#include "Note.h"
#include "TimeGrid.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

// A(t)：包含 source-time 的半开区间 Note 的 outputGainDb；无覆盖 = 0 dB。
float noteGainDbAt(const std::vector<Note>& notes, double sourceSeconds)
{
    for (const auto& note : notes) {
        if (sourceSeconds >= note.startTime && sourceSeconds < note.endTime)
            return note.outputGainDb;
    }
    return 0.0f;
}

// B(t)：点按时间排序，分段常数（点之间保持前值）；之前无点 = 0 dB。
float sibilantGainDbAt(const SibilantGainEnvelope& sibilant, double sourceSeconds)
{
    float gainDb = 0.0f;
    for (const auto& point : sibilant) {
        if (point.time > sourceSeconds)
            break;
        gainDb = point.gainDb;
    }
    return gainDb;
}

} // namespace

std::shared_ptr<const OutputGainEnvelopeSnapshot> buildOutputGainEnvelope(
    const std::vector<Note>& notes,
    const SibilantGainEnvelope& sibilant,
    const std::shared_ptr<const TimeGridSnapshot>& timeGrid,
    int64_t durationSamples,
    double sampleRate)
{
    if (durationSamples <= 0 || sampleRate <= 0.0)
        return nullptr;

    // B 点按时间排序后分段常数；排序幂等（有序输入零开销）。
    SibilantGainEnvelope sortedSibilant = sibilant;
    std::sort(sortedSibilant.begin(), sortedSibilant.end(),
              [](const SibilantGainEnvelopePoint& a, const SibilantGainEnvelopePoint& b) {
                  return a.time < b.time;
              });

    auto snap = std::make_shared<OutputGainEnvelopeSnapshot>();
    snap->sampleRate = sampleRate;
    snap->linearGains.resize(static_cast<size_t>(durationSamples));

    for (int64_t sample = 0; sample < durationSamples; ++sample) {
        const double outputSeconds = static_cast<double>(sample) / sampleRate;
        const double sourceSeconds = timeGrid ? timeGrid->tauInverse(outputSeconds) : outputSeconds;
        const double gainDb = static_cast<double>(noteGainDbAt(notes, sourceSeconds))
                            + static_cast<double>(sibilantGainDbAt(sortedSibilant, sourceSeconds));
        snap->linearGains[static_cast<size_t>(sample)] = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    }
    return snap;
}

std::shared_ptr<const PreparedOutputGainEnvelope> prepareOutputGainEnvelope(
    const std::shared_ptr<const OutputGainEnvelopeSnapshot>& canonical,
    double targetSampleRate)
{
    if (canonical == nullptr || targetSampleRate <= 0.0)
        return nullptr;

    auto prepared = std::make_shared<PreparedOutputGainEnvelope>();
    prepared->canonicalIdentity = canonical;
    prepared->sampleRate = targetSampleRate;

    const auto& source = canonical->linearGains;
    const size_t sourceLen = source.size();
    if (sourceLen == 0)
        return prepared;

    // 同采样率：直接拷贝，不重采样。
    if (std::abs(targetSampleRate - canonical->sampleRate) < 1.0) {
        prepared->linearGains = source;
        return prepared;
    }

    const size_t targetLen = static_cast<size_t>(std::max<int64_t>(
        1, static_cast<int64_t>(std::round(static_cast<double>(sourceLen) * targetSampleRate
                                            / canonical->sampleRate))));
    prepared->linearGains.resize(targetLen);

    for (size_t j = 0; j < targetLen; ++j) {
        double pos = (static_cast<double>(j) + 0.5) * static_cast<double>(sourceLen)
                   / static_cast<double>(targetLen) - 0.5;
        if (pos < 0.0)
            pos = 0.0;
        if (pos > static_cast<double>(sourceLen) - 1.0)
            pos = static_cast<double>(sourceLen) - 1.0;
        const size_t idx = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(idx);
        const float a = source[idx];
        const float b = (idx + 1 < sourceLen) ? source[idx + 1] : a;
        prepared->linearGains[j] = static_cast<float>(a + (b - a) * frac);
    }
    return prepared;
}

} // namespace OpenTune
