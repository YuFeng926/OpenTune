#pragma once

#include <algorithm>
#include <vector>

namespace OpenTune::AudioEditingScheme {

enum class Scheme
{
    CorrectedF0Primary = 0,
    NotesPrimary = 1
};

struct FrameRange
{
    int startFrame = 0;
    int endFrameExclusive = 0;

    bool isValid() const noexcept
    {
        return endFrameExclusive > startFrame;
    }
};

enum class ParameterTarget
{
    None = 0,
    SelectedNotes,
    FrameSelection
};

struct ParameterTargetContext
{
    bool hasSelectedNotes = false;
    bool hasFrameSelection = false;
};

enum class AutoTuneTarget
{
    None = 0,
    SelectedNotes,
    FrameSelection,
    WholeClip
};

struct AutoTuneTargetContext
{
    int totalFrameCount = 0;
    FrameRange selectedNotesRange;
    FrameRange selectionAreaRange;
    FrameRange f0SelectionRange;
};

struct AutoTuneDecision
{
    AutoTuneTarget target = AutoTuneTarget::None;
    FrameRange range;
};

inline bool usesNotesPrimaryScheme(Scheme scheme) noexcept
{
    return scheme == Scheme::NotesPrimary;
}

inline bool isEditableVoicedFrame(float frequencyHz) noexcept
{
    return frequencyHz > 0.0f;
}

inline FrameRange clampFrameRange(FrameRange range, int totalFrameCount) noexcept
{
    range.startFrame = std::clamp(range.startFrame, 0, totalFrameCount);
    range.endFrameExclusive = std::clamp(range.endFrameExclusive, 0, totalFrameCount);
    if (range.endFrameExclusive < range.startFrame) {
        range.endFrameExclusive = range.startFrame;
    }
    return range;
}

inline bool canEditFrame(Scheme scheme, const std::vector<float>& originalF0, int frameIndex) noexcept
{
    if (frameIndex < 0 || frameIndex >= static_cast<int>(originalF0.size())) {
        return false;
    }

    return !usesNotesPrimaryScheme(scheme) || isEditableVoicedFrame(originalF0[static_cast<std::size_t>(frameIndex)]);
}

inline FrameRange trimFrameRangeToEditableBounds(Scheme scheme,
                                                 const std::vector<float>& originalF0,
                                                 FrameRange requestedRange) noexcept
{
    auto trimmedRange = clampFrameRange(requestedRange, static_cast<int>(originalF0.size()));
    if (!trimmedRange.isValid() || !usesNotesPrimaryScheme(scheme)) {
        return trimmedRange;
    }

    while (trimmedRange.startFrame < trimmedRange.endFrameExclusive
           && !canEditFrame(scheme, originalF0, trimmedRange.startFrame)) {
        ++trimmedRange.startFrame;
    }

    while (trimmedRange.endFrameExclusive > trimmedRange.startFrame
           && !canEditFrame(scheme, originalF0, trimmedRange.endFrameExclusive - 1)) {
        --trimmedRange.endFrameExclusive;
    }

    return trimmedRange;
}

inline bool shouldSelectNotesForEditedFrameRange(Scheme scheme) noexcept
{
    return usesNotesPrimaryScheme(scheme);
}

inline bool allowsLineAnchorSegmentSelection(Scheme scheme) noexcept
{
    return !usesNotesPrimaryScheme(scheme);
}

inline ParameterTarget resolveParameterTarget(const ParameterTargetContext& context) noexcept
{
    if (context.hasSelectedNotes) {
        return ParameterTarget::SelectedNotes;
    }

    if (context.hasFrameSelection) {
        return ParameterTarget::FrameSelection;
    }

    return ParameterTarget::None;
}

inline AutoTuneDecision resolveAutoTuneRange(const AutoTuneTargetContext& context) noexcept
{
    auto tryResolve = [](AutoTuneTarget target, const FrameRange& range) -> AutoTuneDecision {
        if (!range.isValid()) return AutoTuneDecision{};
        return AutoTuneDecision{target, range};
    };

    if (auto d = tryResolve(AutoTuneTarget::FrameSelection, context.selectionAreaRange); d.target != AutoTuneTarget::None)
        return d;

    if (auto d = tryResolve(AutoTuneTarget::FrameSelection, context.f0SelectionRange); d.target != AutoTuneTarget::None)
        return d;

    if (auto d = tryResolve(AutoTuneTarget::SelectedNotes, context.selectedNotesRange); d.target != AutoTuneTarget::None)
        return d;

    if (context.totalFrameCount <= 0)
        return AutoTuneDecision{};

    return AutoTuneDecision{AutoTuneTarget::WholeClip, FrameRange{0, context.totalFrameCount}};
}

} // namespace OpenTune::AudioEditingScheme
