// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlDiscovery.h"
#include "ControlJson.h"
#include "ControlProtocol.h"
#include "ControlSocket.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace OpenTune {

struct ControlSessionChannel;

/**
 * Completion handle of one request given to a ControlRequestHandler.
 *
 * Copies share one state. The first respond call sends the response and ends the request's in-flight
 * slot; later calls are ignored. If the last copy is destroyed without a response, the server sends
 * INTERNAL (executed true), so that a lost request never occupies a slot forever. When the connection
 * has closed meanwhile, the response is discarded (PROTOCOL.md section 3.6 rule 1). A response
 * longer than maxLineBytes is replaced by INTERNAL, because the server never sends such a line.
 *
 * Threading: usable from any thread; never blocks on the network.
 */
class ControlCompletion
{
public:
    ControlCompletion() = default;

    /** Sends {"result": result}; result must be an object. */
    void respondResult(const JsonValue& result) const;

    /** Sends an error response (see controlErrorResponse for the rules on detail). */
    void respondError(std::string_view kind, std::string_view reason, std::string_view detail, bool executed,
                      std::optional<bool> retryable = std::nullopt) const;

    /** True until a response has been sent. */
    bool isPending() const;

private:
    friend class ControlConnectionWorker;
    struct State;
    explicit ControlCompletion(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

/**
 * Runs the requests that passed the server's framing and session rules. ControlDispatcher implements
 * this interface in OpenTune; without a handler the server answers every request other than
 * session.hello with UNSUPPORTED(capability_missing).
 *
 * Threading: called on connection threads, concurrently for different sessions; implementations
 * must be thread-safe and must not block a connection thread for long. ControlDispatcher waits at most
 * dispatchTimeoutMs for a method to start on the message thread and then for that method only, which
 * returns quickly; longer work is queued instead.
 */
class ControlRequestHandler
{
public:
    virtual ~ControlRequestHandler() = default;

    /** Capabilities the instance reports in session.hello; must contain "session.v1". Read once by
        ControlServer::start(). */
    virtual std::vector<std::string> capabilities() const = 0;

    /**
     * Handles a request whose method is defined by the protocol, whose capability is reported and
     * which is not session.hello (PROTOCOL.md section 3.3 rule 5 cases 1 to 11 are already applied):
     * validates params (case 12) and runs the method (case 13). Must eventually use the completion,
     * from any thread. Exceptions are caught and answered with INTERNAL.
     */
    virtual void handleRequest(ControlRequest request, ControlCompletion completion) = 0;

    /** Called by ControlServer::stop() after the stop flag is set (new requests already get
        SHUTTING_DOWN) and before any session closes, so that pending waits can end with SHUTTING_DOWN:
        responses sent from here still reach their clients. Sessions close after it returns.
        handleRequest() may still run concurrently with, or just after, this call for a request that
        passed the stop check before the flag was set; the handler must answer such requests itself
        (for example with SHUTTING_DOWN) instead of starting a wait that nothing will end. */
    virtual void serverStopping() {}
};

/** Configuration of a ControlServer. */
struct ControlServerConfig
{
    /** Discovery directory D (PROTOCOL.md section 5.1), resolved by controlResolveDiscoveryDirectory. */
    std::string discoveryDirectory;
    /** Path of the ready file (--control-api-ready-file=), if requested. */
    std::optional<std::string> readyFile;
    /** Value of --control-api-launch-nonce=, if given. */
    std::optional<std::string> launchNonce;
    /** Build identity for the discovery file and session.hello; must pass controlValidateBuildIdentity. */
    ControlBuildIdentity build;
    /** Protocol limits; the defaults are the values PROTOCOL.md section 3.4 fixes. */
    ControlLimits limits;
    /** Request handler; nullptr selects the built-in one that implements only session.hello. */
    std::shared_ptr<ControlRequestHandler> handler;
    /** Diagnostic sink. Never receives the token, the server proof or request content. Called from
        the start/stop thread and from network threads, concurrently. A connection thread that stop()
        had to detach can still call it after stop() returned and after the server is destroyed, so
        what it captures must stay valid, or the sink must turn into a no-op before its captures go
        away (ControlService passes a sink that captures only a gate it closes in stop()). */
    std::function<void(const std::string&)> log;
};

/**
 * The OTCP listener of one OpenTune instance: binds the literal address 127.0.0.1 on a port chosen
 * by the operating system, applies the rules before authentication (PROTOCOL.md section 3.5), the
 * session.hello checks (section 4), the NDJSON framing and the order of section 3.3 rule 5, the
 * limits of section 3.4, and writes and deletes the discovery file (section 5).
 *
 * One accepting thread and one thread per connection. Every network wait is a poll of at most 250 ms
 * that also checks the stop flag, and all sockets are non-blocking, so stop() finishes within a
 * bound even while clients are silent, slow or not reading.
 *
 * Threading: start() and stop() are called from one owner thread (in OpenTune the message thread).
 * The accessors are thread-safe.
 */
class ControlServer
{
public:
    explicit ControlServer(ControlServerConfig config);
    /** Calls stop(). */
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    /**
     * Generates the token, the server proof and the instance id from the system CSPRNG, checks the
     * discovery directory, starts listening and accepting, then writes the discovery file and, when
     * requested, the ready file (a ready-file failure is only logged). On any other failure nothing
     * stays open, no discovery file remains and error says why (PROTOCOL.md section 6.4). Call once.
     */
    bool start(std::string& error);

    /**
     * PROTOCOL.md section 3.6 rule 2: sets the stop flag (new requests are answered with SHUTTING_DOWN
     * and nothing new is accepted), tells the handler (serverStopping()), then lets every session
     * answer the lines that already arrived, send its pending output for a bounded time and close
     * (responses of requests still in flight after that are discarded). Joins the threads with a
     * bounded wait and deletes the discovery file. Idempotent.
     */
    void stop();

    bool isRunning() const { return running_.load(); }
    /** The listening port, 0 before start(). */
    int port() const { return port_.load(); }
    /** The instance id (not secret), empty before start(). */
    std::string instanceId() const;
    /** The session token and the server proof (secrets), after start(); empty before. Only for redacting
        them from text that leaves the process (app.getLog); never log, return or store them elsewhere. */
    std::vector<std::string> secrets() const;
    /** Path of this process's discovery file. */
    std::string discoveryFilePath() const { return discovery_.filePath(); }
    /** Number of authenticated sessions. */
    int sessionCount() const;
    /** Number of accepted connections that have not authenticated yet. */
    int pendingConnectionCount() const;

    /** Internal: state shared with the connection threads, and one connection thread. */
    struct Shared;
    struct Connection;

private:
    void acceptLoop(ControlSocket listener);
    void admit(ControlSocket connection);
    void reapFinishedConnections();
    void shutDown();

    std::shared_ptr<Shared> shared_;
    ControlDiscovery discovery_;
    std::optional<std::string> readyFile_;
    std::optional<std::string> launchNonce_;
    ControlBuildIdentity build_;
    std::mutex lifecycleMutex_;
    bool started_ = false;
    bool stopped_ = false;
    bool networkingAcquired_ = false;
    std::atomic<bool> running_{false};
    std::atomic<int> port_{0};
    std::thread acceptThread_;
    std::vector<std::shared_ptr<Connection>> connections_;
};

} // namespace OpenTune
