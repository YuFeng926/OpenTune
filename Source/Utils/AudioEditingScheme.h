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

enum class ParameterKind
{
    RetuneSpeed = 0,
    VibratoDepth,
    VibratoRate
};

enum class ParameterTarget
{
    None = 0,
    SelectedLineAnchorSegments,
    SelectedNotes,
    FrameSelection,
    WholeClip
};

struct ParameterTargetContext
{
    bool hasSelectedNotes = false;
    bool hasSelectedLineAnchorSegments = false;
    bool hasFrameSelection = false;
    bool allowWholeClipFallback = true;
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
    bool allowWholeClipFallback = true;
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

inline bool usesVoicedOnlyEditing(Scheme scheme) noexcept
{
    return usesNotesPrimaryScheme(scheme);
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

    return !usesVoicedOnlyEditing(scheme) || isEditableVoicedFrame(originalF0[static_cast<std::size_t>(frameIndex)]);
}

inline FrameRange trimFrameRangeToEditableBounds(Scheme scheme,
                                                 const std::vector<float>& originalF0,
                                                 FrameRange requestedRange) noexcept
{
    auto trimmedRange = clampFrameRange(requestedRange, static_cast<int>(originalF0.size()));
    if (!trimmedRange.isValid() || !usesVoicedOnlyEditing(scheme)) {
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

inline ParameterTarget resolveParameterTarget(Scheme scheme,
                                             ParameterKind kind,
                                             const ParameterTargetContext& context) noexcept
{
    const bool canUseSelectedLineAnchors = kind == ParameterKind::RetuneSpeed
        && context.hasSelectedLineAnchorSegments
        && allowsLineAnchorSegmentSelection(scheme);
    if (canUseSelectedLineAnchors) {
        return ParameterTarget::SelectedLineAnchorSegments;
    }

    if (context.hasSelectedNotes) {
        return ParameterTarget::SelectedNotes;
    }

    if (context.hasFrameSelection) {
        return ParameterTarget::FrameSelection;
    }

    return context.allowWholeClipFallback ? ParameterTarget::WholeClip : ParameterTarget::None;
}

inline AutoTuneDecision resolveAutoTuneRange(Scheme scheme,
                                            const AutoTuneTargetContext& context) noexcept
{
    auto makeDecision = [](AutoTuneTarget target, const FrameRange& range) -> AutoTuneDecision {
        AutoTuneDecision decision;
        decision.target = target;
        decision.range = range;
        return decision;
    };

    auto makeNoneDecision = []() -> AutoTuneDecision {
        return AutoTuneDecision{};
    };

    auto tryDecision = [&](AutoTuneTarget target, const FrameRange& range) -> AutoTuneDecision {
        if (!range.isValid()) {
            return makeNoneDecision();
        }

        return makeDecision(target, range);
    };

    auto hasDecision = [](const AutoTuneDecision& decision) noexcept {
        return decision.target != AutoTuneTarget::None;
    };

    if (usesNotesPrimaryScheme(scheme)) {
        const auto selectedNotesDecision = tryDecision(AutoTuneTarget::SelectedNotes, context.selectedNotesRange);
        if (hasDecision(selectedNotesDecision)) {
            return selectedNotesDecision;
        }

        const auto f0SelectionDecision = tryDecision(AutoTuneTarget::FrameSelection, context.f0SelectionRange);
        if (hasDecision(f0SelectionDecision)) {
            return f0SelectionDecision;
        }

        const auto selectionAreaDecision = tryDecision(AutoTuneTarget::FrameSelection, context.selectionAreaRange);
        if (hasDecision(selectionAreaDecision)) {
            return selectionAreaDecision;
        }
    } else {
        const auto selectionAreaDecision = tryDecision(AutoTuneTarget::FrameSelection, context.selectionAreaRange);
        if (hasDecision(selectionAreaDecision)) {
            return selectionAreaDecision;
        }

        const auto f0SelectionDecision = tryDecision(AutoTuneTarget::FrameSelection, context.f0SelectionRange);
        if (hasDecision(f0SelectionDecision)) {
            return f0SelectionDecision;
        }

        const auto selectedNotesDecision = tryDecision(AutoTuneTarget::SelectedNotes, context.selectedNotesRange);
        if (hasDecision(selectedNotesDecision)) {
            return selectedNotesDecision;
        }
    }

    if (!context.allowWholeClipFallback || context.totalFrameCount <= 0) {
        return makeNoneDecision();
    }

    const FrameRange wholeClipRange { 0, context.totalFrameCount };
    return makeDecision(AutoTuneTarget::WholeClip, wholeClipRange);
}

} // namespace OpenTune::AudioEditingScheme
