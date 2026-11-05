// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The project reads the dispatcher runs: project.get (methods/project.get.schema.json) and transport.get
 * (methods/transport.get.schema.json: the transport object of project.get, with an asOf that lists no
 * content).
 *
 * project.get reports, from one message-thread step, the project file and its unsaved-changes flag
 * (through the editor's bridge), every track slot, every live placement, a summary of every live
 * content, the selection (through the bridge), the transport and the history (null until the history
 * capability exists). Values that come from the editor are null with nullReasons when no editor
 * exists, never omitted; where the schema allows no null (the visible track count, the flags of the
 * project) the value is computed and its path is listed in derived. Errors: only the generic ones.
 */
void registerProjectReadOps(ControlMethodTable& table);

} // namespace OpenTune
