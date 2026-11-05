// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlApiOptions.h"
#include "ControlDiscovery.h"
#include "ControlMessageThread.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace OpenTune {

class ControlDispatcher;
class ControlHistory;
class ControlJobs;
class ControlServer;
class ControlUiBridge;
class OpenTuneAudioProcessor;

/**
 * The control API of one OpenTune Standalone processor. A facade that owns the listener
 * (ControlServer, which also writes and deletes the discovery file through ControlDiscovery), the
 * message-thread marshalling (ControlMessageThread), the dispatcher (ControlDispatcher, the listener's
 * request handler) and the jobs (ControlJobs); it keeps the editor's ControlUiBridge pointer.
 *
 * Lifecycle (see Source/Control/README.md#lifecycle):
 * - The processor constructor creates the service and calls scheduleStart(). Creating it only reads
 *   the run-time options (PROTOCOL.md section 6); it starts no thread and touches no file. When the
 *   API is not enabled at run time, scheduleStart() does nothing either, so OpenTune behaves exactly
 *   as without the control API.
 * - When the API is enabled, scheduleStart() posts start() to the message thread, guarded by the
 *   liveness token of the marshalling: start() runs after the constructor has returned, and never
 *   after stop() or the destruction of the service.
 * - start() initialises the OpenTune runtime with OpenTuneAudioProcessor::ensureRuntimeInitialized()
 *   and only when that succeeds creates and starts the listener, which writes the discovery file and,
 *   with --control-api-ready-file=, the ready file.
 * - The processor destructor calls stop() as its first statement.
 *
 * Errors: nothing propagates to the processor. Every reason not to start is logged through AppLogger
 * (never with the token or the server proof) and the API stays off; OpenTune runs normally.
 *
 * Logging from network threads: the listener's diagnostic sink goes through a gate that stop() closes
 * while holding the gate's mutex. After stop() returns, no connection thread can reach AppLogger
 * through this service any more, not even one that stop() had to detach; the sink captures only the
 * gate, never the service or the processor.
 *
 * Threading: message thread, except the accessors documented as thread-safe.
 */
class ControlService
{
public:
    /** Message thread. Reads the command line (juce::JUCEApplicationBase::getCommandLineParameterArray)
        and the environment; starts nothing. */
    explicit ControlService(OpenTuneAudioProcessor& processor);
    /** Calls stop(). */
    ~ControlService();

    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;

    /** Called once at the end of the processor constructor. Posts start() to the message thread when the
        API is enabled at run time; otherwise does nothing at all. */
    void scheduleStart();

    /**
     * Starts the API when it is enabled and the options are valid: OpenTune runtime first, then the
     * listener. Runs at most once; returns whether the API is listening. Failures are logged, never
     * thrown. Normally called by the function scheduleStart() posted.
     */
    bool start();

    /**
     * PROTOCOL.md section 3.6 rule 2, in this order: sets the stop flag (requests are answered with
     * SHUTTING_DOWN from now on); kills the liveness token, which wakes every thread waiting for the
     * message thread and turns every function still queued for it into a no-op; stops the jobs (waiting
     * job.get requests are answered with SHUTTING_DOWN, no job work runs again, and the work of every job
     * is destroyed, joining whatever thread it owns); stops the listener
     * (its own stop flag, SHUTTING_DOWN for new requests, then sessions and listener close, threads are
     * joined with a bounded wait, the discovery file is deleted); deletes the ready file if it still
     * holds what this instance wrote; closes the log gate. Message thread; idempotent and re-entrant
     * (a nested call returns at once); never throws.
     */
    void stop() noexcept;

    /** True while the listener runs (between a successful start() and stop()). Thread-safe. */
    bool isRunning() const noexcept { return running_.load(); }
    /** True once stop() began. Thread-safe. */
    bool isStopping() const noexcept { return stopping_.load(); }
    /** The run-time options read at construction. */
    const ControlApiOptions& options() const noexcept { return options_; }
    /** The listening port while running, otherwise 0. Thread-safe. */
    int port() const noexcept { return port_.load(); }
    /** Path of the discovery file written by start(); empty when the API never started. stop() deletes
        the file but the path stays available. */
    const std::string& discoveryFilePath() const noexcept { return discoveryFile_; }
    /** The message-thread marshalling. Holders may keep it beyond the service; it is stopped with it. */
    std::shared_ptr<ControlMessageThread> messageThread() const noexcept { return messageThread_; }
    /** The dispatcher once the API started (it stays after stop()); nullptr when it never started.
        Holders may keep it beyond the service, for its counters; it never runs a method after stop(). */
    std::shared_ptr<ControlDispatcher> dispatcher() const noexcept { return dispatcher_; }
    /** The jobs once the API started (they stay after stop(), stopped); nullptr when it never started. */
    std::shared_ptr<ControlJobs> jobs() const noexcept { return jobs_; }
    /** What the instance knows about OpenTune's undo history (Edit/ControlOriginAction.h) once the API
        started; nullptr before. Message thread. */
    std::shared_ptr<ControlHistory> history() const noexcept { return history_; }

    /** Registers the editor's bridge, or unregisters it with nullptr. Message thread. */
    void setUiBridge(ControlUiBridge* bridge) noexcept { uiBridge_ = bridge; }
    /** The registered bridge, or nullptr. Message thread. */
    ControlUiBridge* uiBridge() const noexcept { return uiBridge_; }

    /** The identity this binary publishes in the discovery file: OPENTUNE_VERSION and the values
        cmake/OpenTuneControl.cmake records when it configures the build. */
    static ControlBuildIdentity buildIdentity();

    /** How start() initialises the runtime: OpenTuneAudioProcessor::ensureRuntimeInitialized(). */
    using RuntimeInitialiser = std::function<bool(OpenTuneAudioProcessor&)>;
    /** Test seam for the headless tests: replaces the runtime initialisation of every later start(), or
        restores it with an empty function. Never used by OpenTune itself. Message thread. */
    static void setRuntimeInitialiserForTesting(RuntimeInitialiser initialiser);
    /** sizeof(OpenTuneAudioProcessor) as compiled into the Standalone SharedCode, so that a test can
        detect a class-layout mismatch caused by missing compile definitions. */
    static std::size_t processorClassSizeForTesting() noexcept;

private:
    struct LogGate;

    bool startListening();
    void withdrawReadyFile();
    void log(const std::string& message) const;

    OpenTuneAudioProcessor& processor_;
    ControlApiOptions options_;
    std::shared_ptr<ControlMessageThread> messageThread_;
    std::shared_ptr<LogGate> logGate_;
    std::shared_ptr<ControlHistory> history_;
    std::unique_ptr<ControlServer> server_;
    std::shared_ptr<ControlDispatcher> dispatcher_;
    std::shared_ptr<ControlJobs> jobs_;
    ControlUiBridge* uiBridge_ = nullptr;
    std::string discoveryFile_;
    /** Content of the ready file as this instance wrote it; empty when it wrote none. */
    std::string readyFileContent_;
    bool startScheduled_ = false;
    bool startAttempted_ = false;
    bool stopCalled_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> port_{0};
};

} // namespace OpenTune
