// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeGridOps.h"

#include "Content/ContentEditCommands.h"
#include "PluginProcessor.h"
#include "Utils/LocalizationManager.h"
#include "Utils/TimeGridEdit.h"
#include "Utils/TimeGridEditAction.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

/** A handle counts as clamped when it ends further than this from its requested time (the uniform distance of a
    group move is computed from the leading handle, which can differ from deltaSec in the last bits). */
constexpr double kClampToleranceSec = 1e-9;

/** juce::jlimit(lower, upper, value) as the time tool limits a handle; nullopt when lower > upper (no room). */
std::optional<double> limit(double lower, double upper, double value)
{
    if (!(lower <= upper))
        return std::nullopt;
    return std::min(std::max(value, lower), upper);
}

/** The second pass of the time tool's group drag (planTimeGridDrag(), which PianoRollToolHandler::
    handleTimeToolMouseDrag calls): from left to right, each inner handle limited to its limited left neighbour plus
    the spacing and its right neighbour minus the spacing. False when a handle has no room; the time tool then keeps
    the grid as it was. Used here only for the moves of several handles to times of their own (planMoves), which
    planTimeGridDrag() does not do: it moves a selection by one distance. */
bool limitInOrder(std::vector<double>& outputs)
{
    for (std::size_t i = 1; i + 1 < outputs.size(); ++i) {
        const auto limited = limit(outputs[i - 1] + TimeGridOps::kMinSpacingSec,
                                   outputs[i + 1] - TimeGridOps::kMinSpacingSec, outputs[i]);
        if (!limited)
            return false;
        outputs[i] = *limited;
    }
    return true;
}

std::optional<std::size_t> indexOf(const std::vector<TimeHandle>& handles, std::uint64_t id)
{
    for (std::size_t i = 1; i + 1 < handles.size(); ++i)
        if (handles[i].id == id)
            return i;
    return std::nullopt;
}

/** The plan of the new output times (one per handle of the grid), or of no change when room is false. */
TimeGridOps::Plan finish(const std::shared_ptr<const TimeGridSnapshot>& grid, const std::vector<double>& outputs,
                         const std::vector<std::optional<double>>& requested, bool room)
{
    TimeGridOps::Plan plan;
    plan.before = grid;
    const auto& handles = grid->handles();
    std::vector<TimeHandle> next(handles.begin(), handles.end());
    bool changed = false;
    if (room) {
        for (std::size_t i = 0; i < next.size(); ++i) {
            changed = changed || outputs[i] != handles[i].output_seconds;
            next[i].output_seconds = outputs[i];
        }
    }
    // TimeGridSnapshot::makeFromHandles(): the validation every grid of the time tool passes; a grid it rejects
    // is not applied, as the time tool keeps its last valid grid then.
    if (changed)
        plan.after = TimeGridSnapshot::makeFromHandles(next);
    const bool applied = plan.after != nullptr;
    for (std::size_t i = 1; i + 1 < handles.size(); ++i) {
        TimeGridOps::HandleResult result;
        result.handleId = handles[i].id;
        result.index = i - 1;
        result.contentSec = handles[i].source_seconds;
        result.beforeOutputSec = handles[i].output_seconds;
        result.afterOutputSec = applied ? outputs[i] : handles[i].output_seconds;
        result.requestedOutputSec = requested[i];
        if (result.requestedOutputSec || result.moved())
            plan.handles.push_back(result);
    }
    return plan;
}

/**
 * One step of the time tool's drag, by the function that the time tool itself calls (planTimeGridDrag(),
 * Source/Utils/TimeGridEdit.h): the handle `dragged` is dragged from its output time to newOutputSec, limited to its
 * neighbours plus and minus the minimum spacing, and the handles `together` (the others of a selection; empty for a
 * drag of one handle) move by the same distance, each then limited from left to right. Returns the output time of
 * every handle of the grid after the step; nullopt when the step is not made (a handle has no room, or
 * TimeGridSnapshot::makeFromHandles() rejects the grid): the time tool then keeps the grid as it was.
 *
 * The caller checks first that the dragged handle has room between its neighbours (hasRoom): planTimeGridDrag()
 * limits it with juce::jlimit(), which asserts on a lower bound above the upper one.
 */
std::optional<std::vector<double>> dragStep(const std::shared_ptr<const TimeGridSnapshot>& grid,
                                            const TimeHandle& dragged, double newOutputSec,
                                            const std::vector<std::uint64_t>& together)
{
    bool validationRejected = false;
    const auto after = planTimeGridDrag(grid, dragged.id, dragged.output_seconds, newOutputSec, dragged.id, together,
                                        false, validationRejected);
    if (after == nullptr)
        return std::nullopt;
    std::vector<double> outputs;
    outputs.reserve(after->handles().size());
    for (const auto& handle : after->handles())
        outputs.push_back(handle.output_seconds);
    return outputs;
}

/** Whether the handle at index has room between its neighbours: the time tool's first limit
    (juce::jlimit(neighbour + spacing, neighbour - spacing, ...)) has a lower bound that does not exceed the upper. */
bool hasRoom(const std::vector<double>& outputs, std::size_t index)
{
    return outputs[index - 1] + TimeGridOps::kMinSpacingSec <= outputs[index + 1] - TimeGridOps::kMinSpacingSec;
}

std::vector<double> currentOutputs(const std::vector<TimeHandle>& handles)
{
    std::vector<double> outputs;
    outputs.reserve(handles.size());
    for (const auto& handle : handles)
        outputs.push_back(handle.output_seconds);
    return outputs;
}

} // namespace

bool TimeGridOps::HandleResult::clamped() const noexcept
{
    return requestedOutputSec && std::abs(afterOutputSec - *requestedOutputSec) > kClampToleranceSec;
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
    const auto& handles = grid->handles();
    return std::any_of(handles.begin(), handles.end(), [](const TimeHandle& handle) { return !handle.isEndpoint(); });
}

std::string TimeGridOps::handleIdText(std::uint64_t id)
{
    return std::to_string(id);
}

std::optional<std::uint64_t> TimeGridOps::findInnerHandle(const TimeGridSnapshot& grid, const std::string& handleId)
{
    for (const auto& handle : grid.handles())
        if (!handle.isEndpoint() && handleIdText(handle.id) == handleId)
            return handle.id;
    return std::nullopt;
}

TimeGridOps::Plan TimeGridOps::planShift(const std::shared_ptr<const TimeGridSnapshot>& grid, double startOutputSec,
                                         double endOutputSec, double deltaSec)
{
    const auto& handles = grid->handles();
    auto outputs = currentOutputs(handles);
    std::vector<std::optional<double>> requested(handles.size());
    std::vector<std::size_t> section;
    for (std::size_t i = 1; i + 1 < handles.size(); ++i)
        if (handles[i].output_seconds >= startOutputSec && handles[i].output_seconds <= endOutputSec) {
            section.push_back(i);
            requested[i] = handles[i].output_seconds + deltaSec;
        }
    if (section.empty() || deltaSec == 0.0)
        return finish(grid, outputs, requested, true);

    // The time tool's drag of the handle that leads in the direction of the move, which stops the section at its
    // neighbour: that handle is limited by its neighbours as they are (the no-room test is ours, dragStep() needs it
    // made), and with several handles in the section they are a group drag, each moving by the distance the dragged
    // handle was allowed, then limited from left to right (planTimeGridDrag()).
    const std::size_t lead = deltaSec > 0.0 ? section.back() : section.front();
    if (!hasRoom(outputs, lead))
        return finish(grid, outputs, requested, false);
    std::vector<std::uint64_t> together;
    for (const auto i : section)
        if (i != lead)
            together.push_back(handles[i].id);
    const auto dragged = dragStep(grid, handles[lead], outputs[lead] + deltaSec, together);
    if (!dragged)
        return finish(grid, outputs, requested, false);
    return finish(grid, *dragged, requested, true);
}

TimeGridOps::Plan TimeGridOps::planMoves(const std::shared_ptr<const TimeGridSnapshot>& grid,
                                         const std::vector<Move>& moves)
{
    const auto& handles = grid->handles();
    auto outputs = currentOutputs(handles);
    std::vector<std::optional<double>> requested(handles.size());
    std::vector<std::size_t> moved;
    for (const auto& move : moves)
        if (const auto index = indexOf(handles, move.handleId)) {
            requested[*index] = move.outputSec;
            moved.push_back(*index);
        }
    if (moved.empty())
        return finish(grid, outputs, requested, true);

    if (moved.size() == 1) {
        // The time tool's drag of one handle: limited by its neighbours, which stay.
        const auto i = moved.front();
        if (!hasRoom(outputs, i))
            return finish(grid, outputs, requested, false);
        const auto dragged = dragStep(grid, handles[i], *requested[i], {});
        if (!dragged)
            return finish(grid, outputs, requested, false);
        return finish(grid, *dragged, requested, true);
    }

    // First pass: each moved handle limited by its neighbours as they are requested to be. A handle whose
    // requested neighbours leave it no room is left to the second pass.
    std::vector<double> targets = outputs;
    for (const auto i : moved)
        targets[i] = *requested[i];
    auto limitedOutputs = targets;
    for (const auto i : moved)
        if (const auto limited = limit(targets[i - 1] + kMinSpacingSec, targets[i + 1] - kMinSpacingSec, targets[i]))
            limitedOutputs[i] = *limited;
    const bool room = limitInOrder(limitedOutputs);
    return finish(grid, limitedOutputs, requested, room);
}

juce::String TimeGridOps::moveDescription()
{
    return LOC_RAW("Drag time handle");
}

std::optional<ControlOriginAction::Inner> TimeGridOps::commit(OpenTuneAudioProcessor& processor, const ContentKey& key,
                                                              const Plan& plan, const juce::String& description)
{
    if (plan.before == nullptr || plan.after == nullptr || !key.isValid())
        return std::nullopt;
    // The processor's ContentEditCommands, the instance the piano roll's commitTimeGrid uses.
    auto commands = processor.getContentCommands();
    if (commands == nullptr)
        return std::nullopt;
    auto action = std::make_unique<TimeGridEditAction>(commands, key, description, plan.before, plan.after);
    if (!commands->setTimeGrid(key, plan.after))
        return std::nullopt;
    ControlOriginAction::Inner inner;
    inner.action = std::move(action);
    inner.name = "TimeGridEditAction";
    inner.content = key;
    inner.aspect = ControlContentAspect::TimeGrid;
    return inner;
}

} // namespace OpenTune
