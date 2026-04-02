#pragma once

namespace OpenTune::PitchControlConfig {

constexpr float kDefaultRetuneSpeedPercent = 15.0f;
constexpr float kDefaultRetuneSpeedNormalized = 0.15f;

constexpr float kDefaultVibratoDepth = 0.0f;
constexpr float kDefaultVibratoRateHz = 7.5f;

constexpr float kDefaultNoteSplitCents = 80.0f;
constexpr float kMinNoteSplitCents = 0.0f;
constexpr float kMaxNoteSplitCents = 200.0f;

} // namespace OpenTune::PitchControlConfig
