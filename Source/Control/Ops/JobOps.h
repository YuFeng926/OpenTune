// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/** Registers job.get and job.cancel (capability jobs.v1). Both run on the connection thread with the
    jobs registry only, never on the message thread (PROTOCOL.md section 13.3 rule 2). */
void registerJobOps(ControlMethodTable& table);

} // namespace OpenTune
