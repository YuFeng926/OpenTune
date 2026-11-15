// SPDX-License-Identifier: AGPL-3.0-or-later
// Control selection enters the editor's selection owner, shared with arrangement and track-panel listeners.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

#include <utility>

namespace OpenTune {

std::optional<ControlUiBridge::SelectionPlan> ControlEditorBridge::planSelection(
    int trackId, std::uint64_t placementId, bool openEditor) const
{
    return editor_.planPlacementSelectionContext(trackId, placementId, openEditor);
}

bool ControlEditorBridge::select(SelectionPlan plan)
{
    return editor_.applyPlacementSelectionPlan(std::move(plan), true, false);
}

} // namespace OpenTune
