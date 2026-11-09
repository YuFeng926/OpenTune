// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * content.getDetails (methods/content.getDetails.schema.json): the state of one content besides its
 * notes and curves: duration, key, pitch shift, analysis state and the reason of a failure, whether
 * the audible render is current, the time grid with its handles and the AUTO modes. Values that depend on a placement
 * are taken through the placement named in the request, or else the first placement that shows the content.
 *
 * Reads are never gated (PROTOCOL.md section 11). Handles are resolved as section 9 rule 1 requires
 * (NOT_FOUND with stale_handle or unknown_handle). Everything runs on the message thread.
 */
void registerContentDetailsOps(ControlMethodTable& table);

} // namespace OpenTune
