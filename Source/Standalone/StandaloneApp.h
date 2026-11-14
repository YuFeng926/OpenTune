// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {
namespace StandaloneApp {

/** Starts the Standalone application's exit sequence after its caller has resolved project changes. Message thread.
 *
 * The window close and system quit first ask the editor to resolve a dirty project. The editor's save-and-return and
 * ControlEditorBridge enter here after their own flow or explicit discard authorization. This coordinator saves the
 * plug-in state once, then destroys the window, plug-in holder and processor on the message thread, requests the
 * process runtime's asynchronous final close and keeps the dispatch loop live until that completion ends the
 * application.
 */
void requestQuit();

} // namespace StandaloneApp
} // namespace OpenTune
