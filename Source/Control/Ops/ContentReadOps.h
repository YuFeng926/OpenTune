// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The content reads the dispatcher runs:
 * - content.getNotes: the notes of a content in start-time
 *   order with their indexes at the current revision, by contentId, or by placementId with timeline
 *   times through that placement; paginated.
 * - content.getPitchCurve: the original and the corrected
 *   (rendered) pitch per analysis frame, at full resolution or every stepFrame-th frame; paginated.
 * - content.segmentNotes: the original pitch segmented into
 *   notes by OpenTune's note generator (LegacyNoteGenerator::generate, as AUTO and the import call it),
 *   for analysis only; nothing is stored.
 *
 * Reads are never gated: before the pitch analysis is ready the arrays are empty and f0State says why.
 * Handles are resolved before access, with NOT_FOUND(stale_handle) or NOT_FOUND(unknown_handle) when they
 * do not identify live content. Cursors are bound to the selection params and to the tokens of the read
 * that issued them: other params give INVALID_ARGUMENT(invalid_params), changed tokens give
 * REVISION_CONFLICT(cursor_stale). Everything runs on the message thread.
 */
void registerContentReadOps(ControlMethodTable& table);

} // namespace OpenTune
