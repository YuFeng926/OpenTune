#pragma once

#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../MaterializationStore.h"
#include <vector>
#include <memory>

namespace OpenTune {

struct AlignResult {
    bool success{false};

    // 执行成功时填充
    std::vector<Note> correctedNotes;
    std::vector<CorrectedSegment> correctedSegments;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;

    // 执行失败时填充
    enum class ErrorCode : uint8_t {
        None = 0,
        NoReference,              // 无 reference binding
        SelfReference,            // self-reference
        NoOverlap,                // 无时间重叠
        TargetAnalysisNotReady,   // target derived analysis 未就绪
        ReferenceAnalysisNotReady,// reference derived analysis 未就绪
        AnalysisFailed,           // 分析过程失败
        InsufficientAnchors,      // anchors 数量 < 2
        InsufficientNotes,        // 无 reference notes
        TargetF0Missing,          // target F0 缺失
        TimeGridInvalid           // 生成的 TimeGrid 非法
    };
    ErrorCode error{ErrorCode::None};
    juce::String errorMessage;
};

/**
 * @brief 参考对轨算法：按 reference clip 的 derived notes/anchors，对 target clip 进行音高+时间对齐。
 *
 * 对齐范围：仅 target 与 reference 的 timeline overlap 区间。
 * 非重叠部分不变。
 *
 * 静态方法，无状态。错误通过 AlignResult 返回，不抛异常。
 */
class ReferenceAutoAlign {
public:
    ReferenceAutoAlign() = default;
    ~ReferenceAutoAlign() = default;

    /**
     * @brief 执行参考对轨。
     * @param targetAnalysis               target materialization 的 derived analysis
     * @param referenceAnalysis            reference materialization 的 derived analysis
     * @param overlapStartSeconds          timeline 上的重叠起始时间
     * @param overlapEndSeconds            timeline 上的重叠结束时间
     * @param targetTimelineStartSeconds   target placement 在 timeline 上的起始时间
     * @param referenceTimelineStartSeconds reference placement 在 timeline 上的起始时间
     * @param targetTotalDurationSeconds   target materialization 源音频总时长（秒），用于 TimeGrid 端点
     * @return AlignResult
     */
    static AlignResult align(
        const MaterializationStore::DerivedAnalysis& targetAnalysis,
        const MaterializationStore::DerivedAnalysis& referenceAnalysis,
        double overlapStartSeconds,
        double overlapEndSeconds,
        double targetTimelineStartSeconds,
        double referenceTimelineStartSeconds,
        double targetTotalDurationSeconds);

private:
    /** Step 3: 音高对齐。
     *  对 overlap 区间内的每个 reference note，找最近的 target note，
     *  生成 corrected Note（pitch 采用 reference）和 CorrectedSegment。
     */
    static void alignPitch(
        const std::vector<Note>& targetNotes,
        const std::vector<Note>& referenceNotes,
        double overlapStart,
        double overlapEnd,
        double targetTimelineStart,
        double referenceTimelineStart,
        std::vector<Note>& outCorrectedNotes,
        std::vector<CorrectedSegment>& outSegments);

    /** Step 4: 时间对齐。
     *  将 reference anchors 映射到 target output time，
     *  生成 target TimeGridSnapshot（使 target handles 对齐到 reference 位置）。
     *  返回 false 表示生成失败。
     */
    static bool alignTimeGrid(
        const std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& targetAnchors,
        const std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& referenceAnchors,
        double overlapStart,
        double overlapEnd,
        double targetTimelineStart,
        double referenceTimelineStart,
        double totalDurationSeconds,
        std::shared_ptr<const TimeGridSnapshot>& outTimeGrid);

};

} // namespace OpenTune
