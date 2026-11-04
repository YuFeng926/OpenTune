// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

namespace OpenTune {

/**
 * The reads about the application rather than the project:
 * - prefs.get (methods/prefs.get.schema.json): the processing settings, whether the first-run
 *   onboarding was shown, the language and the recent projects. Preferences belong to the installation,
 *   so the result carries no asOf. Recent project paths are untrusted data (PROTOCOL.md section 20.2).
 * - app.getLog (methods/app.getLog.schema.json): the last lines of OpenTune's log, oldest first, with
 *   the session token, the server proof and the user's home directory removed (PROTOCOL.md sections 4.5
 *   and 20.2). The lines are read from the log file at most one megabyte from its end.
 *
 * prefs.get runs on the message thread. app.getLog runs on the connection thread (its schema says
 * x-otcp.mainThread false): it reads only the log file and the instance facts snapshot, so a busy main
 * thread never delays it and it never answers TIMEOUT.
 */
void registerAppReadOps(ControlMethodTable& table);

} // namespace OpenTune
