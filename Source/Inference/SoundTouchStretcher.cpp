#include "SoundTouchStretcher.h"

#include <SoundTouch.h>

#include "../Utils/AppLogger.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

// SoundTouch settings chosen per spec §"WSOLA 引擎配置".
// Vocal-friendly defaults: AA filter on (anti-alias), QUICKSEEK off (quality),
// SEEKWINDOW=25ms / OVERLAP=8ms (SoundTouch defaults).
constexpr int kSettingUseAaFilter   = 1;
constexpr int kSettingAaFilterLength = 64;
constexpr int kSettingUseQuickSeek  = 0;
constexpr int kSettingSeekWindowMs  = 25;
constexpr int kSettingOverlapMs     = 8;

// Sample-positional tolerance for "is at segment boundary" checks (1 sample @44.1k = 23µs).
constexpr double kBoundaryEpsilonSeconds = 1.0 / 44100.0;

} // namespace

// ---------------------------------------------------------------------------
// TempoSchedule helpers
// ---------------------------------------------------------------------------

size_t SoundTouchStretcher::TempoSchedule::findSegmentIndex(double sourceSeconds) const noexcept
{
    if (anchors.size() < 2) return 0;
    // Last anchor's segment index = anchors.size() - 2 (segment between i and i+1).
    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        if (sourceSeconds < anchors[i + 1].sourceSeconds - kBoundaryEpsilonSeconds) {
            return i;
        }
    }
    return anchors.size() - 2;   // beyond last → last segment
}

double SoundTouchStretcher::TempoSchedule::soundTouchTempoAt(double sourceSeconds) const noexcept
{
    if (anchors.size() < 2) return 1.0;
    const size_t i = findSegmentIndex(sourceSeconds);
    const double dSrc = anchors[i + 1].sourceSeconds - anchors[i].sourceSeconds;
    const double dOut = anchors[i + 1].outputSeconds - anchors[i].outputSeconds;
    if (dSrc <= 0.0 || dOut <= 0.0) return 1.0;
    const double ratio = dOut / dSrc;
    return 1.0 / ratio;          // SoundTouch tempo semantics: tempo > 1 = faster
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SoundTouchStretcher::SoundTouchStretcher(double sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels)
{
    impl_ = std::make_unique<soundtouch::SoundTouch>();
    impl_->setSampleRate(static_cast<unsigned int>(sampleRate));
    impl_->setChannels(static_cast<unsigned int>(channels));
    impl_->setRate(1.0);     // no resample (Stage 2 keeps pitch unchanged)
    impl_->setPitch(1.0);    // no pitch shift
    impl_->setTempo(1.0);    // initial tempo; updated per-segment in push()
    impl_->setSetting(SETTING_USE_AA_FILTER,    kSettingUseAaFilter);
    impl_->setSetting(SETTING_AA_FILTER_LENGTH, kSettingAaFilterLength);
    impl_->setSetting(SETTING_USE_QUICKSEEK,    kSettingUseQuickSeek);
    impl_->setSetting(SETTING_SEEKWINDOW_MS,    kSettingSeekWindowMs);
    impl_->setSetting(SETTING_OVERLAP_MS,       kSettingOverlapMs);

    AppLogger::log(juce::String("[SoundTouchStretcher] Created (sr=")
                   + juce::String(sampleRate) + " ch=" + juce::String(channels)
                   + " settings=AA[1]+AA_LEN[64]+QUICKSEEK[0]+SEEKWIN[25ms]+OVERLAP[8ms])");
}

SoundTouchStretcher::~SoundTouchStretcher() = default;

uint32_t SoundTouchStretcher::getSoundTouchSettings() noexcept
{
    uint32_t v = 0;
    v |= (static_cast<uint32_t>(kSettingUseAaFilter) & 0xFFu);
    v |= (static_cast<uint32_t>(kSettingUseQuickSeek) & 0xFFu) << 8;
    v |= (static_cast<uint32_t>(kSettingSeekWindowMs) & 0xFFu) << 16;
    v |= (static_cast<uint32_t>(kSettingOverlapMs)    & 0xFFu) << 24;
    return v;
}

// ---------------------------------------------------------------------------
// TempoSchedule build
// ---------------------------------------------------------------------------

SoundTouchStretcher::TempoSchedule
SoundTouchStretcher::buildTempoScheduleFromTimeGrid(const TimeGridSnapshot& grid) const
{
    TempoSchedule schedule;
    schedule.anchors.reserve(grid.handles().size());
    for (const auto& h : grid.handles()) {
        schedule.anchors.push_back({h.source_seconds, h.output_seconds});
    }
    if (!schedule.anchors.empty()) {
        schedule.totalSourceSeconds = schedule.anchors.back().sourceSeconds;
        schedule.totalOutputSeconds = schedule.anchors.back().outputSeconds;
    }
    schedule.identity = grid.isIdentity();
    return schedule;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SoundTouchStretcher::beginRebuild(const TempoSchedule& schedule)
{
    if (impl_ == nullptr) return;

    impl_->clear();
    impl_->setRate(1.0);
    impl_->setPitch(1.0);
    impl_->setTempo(1.0);
    currentTempo_ = 1.0;

    activeSchedule_ = schedule;
    totalPushedSamples_ = 0;
    expectedOutputSamples_ = static_cast<size_t>(
        std::llround(schedule.totalOutputSeconds * sampleRate_));

    phase_ = Phase::Pushing;
}

void SoundTouchStretcher::setTempoIfChanged(double newTempo)
{
    if (std::abs(newTempo - currentTempo_) < 1e-9) return;
    impl_->setTempo(static_cast<float>(newTempo));
    currentTempo_ = newTempo;
}

void SoundTouchStretcher::push(const float* input, size_t numSamples, bool isLast)
{
    if (impl_ == nullptr || phase_ != Phase::Pushing) {
        AppLogger::warn(juce::String("[SoundTouchStretcher] push() called outside Pushing phase (phase=")
                        + juce::String(static_cast<int>(phase_)) + ")");
        return;
    }
    if (numSamples == 0 && !isLast) return;

    // Identity schedule fast path: single tempo=1.0, no segment splitting.
    if (activeSchedule_.identity || activeSchedule_.anchors.size() < 2) {
        setTempoIfChanged(1.0);
        if (numSamples > 0) {
            impl_->putSamples(input, static_cast<unsigned int>(numSamples));
            totalPushedSamples_ += numSamples;
        }
    } else {
        // Walk through segments; if a single push spans a boundary, sub-split.
        size_t consumed = 0;
        while (consumed < numSamples) {
            const double tSrcSec = static_cast<double>(totalPushedSamples_ + consumed) / sampleRate_;
            const size_t segIdx = activeSchedule_.findSegmentIndex(tSrcSec);
            const double segEndSrcSec = activeSchedule_.anchors[segIdx + 1].sourceSeconds;
            const size_t framesUntilSegEnd =
                static_cast<size_t>(std::max<double>(0.0,
                    std::ceil((segEndSrcSec - tSrcSec) * sampleRate_)));
            size_t pushCount = std::min(numSamples - consumed,
                                         std::max<size_t>(framesUntilSegEnd, 1));

            const double newTempo = activeSchedule_.soundTouchTempoAt(tSrcSec);
            setTempoIfChanged(newTempo);

            impl_->putSamples(input + consumed * static_cast<size_t>(channels_),
                              static_cast<unsigned int>(pushCount));
            consumed += pushCount;
            totalPushedSamples_ += pushCount;
        }
    }

    if (isLast) {
        impl_->flush();
        phase_ = Phase::Drained;
    }
}

size_t SoundTouchStretcher::pull(float* output, size_t maxSamples)
{
    if (impl_ == nullptr || maxSamples == 0) return 0;
    const unsigned int got = impl_->receiveSamples(output,
                                                    static_cast<unsigned int>(maxSamples));
    return static_cast<size_t>(got);
}

size_t SoundTouchStretcher::available() const
{
    if (impl_ == nullptr) return 0;
    return static_cast<size_t>(impl_->numSamples());
}

} // namespace OpenTune
