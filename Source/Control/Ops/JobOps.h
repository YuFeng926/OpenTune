// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/** Registers job.get and job.cancel (capability jobs.v1). Both run on the connection thread with the
    jobs registry only, never on the message thread. */
void registerJobOps(ControlMethodTable& table);

} // namespace OpenTune
