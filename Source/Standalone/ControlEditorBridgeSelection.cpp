// SPDX-License-Identifier: AGPL-3.0-or-later
// Control selection enters the editor's selection owner, shared with arrangement and track-panel listeners.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

#include "Utils/ScaleUiMapping.h"

namespace OpenTune {

bool ControlEditorBridge::canSelect() const
{
    return true;
}

bool ControlEditorBridge::select(int trackId, std::uint64_t placementId, bool openEditor)
{
    return editor_.selectPlacementContext(trackId, placementId, openEditor);
}

std::optional<ControlUiBridge::UiScale> ControlEditorBridge::scaleForSelection(int trackId,
                                                                               std::uint64_t placementId) const
{
    // resolveScaleForPlacementContent(): the scale that applyResolvedScaleForPlacementContent() shows once the clip is
    // selected.
    const int placementIndex = editor_.processorRef_.findPlacementIndexById(trackId, placementId);
    if (placementIndex < 0)
        return std::nullopt;
    const auto key = editor_.resolveScaleForPlacementContent(trackId, placementIndex);
    UiScale scale;
    scale.rootNote = static_cast<int>(key.root);
    scale.scaleType = scaleToUiScaleType(key.scale);
    return scale;
}

} // namespace OpenTune
