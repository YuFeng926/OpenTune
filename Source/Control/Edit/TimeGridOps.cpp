// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeGridOps.h"

#include "Content/ContentEditCommands.h"
#include "PluginProcessor.h"
#include "Utils/LocalizationManager.h"
#include "Utils/TimeGridEdit.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

TimeGridOps::Plan makePlan(const std::shared_ptr<const TimeGridSnapshot>& grid,
                           std::shared_ptr<const TimeGridSnapshot> after,
                           const std::vector<TimeGridOps::Move>& requestedMoves)
{
    TimeGridOps::Plan plan;
    plan.before = grid;
    plan.after = std::move(after);
    const auto& handles = grid->handles();
    for (std::size_t i = 1; i + 1 < handles.size(); ++i) {
        const auto request = std::find_if(requestedMoves.begin(), requestedMoves.end(), [&](const auto& move) {
            return move.handleId == handles[i].id;
        });
        const double afterOutput = plan.after != nullptr
            ? plan.after->handles()[i].output_seconds
            : handles[i].output_seconds;
        if (request == requestedMoves.end() && afterOutput == handles[i].output_seconds)
            continue;
        TimeGridOps::HandleResult result;
        result.handleId = handles[i].id;
        result.index = i - 1;
        result.contentSec = handles[i].source_seconds;
        result.beforeOutputSec = handles[i].output_seconds;
        result.afterOutputSec = afterOutput;
        if (request != requestedMoves.end())
            result.requestedOutputSec = request->outputSec;
        plan.handles.push_back(std::move(result));
    }
    return plan;
}

} // namespace

bool TimeGridOps::HandleResult::clamped() const noexcept
{
    constexpr double toleranceSec = 1e-9;
    return requestedOutputSec && std::abs(afterOutputSec - *requestedOutputSec) > toleranceSec;
}

std::vector<TimeHandle> TimeGridOps::innerHandles(const TimeGridSnapshot& grid)
{
    std::vector<TimeHandle> inner;
    for (const auto& handle : grid.handles())
        if (!handle.isEndpoint())
            inner.push_back(handle);
    return inner;
}

bool TimeGridOps::isSeeded(const TimeGridSnapshot* grid)
{
    if (grid == nullptr)
        return false;
    return std::any_of(grid->handles().begin(), grid->handles().end(),
                       [](const TimeHandle& handle) { return !handle.isEndpoint(); });
}

std::string TimeGridOps::handleIdText(std::uint64_t id)
{
    return std::to_string(id);
}

std::optional<std::uint64_t> TimeGridOps::findInnerHandle(
    const TimeGridSnapshot& grid, const std::string& handleId)
{
    for (const auto& handle : grid.handles())
        if (!handle.isEndpoint() && handleIdText(handle.id) == handleId)
            return handle.id;
    return std::nullopt;
}

TimeGridOps::Plan TimeGridOps::planShift(
    const std::shared_ptr<const TimeGridSnapshot>& grid,
    double startOutputSec,
    double endOutputSec,
    double deltaSec)
{
    const auto& handles = grid->handles();
    std::vector<Move> requests;
    std::vector<std::size_t> section;
    for (std::size_t i = 1; i + 1 < handles.size(); ++i)
        if (handles[i].output_seconds >= startOutputSec && handles[i].output_seconds <= endOutputSec) {
            section.push_back(i);
            requests.push_back({handles[i].id, handles[i].output_seconds + deltaSec});
        }
    if (section.empty() || deltaSec == 0.0)
        return makePlan(grid, nullptr, requests);

    const std::size_t lead = deltaSec > 0.0 ? section.back() : section.front();
    const double lower = handles[lead - 1].output_seconds + kMinSpacingSec;
    const double upper = handles[lead + 1].output_seconds - kMinSpacingSec;
    if (lower > upper)
        return makePlan(grid, nullptr, requests);

    std::vector<std::uint64_t> together;
    for (const auto index : section)
        if (index != lead)
            together.push_back(handles[index].id);
    bool validationRejected = false;
    auto after = planTimeGridDrag(grid, handles[lead].id, handles[lead].output_seconds,
                                  handles[lead].output_seconds + deltaSec, handles[lead].id,
                                  together, false, validationRejected);
    if (after != nullptr) {
        bool changed = false;
        for (std::size_t i = 0; i < handles.size(); ++i)
            changed = changed || after->handles()[i].output_seconds != handles[i].output_seconds;
        if (!changed)
            after.reset();
    }
    return makePlan(grid, std::move(after), requests);
}

TimeGridOps::Plan TimeGridOps::planMoves(
    const std::shared_ptr<const TimeGridSnapshot>& grid,
    const std::vector<Move>& moves)
{
    std::vector<TimeGridHandleMove> requested;
    requested.reserve(moves.size());
    for (const auto& move : moves)
        requested.push_back({move.handleId, move.outputSec});
    return makePlan(grid, planTimeGridMoves(grid, requested), moves);
}

juce::String TimeGridOps::moveDescription()
{
    return LOC_RAW("Drag time handle");
}

TimeGridEditAction* TimeGridOps::commit(
    OpenTuneAudioProcessor& processor,
    const ContentKey& key,
    const Plan& plan,
    const juce::String& description)
{
    if (plan.before == nullptr || plan.after == nullptr || !key.isValid())
        return nullptr;
    auto commands = processor.getContentCommands();
    if (commands == nullptr)
        return nullptr;
    return commands->commitTimeGridEdit(key, plan.before, plan.after, description);
}

} // namespace OpenTune
