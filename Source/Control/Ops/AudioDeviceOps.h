// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers audio.get and audio.set (capability audio.device).
 *
 * Both reach the Standalone application's editor-owned device manager through ControlUiBridge::audioDevice() and use
 * Utils/AudioDeviceSettings as the shared product owner used by the preferences page. This file handles protocol
 * parameters, manager availability, API result JSON and the write envelope.
 *
 * - audio.get reports the device the page shows (type, output device, sample rate, buffer size, output channel pair)
 *   and the values the page offers for them. Without a running device the members of device other than running are
 *   null with the null reason no_audio_device; without an editor or without a device manager they are null with
 *   no_editor or unavailable and nothing is offered.
 * - audio.set applies the requested type, output device and settings through one shared operation. Changing to a type
 *   with multiple outputs requires outputDevice; a type with one output uses that device. A new device uses its own
 *   defaults for omitted sample rate and buffer size. Values that are not on offer give
 *   INVALID_ARGUMENT(audio_setting_unavailable). If the device cannot be opened, the shared owner restores the
 *   previous setup and the call fails with
 *   IO_ERROR(device_open_failed) and executed false; if the previous setup cannot be opened either, executed is true
 *   and no device runs. The setup is user-global: the result lists the change (entity audioDevice) with the warning
 *   GLOBAL_PREFERENCE, there is no undo entry (reason ui_parity), and the revert recipe is the same call with the
 *   previous values. On audio.set failure, error.data.audioDevice contains the actual device state, raw device and
 *   recovery errors, and the available output devices for the requested type. A setup equal to the current one is a
 *   no-op. Without an editor audio.set answers
 *   UNSUPPORTED(no_editor_window); with an editor that has no device manager, UNSUPPORTED(capability_missing).
 *
 * Threading: parse on the connection thread; run on the message thread, like the page.
 */
void registerAudioDeviceOps(ControlMethodTable& table);

} // namespace OpenTune
