// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/** Registers import.start (capability import.v1): imports audio files as new clips in a job of kind import. */
void registerImportOps(ControlMethodTable& table);

} // namespace OpenTune
