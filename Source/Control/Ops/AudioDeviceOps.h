// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {

class ControlMethodTable;

/**
 * Registers audio.get and audio.set (capability audio.device; methods/audio.get.schema.json and
 * methods/audio.set.schema.json).
 *
 * Both work on the Standalone application's audio device manager, which only the editor owns and which they reach
 * through ControlUiBridge::audioDevice(); they repeat the steps of OpenTune's audio settings page
 * (AudioDeviceSettingsContent in Source/Editor/Preferences/StandalonePreferencePages.cpp) on that manager and call
 * nothing but the manager and the device it opens.
 *
 * - audio.get reports the device the page shows (type, output device, sample rate, buffer size, output channel pair)
 *   and the values the page offers for them. Without a running device the members of device other than running are
 *   null with the null reason no_audio_device; without an editor or without a device manager they are null with
 *   no_editor or unavailable and nothing is offered.
 * - audio.set replaces the given members of the setup and opens the device again, as the page does when a selector
 *   changes: first the device type, then the output device, then the sample rate, buffer size and output channel pair
 *   together. Values that are not on offer give INVALID_ARGUMENT(audio_setting_unavailable) and change nothing. If the
 *   device cannot be opened, the previous setup is restored, as the page does, and the call fails with
 *   IO_ERROR(device_open_failed) and executed false; if the previous setup cannot be opened either, executed is true
 *   and no device runs. The setup is user-global: the result lists the change (entity audioDevice) with the warning
 *   GLOBAL_PREFERENCE, there is no undo entry (reason ui_parity), and the revert recipe is the same call with the
 *   previous values. A setup equal to the current one is a no-op. Without an editor audio.set answers
 *   UNSUPPORTED(no_editor_window); with an editor that has no device manager, UNSUPPORTED(capability_missing).
 *
 * Threading: parse on the connection thread; run on the message thread, like the page.
 */
void registerAudioDeviceOps(ControlMethodTable& table);

} // namespace OpenTune
