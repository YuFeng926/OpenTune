#include "ReferenceAutoAlign.h"

#include "../Utils/F0Timeline.h"
#include "../Utils/MaterializationState.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

namespace {
    constexpr double kF0FramesPerSecond = 100.0;

    int timeToFrame(double timeSeconds) {
        return static_cast<int>(std::round(timeSeconds * kF0FramesPerSecond));
    }

    /** 找到时间最接近 targetTime 的 note，返回指针（可能为 nullptr） */
    const Note* findNearestNote(const std::vector<Note>& notes, double targetTime) {
        const Note* best = nullptr;
        double bestDist = std::numeric_limits<double>::max();

        for (const auto& note : notes) {
            const double center = (note.startTime + note.endTime) * 0.5;
            const double dist = std::abs(center - targetTime);
            if (dist < bestDist) {
                bestDist = dist;
                best = &note;
            }
        }
        return best;
    }

    /** 找到 sourceSeconds 最接近 targetSourceSeconds 的 anchor */
    const MaterializationStore::DerivedAnalysis::TimeAnchor*
    findNearestAnchor(const std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& anchors,
                      double targetSourceSeconds) {
        const MaterializationStore::DerivedAnalysis::TimeAnchor* best = nullptr;
        double bestDist = std::numeric_limits<double>::max();

        for (const auto& a : anchors) {
            const double dist = std::abs(a.sourceSeconds - targetSourceSeconds);
            if (dist < bestDist) {
                bestDist = dist;
                best = &a;
            }
        }
        return best;
    }

    /** 将 source time 上的 anchor 投影到 timeline */
    double anchorToTimeline(double sourceSeconds, double timelineStart) {
        return sourceSeconds + timelineStart;
    }

    /** 将 timeline 位置投影回 target 的 materialization-local time */
    double timelineToTargetLocal(double timelineSeconds, double targetTimelineStart) {
        return timelineSeconds - targetTimelineStart;
    }

} // anonymous namespace

// ============================================================================
// Public: align()
// ============================================================================

AlignResult ReferenceAutoAlign::align(
    const MaterializationStore::DerivedAnalysis& targetAnalysis,
    const MaterializationStore::DerivedAnalysis& referenceAnalysis,
    double overlapStartSeconds,
    double overlapEndSeconds,
    double targetTimelineStartSeconds,
    double referenceTimelineStartSeconds,
    double targetTotalDurationSeconds)
{
    AlignResult result;

    // ================================================================
    // 前置检查（失败矩阵）
    // ================================================================

    if (referenceAnalysis.basicDerivedNotes.empty()) {
        result.error = AlignResult::ErrorCode::InsufficientNotes;
        result.errorMessage = "Reference analysis has no derived notes";
        return result;
    }

    if (referenceAnalysis.basicDerivedAnchors.size() < 2) {
        result.error = AlignResult::ErrorCode::InsufficientAnchors;
        result.errorMessage = juce::String("Reference analysis has ") + juce::String(referenceAnalysis.basicDerivedAnchors.size()) + " derived anchors (need >= 2)";
        return result;
    }

    if (targetAnalysis.basicDerivedNotes.empty()) {
        result.error = AlignResult::ErrorCode::InsufficientNotes;
        result.errorMessage = "Target analysis has no derived notes";
        return result;
    }

    if (targetAnalysis.basicDerivedAnchors.size() < 2) {
        result.error = AlignResult::ErrorCode::InsufficientAnchors;
        result.errorMessage = juce::String("Target analysis has ") + juce::String(targetAnalysis.basicDerivedAnchors.size()) + " derived anchors (need >= 2)";
        return result;
    }

    if (targetAnalysis.state != F0ExtractionState::Ready) {
        result.error = AlignResult::ErrorCode::TargetAnalysisNotReady;
        result.errorMessage = "Target derived analysis not ready (state != Ready)";
        return result;
    }

    if (referenceAnalysis.state != F0ExtractionState::Ready) {
        result.error = AlignResult::ErrorCode::ReferenceAnalysisNotReady;
        result.errorMessage = "Reference derived analysis not ready (state != Ready)";
        return result;
    }

    if (overlapEndSeconds <= overlapStartSeconds) {
        result.error = AlignResult::ErrorCode::NoOverlap;
        result.errorMessage = "No time overlap between target and reference";
        return result;
    }

    if (targetTotalDurationSeconds <= 0.0) {
        result.error = AlignResult::ErrorCode::TimeGridInvalid;
        result.errorMessage = "targetTotalDurationSeconds must be positive";
        return result;
    }

    // ================================================================
    // Step 3: 音高对齐
    // ================================================================

    alignPitch(targetAnalysis.basicDerivedNotes,
               referenceAnalysis.basicDerivedNotes,
               overlapStartSeconds,
               overlapEndSeconds,
               targetTimelineStartSeconds,
               referenceTimelineStartSeconds,
               result.correctedNotes,
               result.correctedSegments);

    // ================================================================
    // Step 4: 时间对齐（生成 TimeGrid）
    // ================================================================

    const bool timeOk = alignTimeGrid(targetAnalysis.basicDerivedAnchors,
                                      referenceAnalysis.basicDerivedAnchors,
                                      overlapStartSeconds,
                                      overlapEndSeconds,
                                      targetTimelineStartSeconds,
                                      referenceTimelineStartSeconds,
                                      targetTotalDurationSeconds,
                                      result.timeGrid);

    if (!timeOk || !result.timeGrid) {
        result.error = AlignResult::ErrorCode::TimeGridInvalid;
        result.errorMessage = "Failed to generate valid time grid";
        return result;
    }

    // ================================================================
    // Step 5: 验证生成的 TimeGrid
    // ================================================================

    if (!validateTimeGrid(*result.timeGrid)) {
        result.error = AlignResult::ErrorCode::TimeGridInvalid;
        result.errorMessage = "Generated time grid failed invariant validation";
        result.timeGrid.reset();
        return result;
    }

    result.success = true;
    result.error = AlignResult::ErrorCode::None;
    return result;
}

// ============================================================================
// Private: alignPitch (Step 3)
// ============================================================================

void ReferenceAutoAlign::alignPitch(
    const std::vector<Note>& targetNotes,
    const std::vector<Note>& referenceNotes,
    double overlapStart,
    double overlapEnd,
    double targetTimelineStart,
    double referenceTimelineStart,
    std::vector<Note>& outCorrectedNotes,
    std::vector<CorrectedSegment>& outSegments)
{
    // 遍历所有 reference notes，过滤出落在 overlap 区间内的
    for (const auto& refNote : referenceNotes) {
        // 将 reference note 的中心时间投影到 timeline
        const double refNoteCenterSource = (refNote.startTime + refNote.endTime) * 0.5;
        const double refNoteTimeline = anchorToTimeline(refNoteCenterSource, referenceTimelineStart);

        // 仅处理 overlap 区间内的 reference note
        if (refNoteTimeline < overlapStart || refNoteTimeline > overlapEnd) {
            continue;
        }

        // 将 timeline 位置映射回 target 的 materialization-local time
        const double targetLocalTime = timelineToTargetLocal(refNoteTimeline, targetTimelineStart);

        // 找时间最近的 target derived note
        const Note* tgtNote = findNearestNote(targetNotes, targetLocalTime);
        if (!tgtNote) {
            continue;
        }

        // ---- 生成 corrected Note ----
        // 保留 target note 的时间范围，但采纳 reference note 的音高参数
        Note corrected = *tgtNote;
        corrected.pitch = refNote.pitch;
        corrected.originalPitch = refNote.originalPitch;
        corrected.pitchOffset = refNote.pitchOffset;
        corrected.retuneSpeed = refNote.retuneSpeed;
        corrected.vibratoDepth = refNote.vibratoDepth;
        corrected.vibratoRate = refNote.vibratoRate;
        corrected.selected = false;
        corrected.dirty = true;
        outCorrectedNotes.push_back(corrected);

        // ---- 生成 CorrectedSegment ----
        // F0 frame 范围从 target note 的时间换算（100 fps）
        CorrectedSegment seg;
        seg.startFrame = timeToFrame(tgtNote->startTime);
        seg.endFrame = timeToFrame(tgtNote->endTime);
        seg.source = CorrectedSegment::Source::NoteBased;
        seg.retuneSpeed = refNote.retuneSpeed;
        seg.vibratoDepth = refNote.vibratoDepth;
        seg.vibratoRate = refNote.vibratoRate;
        outSegments.push_back(seg);
    }
}

// ============================================================================
// Private: alignTimeGrid (Step 4)
// ============================================================================

bool ReferenceAutoAlign::alignTimeGrid(
    const std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& targetAnchors,
    const std::vector<MaterializationStore::DerivedAnalysis::TimeAnchor>& referenceAnchors,
    double overlapStart,
    double overlapEnd,
    double targetTimelineStart,
    double referenceTimelineStart,
    double totalDurationSeconds,
    std::shared_ptr<const TimeGridSnapshot>& outTimeGrid)
{
    if (targetAnchors.empty()) {
        return false;
    }

    std::vector<TimeHandle> handles;
    uint64_t nextId = 1;

    // ---- Handle 0: ClipStart（端点锁定） ----
    {
        TimeHandle h;
        h.id = nextId++;
        h.source_seconds = 0.0;
        h.output_seconds = 0.0;
        h.kind = HandleKind::ClipStart;
        h.locked = true;
        h.confidence = Confidence::Default;
        handles.push_back(h);
    }

    // ---- Interior handles: 对齐 overlap 区间内的 target anchors ----
    for (const auto& tgtAnchor : targetAnchors) {
        // 将 target anchor 投影到 timeline
        const double tgtTimeline = anchorToTimeline(tgtAnchor.sourceSeconds, targetTimelineStart);

        // 仅处理 overlap 区间内的 target anchor
        if (tgtTimeline < overlapStart || tgtTimeline > overlapEnd) {
            continue;
        }

        // 找到 reference 中最接近的 anchor（在 reference 的 materialization-local 时间中搜索）
        const double refLocalTarget = timelineToTargetLocal(tgtTimeline, referenceTimelineStart);
        const auto* refAnchor = findNearestAnchor(referenceAnchors, refLocalTarget);
        if (!refAnchor) {
            continue;
        }

        // 计算期望的 output_seconds：
        // 我们想要 target anchor 在播放时，其 output 时间与 reference anchor 的 timeline 位置对齐。
        // τ(tgtAnchor.sourceSeconds) = refAnchor.timeline - targetTimelineStart
        const double refTimeline = anchorToTimeline(refAnchor->sourceSeconds, referenceTimelineStart);
        double desiredOutput = timelineToTargetLocal(refTimeline, targetTimelineStart);

        // 将 output_seconds clamp 到 [0, totalDurationSeconds] 范围
        desiredOutput = std::max(0.0, std::min(desiredOutput, totalDurationSeconds));

        TimeHandle h;
        h.id = nextId++;
        h.source_seconds = tgtAnchor.sourceSeconds;
        h.output_seconds = desiredOutput;
        h.kind = HandleKind::UserAdded;
        h.locked = false;
        h.confidence = Confidence::Default;
        handles.push_back(h);
    }

    // ---- Handle N-1: ClipEnd（端点锁定：总时长守恒） ----
    {
        TimeHandle h;
        h.id = nextId++;
        h.source_seconds = totalDurationSeconds;
        h.output_seconds = totalDurationSeconds;
        h.kind = HandleKind::ClipEnd;
        h.locked = true;
        h.confidence = Confidence::Default;
        handles.push_back(h);
    }

    // ---- 按 source_seconds 升序排序 ----
    std::sort(handles.begin(), handles.end(),
              [](const TimeHandle& a, const TimeHandle& b) {
                  return a.source_seconds < b.source_seconds;
              });

    // ---- 强制 output_seconds 非单调递减 ----
    for (size_t i = 1; i < handles.size(); ++i) {
        if (handles[i].output_seconds < handles[i - 1].output_seconds) {
            handles[i].output_seconds = handles[i - 1].output_seconds;
        }
    }

    // ---- 强制端点约束 ----
    if (!handles.empty()) {
        handles.front().output_seconds = 0.0;
        handles.back().output_seconds = totalDurationSeconds;
    }

    // ---- 用工厂方法构建不可变 snapshot ----
    auto snapshot = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (!snapshot) {
        return false;
    }

    outTimeGrid = snapshot;
    return true;
}

// ============================================================================
// Private: validateTimeGrid (Step 5)
// ============================================================================

bool ReferenceAutoAlign::validateTimeGrid(const TimeGridSnapshot& grid)
{
    juce::String error;
    return TimeGridSnapshot::validate(grid.handles(), error);
}

} // namespace OpenTune
