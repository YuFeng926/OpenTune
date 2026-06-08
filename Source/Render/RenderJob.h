#pragma once

#include "../Content/ContentKey.h"
// TODO Phase 0.1: ContentPayloadSnapshot.h does not exist yet.
// #include "../Content/ContentPayloadSnapshot.h"
#include "../Inference/RenderCache.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/SilentGapDetector.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

/**
 * RenderJob — 待执行的局部重渲染任务。
 *
 * 统一定义，使用 ContentKey 标识内容。
 *
 * Phase 0: 提取自 MaterializationStore::PendingRenderJob 和
 *          ContentRenderService::PendingRenderJob
 * 关键改变：
 *   - 使用 ContentKey 替代 materializationId
 *   - 包含完整 revisions（修正 CRS 丢失 targetRevision 的 bug）
 *   - 可选 contentSnapshot（支持 worker 不回查 store）
 */
struct RenderJob
{
    ContentKey contentKey;

    // TODO Phase 0.1: ContentPayloadSnapshot 尚未创建，暂时注释。
    //     完成后取消注释，支持 worker 不回查 store 的零拷贝路径。
    // std::shared_ptr<const ContentPayloadSnapshot> contentSnapshot;

    std::shared_ptr<RenderCache> renderCache;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    std::shared_ptr<PitchCurve> pitchCurve;
    std::vector<SilentGap> silentGaps;

    double startSeconds{0.0};
    double endSeconds{0.0};
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    // Revisions — 必须包含完整 revisions，修正 CRS targetRevision bug
    uint64_t targetRevision{0};
    uint64_t renderRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
};

} // namespace OpenTune