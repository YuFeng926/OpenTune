// SPDX-License-Identifier: AGPL-3.0-or-later
// Workspace commands enter the editor's viewToggled() owner, shared with the transport bar and editor shortcuts.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

namespace OpenTune {

bool ControlEditorBridge::setWorkspace(bool arrangement)
{
    return editor_.setWorkspaceFromCommand(arrangement);
}

} // namespace OpenTune
