#include "ContentEditCommands.h"
#include "../Utils/PitchUtils.h"
#include "../Utils/TuningConfig.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenTune {

ContentNoteValueEditPlan ContentEditCommands::planNoteValueEdit(
    const std::vector<Note>& notes,
    const ContentNoteValueEditRequest& request) const
{
    ContentNoteValueEditPlan plan;
    plan.notes = notes;
    const float tuningHz = TuningConfig::currentTuningHz();

    const auto setParameter = [](float& field, const std::optional<double>& value) {
        if (!value)
            return false;
        const float target = static_cast<float>(*value);
        if (std::abs(static_cast<double>(field) - target) <= 1.0e-6)
            return false;
        field = target;
        return true;
    };

    for (const auto& edit : request.edits) {
        const auto position = edit.position;
        if (position >= plan.notes.size())
            continue;
        auto& note = plan.notes[position];
        bool changed = false;
        switch (edit.kind) {
            case ContentNoteValueEdit::Kind::TargetPitch: {
                const float pitch = PitchUtils::midiToFreq(static_cast<float>(edit.targetMidi), tuningHz);
                const double currentMidi = PitchUtils::freqToMidi(note.getAdjustedPitch(), tuningHz);
                const double targetMidi = PitchUtils::freqToMidi(pitch, tuningHz);
                if (note.pitchOffset != 0.0f || std::abs(currentMidi - targetMidi) >= 1.0e-4) {
                    note.pitch = pitch;
                    note.pitchOffset = 0.0f;
                    changed = true;
                }
                break;
            }
            case ContentNoteValueEdit::Kind::ShiftCents:
                if (std::abs(edit.deltaCents) > 1.0e-6) {
                    note.pitchOffset += static_cast<float>(edit.deltaCents / 100.0);
                    changed = true;
                }
                break;
            case ContentNoteValueEdit::Kind::SetPitchOffset:
                if (std::abs(note.pitchOffset - edit.pitchOffset) > 1.0e-3) {
                    note.pitchOffset = static_cast<float>(edit.pitchOffset);
                    changed = true;
                }
                break;
            case ContentNoteValueEdit::Kind::Parameters: {
                const auto retune = edit.retuneSpeed
                    ? std::optional<double>(std::clamp(*edit.retuneSpeed, 0.0, 1.0))
                    : std::nullopt;
                const auto depth = edit.vibratoDepth
                    ? std::optional<double>(std::clamp(*edit.vibratoDepth, 0.0, 100.0))
                    : std::nullopt;
                const auto rate = edit.vibratoRate
                    ? std::optional<double>(std::clamp(*edit.vibratoRate, 3.0, 12.0))
                    : std::nullopt;
                changed = setParameter(note.retuneSpeed, retune) || changed;
                changed = setParameter(note.vibratoDepth, depth) || changed;
                changed = setParameter(note.vibratoRate, rate) || changed;
                changed = setParameter(note.pitchDriftScale, edit.pitchDriftScale) || changed;
                break;
            }
        }
        if (changed) {
            note.dirty = true;
            plan.changedPositions.push_back(position);
        }
    }
    return plan;
}

} // namespace OpenTune
