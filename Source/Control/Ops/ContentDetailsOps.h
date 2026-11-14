// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * content.getDetails: the state of one content besides its
 * notes and curves: duration, key, pitch shift, analysis state and the reason of a failure, whether
 * the audible render is current, the time grid with its handles and the AUTO modes. Values that depend on a placement
 * are taken through the placement named in the request, or else the first placement that shows the content.
 *
 * Reads are never gated. Handles are resolved before access, with NOT_FOUND(stale_handle) or
 * NOT_FOUND(unknown_handle) when they do not identify live content. Everything runs on the message thread.
 */
void registerContentDetailsOps(ControlMethodTable& table);

} // namespace OpenTune
