#pragma once

#include "AudioEditingScheme.h"
#include "Content/ContentEditCommands.h"
#include <functional>
#include <vector>

namespace OpenTune {

struct ManualCorrectionOp
{
    int startFrame = 0;
    int endFrameExclusive = 0;
    std::vector<float> f0Data;
    PitchCorrectionSegment::Source source = PitchCorrectionSegment::Source::HandDraw;
    PitchCorrectionSegment::ParameterSnapshot parameterSnapshot;
};

struct ManualCorrectionStrokePoint
{
    int frame = 0;
    float f0 = 0.0f;
};

struct ManualCorrectionStrokePlan
{
    std::vector<ManualCorrectionOp> ops;
    AudioEditingScheme::FrameRange actualRange;
};

using ManualCorrectionFrameQuery = std::function<float(int)>;

struct ManualCorrectionLogLineFit
{
    float intercept = 0.0f;
    float slope = 0.0f;
};

enum class ManualCorrectionEditStatus
{
    Applied,
    Failed
};

struct ManualCorrectionEditResult
{
    ManualCorrectionEditStatus status = ManualCorrectionEditStatus::Failed;
    ContentCommitSnapshot contentSnapshot;
};

std::vector<ManualCorrectionOp> planManualCorrectionOps(
    AudioEditingScheme::Scheme scheme,
    const std::vector<float>& originalF0,
    AudioEditingScheme::FrameRange requestedRange,
    const ManualCorrectionFrameQuery& valueForFrame,
    PitchCorrectionSegment::Source source,
    PitchCorrectionSegment::ParameterSnapshot parameterSnapshot = {});

ManualCorrectionStrokePlan planManualCorrectionStroke(
    AudioEditingScheme::Scheme scheme,
    const std::vector<float>& originalF0,
    ManualCorrectionStrokePoint first,
    ManualCorrectionStrokePoint second,
    PitchCorrectionSegment::Source source = PitchCorrectionSegment::Source::HandDraw,
    PitchCorrectionSegment::ParameterSnapshot parameterSnapshot = {});

float interpolateLogF0(float leftF0, float rightF0, float t);

ManualCorrectionLogLineFit fitOriginalF0LogTrend(const std::vector<float>& originalF0,
                                                int startFrame,
                                                int endFrameExclusive);

// LineAnchor 残差传递上限（半音）：锚点线按 (1-retuneSpeed) 保留原始音高形状时，
// 单帧最多传递 ±3 半音的残差，八度级错误不得无界传入锚点线。
constexpr float kLineAnchorShapeCapSemitones = 3.0f;

float lineAnchorF0WithSourceShape(const std::vector<float>& originalF0,
                                  int frame,
                                  int startFrame,
                                  int endFrameExclusive,
                                  float leftTargetF0,
                                  float rightTargetF0,
                                  float retuneSpeed,
                                  ManualCorrectionLogLineFit sourceTrend);

} // namespace OpenTune
