// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The methods of OpenTune's undo history (capability history.v1), on the message
 * thread, through OpenTune's UndoManager (the processor's only instance, which the UI uses too) and its
 * read hook (peekUndo, peekRedo, size, cursor):
 * - edit.getHistory: the tops of the undo and redo histories with
 *   their undoId, description, origin and unsafe (false).
 * - edit.undo and edit.redo: apply the top
 *   entry when it is the one the client names (expectedUndoId), or whatever it is with force. The step is applied
 *   through the editor's complete undo or redo command, exactly as the UI's Ctrl+Z applies it. The result describes
 *   the effect from snapshots before and after the step (controlDescribeEffect); a step that changed nothing is a
 *   success with noOp and UNDO_NO_EFFECT.
 */
void registerUndoOps(ControlMethodTable& table);

} // namespace OpenTune
