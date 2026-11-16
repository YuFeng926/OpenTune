#include "TimeGridEdit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

std::shared_ptr<const TimeGridSnapshot> planTimeGridMoves(
    const std::shared_ptr<const TimeGridSnapshot>& original,
    const std::vector<TimeGridHandleMove>& moves)
{
    if (original == nullptr)
        return nullptr;
    const auto& handles = original->handles();
    std::vector<double> outputs;
    outputs.reserve(handles.size());
    for (const auto& handle : handles)
        outputs.push_back(handle.output_seconds);

    std::vector<std::pair<std::size_t, double>> requested;
    for (const auto& move : moves)
        for (std::size_t i = 1; i + 1 < handles.size(); ++i)
            if (handles[i].id == move.handleId) {
                requested.emplace_back(i, move.outputSeconds);
                break;
            }
    if (requested.empty())
        return nullptr;

    if (requested.size() == 1) {
        const auto [index, output] = requested.front();
        const double lower = outputs[index - 1] + TimeGridSnapshot::kMinOutputSpacingSeconds;
        const double upper = outputs[index + 1] - TimeGridSnapshot::kMinOutputSpacingSeconds;
        if (lower > upper)
            return nullptr;
        bool validationRejected = false;
        const auto moved = planTimeGridDrag(
            original, handles[index].id, handles[index].output_seconds, output,
            handles[index].id, {}, false, validationRejected);
        if (moved == nullptr)
            return nullptr;
        for (std::size_t i = 0; i < handles.size(); ++i)
            if (moved->handles()[i].output_seconds != outputs[i])
                return moved;
        return nullptr;
    }

    auto targets = outputs;
    for (const auto& [index, output] : requested)
        targets[index] = output;
    auto limited = targets;
    for (const auto& [index, output] : requested) {
        juce::ignoreUnused(output);
        const double lower = targets[index - 1] + TimeGridSnapshot::kMinOutputSpacingSeconds;
        const double upper = targets[index + 1] - TimeGridSnapshot::kMinOutputSpacingSeconds;
        if (lower <= upper)
            limited[index] = juce::jlimit(lower, upper, targets[index]);
    }
    for (std::size_t i = 1; i + 1 < limited.size(); ++i) {
        const double lower = limited[i - 1] + TimeGridSnapshot::kMinOutputSpacingSeconds;
        const double upper = limited[i + 1] - TimeGridSnapshot::kMinOutputSpacingSeconds;
        if (lower > upper)
            return nullptr;
        limited[i] = juce::jlimit(lower, upper, limited[i]);
    }

    std::vector<TimeHandle> next(handles.begin(), handles.end());
    bool changed = false;
    for (std::size_t i = 0; i < next.size(); ++i) {
        changed = changed || limited[i] != outputs[i];
        next[i].output_seconds = limited[i];
    }
    return changed ? TimeGridSnapshot::makeFromHandles(std::move(next)) : nullptr;
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

} // namespace OpenTune
