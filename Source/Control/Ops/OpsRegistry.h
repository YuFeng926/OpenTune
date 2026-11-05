// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Control/ControlDiscovery.h"
#include "Control/ControlJson.h"
#include "Control/ControlProtocol.h"
#include "Utils/Error.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenTune {

class ControlHistory;
class ControlJobs;
class ControlUiBridge;
class OpenTuneAudioProcessor;

/**
 * Counts accesses to OpenTune state made through ControlOpContext, by thread. The control API reads
 * and changes project content only on the message thread; an access from any other thread is refused
 * (always, also in release builds) and counted here, so that tests can assert that it never happens.
 * Thread-safe.
 */
struct ControlThreadProbe
{
    std::atomic<int> messageThreadAccesses{0};
    std::atomic<int> otherThreadAccesses{0};
};

/** Thrown by ControlOpContext when OpenTune state is reached from a thread other than the message
    thread. ControlDispatcher answers the request with INTERNAL; nothing was read or changed. */
class ControlThreadViolation : public std::runtime_error
{
public:
    ControlThreadViolation() : std::runtime_error("OpenTune state accessed off the message thread") {}
};

/**
 * What session.info, the handles and app.getLog need to know about the running instance. ControlDispatcher
 * keeps it as an immutable snapshot: a change makes a new copy, so a method can read the copy it got from
 * any thread without a lock.
 */
struct ControlInstanceFacts
{
    /** The instance id of the listener (32 lowercase hexadecimal digits). */
    std::string instanceId;
    ControlLimits limits;
    ControlBuildIdentity build;
    /** The capabilities the instance reports, in the order session.hello lists them. */
    std::vector<std::string> capabilities;
    /** The session token and the server proof: texts that leave the process (app.getLog) are redacted
        with them. Never reported, logged or stored elsewhere. */
    std::vector<std::string> redactionSecrets;
    /** The spellings of the user's home directory (controlHomeDirectorySpellings()), which app.getLog
        replaces with "~"; taken once on the message thread when the dispatcher is created. */
    std::vector<std::string> homeDirectorySpellings;
};

/**
 * What an operation needs to make a ControlOpContext again on a later message-thread step, for the steps
 * of a job it started: the processor, the accessor of the editor's bridge (the bridge can come and go
 * between steps), the facts of the instance, the thread probe and the jobs. Valid while the job's work
 * exists: ControlJobs::stop() destroys every work before the dispatcher and the processor go away.
 */
struct ControlOpEnvironment
{
    OpenTuneAudioProcessor* processor = nullptr;
    std::function<ControlUiBridge*()> bridge;
    ControlInstanceFacts facts;
    ControlThreadProbe* probe = nullptr;
    ControlJobs* jobs = nullptr;
    std::shared_ptr<ControlHistory> history;
};

/**
 * What an operation may use while it runs on the message thread: the processor, the editor's bridge,
 * the facts of the instance and the jobs registry. Every access to the processor or the bridge checks
 * the thread, so an operation can never read or change content anywhere but on the message thread.
 *
 * Threading: created by ControlDispatcher (or by the step of a job) on the message thread; not shared.
 */
class ControlOpContext
{
public:
    /** jobs and bridgeAccessor may be empty: such a context cannot start jobs. history may be null: such a
        context cannot run the methods that read or record undo entries. */
    ControlOpContext(OpenTuneAudioProcessor& processor, ControlUiBridge* bridge, const ControlInstanceFacts& facts,
                     ControlThreadProbe& probe, ControlJobs* jobs = nullptr,
                     std::function<ControlUiBridge*()> bridgeAccessor = {},
                     std::shared_ptr<ControlHistory> history = nullptr);
    /** A context for one step of a job, from the environment that the operation which started it kept;
        the environment must outlive the context. */
    explicit ControlOpContext(const ControlOpEnvironment& environment);

    /** The processor. Throws ControlThreadViolation off the message thread. */
    OpenTuneAudioProcessor& processor();
    /** The editor's bridge, or nullptr without an editor. Throws ControlThreadViolation off the message
        thread. */
    ControlUiBridge* bridge();

    const ControlInstanceFacts& facts() const noexcept { return facts_; }
    /** The first 8 characters of the instance id, the instance part of every handle. */
    const std::string& instanceShort() const noexcept { return instanceShort_; }
    /** The jobs registry, or nullptr. Throws ControlThreadViolation off the message thread. */
    ControlJobs* jobs();
    /** What a job started by this operation needs for its later steps. Throws ControlThreadViolation off
        the message thread. */
    ControlOpEnvironment environment();
    /** What the instance knows about OpenTune's undo history (Edit/ControlOriginAction.h), or nullptr.
        Throws ControlThreadViolation off the message thread. */
    std::shared_ptr<ControlHistory> history();

    /**
     * For a method that ends the instance (app.quit), called once, when the method is about to return its result:
     * the dispatcher answers every other request with SHUTTING_DOWN from the moment the method returns, so that
     * nothing can change what the caller has just checked, and, once it has written the result to the connection,
     * posts action to the message thread, where it runs at most once (PROTOCOL.md section 14.1: the server answers
     * first and quits afterwards). Without effect when the method returns an error, and in the context of a job's
     * step. Throws ControlThreadViolation off the message thread.
     */
    void endInstanceAfterReply(std::function<void()> action);

    /** For the dispatcher: the action given to endInstanceAfterReply(), moved out; empty when none. Leaves the context
        without an action: the moved-from state of a std::function is not specified, so it is reset explicitly. */
    std::function<void()> takeEndInstanceAction() { return std::exchange(endInstanceAction_, nullptr); }

private:
    void requireMessageThread();

    OpenTuneAudioProcessor& processor_;
    ControlUiBridge* bridge_;
    const ControlInstanceFacts& facts_;
    ControlThreadProbe& probe_;
    ControlJobs* jobs_;
    std::function<ControlUiBridge*()> bridgeAccessor_;
    std::shared_ptr<ControlHistory> history_;
    std::string instanceShort_;
    std::function<void()> endInstanceAction_;
};

/**
 * The error an operation returns, as an OpenTune Error: the protocol's error kind and reason travel
 * in Error::context as "otcp:<KIND>:<reason>" (reason may be empty) and the diagnostic text in
 * Error::message. Only ControlDispatcher maps errors to the protocol (controlMapError). detail must be
 * printable ASCII and must never contain secrets or text taken from files or projects.
 *
 * The error states that nothing changed (`executed` false, PROTOCOL.md section 17.4 rule 1): a write
 * returns it only from its checks, before its first change. An error after the first change is made with
 * controlOpErrorAfterChange.
 */
Error controlOpError(std::string_view kind, std::string_view reason, std::string detail);

/**
 * As controlOpError, for an error of a write after it began to change state and did not take the change
 * back completely: `executed` true (section 17.4 rule 3), so that the client reads the state again
 * before it decides what to do. The context is "otcp!<KIND>:<reason>".
 */
Error controlOpErrorAfterChange(std::string_view kind, std::string_view reason, std::string detail);

/** Error kind, reason and detail that ControlDispatcher sends for an Error. */
struct ControlErrorReply
{
    std::string kind;
    std::string reason;
    std::string detail;
    /** `executed` as the operation stated it (controlOpError: false, controlOpErrorAfterChange: true);
        empty for other OpenTune errors, for which the dispatcher assumes that a write may have changed
        state. */
    std::optional<bool> executed;
};

/**
 * Maps an Error to the protocol: errors made by controlOpError and controlOpErrorAfterChange keep their
 * kind, reason, detail and executed; other OpenTune errors are mapped by their ErrorCode
 * (InvalidParameter: INVALID_ARGUMENT, NotInitialized: NOT_READY, model and session failures:
 * MODEL_UNAVAILABLE, OperationCancelled: CANCELLED, the rest: INTERNAL), with the fixed text of the code
 * as detail, never the error's own message.
 */
ControlErrorReply controlMapError(const Error& error);

/**
 * One method of the protocol as the dispatcher runs it.
 *
 * parse runs on the network thread: it checks params against the method's params schema (shape and
 * ranges only, never OpenTune state) and keeps what it parsed. On failure it sets why (printable
 * ASCII, no request content) and the request is answered with INVALID_ARGUMENT(invalid_params).
 *
 * run runs on the message thread with what parse kept, and returns the result object or an Error.
 *
 * A method whose schema says x-otcp.mainThread false has one of two functions instead of run, and both
 * run on the connection thread, never wait for the message thread and so never answer TIMEOUT:
 * - runOffMainThread (app.getLog) gets only the instance facts (an immutable snapshot) and what parse
 *   kept, never the processor or the editor bridge, so it cannot read or change project content;
 * - runWithoutMessageThread (job.get, job.cancel) gets the jobs registry and may answer later.
 */
struct ControlMethod
{
    std::string name;
    /** Runs on the message thread (x-otcp.mainThread of the schema): run is set when true,
        runOffMainThread or runWithoutMessageThread when false. */
    bool mainThread = true;
    /** Changes state (x-otcp.write is not "no"): decides `executed` of an error that does not state it
        (an exception, or an OpenTune error that is not a controlOpError). */
    bool write = false;
    std::function<bool(const JsonValue& params, std::shared_ptr<const void>& parsed, std::string& why)> parse;
    std::function<Result<JsonValue>(ControlOpContext& context, const void* parsed)> run;
    /** Any thread; must not block for long, read files only within a fixed bound, and reach no OpenTune
        state other than what is thread-safe by its own contract (for example AppLogger). */
    std::function<Result<JsonValue>(const ControlInstanceFacts& facts, const void* parsed)> runOffMainThread;
    /** Answers the request exactly once, possibly later and from another thread. */
    using Reply = std::function<void(Result<JsonValue>)>;
    /** Instead of run, for a method with mainThread false (job.get, job.cancel): runs on the connection
        thread with the jobs registry and reaches no OpenTune state. It copies what it needs of parsed
        before it returns, and never waits for the message thread or blocks the connection thread. */
    std::function<void(ControlJobs& jobs, const void* parsed, Reply reply)> runWithoutMessageThread;
};

/**
 * A read method from typed functions: parse fills a Params value on the network thread, run gets it
 * on the message thread.
 */
template <typename Params>
ControlMethod controlReadMethod(std::string name, bool (*parse)(const JsonValue&, Params&, std::string&),
                                Result<JsonValue> (*run)(ControlOpContext&, const Params&))
{
    ControlMethod method;
    method.name = std::move(name);
    method.parse = [parse](const JsonValue& params, std::shared_ptr<const void>& parsed, std::string& why) {
        auto value = std::make_shared<Params>();
        if (!parse(params, *value, why))
            return false;
        parsed = std::move(value);
        return true;
    };
    method.run = [run](ControlOpContext& context, const void* parsed) {
        return run(context, *static_cast<const Params*>(parsed));
    };
    return method;
}

/**
 * A read method whose schema says x-otcp.mainThread false: parse fills a Params value and run gets it,
 * with the instance facts, on the same connection thread. Only for methods that read no project state.
 */
template <typename Params>
ControlMethod controlOffMainThreadReadMethod(std::string name, bool (*parse)(const JsonValue&, Params&, std::string&),
                                             Result<JsonValue> (*run)(const ControlInstanceFacts&, const Params&))
{
    ControlMethod method = controlReadMethod<Params>(std::move(name), parse, nullptr);
    method.mainThread = false;
    method.run = nullptr;
    method.runOffMainThread = [run](const ControlInstanceFacts& facts, const void* parsed) {
        return run(facts, *static_cast<const Params*>(parsed));
    };
    return method;
}

/** A write method (x-otcp.write "yes") from typed functions, as controlReadMethod. */
template <typename Params>
ControlMethod controlWriteMethod(std::string name, bool (*parse)(const JsonValue&, Params&, std::string&),
                                 Result<JsonValue> (*run)(ControlOpContext&, const Params&))
{
    auto method = controlReadMethod<Params>(std::move(name), parse, run);
    method.write = true;
    return method;
}

/** A method that never waits for the message thread (x-otcp.mainThread false) from typed functions:
    parse and run on the connection thread, run with the jobs registry. */
template <typename Params>
ControlMethod controlJobMethod(std::string name, bool (*parse)(const JsonValue&, Params&, std::string&),
                               void (*run)(ControlJobs&, const Params&, ControlMethod::Reply))
{
    ControlMethod method;
    method.name = std::move(name);
    method.mainThread = false;
    method.parse = [parse](const JsonValue& params, std::shared_ptr<const void>& parsed, std::string& why) {
        auto value = std::make_shared<Params>();
        if (!parse(params, *value, why))
            return false;
        parsed = std::move(value);
        return true;
    };
    method.runWithoutMessageThread = [run](ControlJobs& jobs, const void* parsed, ControlMethod::Reply reply) {
        run(jobs, *static_cast<const Params*>(parsed), std::move(reply));
    };
    return method;
}

/** The methods the dispatcher implements. Filled once before the listener starts; read-only after. */
class ControlMethodTable
{
public:
    /** Adds a method; its name must be a method of protocol version 1 that is not yet in the table. */
    void add(ControlMethod method);
    /** The method, or nullptr when this build does not implement it. Thread-safe once filled. */
    const ControlMethod* find(std::string_view name) const;
    /** session.v1 and the capabilities of the methods added, in the order they were first added, without the
        incomplete ones (controlCapabilityIsIncomplete); ui.onboardingClose, a capability without methods of its own,
        right after ui.bridge. */
    std::vector<std::string> capabilities() const;

private:
    std::vector<ControlMethod> methods_;
};

/**
 * True for a capability of which this build registers some methods but not all. The instance does not
 * report it, because an instance implements every method of a capability it reports (PROTOCOL.md section 3.3
 * rule 5 case 10); the listener therefore answers its methods with UNSUPPORTED(capability_missing). No capability
 * is incomplete in this build. A capability whose methods are registered in several steps is listed here until its
 * last method is registered, so that the instance never reports a capability it implements only in part.
 */
bool controlCapabilityIsIncomplete(std::string_view capability);

/** Test seam: dispatchers created from now on report the incomplete capabilities as well, so that the
    headless tests can call their methods. Never used by OpenTune itself. Any thread. */
void controlSetReportIncompleteCapabilitiesForTesting(bool report);

/** Test seam: dispatchers created from now on leave these capabilities out of what they report, as the
    precondition withoutCapabilities of the specification's vectors asks (an instance that does not report
    ui.onboardingClose). Empty by default; never used by OpenTune itself. Any thread. */
void controlSetWithheldCapabilitiesForTesting(std::vector<std::string> capabilities);

/**
 * Registers every method this build implements, one group per line (Ops/OpsRegistry.cpp).
 */
void registerControlMethods(ControlMethodTable& table);

} // namespace OpenTune
