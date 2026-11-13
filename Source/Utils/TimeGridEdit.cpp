#include "TimeGridEdit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace OpenTune {

std::shared_ptr<const TimeGridSnapshot> planTimeGridDrag(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t draggedHandleId,
    double dragStartOutputSeconds,
    double newOutputSeconds,
    uint64_t selectedHandleId,
    const std::vector<uint64_t>& additionalSelectedIds,
    bool snapDisabled,
    bool& validationRejected)
{
    validationRejected = false;
    if (original == nullptr)
        return nullptr;

    const auto& originalHandles = original->handles();
    int draggedIndex = -1;
    for (int i = 0; i < static_cast<int>(originalHandles.size()); ++i) {
        if (originalHandles[static_cast<size_t>(i)].id == draggedHandleId) {
            draggedIndex = i;
            break;
        }
    }
    if (draggedIndex <= 0 || draggedIndex >= static_cast<int>(originalHandles.size()) - 1)
        return nullptr;

    const auto isSelected = [selectedHandleId, &additionalSelectedIds](uint64_t id) {
        return id != 0 && (id == selectedHandleId
            || std::find(additionalSelectedIds.begin(), additionalSelectedIds.end(), id)
                != additionalSelectedIds.end());
    };
    const bool draggedHandleSelected = isSelected(draggedHandleId);
    const bool isGroupDrag = !additionalSelectedIds.empty() && draggedHandleSelected;
    const double minSpacing = snapDisabled
        ? 0.000
        : TimeGridSnapshot::kMinOutputSpacingSeconds;
    const double minOutput = originalHandles[static_cast<size_t>(draggedIndex - 1)].output_seconds
        + minSpacing;
    const double maxOutput = originalHandles[static_cast<size_t>(draggedIndex + 1)].output_seconds
        - minSpacing;
    const double clampedOutput = juce::jlimit(minOutput, maxOutput, newOutputSeconds);
    const double uniformDelta = clampedOutput - dragStartOutputSeconds;

    std::vector<TimeHandle> newHandles(originalHandles.begin(), originalHandles.end());
    if (isGroupDrag) {
        std::vector<double> tentative(newHandles.size());
        for (size_t i = 0; i < newHandles.size(); ++i)
            tentative[i] = newHandles[i].output_seconds;

        for (size_t i = 1; i < newHandles.size() - 1; ++i) {
            if (newHandles[i].isEndpoint())
                continue;
            if (isSelected(newHandles[i].id))
                tentative[i] = newHandles[i].output_seconds + uniformDelta;
        }

        for (size_t i = 1; i < tentative.size() - 1; ++i) {
            const double localMin = tentative[i - 1] + minSpacing;
            const double localMax = tentative[i + 1] - minSpacing;
            if (localMin > localMax)
                return nullptr;
            tentative[i] = juce::jlimit(localMin, localMax, tentative[i]);
        }

        for (size_t i = 0; i < newHandles.size(); ++i)
            newHandles[i].output_seconds = tentative[i];
    } else {
        newHandles[static_cast<size_t>(draggedIndex)].output_seconds = clampedOutput;
    }

    auto snapshot = TimeGridSnapshot::makeFromHandles(std::move(newHandles));
    validationRejected = snapshot == nullptr;
    return snapshot;
}

TimeGridInsertResult planTimeGridInsert(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    double clickedOutputSeconds)
{
    TimeGridInsertResult result;
    if (clickedOutputSeconds <= 0.0) {
        result.status = TimeGridInsertStatus::InvalidTime;
        return result;
    }
    if (original == nullptr) {
        result.status = TimeGridInsertStatus::InvalidGrid;
        return result;
    }

    const auto& handles = original->handles();
    if (handles.size() < 2) {
        result.status = TimeGridInsertStatus::InvalidGrid;
        return result;
    }

    const double clickedSourceSeconds = original->tauInverse(clickedOutputSeconds);
    for (const auto& handle : handles) {
        if (std::abs(handle.output_seconds - clickedOutputSeconds)
            < TimeGridSnapshot::kMinOutputSpacingSeconds) {
            result.status = TimeGridInsertStatus::OutputSpacing;
            return result;
        }
        const double previous = std::min(handle.source_seconds, clickedSourceSeconds);
        const double current = std::max(handle.source_seconds, clickedSourceSeconds);
        if (!TimeGridSnapshot::hasMinimumSourceSpacing(previous, current)) {
            result.status = TimeGridInsertStatus::SourceSpacing;
            return result;
        }
    }

    size_t insertIndex = handles.size();
    for (size_t i = 0; i + 1 < handles.size(); ++i) {
        if (handles[i].output_seconds < clickedOutputSeconds
            && clickedOutputSeconds < handles[i + 1].output_seconds) {
            insertIndex = i + 1;
            break;
        }
    }
    if (insertIndex == handles.size()) {
        result.status = TimeGridInsertStatus::OutsideRange;
        return result;
    }

    std::vector<TimeHandle> newHandles(handles.begin(), handles.end());
    uint64_t maxId = 0;
    for (const auto& handle : newHandles)
        maxId = std::max(maxId, handle.id);

    TimeHandle insertedHandle;
    insertedHandle.id = maxId + 1;
    insertedHandle.source_seconds = clickedSourceSeconds;
    insertedHandle.output_seconds = clickedOutputSeconds;
    insertedHandle.kind = HandleKind::UserAdded;
    newHandles.insert(newHandles.begin() + static_cast<std::ptrdiff_t>(insertIndex), insertedHandle);

    result.snapshot = TimeGridSnapshot::makeFromHandles(std::move(newHandles));
    if (result.snapshot == nullptr) {
        result.status = TimeGridInsertStatus::ValidationRejected;
        return result;
    }

    result.status = TimeGridInsertStatus::Inserted;
    result.insertedHandleId = insertedHandle.id;
    return result;
}

TimeGridDeleteResult planTimeGridDelete(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    uint64_t selectedHandleId)
{
    TimeGridDeleteResult result;
    if (original == nullptr)
        return result;

    const auto& handles = original->handles();
    int targetIndex = -1;
    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
        if (handles[static_cast<size_t>(i)].id == selectedHandleId) {
            targetIndex = i;
            break;
        }
    }
    if (targetIndex == -1)
        return result;
    if (targetIndex <= 0 || targetIndex >= static_cast<int>(handles.size()) - 1
        || handles[static_cast<size_t>(targetIndex)].isEndpoint()) {
        result.status = TimeGridDeleteStatus::Endpoint;
        return result;
    }

    std::vector<TimeHandle> newHandles(handles.begin(), handles.end());
    newHandles.erase(newHandles.begin() + targetIndex);
    result.snapshot = TimeGridSnapshot::makeFromHandles(std::move(newHandles));
    if (result.snapshot == nullptr) {
        result.status = TimeGridDeleteStatus::ValidationRejected;
        return result;
    }

    result.status = TimeGridDeleteStatus::Deleted;
    return result;
}

namespace {

// 收集一个快照里 index 两侧相邻锚点的输出位置。
// 区间 [i-1, i] 与 [i, i+1] 是唯一随锚点 i 移动而改变斜率的段。
void collectNeighbourOutputs(const std::vector<TimeHandle>& handles,
                             size_t index,
                             double& lo,
                             double& hi)
{
    if (index > 0)
    {
        const double prev = handles[index - 1].output_seconds;
        lo = std::min(lo, prev);
        hi = std::max(hi, prev);
    }
    if (index + 1 < handles.size())
    {
        const double next = handles[index + 1].output_seconds;
        lo = std::min(lo, next);
        hi = std::max(hi, next);
    }
}

int indexOfId(const std::vector<TimeHandle>& handles, uint64_t id)
{
    for (size_t i = 0; i < handles.size(); ++i)
    {
        if (handles[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

bool timeGridAffectedOutputRange(
    const std::shared_ptr<const TimeGridSnapshot>& before,
    const std::shared_ptr<const TimeGridSnapshot>& after,
    double& outStartSeconds,
    double& outEndSeconds)
{
    outStartSeconds = 0.0;
    outEndSeconds = 0.0;

    if (before == nullptr || after == nullptr || before == after)
        return false;

    const auto& beforeHandles = before->handles();
    const auto& afterHandles = after->handles();
    if (beforeHandles.size() < 2 || afterHandles.size() < 2)
        return false;

    const double duration = after->totalDurationSeconds();
    if (!(duration > 0.0) || !std::isfinite(duration))
        return false;

    double lo = duration;
    double hi = 0.0;
    bool anyChange = false;

    // 位置改变、或已删除的锚点：取它在两个快照中各自相邻锚点的输出位置。
    // handle id 在拖拽与 undo/redo 之间稳定；source_seconds 插入后不可变，
    // 所以 id 相同而 output_seconds 不同即为被拖动。
    for (const auto& handle : beforeHandles)
    {
        const int afterIndex = indexOfId(afterHandles, handle.id);
        if (afterIndex < 0)
        {
            // 已删除：删除点前后的区间合并成一段，斜率和跨度都变。
            const int beforeIndex = indexOfId(beforeHandles, handle.id);
            anyChange = true;
            lo = std::min(lo, handle.output_seconds);
            hi = std::max(hi, handle.output_seconds);
            if (beforeIndex >= 0)
            {
                collectNeighbourOutputs(beforeHandles,
                                        static_cast<size_t>(beforeIndex), lo, hi);
            }
            continue;
        }

        if (std::abs(afterHandles[static_cast<size_t>(afterIndex)].output_seconds
                     - handle.output_seconds) <= 1.0e-9)
            continue;

        anyChange = true;
        const int beforeIndex = indexOfId(beforeHandles, handle.id);
        if (beforeIndex >= 0)
        {
            collectNeighbourOutputs(beforeHandles,
                                    static_cast<size_t>(beforeIndex), lo, hi);
        }
        collectNeighbourOutputs(afterHandles,
                                static_cast<size_t>(afterIndex), lo, hi);
    }

    // 新增的锚点：插入把一段拆成两段。
    for (size_t i = 0; i < afterHandles.size(); ++i)
    {
        if (indexOfId(beforeHandles, afterHandles[i].id) >= 0)
            continue;
        anyChange = true;
        collectNeighbourOutputs(afterHandles, i, lo, hi);
    }

    if (!anyChange)
        return false;

    outStartSeconds = juce::jlimit(0.0, duration, lo);
    outEndSeconds = juce::jlimit(0.0, duration, hi);
    return outEndSeconds > outStartSeconds;
}

ContentMutationRenderRange mapContentMutationRangeToOutput(
    const std::shared_ptr<const TimeGridSnapshot>& grid,
    double sourceStartSeconds,
    double sourceEndSeconds,
    double durationSeconds)
{
    // Only warped grids bake the time mapping into Stage 1, creating two timelines.
    const bool bakesGrid = grid != nullptr && !grid->empty() && !grid->isIdentity();
    if (!bakesGrid)
        return { sourceStartSeconds, sourceEndSeconds };

    const bool durationValid = std::isfinite(durationSeconds) && durationSeconds > 0.0;
    if (!durationValid
        || !std::isfinite(sourceStartSeconds)
        || !std::isfinite(sourceEndSeconds)
        || sourceEndSeconds <= sourceStartSeconds)
    {
        // Without a trustworthy range, re-render the whole track.
        if (durationValid)
            return { 0.0, durationSeconds };
        return { sourceStartSeconds, sourceEndSeconds };
    }

    // Guard for adjacent F0 frames and conditioning context, applied on the
    // source axis so it scales with each segment's slope.
    constexpr double kEdgeGuardSeconds = 0.025;
    const double start = juce::jlimit(0.0, durationSeconds,
        grid->tauForward(sourceStartSeconds - kEdgeGuardSeconds));
    const double end = juce::jlimit(0.0, durationSeconds,
        grid->tauForward(sourceEndSeconds + kEdgeGuardSeconds));

    if (!(end > start))
        return { 0.0, durationSeconds };

    return { start, end };
}

} // namespace OpenTune
