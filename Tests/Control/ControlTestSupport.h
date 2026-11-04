// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Helpers for the control API tests: a minimal check macro, a loopback test client, a request
// handler that stands in for the dispatcher, and a server fixture with an isolated discovery
// directory under OPENTUNE_DATA_DIR (or the build directory).

#include "Control/ControlApiOptions.h"
#include "Control/ControlDiscovery.h"
#include "Control/ControlFileIdentity.h"
#include "Control/ControlJson.h"
#include "Control/ControlProtocol.h"
#include "Control/ControlServer.h"
#include "Control/ControlSocket.h"
#include "Control/SecureRandom.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {

using Clock = std::chrono::steady_clock;

/** Time limits of the tests; generous so that a loaded build machine does not cause failures. */
constexpr int kConnectTimeoutMs = 2000;
constexpr int kSendTimeoutMs = 10000;
constexpr int kReadTimeoutMs = 5000;
constexpr int kOpenCheckMs = 150;

inline int& failureCount()
{
    static int count = 0;
    return count;
}

inline bool check(bool ok, const char* expression, const char* file, int line)
{
    if (!ok) {
        ++failureCount();
        std::cerr << "    FAILED " << std::filesystem::path(file).filename().string() << ":" << line << ": "
                  << expression << std::endl;
    }
    return ok;
}

#define CONTROL_CHECK(condition) ::OpenTune::ControlTest::check((condition), #condition, __FILE__, __LINE__)

inline void note(const std::string& message)
{
    std::cout << "    " << message << std::endl;
}

inline long long millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

inline void sleepMs(int milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/** Polls a condition until it holds or the timeout passes. */
inline bool waitUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (Clock::now() >= deadline)
            return false;
        sleepMs(5);
    }
    return true;
}

inline std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** Per-vector results of one vector runner. The id of a vector is its file path relative to the `vectors` directory
    without `.json`, which is the `id` member of every vector. When the environment variable OTCP_VECTOR_REPORT_DIR
    names a directory (an absolute path), write() puts `<directory>/<testName>.json` there:
    {"runner": "cpp-headless:<testName>", "commit": <OTCP_VECTOR_REPORT_COMMIT or "">,
     "vectors": [{"id", "status": "PASS"|"FAIL"|"SKIP", "reason"}]}.
    Without the variable nothing happens. A failure to write is reported in one line and never fails the test. Plain
    standard-library file I/O, so that tests without JUCE use it as well. */
class VectorReport
{
public:
    explicit VectorReport(std::string testName) : testName_(std::move(testName)) {}

    void pass(const std::string& id) { add(id, "PASS", ""); }
    void fail(const std::string& id, const std::string& reason) { add(id, "FAIL", reason); }
    void skip(const std::string& id, const std::string& reason) { add(id, "SKIP", reason); }

    /** Records the outcome of a vector that ran: an empty failure text is a pass. */
    void ran(const std::string& id, const std::string& failure)
    {
        if (failure.empty())
            pass(id);
        else
            fail(id, failure);
    }

    void write() const
    {
        const auto directory = controlSystemEnvironment("OTCP_VECTOR_REPORT_DIR");
        if (!directory || directory->empty())
            return;
        auto vectors = JsonValue::array();
        for (const auto& entry : entries_) {
            auto item = JsonValue::object();
            item.appendMember("id", JsonValue::string(entry.id));
            item.appendMember("status", JsonValue::string(entry.status));
            item.appendMember("reason", JsonValue::string(entry.reason));
            vectors.append(std::move(item));
        }
        auto report = JsonValue::object();
        report.appendMember("runner", JsonValue::string("cpp-headless:" + testName_));
        const auto commit = controlSystemEnvironment("OTCP_VECTOR_REPORT_COMMIT");
        report.appendMember("commit", JsonValue::string(commit ? *commit : std::string()));
        report.appendMember("vectors", std::move(vectors));
        try {
            const auto folder = std::filesystem::u8path(*directory);
            if (!folder.is_absolute()) {
                std::cout << "    vector report for " << testName_
                          << " could not be written: OTCP_VECTOR_REPORT_DIR is not absolute" << std::endl;
                return;
            }
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            std::ofstream out(folder / std::filesystem::u8path(testName_ + ".json"), std::ios::binary);
            out << toJsonString(report);
            out.close();
            if (!out)
                std::cout << "    vector report for " << testName_ << " could not be written" << std::endl;
        } catch (const std::exception&) {
            std::cout << "    vector report for " << testName_ << " could not be written" << std::endl;
        }
    }

private:
    struct Entry
    {
        std::string id;
        std::string status;
        std::string reason;
    };

    void add(const std::string& id, const char* status, const std::string& reason)
    {
        entries_.push_back({id, status, reason});
    }

    std::string testName_;
    std::vector<Entry> entries_;
};

/** The check behind every must-run list: the vector with this id passed. A must-run vector that is missing from the
    copy of the specification never passes, so a renamed or deleted vector fails the list instead of being skipped. */
inline bool mustRunHolds(const std::set<std::string>& passedIds, const std::string& id)
{
    return passedIds.count(id) == 1;
}

/** Root for test files: OPENTUNE_DATA_DIR when set (CMake sets one per processor scenario), otherwise a
    directory under the build tree. Never the user's real data directories. */
inline std::filesystem::path testDataRoot()
{
    if (const auto fromEnvironment = controlSystemEnvironment("OPENTUNE_DATA_DIR");
        fromEnvironment && !fromEnvironment->empty())
        return std::filesystem::u8path(*fromEnvironment).make_preferred();
    return std::filesystem::u8path(OPENTUNE_CONTROL_TEST_DATA_DIR).make_preferred();
}

/** A fresh, empty directory for one test case. */
inline std::filesystem::path freshDirectory(const std::string& name)
{
    static std::atomic<int> counter{0};
    const auto path =
        testDataRoot() / (name + "-" + std::to_string(controlCurrentProcessId()) + "-" + std::to_string(++counter));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path, ignored);
    return path;
}

inline std::string pathString(const std::filesystem::path& path)
{
    return path.u8string();
}

/**
 * A test file that uses this class on Windows must include ControlWinAcl.h after the JUCE headers: the Windows half
 * of the members is defined there, and without it the test links on POSIX but fails to link on Windows only.
 *
 * Establishes the vectors' file kind noAccess: an existing file in a directory the current user cannot open, so
 * that controlFileIdentity() on it fails (neither found nor missing). Everything is made inside a directory the
 * caller gives, which is a test directory and never a user directory. The destructor restores the rights, so
 * that the test directory can be deleted.
 *
 * One instance manages one file: while it holds a locked file, create() refuses (returns an empty path) and
 * isLocked() is true; call restore() first to reuse the instance.
 *
 * POSIX: the directory gets no permission bits at all. Windows: the Win32 ACL API adds entries that deny the
 * current user (by SID, from the process token) the attributes of the file and the listing of the directory;
 * the saved original DACLs are written back on restore. Nothing goes through a shell or an ANSI code page, and
 * no step throws. create() checks the effect and returns an empty path when it was not reached (a user who
 * ignores the rights, such as root).
 */
class NoAccessFile
{
public:
    NoAccessFile() = default;
    NoAccessFile(const NoAccessFile&) = delete;
    NoAccessFile& operator=(const NoAccessFile&) = delete;
    ~NoAccessFile() { restore(); }

    /** True while this instance holds a locked file. */
    bool isLocked() const { return locked_; }

    /** The path of the file in its locked directory, or an empty string when none could be made. */
    std::string create(const std::filesystem::path& parent, const std::string& name)
    {
        if (locked_)
            return {};
        std::error_code ec;
        directory_ = parent / "locked-directory";
        std::filesystem::create_directories(directory_, ec);
        const auto file = directory_ / std::filesystem::u8path(name);
        {
            std::ofstream out(file, std::ios::binary);
            out << "locked";
        }
        locked_ = true;
#if defined(_WIN32)
        file_ = file;
        const bool denied = deny(file_, 0x80UL /* FILE_READ_ATTRIBUTES */, fileSaved_)
                            && deny(directory_, 0x1UL /* FILE_LIST_DIRECTORY */, directorySaved_);
#else
        std::filesystem::permissions(directory_, std::filesystem::perms::none, ec);
        const bool denied = !ec;
#endif
        ControlFileId id;
        if (denied && controlFileIdentity(pathString(file), id) == ControlFileIdStatus::Failed)
            return pathString(file);
        restore();
        return {};
    }

    /** Gives the rights back. Safe to call more than once. */
    void restore()
    {
        if (!locked_)
            return;
        locked_ = false;
#if defined(_WIN32)
        writeBack(directory_, directorySaved_);
        writeBack(file_, fileSaved_);
#else
        std::error_code ec;
        std::filesystem::permissions(directory_, std::filesystem::perms::owner_all, ec);
#endif
    }

private:
#if defined(_WIN32)
    /** The original DACL of a path; dacl points into descriptor. Plain pointers, so that this header needs no
        windows.h (its macros break the JUCE headers): the bodies are in ControlWinAcl.h, which the test files
        that use NoAccessFile include after the JUCE headers. A path can have a NULL DACL (everyone has full access),
        which differs from an empty one: daclWasNull records it, and the write-back then restores a NULL DACL. */
    struct SavedDacl
    {
        void* descriptor = nullptr;
        void* dacl = nullptr;
        bool daclWasNull = false;
        bool changed = false;
    };

    /** Adds a DENY entry for the current user (by SID) to the DACL of path, keeping the original in saved. */
    static bool deny(const std::filesystem::path& path, unsigned long rights, SavedDacl& saved);
    /** Writes the saved original DACL back (when it was changed) and releases it. */
    static void writeBack(const std::filesystem::path& path, SavedDacl& saved);

    std::filesystem::path file_;
    SavedDacl fileSaved_;
    SavedDacl directorySaved_;
#endif
    std::filesystem::path directory_;
    bool locked_ = false;
};

/** A loopback client with line reading and close detection. */
class TestClient
{
public:
    bool connect(int port)
    {
        socket_ = ControlSocket::connectToLoopback(port, kConnectTimeoutMs);
        buffer_.clear();
        closed_ = false;
        return socket_.isValid();
    }

    bool isConnected() const { return socket_.isValid() && !closed_; }

    bool sendRaw(std::string_view bytes, int timeoutMs = kSendTimeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const long count = socket_.send(bytes.data() + sent, bytes.size() - sent);
            if (count == ControlSocket::failed)
                return false;
            if (count > 0) {
                sent += static_cast<std::size_t>(count);
                continue;
            }
            if (Clock::now() >= deadline)
                return false;
            socket_.poll(false, true, 50);
        }
        return true;
    }

    bool sendLine(std::string_view line) { return sendRaw(std::string(line) + "\n"); }

    /** One line without its LF; nullopt when the connection closes first or the timeout passes. */
    std::optional<std::string> readLine(int timeoutMs = kReadTimeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const std::size_t lineFeed = buffer_.find('\n');
            if (lineFeed != std::string::npos) {
                std::string line = buffer_.substr(0, lineFeed);
                buffer_.erase(0, lineFeed + 1);
                return line;
            }
            if (closed_ || !fill(deadline))
                return std::nullopt;
        }
    }

    /**
     * Waits until the server closes the connection. Returns true when it closed within the timeout and
     * sets bytes to the number of bytes that arrived (unread buffered bytes included) before the close.
     * A reset counts as a close.
     */
    bool waitClosed(int timeoutMs, std::size_t& bytes)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!closed_) {
            if (!fill(deadline))
                break;
        }
        bytes = buffer_.size();
        return closed_;
    }

    /** True when the connection neither closes nor delivers data within the given time. */
    bool staysQuiet(int milliseconds)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
        while (Clock::now() < deadline) {
            if (!buffer_.empty() || closed_)
                return false;
            fill(deadline);
        }
        return buffer_.empty() && !closed_;
    }

    void close()
    {
        socket_.close();
        closed_ = true;
    }

private:
    // Reads what is available before the deadline. False on timeout or close.
    bool fill(Clock::time_point deadline)
    {
        char chunk[65536];
        for (;;) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0)
                return false;
            const int ready = socket_.poll(true, false, static_cast<int>(std::min<long long>(remaining, 100)));
            if (ready < 0) {
                closed_ = true;
                return false;
            }
            if (ready == 0)
                continue;
            const long count = socket_.receive(chunk, sizeof(chunk));
            if (count == ControlSocket::wouldBlock)
                continue;
            if (count <= 0) {
                closed_ = true;
                return false;
            }
            buffer_.append(chunk, static_cast<std::size_t>(count));
            return true;
        }
    }

    ControlSocket socket_;
    std::string buffer_;
    bool closed_ = true;
};

/**
 * Stands in for the dispatcher. session.info is answered with an empty result (its real result is
 * the dispatcher's job) and with INVALID_ARGUMENT(invalid_params) when params has members. job.get is
 * held until release(), so that requests stay in flight. transport.get is held too, and in addition
 * keeps the connection thread inside handleRequest until unblock(), so that later lines arrive without
 * being read. project.get answers with a result of bigResultBytes, to produce output faster than a
 * client reads. Everything else is answered with UNSUPPORTED(capability_missing).
 *
 * With answerJobGetAfterWait, job.get answers for the job ids in knownJobIds only; any other id is answered as the
 * dispatcher answers an id it does not retain, with NOT_FOUND(job_not_found) and executed false.
 *
 * serverStopping() runs onStopping (if set), then, with endHeldOnStop, ends every held request with
 * SHUTTING_DOWN as the dispatcher ends its waiting job.get requests.
 */
class TestHandler : public ControlRequestHandler
{
public:
    explicit TestHandler(std::vector<std::string> capabilities = {"session.v1"})
        : capabilities_(std::move(capabilities))
    {}

    std::vector<std::string> capabilities() const override { return capabilities_; }

    void handleRequest(ControlRequest request, ControlCompletion completion) override
    {
        if (request.method == "session.info") {
            if (request.params.size() != 0)
                completion.respondError("INVALID_ARGUMENT", "invalid_params", "session.info takes no params", false);
            else
                completion.respondResult(JsonValue::object());
            return;
        }
        if (request.method == "job.get" && answerJobGetAfterWait) {
            // Stands in for a job.get on a running job: waits waitMs, then answers with the job, still running.
            const JsonValue* wait = request.params.find("waitMs");
            const JsonValue* jobId = request.params.find("jobId");
            const auto waitMs = wait != nullptr && wait->isNumber() ? static_cast<int>(wait->asDouble()) : 0;
            const auto id = jobId != nullptr ? jobId->text() : std::string();
            if (knownJobIds.count(id) == 0) {
                completion.respondError("NOT_FOUND", "job_not_found", "the test handler retains no job with this id",
                                        false);
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            waiters_.emplace_back([this, completion, waitMs, id] {
                std::unique_lock<std::mutex> waiting(mutex_);
                if (heldChanged_.wait_for(waiting, std::chrono::milliseconds(waitMs), [this] { return closing_; })) {
                    waiting.unlock();
                    completion.respondError("SHUTTING_DOWN", {}, "the control API is stopping", false);
                    return;
                }
                waiting.unlock();
                const auto parsed = parseJson(
                    "{\"job\":{\"jobId\":" + toJsonString(JsonValue::string(id))
                    + ",\"kind\":\"export\",\"state\":\"running\",\"cancellable\":true,\"createdAtMs\":1,"
                      "\"updatedAtMs\":1,\"finishedAtMs\":null,\"progress\":{},\"result\":null,\"error\":null}}");
                completion.respondResult(parsed.value);
            });
            return;
        }
        if (request.method == "job.get") {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                held_.push_back(completion);
            }
            heldChanged_.notify_all();
            return;
        }
        if (request.method == "transport.get") {
            std::unique_lock<std::mutex> lock(mutex_);
            held_.push_back(completion);
            ++blocked_;
            heldChanged_.notify_all();
            heldChanged_.wait_for(lock, std::chrono::milliseconds(kReadTimeoutMs), [this] { return unblocked_; });
            return;
        }
        if (request.method == "project.get") {
            auto result = JsonValue::object();
            result.appendMember("pad", JsonValue::string(std::string(bigResultBytes, 'p')));
            completion.respondResult(result);
            return;
        }
        completion.respondError("UNSUPPORTED", "capability_missing", "not implemented by the test handler", false);
    }

    void serverStopping() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        heldChanged_.notify_all();
        if (onStopping)
            onStopping();
        if (endHeldOnStop) {
            std::vector<ControlCompletion> held;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                held.swap(held_);
            }
            for (const auto& completion : held)
                completion.respondError("SHUTTING_DOWN", {}, "the control API is stopping", false);
        }
        ++stoppingCalls;
    }

    /** Waits until at least count requests are held. */
    bool waitForHeld(std::size_t count, int timeoutMs = kReadTimeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return heldChanged_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                     [this, count] { return held_.size() >= count; });
    }

    /** Waits until a transport.get request keeps a connection thread inside handleRequest. */
    bool waitForBlocked(int timeoutMs = kReadTimeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return heldChanged_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return blocked_ > 0; });
    }

    /** Lets every transport.get request, current and future, return from handleRequest. */
    void unblock()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            unblocked_ = true;
        }
        heldChanged_.notify_all();
    }

    /** Answers every held request with an empty result. */
    void releaseAll()
    {
        std::vector<ControlCompletion> held;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            held.swap(held_);
        }
        for (const auto& completion : held)
            completion.respondResult(JsonValue::object());
    }

    ~TestHandler() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        heldChanged_.notify_all();
        for (auto& waiter : waiters_)
            waiter.join();
    }

    /** Set before the first request: job.get answers after its waitMs with a running export job. */
    bool answerJobGetAfterWait = false;
    /** Set before the first request, with answerJobGetAfterWait: the ids of the jobs that exist. */
    std::set<std::string> knownJobIds;
    std::size_t bigResultBytes = 1024 * 1024;
    std::atomic<int> stoppingCalls{0};
    /** Set before stop(); both are read on the thread that calls stop(). */
    bool endHeldOnStop = false;
    std::function<void()> onStopping;

private:
    std::vector<std::string> capabilities_;
    std::mutex mutex_;
    std::condition_variable heldChanged_;
    std::vector<ControlCompletion> held_;
    int blocked_ = 0;
    bool unblocked_ = false;
    bool closing_ = false;
    std::vector<std::thread> waiters_;
};

inline ControlBuildIdentity testBuildIdentity()
{
    ControlBuildIdentity identity;
    identity.appVersion = "0.0.0-test";
    identity.commit = "0123456789abcdef0123456789abcdef01234567";
    identity.sourceUrl = "https://example.invalid/source";
    return identity;
}

/** A running server with an isolated discovery directory and the parsed discovery file. */
class ServerFixture
{
public:
    std::filesystem::path directory;
    std::shared_ptr<TestHandler> handler;
    std::unique_ptr<ControlServer> server;
    JsonValue discovery;
    std::string token;
    int port = 0;

    /** Log lines the server wrote (checked for secrets). */
    std::vector<std::string> logLines() const
    {
        std::lock_guard<std::mutex> lock(*logMutex_);
        return *logLines_;
    }

    bool start(const std::string& name, ControlLimits limits = {},
               std::vector<std::string> capabilities = {"session.v1"}, bool useTestHandler = true)
    {
        directory = freshDirectory(name) / "instances";
        ControlServerConfig config;
        config.discoveryDirectory = pathString(directory);
        config.build = testBuildIdentity();
        config.limits = limits;
        if (useTestHandler) {
            handler = std::make_shared<TestHandler>(std::move(capabilities));
            config.handler = handler;
        }
        auto lines = logLines_;
        auto mutex = logMutex_;
        config.log = [lines, mutex](const std::string& line) {
            std::lock_guard<std::mutex> lock(*mutex);
            lines->push_back(line);
        };
        server = std::make_unique<ControlServer>(std::move(config));
        std::string error;
        if (!CONTROL_CHECK(server->start(error))) {
            note("start failed: " + error);
            return false;
        }
        const auto parsed = parseJson(readFile(std::filesystem::u8path(server->discoveryFilePath())));
        if (!CONTROL_CHECK(parsed.ok && parsed.value.isObject()))
            return false;
        discovery = parsed.value;
        token = discovery.find("token")->text();
        port = static_cast<int>(discovery.find("port")->asDouble());
        return CONTROL_CHECK(port == server->port());
    }

    std::string helloLine(const std::string& idJson = "\"hello\"", const std::string& protocol = "1.0") const
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"method\":\"session.hello\",\"params\":{\"token\":\""
               + token + "\",\"protocol\":\"" + protocol
               + "\",\"client\":{\"name\":\"control-unit-tests\",\"version\":\"1.0.0\"}}}";
    }

    /** Connects and completes session.hello; returns the parsed hello response. */
    std::optional<JsonValue> authenticate(TestClient& client, const std::string& idJson = "\"hello\"")
    {
        if (!client.connect(port) || !client.sendLine(helloLine(idJson)))
            return std::nullopt;
        const auto line = client.readLine();
        if (!line)
            return std::nullopt;
        auto parsed = parseJson(*line);
        if (!parsed.ok || parsed.value.find("result") == nullptr)
            return std::nullopt;
        return parsed.value;
    }

    ~ServerFixture()
    {
        if (handler)
            handler->unblock();
        if (server)
            server->stop();
        if (handler)
            handler->releaseAll();
    }

private:
    std::shared_ptr<std::vector<std::string>> logLines_ = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::mutex> logMutex_ = std::make_shared<std::mutex>();
};

/** The error member of a response line, or nullptr. */
inline const JsonValue* errorData(const JsonValue& response)
{
    const JsonValue* error = response.find("error");
    return error != nullptr ? error->find("data") : nullptr;
}

/** True when the line is an error response with this kind and reason (reason "" for none). */
inline bool isError(const std::optional<std::string>& line, const std::string& kind, const std::string& reason)
{
    if (!line)
        return false;
    const auto parsed = parseJson(*line);
    if (!parsed.ok)
        return false;
    const JsonValue* data = errorData(parsed.value);
    if (data == nullptr || data->find("kind") == nullptr || data->find("kind")->text() != kind)
        return false;
    const JsonValue* reasonMember = data->find("reason");
    return reason.empty() ? reasonMember == nullptr : (reasonMember != nullptr && reasonMember->text() == reason);
}

/** The id member of a response line as it was written, or "" when absent. */
inline std::string responseIdText(const std::optional<std::string>& line)
{
    if (!line)
        return {};
    const auto parsed = parseJson(*line);
    const JsonValue* id = parsed.ok ? parsed.value.find("id") : nullptr;
    if (id == nullptr)
        return {};
    return id->isString() ? "\"" + id->text() + "\"" : toJsonString(*id);
}

/**
 * The bindings and matchers of the specification's vectors (Tests/Control/spec/otcp/v1/vectors/README.md):
 * replaces $ref matchers in values to send, matches expected values ($any, $approx with $tol, $capture,
 * $ref, JSON equality with numbers by value) and checks that a number id is written as a plain integer
 * literal (matching rule 4). Shared by the vector runners of the channel tests and of the processor tests.
 */
class VectorMatcher
{
public:
    /** Bound names: discovery.*, runner.*, the names a precondition establishes and captures. */
    std::map<std::string, JsonValue> bindings;

    /** An object whose member names all start with '$'. */
    static bool isMatcher(const JsonValue& value)
    {
        if (!value.isObject() || value.size() == 0)
            return false;
        for (std::size_t i = 0; i < value.size(); ++i)
            if (value.keyAt(i).empty() || value.keyAt(i)[0] != '$')
                return false;
        return true;
    }

    /** The value with every {"$ref": name} replaced by the bound value; sets failure for other matchers
        and for unbound names. */
    JsonValue substitute(const JsonValue& value, std::string& failure) const
    {
        if (isMatcher(value)) {
            const JsonValue* reference = value.find("$ref");
            const auto bound = reference != nullptr ? bindings.find(reference->text()) : bindings.end();
            if (bound == bindings.end())
                failure = "unbound or unsupported matcher in a value to send";
            return bound != bindings.end() ? bound->second : JsonValue();
        }
        if (value.isArray()) {
            auto out = JsonValue::array();
            for (std::size_t i = 0; i < value.size(); ++i)
                out.append(substitute(value.at(i), failure));
            return out;
        }
        if (value.isObject()) {
            auto out = JsonValue::object();
            for (std::size_t i = 0; i < value.size(); ++i)
                out.appendMember(value.keyAt(i), substitute(value.at(i), failure));
            return out;
        }
        return value;
    }

    /** JSON equality; numbers compare by value, object members in any order. */
    static bool equal(const JsonValue& a, const JsonValue& b)
    {
        if (a.type() != b.type())
            return false;
        switch (a.type()) {
            case JsonValue::Type::Null:
                return true;
            case JsonValue::Type::Boolean:
                return a.asBoolean() == b.asBoolean();
            case JsonValue::Type::Number:
                return a.asDouble() == b.asDouble();
            case JsonValue::Type::String:
                return a.text() == b.text();
            case JsonValue::Type::Array:
                if (a.size() != b.size())
                    return false;
                for (std::size_t i = 0; i < a.size(); ++i)
                    if (!equal(a.at(i), b.at(i)))
                        return false;
                return true;
            case JsonValue::Type::Object:
                if (a.size() != b.size())
                    return false;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    const JsonValue* other = b.find(a.keyAt(i));
                    if (other == nullptr || !equal(a.at(i), *other))
                        return false;
                }
                return true;
        }
        return false;
    }

    /** Matching rules 1 and 2: expected members must be present and match; extra actual members are
        allowed; arrays match element by element. where names the position for failure. */
    bool match(const JsonValue& expected, const JsonValue& actual, const std::string& where, std::string& failure)
    {
        if (isMatcher(expected))
            return matchMatcher(expected, actual, where, failure);
        if (expected.isObject()) {
            if (!actual.isObject()) {
                failure = where + ": not an object";
                return false;
            }
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const JsonValue* member = actual.find(expected.keyAt(i));
                if (member == nullptr) {
                    failure = where + "." + expected.keyAt(i) + ": missing";
                    return false;
                }
                if (!match(expected.at(i), *member, where + "." + expected.keyAt(i), failure))
                    return false;
            }
            return true;
        }
        if (expected.isArray()) {
            if (!actual.isArray() || actual.size() != expected.size()) {
                failure = where + ": array of another length";
                return false;
            }
            for (std::size_t i = 0; i < expected.size(); ++i)
                if (!match(expected.at(i), actual.at(i), where + "[" + std::to_string(i) + "]", failure))
                    return false;
            return true;
        }
        if (!equal(expected, actual)) {
            failure = where + ": expected " + toJsonString(expected) + ", got " + toJsonString(actual);
            return false;
        }
        return true;
    }

    /** Reads the next line of the connection and matches it; empty on success, otherwise why not. */
    std::string expectLine(TestClient& connection, const JsonValue& expected)
    {
        const auto line = connection.readLine();
        if (!line)
            return "no response line";
        const auto actual = parseJson(*line);
        if (!actual.ok)
            return "the response is not JSON";
        std::string failure;
        if (!match(expected, actual.value, "$", failure))
            return failure;
        // Matching rule 4: a number id is written as a plain integer literal.
        const JsonValue* expectedId = expected.find("id");
        if (expectedId != nullptr && expectedId->isNumber()) {
            const std::string& literal = actual.value.find("id")->text();
            const bool plain = !literal.empty() && literal.find_first_not_of("0123456789") == std::string::npos
                               && (literal.size() == 1 || literal[0] != '0');
            if (!plain)
                return "the id is not written as a plain integer: " + literal;
        }
        return {};
    }

private:
    bool matchMatcher(const JsonValue& matcher, const JsonValue& actual, const std::string& where, std::string& failure)
    {
        if (const JsonValue* reference = matcher.find("$ref")) {
            const auto bound = bindings.find(reference->text());
            if (bound == bindings.end() || !equal(bound->second, actual)) {
                failure = where + ": does not equal " + reference->text();
                return false;
            }
            return true;
        }
        if (const JsonValue* any = matcher.find("$any"); any != nullptr && any->isString()) {
            const std::string& type = any->text();
            const bool ok = (type == "string" && actual.isString()) || (type == "number" && actual.isNumber())
                            || (type == "integer" && actual.isNumber()
                                && actual.asDouble() == static_cast<double>(static_cast<long long>(actual.asDouble())))
                            || (type == "boolean" && actual.isBoolean()) || (type == "object" && actual.isObject())
                            || (type == "array" && actual.isArray()) || (type == "null" && actual.isNull());
            if (!ok) {
                failure = where + ": not of type " + type;
                return false;
            }
        }
        if (const JsonValue* approx = matcher.find("$approx")) {
            const JsonValue* tolerance = matcher.find("$tol");
            const double tol = tolerance != nullptr ? tolerance->asDouble() : 1e-6;
            if (!actual.isNumber() || std::abs(actual.asDouble() - approx->asDouble()) > tol) {
                failure = where + ": not within the tolerance";
                return false;
            }
        }
        if (const JsonValue* capture = matcher.find("$capture")) {
            if (bindings.count(capture->text()) != 0) {
                failure = where + ": the vector binds a name twice";
                return false;
            }
            bindings[capture->text()] = actual;
        }
        return true;
    }
};

} // namespace ControlTest
} // namespace OpenTune
