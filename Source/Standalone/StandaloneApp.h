// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace OpenTune {
namespace StandaloneApp {

/** Requests the Standalone application's one exit sequence. Message thread.
 *
 * The OS quit request (systemRequestedQuit), the window's close button and the editor's save-and-return all enter
 * here, and ControlEditorBridge::requestQuit() enters through the window's close button handler. The coordinator
 * saves the plug-in state once, then destroys the window, the plug-in holder and the processor on the message
 * thread, requests the process runtime's asynchronous final close and keeps the dispatch loop live until that
 * completion ends the application.
 */
void requestQuit();

} // namespace StandaloneApp
} // namespace OpenTune
