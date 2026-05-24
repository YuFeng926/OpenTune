#include "TimeGridPatchBuilder.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

bool isFiniteIntent(const TimeGridIntent& intent) noexcept
{
    return std::isfinite(intent.targetSourceSeconds)
        && std::isfinite(intent.desiredOutputSeconds)
        && std::isfinite(intent.confidence);
}

bool isInsideAffectedRange(double sourceSeconds, double startSeconds, double endSeconds) noexcept
{
    return sourceSeconds >= startSeconds && sourceSeconds <= endSeconds;
}

uint64_t nextHandleId(const std::vector<TimeHandle>& handles) noexcept
{
    uint64_t nextId = 1;
    for (const auto& handle : handles) {
        nextId = std::max(nextId, handle.id + 1);
    }
    return nextId;
}

} // namespace

TimeGridPatchResult TimeGridPatchBuilder::build(const TimeGridPatchRequest& request)
{
    TimeGridPatchResult result;

    if (request.before == nullptr) {
        result.diagnostic = "AUTO Ref TimeGrid patch requires a source grid";
        return result;
    }

    const double affectedStart = std::min(request.affectedSourceStartSeconds,
                                          request.affectedSourceEndSeconds);
    const double affectedEnd = std::max(request.affectedSourceStartSeconds,
                                        request.affectedSourceEndSeconds);
    if (!std::isfinite(affectedStart) || !std::isfinite(affectedEnd)
        || affectedEnd <= affectedStart) {
        result.diagnostic = "AUTO Ref TimeGrid patch has an invalid affected source range";
        return result;
    }

    const double duration = request.before->totalDurationSeconds();
    std::vector<TimeHandle> handles;
    handles.reserve(request.before->handles().size() + request.intents.size());

    for (const auto& handle : request.before->handles()) {
        const bool removeReferenceAuto = handle.kind == HandleKind::ReferenceAuto
                                      && handle.source_seconds > affectedStart
                                      && handle.source_seconds < affectedEnd;
        if (!removeReferenceAuto) {
            handles.push_back(handle);
        }
    }

    uint64_t nextId = nextHandleId(handles);

    std::vector<TimeGridIntent> intents = request.intents;
    std::sort(intents.begin(), intents.end(),
              [](const TimeGridIntent& a, const TimeGridIntent& b) {
                  return a.targetSourceSeconds < b.targetSourceSeconds;
              });

    for (const auto& intent : intents) {
        if (!isFiniteIntent(intent)) {
            result.diagnostic = "AUTO Ref TimeGrid intent contains non-finite values";
            return result;
        }
        if (intent.targetSourceSeconds <= 0.0 || intent.targetSourceSeconds >= duration
            || intent.desiredOutputSeconds <= 0.0 || intent.desiredOutputSeconds >= duration) {
            result.diagnostic = "AUTO Ref TimeGrid intent lies outside target duration";
            return result;
        }
        if (!isInsideAffectedRange(intent.targetSourceSeconds, affectedStart, affectedEnd)) {
            result.diagnostic = "AUTO Ref TimeGrid intent lies outside affected source range";
            return result;
        }
        if (std::abs(intent.desiredOutputSeconds - intent.targetSourceSeconds) < 1.0e-9) {
            continue;
        }

        TimeHandle handle;
        handle.id = nextId++;
        handle.source_seconds = intent.targetSourceSeconds;
        handle.output_seconds = intent.desiredOutputSeconds;
        handle.kind = HandleKind::ReferenceAuto;
        handle.locked = false;
        handle.confidence = intent.confidence >= 0.75f ? Confidence::High : Confidence::Default;
        handles.push_back(handle);
    }

    std::sort(handles.begin(), handles.end(),
              [](const TimeHandle& a, const TimeHandle& b) {
                  if (a.source_seconds == b.source_seconds) {
                      return a.output_seconds < b.output_seconds;
                  }
                  return a.source_seconds < b.source_seconds;
              });

    juce::String validationError;
    if (!TimeGridSnapshot::validate(handles, validationError)) {
        result.diagnostic = validationError;
        return result;
    }

    result.after = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (result.after == nullptr) {
        result.diagnostic = "AUTO Ref produced an invalid TimeGrid";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace OpenTune
