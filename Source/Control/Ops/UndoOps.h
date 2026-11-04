// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The methods of OpenTune's undo history (capability history.v1, PROTOCOL.md section 15.3), on the message
 * thread, through OpenTune's UndoManager (the processor's only instance, which the UI uses too) and its
 * read hook (peekUndo, peekRedo, size, cursor):
 * - edit.getHistory (methods/edit.getHistory.schema.json): the tops of the undo and redo histories with
 *   their undoId, description, origin and whether edit.undo and edit.redo would refuse them.
 * - edit.undo and edit.redo (methods/edit.undo.schema.json, methods/edit.redo.schema.json): apply the top
 *   entry when it is the one the client names (expectedUndoId), or whatever it is with force. A clip split,
 *   merge or delete on top is refused (UNSUPPORTED(undo_unsafe_build)), also with force. The step is
 *   applied with UndoManager::undo() / redo(), exactly as the UI's Ctrl+Z applies it; then the project is
 *   marked as changed and the editor redraws the arrangement as after its own undo
 *   (ControlUiBridge::onUndoRedoApplied(), the editor's afterUndoRedo()). The result describes the effect
 *   from snapshots before and after the step (controlDescribeEffect); a step that changed nothing is a
 *   success with noOp and UNDO_NO_EFFECT.
 *
 * Track deletion barrier (section 15.3 rule 6, ControlHistory::onTrackDeleted()): an entry recorded before a track
 * deletion is possibly stale (edit.getHistory possiblyStale). Without force, an entry recorded through the API
 * that addressed a moved track fails with UNDO_CONFLICT(stale_entry), and an entry of the UI is applied with the
 * warning UNDO_ENTRY_POSSIBLY_STALE. A call with force applies either without that warning and confirms the entry.
 */
void registerUndoOps(ControlMethodTable& table);

} // namespace OpenTune
