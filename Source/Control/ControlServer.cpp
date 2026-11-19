// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlServer.h"

#include "SecureRandom.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <unordered_set>
#include <utility>

namespace OpenTune {

namespace {

using Clock = std::chrono::steady_clock;

/** Longest single network wait; every wait also checks the stop flag at least this often. */
constexpr int kPollIntervalMs = 250;
/** Bytes read from a connection at a time. */
constexpr std::size_t kReadChunkBytes = 64 * 1024;
/** While more response bytes than this wait to be sent, the server stops reading requests. */
constexpr std::size_t kOutputHighWaterBytes = 1024 * 1024;
/** How long a final response may take to leave before the connection closes anyway. */
constexpr int kCloseFlushMs = 1000;
/** While the server stops, how long a connection may take to answer the lines that already arrived
    and to send its last responses before it closes anyway. */
constexpr int kStopFlushMs = 500;
/** After the server ends a connection, it reads and discards at most this much for at most this long,
    so that unread client data does not turn the close into a reset that could destroy the response. */
constexpr int kCloseDrainMs = 250;
constexpr std::size_t kCloseDrainBytes = 64 * 1024;
/** How long stop() waits for connection threads before it detaches the ones still running. */
constexpr int kStopJoinTimeoutMs = 2000;
static_assert(kStopFlushMs < kCloseFlushMs, "closing for a stop must not wait longer than a normal close");
static_assert(kPollIntervalMs + kStopFlushMs < kStopJoinTimeoutMs,
              "a connection must notice the stop and close before stop() stops waiting for it");
/** Size of the 128-bit values and of the token, in bytes. */
constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kServerProofBytes = 16;
constexpr std::size_t kInstanceIdBytes = 16;

int millisecondsUntil(Clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return remaining <= 0 ? 0 : static_cast<int>(std::min<long long>(remaining + 1, kPollIntervalMs));
}

/** Implements only session.hello (handled by the server): every other request is answered with
    UNSUPPORTED(capability_missing), the error for a defined method the instance does not implement. */
class SessionOnlyHandler final : public ControlRequestHandler
{
public:
    std::vector<std::string> capabilities() const override { return {"session.v1"}; }

    void handleRequest(ControlRequest request, ControlCompletion completion) override
    {
        const char* capability = controlMethodCapability(request.method);
        completion.respondError("UNSUPPORTED", "capability_missing",
                                request.method + " (capability " + (capability != nullptr ? capability : "?")
                                    + ") is not implemented by this build",
                                false);
    }
};

} // namespace

/** State shared by the server, its connection threads and nothing else. */
struct ControlServer::Shared
{
    ControlLimits limits;
    std::shared_ptr<ControlRequestHandler> handler;
    std::vector<std::string> capabilities;
    std::function<void(const std::string&)> logSink;
    std::string token;
    std::string serverProof;
    std::string instanceId;
    std::string appVersion;
    std::int64_t pid = 0;
    /** Set first by stop(): new requests are answered with SHUTTING_DOWN, nothing new is accepted. */
    std::atomic<bool> stopping{false};
    /** Set by stop() after the handler ended its waiting requests: sessions answer what already
        arrived, send their pending output and close. */
    std::atomic<bool> closing{false};

    mutable std::mutex mutex;
    std::condition_variable connectionFinished;
    int pendingConnections = 0;
    int sessions = 0;
    int runningConnections = 0;

    void log(const std::string& message) const
    {
        if (logSink)
            logSink(message);
    }

    bool reports(std::string_view capability) const
    {
        return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
    }
};

struct ControlServer::Connection
{
    std::thread thread;
    std::atomic<bool> finished{false};
};

/** Output side of one connection, shared with the completions of its requests. */
struct ControlSessionChannel
{
    std::mutex mutex;
    /** The connection thread's socket while it may be written; nullptr once closing. */
    ControlSocket* socket = nullptr;
    /** False once the connection closes: later responses are discarded. */
    bool open = true;
    /** True after a send failed. */
    bool broken = false;
    std::string pending;
    std::size_t pendingSent = 0;
    struct SendNotice
    {
        std::size_t end;
        ControlCompletion::SendCallback callback;
    };
    std::deque<SendNotice> notices;
    bool finalReply = false;
    std::unordered_set<std::string> inFlight;
    Clock::time_point lastActivity = Clock::now();
    int maxLineBytes = 0;

    std::size_t unsentBytesLocked() const { return pending.size() - pendingSent; }

    /** Sends as much of the pending output as the socket takes without blocking. */
    void flush()
    {
        std::vector<std::pair<ControlCompletion::SendCallback, ControlCompletion::SendResult>> ready;
        {
            std::lock_guard<std::mutex> lock(mutex);
            while (socket != nullptr && !broken && pendingSent < pending.size()) {
                const long sent = socket->send(pending.data() + pendingSent, pending.size() - pendingSent);
                if (sent == ControlSocket::wouldBlock)
                    break;
                if (sent <= 0) {
                    broken = true;
                    break;
                }
                pendingSent += static_cast<std::size_t>(sent);
            }
            while (!notices.empty() && (notices.front().end <= pendingSent || broken)) {
                const auto result = notices.front().end <= pendingSent ? ControlCompletion::SendResult::Written
                                                                       : ControlCompletion::SendResult::Failed;
                ready.emplace_back(std::move(notices.front().callback), result);
                notices.pop_front();
            }
            if (pendingSent == pending.size()) {
                pending.clear();
                pendingSent = 0;
            } else if (pendingSent > kOutputHighWaterBytes) {
                for (auto& notice : notices)
                    notice.end -= pendingSent;
                pending.erase(0, pendingSent);
                pendingSent = 0;
            }
        }
        for (auto& [callback, result] : ready)
            callback(result);
    }

    void queueLineLocked(std::string_view line)
    {
        if (!open || broken)
            return;
        pending.append(line.data(), line.size());
        pending += '\n';
    }

    void sendLine(std::string_view line)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queueLineLocked(line);
        }
        flush();
    }

    /** Ends a request's in-flight slot and sends its response. */
    void complete(const std::string& idKey, const std::string& line, ControlCompletion::SendCallback sent)
    {
        ControlCompletion::SendResult result = ControlCompletion::SendResult::Closed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            inFlight.erase(idKey);
            if (inFlight.empty())
                lastActivity = Clock::now();
            queueLineLocked(line);
            if (sent && open && !broken) {
                finalReply = true;
                notices.push_back({pending.size(), std::move(sent)});
                sent = {};
            }
            if (broken)
                result = ControlCompletion::SendResult::Failed;
        }
        flush();
        if (sent)
            sent(result);
    }

    void close()
    {
        std::deque<SendNotice> closed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = false;
            socket = nullptr;
            pending.clear();
            pendingSent = 0;
            closed.swap(notices);
        }
        for (auto& notice : closed)
            notice.callback(ControlCompletion::SendResult::Closed);
    }
};

struct ControlCompletion::State
{
    State(std::shared_ptr<ControlSessionChannel> channelToUse, ControlRequestId requestId)
        : channel(std::move(channelToUse))
        , id(std::move(requestId))
        , idKey(id.key())
    {}

    ~State()
    {
        if (!done.exchange(true))
            deliver(controlErrorResponse(id, "INTERNAL", {}, "the request ended without a response", true));
    }

    void deliver(std::string line, ControlCompletion::SendCallback sent = {})
    {
        if (line.size() > static_cast<std::size_t>(channel->maxLineBytes))
            line = controlErrorResponse(id, "INTERNAL", {}, "the response would exceed maxLineBytes", true);
        channel->complete(idKey, line, std::move(sent));
    }

    std::shared_ptr<ControlSessionChannel> channel;
    ControlRequestId id;
    std::string idKey;
    std::atomic<bool> done{false};
};

ControlCompletion::ControlCompletion(std::shared_ptr<State> state) : state_(std::move(state)) {}

void ControlCompletion::respondResult(const JsonValue& result) const
{
    if (state_ == nullptr || state_->done.exchange(true))
        return;
    state_->deliver(controlResultResponse(state_->id, result));
}

void ControlCompletion::respondFinalResult(const JsonValue& result, SendCallback sent) const
{
    const auto state = state_;
    if (state == nullptr || state->done.exchange(true))
        return;
    state->deliver(controlResultResponse(state->id, result), std::move(sent));
}

void ControlCompletion::respondError(std::string_view kind, std::string_view reason, std::string_view detail,
                                     bool executed, std::optional<bool> retryable,
                                     const JsonValue* extraData) const
{
    if (state_ == nullptr || state_->done.exchange(true))
        return;
    state_->deliver(controlErrorResponse(state_->id, kind, reason, detail, executed, retryable, extraData));
}

bool ControlCompletion::isPending() const
{
    return state_ != nullptr && !state_->done.load();
}

/**
 * Runs one accepted connection on its own thread: the rules before authentication, session.hello,
 * then the session until the peer closes, the session idles out, a fatal framing error occurs or the
 * server stops.
 */
class ControlConnectionWorker
{
public:
    /** authenticated is set (under the shared mutex) when the connection becomes a session. */
    ControlConnectionWorker(std::shared_ptr<ControlServer::Shared> shared, ControlSocket socket,
                            Clock::time_point acceptedAt, bool& authenticated)
        : shared_(std::move(shared))
        , socket_(std::move(socket))
        , channel_(std::make_shared<ControlSessionChannel>())
        , acceptedAt_(acceptedAt)
        , readBuffer_(kReadChunkBytes)
        , authenticated_(authenticated)
    {
        channel_->socket = &socket_;
        channel_->maxLineBytes = shared_->limits.maxLineBytes;
    }

    /** However run() ended, an exception included, the channel stops referring to socket_ here:
        completions that arrive later (from any thread) find it closed and discard their responses. */
    ~ControlConnectionWorker()
    {
        channel_->close();
    }

    ControlConnectionWorker(const ControlConnectionWorker&) = delete;
    ControlConnectionWorker& operator=(const ControlConnectionWorker&) = delete;

    void run()
    {
        if (runBeforeAuthentication())
            runSession();
    }

private:
    enum class Next {
        Continue,
        CloseAfterResponse
    };

    bool stopping() const { return shared_->stopping.load(); }
    bool closing() const { return shared_->closing.load(); }

    /** The time by which this connection closes while the server stops; fixed when first asked. */
    Clock::time_point stopDeadline()
    {
        if (!stopDeadline_)
            stopDeadline_ = Clock::now() + std::chrono::milliseconds(kStopFlushMs);
        return *stopDeadline_;
    }

    void sendError(const ControlRequestId& id, std::string_view kind, std::string_view reason, std::string_view detail,
                   std::optional<bool> retryable = std::nullopt)
    {
        channel_->sendLine(controlErrorResponse(id, kind, reason, detail, false, retryable));
    }

    /** Waits until pending output has left (or the deadline passes), then closes the connection. With
        flush false nothing more is sent: used for every close that must not send a byte. While the
        server stops, the wait ends by the stop deadline (kStopFlushMs). */
    void closeConnection(bool flush)
    {
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            channel_->open = false;
        }
        if (flush) {
            auto deadline = Clock::now() + std::chrono::milliseconds(kCloseFlushMs);
            for (;;) {
                if (stopping())
                    deadline = std::min(deadline, stopDeadline());
                std::size_t unsent = 0;
                channel_->flush();
                {
                    std::lock_guard<std::mutex> lock(channel_->mutex);
                    unsent = channel_->broken ? 0 : channel_->unsentBytesLocked();
                }
                if (unsent == 0 || Clock::now() >= deadline)
                    break;
                socket_.poll(false, true, millisecondsUntil(deadline));
            }
        }
        channel_->close();
        socket_.shutdownSend();
        if (!stopping())
            drainBeforeClose();
        socket_.close();
    }

    void drainBeforeClose()
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(kCloseDrainMs);
        std::size_t drained = 0;
        while (drained < kCloseDrainBytes && !stopping()) {
            const int wait = millisecondsUntil(deadline);
            if (wait <= 0 || socket_.poll(true, false, wait) <= 0)
                return;
            const long received = socket_.receive(readBuffer_.data(), readBuffer_.size());
            if (received == ControlSocket::wouldBlock)
                continue;
            if (received <= 0)
                return;
            drained += static_cast<std::size_t>(received);
        }
    }

    /** Runs the checks before authentication. Returns true when the connection became a session. */
    bool runBeforeAuthentication()
    {
        const auto& limits = shared_->limits;
        const auto deadline = acceptedAt_ + std::chrono::milliseconds(limits.helloTimeoutMs);
        const auto readLimit = static_cast<std::size_t>(limits.preAuthReadBytes);
        for (;;) {
            if (stopping() || Clock::now() >= deadline) {
                closeConnection(false);
                return false;
            }
            const int ready = socket_.poll(true, false, millisecondsUntil(deadline));
            if (ready < 0) {
                closeConnection(false);
                return false;
            }
            if (ready == 0)
                continue;
            // Never read more than preAuthReadBytes before authentication.
            const std::size_t room = std::min(readLimit - buffer_.size(), readBuffer_.size());
            const long received = socket_.receive(readBuffer_.data(), room);
            if (received == ControlSocket::wouldBlock)
                continue;
            if (received <= 0) {
                closeConnection(false);
                return false;
            }
            const std::size_t searchFrom = buffer_.size();
            buffer_.append(readBuffer_.data(), static_cast<std::size_t>(received));
            const std::size_t lineFeed = buffer_.find('\n', searchFrom);
            if (lineFeed != std::string::npos) {
                const std::string line = buffer_.substr(0, lineFeed);
                buffer_.erase(0, lineFeed + 1);
                return handleHello(line);
            }
            if (buffer_.size() >= readLimit) {
                closeConnection(false);
                return false;
            }
        }
    }

    JsonValue helloResult() const
    {
        auto capabilities = JsonValue::array();
        for (const auto& capability : shared_->capabilities)
            capabilities.append(JsonValue::string(capability));
        auto result = JsonValue::object();
        result.appendMember("protocol", JsonValue::string(kControlProtocolVersion));
        result.appendMember("instanceId", JsonValue::string(shared_->instanceId));
        result.appendMember("pid", JsonValue::integer(shared_->pid));
        result.appendMember("serverProof", JsonValue::string(shared_->serverProof));
        result.appendMember("appVersion", JsonValue::string(shared_->appVersion));
        result.appendMember("flavor", JsonValue::string("standalone"));
        result.appendMember("capabilities", std::move(capabilities));
        result.appendMember("limits", controlLimitsToJson(shared_->limits));
        return result;
    }

    /** Handles session.hello. Returns true when the connection became a session. */
    bool handleHello(const std::string& line)
    {
        const auto parsed = parseJson(line);
        ControlRequest request;
        ControlRequestId errorId;
        std::string why;
        if (!parsed.ok || !controlParseRequest(parsed.value, request, errorId, why)
            || request.method != "session.hello") {
            // Not a session.hello request (for example an HTTP request from a web page): close
            // without sending a single byte.
            closeConnection(false);
            return false;
        }
        std::string detail;
        switch (controlCheckHello(parsed.value.find("params"), shared_->token, detail)) {
            case ControlHelloVerdict::AuthFailed:
                channel_->sendLine(controlAuthFailedResponse(request.id));
                closeConnection(true);
                return false;
            case ControlHelloVerdict::ProtocolMismatch:
                channel_->sendLine(controlProtocolMismatchResponse(request.id));
                closeConnection(true);
                return false;
            case ControlHelloVerdict::InvalidParams:
                sendError(request.id, "INVALID_ARGUMENT", "invalid_params", detail);
                closeConnection(true);
                return false;
            case ControlHelloVerdict::Accepted:
                break;
        }
        if (stopping()) {
            sendError(request.id, "SHUTTING_DOWN", {}, "the control API is stopping");
            closeConnection(true);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->sessions < shared_->limits.maxSessions) {
                ++shared_->sessions;
                --shared_->pendingConnections;
                authenticated_ = true;
            }
        }
        if (!authenticated_) {
            sendError(request.id, "BUSY", "session_limit", "the maximum number of sessions is reached", true);
            closeConnection(true);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            channel_->lastActivity = Clock::now();
        }
        channel_->sendLine(controlResultResponse(request.id, helloResult()));
        return true;
    }

    void runSession()
    {
        if (processBufferedLines() == Next::CloseAfterResponse) {
            closeConnection(true);
            return;
        }
        const auto idleTimeout = std::chrono::milliseconds(shared_->limits.idleTimeoutMs);
        for (;;) {
            // While the server stops but before closing, the session keeps reading: handleLine answers
            // new requests with SHUTTING_DOWN, and responses the handler sends from serverStopping()
            // still go out.
            if (closing()) {
                closeForStop();
                return;
            }
            std::size_t unsent = 0;
            bool broken = false;
            bool idle = false;
            bool finalReply = false;
            Clock::time_point idleDeadline;
            {
                std::lock_guard<std::mutex> lock(channel_->mutex);
                unsent = channel_->unsentBytesLocked();
                broken = channel_->broken;
                idle = channel_->inFlight.empty();
                finalReply = channel_->finalReply;
                idleDeadline = channel_->lastActivity + idleTimeout;
            }
            if (finalReply) {
                closeConnection(true);
                return;
            }
            // Idle: no request in flight and no complete line for idleTimeoutMs.
            if (broken || (idle && Clock::now() >= idleDeadline)) {
                closeConnection(false);
                return;
            }
            const bool wantRead = unsent < kOutputHighWaterBytes;
            const int wait = idle ? millisecondsUntil(idleDeadline) : kPollIntervalMs;
            const int ready = socket_.poll(wantRead, unsent > 0, std::max(wait, 1));
            if (ready < 0) {
                closeConnection(false);
                return;
            }
            if ((ready & ControlSocket::writable) != 0) {
                channel_->flush();
            }
            if ((ready & ControlSocket::readable) == 0 || (!wantRead && (ready & ControlSocket::hangUp) == 0))
                continue;
            const long received = socket_.receive(readBuffer_.data(), readBuffer_.size());
            if (received == ControlSocket::wouldBlock)
                continue;
            if (received <= 0) {
                closeConnection(false);
                return;
            }
            buffer_.append(readBuffer_.data(), static_cast<std::size_t>(received));
            if (processBufferedLines() == Next::CloseAfterResponse) {
                closeConnection(true);
                return;
            }
        }
    }

    /**
     * After the handler ended its waiting requests, answers the
     * complete lines that already arrived with SHUTTING_DOWN (handleLine does so while the server
     * stops), without waiting for more, then sends the pending output and closes, all within
     * kStopFlushMs.
     */
    void closeForStop()
    {
        const auto deadline = stopDeadline();
        Next next = Next::Continue;
        while (next == Next::Continue && Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(channel_->mutex);
                if (channel_->broken || channel_->unsentBytesLocked() >= kOutputHighWaterBytes)
                    break;
            }
            // wouldBlock: nothing more has arrived; 0 or failed: the peer closed or the connection failed.
            const long received = socket_.receive(readBuffer_.data(), readBuffer_.size());
            if (received <= 0)
                break;
            buffer_.append(readBuffer_.data(), static_cast<std::size_t>(received));
            next = processBufferedLines();
        }
        closeConnection(true);
    }

    /** Splits the buffered bytes into lines and handles each complete line. */
    Next processBufferedLines()
    {
        const auto maxLine = static_cast<std::size_t>(shared_->limits.maxLineBytes);
        std::size_t start = 0;
        for (;;) {
            const std::size_t lineFeed = buffer_.find('\n', std::max(start, scanned_));
            if (lineFeed == std::string::npos)
                break;
            const std::size_t length = lineFeed - start;
            if (length > maxLine)
                return lineTooLong();
            {
                std::lock_guard<std::mutex> lock(channel_->mutex);
                channel_->lastActivity = Clock::now();
            }
            const Next next = handleLine(std::string_view(buffer_).substr(start, length));
            start = lineFeed + 1;
            if (next != Next::Continue)
                return next;
        }
        buffer_.erase(0, start);
        scanned_ = buffer_.size();
        // Answer as soon as more than maxLineBytes arrived without an LF.
        if (buffer_.size() > maxLine)
            return lineTooLong();
        return Next::Continue;
    }

    Next lineTooLong()
    {
        sendError({}, "INVALID_ARGUMENT", "line_too_long", "the line is longer than maxLineBytes");
        return Next::CloseAfterResponse;
    }

    /** Applies the request checks in order. */
    Next handleLine(std::string_view line)
    {
        if (isJsonWhitespaceOnly(line))
            return Next::Continue;
        const auto parsed = parseJson(line);
        if (!parsed.ok) {
            sendError({}, "INVALID_ARGUMENT", "parse_error",
                      std::string("the line is not valid JSON (") + parsed.error + ")");
            return Next::Continue;
        }
        if (parsed.value.isArray()) {
            sendError({}, "INVALID_ARGUMENT", "batch_unsupported", "JSON-RPC batches are not supported");
            return Next::Continue;
        }
        if (controlIsNotification(parsed.value))
            return Next::Continue;
        ControlRequest request;
        ControlRequestId errorId;
        std::string why;
        if (!controlParseRequest(parsed.value, request, errorId, why)) {
            sendError(errorId, "INVALID_ARGUMENT", "invalid_request", why);
            return Next::Continue;
        }
        const std::string idKey = request.id.key();
        std::string refused;
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            if (channel_->inFlight.count(idKey) != 0) {
                // id null, so that it cannot be mistaken for the response of the request in flight.
                refused = controlErrorResponse({}, "INVALID_ARGUMENT", "duplicate_id",
                                               "a request with this id is still in flight", false);
            } else if (static_cast<int>(channel_->inFlight.size()) >= shared_->limits.maxInFlightPerSession) {
                refused = controlErrorResponse(request.id, "BUSY", "request_limit",
                                               "too many requests in flight on this session", false, true);
            }
        }
        if (!refused.empty()) {
            channel_->sendLine(refused);
            return Next::Continue;
        }
        if (stopping()) {
            sendError(request.id, "SHUTTING_DOWN", {}, "the control API is stopping");
            return Next::Continue;
        }
        const char* capability = controlMethodCapability(request.method);
        if (capability == nullptr) {
            sendError(request.id, "UNSUPPORTED", "unknown_method", "the method is not defined by protocol version 1");
            return Next::Continue;
        }
        if (!shared_->reports(capability)) {
            sendError(request.id, "UNSUPPORTED", "capability_missing",
                      std::string("this instance does not report the capability ") + capability);
            return Next::Continue;
        }
        if (request.method == "session.hello") {
            sendError(request.id, "INVALID_ARGUMENT", "hello_repeated", "the session is already authenticated");
            return Next::CloseAfterResponse;
        }
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            channel_->inFlight.insert(idKey);
        }
        const ControlCompletion completion(std::make_shared<ControlCompletion::State>(channel_, request.id));
        try {
            shared_->handler->handleRequest(std::move(request), completion);
        } catch (...) {
            completion.respondError("INTERNAL", {}, "the request handler failed", true);
        }
        return Next::Continue;
    }

    std::shared_ptr<ControlServer::Shared> shared_;
    ControlSocket socket_;
    std::shared_ptr<ControlSessionChannel> channel_;
    Clock::time_point acceptedAt_;
    std::vector<char> readBuffer_;
    std::string buffer_;
    /** Bytes at the start of buffer_ already known to contain no LF. */
    std::size_t scanned_ = 0;
    bool& authenticated_;
    std::optional<Clock::time_point> stopDeadline_;
};

namespace {

void runConnection(std::shared_ptr<ControlServer::Shared> shared, std::shared_ptr<ControlServer::Connection> record,
                   ControlSocket socket, Clock::time_point acceptedAt)
{
    bool authenticated = false;
    try {
        ControlConnectionWorker worker(shared, std::move(socket), acceptedAt, authenticated);
        worker.run();
    } catch (...) {
        shared->log("control API: a connection ended with an unexpected error");
    }
    // finished is set first, so that stop() never detaches a thread it could join.
    record->finished = true;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (authenticated)
            --shared->sessions;
        else
            --shared->pendingConnections;
        --shared->runningConnections;
    }
    shared->connectionFinished.notify_all();
}

} // namespace

ControlServer::ControlServer(ControlServerConfig config)
    : shared_(std::make_shared<Shared>())
    , discovery_(std::move(config.discoveryDirectory))
    , readyFile_(std::move(config.readyFile))
    , launchNonce_(std::move(config.launchNonce))
    , build_(std::move(config.build))
{
    shared_->limits = config.limits;
    shared_->handler = config.handler != nullptr ? std::move(config.handler) : std::make_shared<SessionOnlyHandler>();
    shared_->logSink = std::move(config.log);
}

ControlServer::~ControlServer()
{
    stop();
}

std::string ControlServer::instanceId() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->instanceId;
}

std::vector<std::string> ControlServer::secrets() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return {shared_->token, shared_->serverProof};
}

int ControlServer::sessionCount() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->sessions;
}

int ControlServer::pendingConnectionCount() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->pendingConnections;
}

bool ControlServer::start(std::string& error)
{
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (started_) {
        error = "the control server can be started only once";
        return false;
    }
    started_ = true;

    if (const auto problem = controlValidateBuildIdentity(build_); !problem.empty()) {
        error = "invalid build identity: " + problem;
        return false;
    }
    shared_->capabilities = shared_->handler->capabilities();
    if (!shared_->reports("session.v1")) {
        error = "the request handler does not report session.v1";
        return false;
    }
    const auto token = SecureRandom::hex(kTokenBytes);
    const auto serverProof = SecureRandom::hex(kServerProofBytes);
    const auto instanceId = SecureRandom::hex(kInstanceIdBytes);
    if (!token || !serverProof || !instanceId) {
        error = "the system random number generator failed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->token = *token;
        shared_->serverProof = *serverProof;
        shared_->instanceId = *instanceId;
        shared_->appVersion = build_.appVersion;
        shared_->pid = controlCurrentProcessId();
    }
    if (!discovery_.prepareDirectory(error))
        return false;
    if (!ControlSocket::acquireNetworking()) {
        error = "cannot initialise networking";
        return false;
    }
    networkingAcquired_ = true;
    ControlSocket listener = ControlSocket::listenOnLoopback(error);
    if (!listener.isValid()) {
        shutDown();
        return false;
    }
    port_ = listener.localPort();
    const std::int64_t startedAtMs = controlNowMs();
    try {
        acceptThread_ = std::thread(&ControlServer::acceptLoop, this, std::move(listener));
    } catch (...) {
        error = "cannot start the accepting thread";
        shutDown();
        return false;
    }

    // The listener accepts connections now; only then is the discovery file written.
    ControlDiscoveryRecord record;
    record.pid = shared_->pid;
    record.processStartTimeMs = controlProcessStartTimeMs();
    record.instanceId = *instanceId;
    record.port = port_;
    record.token = *token;
    record.serverProof = *serverProof;
    record.launchNonce = launchNonce_;
    record.build = build_;
    record.startedAtMs = startedAtMs;
    if (!discovery_.publish(record, error)) {
        shutDown();
        return false;
    }
    if (readyFile_) {
        std::string readyError;
        if (!controlWritePrivateFile(*readyFile_, controlReadyFileContent(record.pid, record.instanceId), readyError))
            shared_->log("control API: the ready file was not written: " + readyError);
    }
    running_ = true;
    shared_->log("control API: listening on 127.0.0.1:" + std::to_string(port_.load()));
    return true;
}

void ControlServer::stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (!started_ || stopped_)
        return;
    shutDown();
    if (running_.exchange(false))
        shared_->log("control API: stopped");
}

void ControlServer::shutDown()
{
    stopped_ = true;
    // From here on new requests are answered with
    // SHUTTING_DOWN; the handler ends its waiting requests with SHUTTING_DOWN; only then do the
    // sessions send their last responses and close.
    shared_->stopping = true;
    shared_->handler->serverStopping();
    shared_->closing = true;
    if (acceptThread_.joinable())
        acceptThread_.join();
    {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        shared_->connectionFinished.wait_for(lock, std::chrono::milliseconds(kStopJoinTimeoutMs),
                                             [this] { return shared_->runningConnections == 0; });
    }
    bool allJoined = true;
    for (auto& connection : connections_) {
        if (connection->finished) {
            connection->thread.join();
        } else {
            // Cannot happen while every wait is bounded. The thread owns its share of the shared state
            // (handler included); it may still call the log sink, whose captures must stay valid for
            // that (see ControlServerConfig::log).
            connection->thread.detach();
            allJoined = false;
        }
    }
    connections_.clear();
    if (!allJoined)
        shared_->log("control API: a connection thread did not end in time and was detached");
    discovery_.withdraw();
    if (networkingAcquired_ && allJoined)
        ControlSocket::releaseNetworking();
    networkingAcquired_ = false;
}

void ControlServer::acceptLoop(ControlSocket listener)
{
    while (!shared_->stopping) {
        reapFinishedConnections();
        const int ready = listener.poll(true, false, kPollIntervalMs);
        if (ready < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            continue;
        }
        if (ready == 0)
            continue;
        for (;;) {
            ControlSocket connection = listener.accept();
            if (!connection.isValid())
                break;
            admit(std::move(connection));
        }
    }
    listener.close();
}

void ControlServer::admit(ControlSocket connection)
{
    // Only loopback peers can reach a 127.0.0.1 listener; checked anyway.
    if (shared_->stopping || !connection.peerIsLoopback())
        return;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        // Close at once, without a byte, when enough connections already wait.
        if (shared_->pendingConnections >= shared_->limits.maxPreAuthConnections)
            return;
        ++shared_->pendingConnections;
        ++shared_->runningConnections;
    }
    auto record = std::make_shared<Connection>();
    try {
        record->thread = std::thread(runConnection, shared_, record, std::move(connection), Clock::now());
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            --shared_->pendingConnections;
            --shared_->runningConnections;
        }
        shared_->log("control API: cannot start a connection thread");
        return;
    }
    connections_.push_back(std::move(record));
}

void ControlServer::reapFinishedConnections()
{
    auto finished = std::partition(connections_.begin(), connections_.end(),
                                   [](const std::shared_ptr<Connection>& c) { return !c->finished; });
    for (auto it = finished; it != connections_.end(); ++it)
        (*it)->thread.join();
    connections_.erase(finished, connections_.end());
}

} // namespace OpenTune
