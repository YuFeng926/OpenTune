// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Processor-level tests of the control service without a window or an audio device: the real
// OpenTuneAudioProcessor of the Standalone SharedCode with its control-service hooks, the
// message-thread liveness token and the OPENTUNE_DATA_DIR redirection; the scenarios of the dispatcher
// and the read methods are in ControlDispatcherTests.cpp.
//
// Each scenario runs in a process of its own (one ctest test per scenario, registered in
// cmake/OpenTuneControl.cmake). The first argument names the scenario; the control API reads the other
// arguments from the real process command line, as in the application. The lifecycle scenarios run
// under the upstream lifecycle watchdog (Tests/LifecycleWatchdog.h), which bounds the whole process.
//
// Every scenario requires OPENTUNE_DATA_DIR (set per test by CMake): without it the
// processor would write the user's real preferences and logs.

#if defined(_MSC_VER)
// As Tests/LifecycleTestTrace.h intends; it is included last here, after the CRT headers.
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Control/ControlDataDirectory.h"
#include "Control/ControlMessageThread.h"
#include "Control/ControlService.h"
#include "Control/ControlUiBridge.h"
#include "Editor/EditorFactory.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// The watchdog includes <windows.h>; leave out the service-control API, whose ControlService()
// function would make the name ControlService ambiguous under `using namespace OpenTune`.
#ifndef NOSERVICE
#define NOSERVICE
#endif
#endif
#include "LifecycleWatchdog.h"

namespace OpenTune {

// The editor factory belongs to the application wrapper, which these tests do not link: the processor
// runs without an editor.
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    return nullptr;
}

} // namespace OpenTune

using namespace OpenTune;
using namespace OpenTune::ControlTest;

namespace {

/** Timeout of the waiting call(): far longer than the test, so that only stop() can end it. */
constexpr int kLongCallTimeoutMs = 30000;
/** Functions queued for the message thread before the processor is destroyed. */
constexpr int kQueuedFunctions = 3;
/** Bound of a connection attempt to a port that should be closed. A listening loopback port accepts at
    once; a closed one is refused, which Windows reports only after its SYN retries (about 2 s). */
constexpr int kClosedPortProbeMs = 500;
/** Must equal the value cmake/OpenTuneControl.cmake passes to the StartAndHello scenario. */
const char* const kLaunchNonce = "headless-test-nonce-0123456789";

std::string pidText()
{
    return std::to_string(controlCurrentProcessId());
}

juce::File juceFileOf(const std::filesystem::path& path)
{
    return juce::File(juce::String::fromUTF8(path.u8string().c_str()));
}

/** Where the discovery file goes with OPENTUNE_DATA_DIR (PROTOCOL.md section 5.1 rule 2). */
std::filesystem::path isolatedDiscoveryFile()
{
    return isolatedDataDirectory() / "control" / "instances" / (pidText() + ".json");
}

/** Whether something still accepts connections on the loopback port, within kClosedPortProbeMs. */
bool portAccepts(int port)
{
    return ControlSocket::connectToLoopback(port, kClosedPortProbeMs).isValid();
}

std::string logText()
{
    const auto file = AppLogger::getCurrentLogFile();
    return file == juce::File{} ? std::string() : file.loadFileAsString().toStdString();
}

std::size_t occurrences(const std::string& text, const std::string& part)
{
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++count;
    return count;
}

bool isLowerHex(const std::string& text, std::size_t length)
{
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool matchesOptional(const JsonValue& value, const std::optional<std::string>& expected)
{
    return expected ? (value.isString() && value.text() == *expected) : value.isNull();
}

/** The discovery file against schemas/discovery-file.schema.json: closed, so exactly its members in
    schema order, with the types and forms the schema gives them. */
void checkDiscoveryFile(const JsonValue& json, const ControlService& service,
                        const std::optional<std::string>& launchNonce)
{
    const char* const members[] = {"protocolMajors", "pid",         "processStartTimeMs", "instanceId", "port",
                                   "token",          "serverProof", "launchNonce",        "appVersion", "build",
                                   "buildTag",       "sourceUrl",   "startedAtMs",        "flavor"};
    if (!CONTROL_CHECK(json.isObject() && json.size() == std::size(members)))
        return;
    for (std::size_t i = 0; i < std::size(members); ++i)
        CONTROL_CHECK(json.keyAt(i) == members[i]);
    CONTROL_CHECK(toJsonString(memberOf(json, "protocolMajors")) == "[1]");
    CONTROL_CHECK(memberOf(json, "pid").isNumber()
                  && memberOf(json, "pid").asDouble() == static_cast<double>(controlCurrentProcessId()));
    const auto& processStart = memberOf(json, "processStartTimeMs");
    CONTROL_CHECK(processStart.isNumber() && processStart.asDouble() > 0);
    CONTROL_CHECK(isLowerHex(memberOf(json, "instanceId").text(), 32));
    const auto& port = memberOf(json, "port");
    CONTROL_CHECK(port.isNumber() && port.asDouble() == service.port() && service.port() >= 1
                  && service.port() <= 65535);
    const auto& token = memberOf(json, "token").text();
    CONTROL_CHECK(token.size() >= 32 && token.size() <= 128 && token.size() % 2 == 0
                  && isLowerHex(token, token.size()));
    CONTROL_CHECK(isLowerHex(memberOf(json, "serverProof").text(), 32));
    CONTROL_CHECK(matchesOptional(memberOf(json, "launchNonce"), launchNonce));

    const auto identity = ControlService::buildIdentity();
    CONTROL_CHECK(controlValidateBuildIdentity(identity).empty());
    CONTROL_CHECK(memberOf(json, "appVersion").text() == identity.appVersion);
    const auto& build = memberOf(json, "build");
    CONTROL_CHECK(build.isObject() && build.size() == 2 && build.keyAt(0) == "commit" && build.keyAt(1) == "tag");
    CONTROL_CHECK(isLowerHex(memberOf(build, "commit").text(), 40)
                  && memberOf(build, "commit").text() == identity.commit);
    CONTROL_CHECK(matchesOptional(memberOf(build, "tag"), identity.commitTag));
    CONTROL_CHECK(matchesOptional(memberOf(json, "buildTag"), identity.buildTag));
    CONTROL_CHECK(memberOf(json, "sourceUrl").text() == identity.sourceUrl
                  && identity.sourceUrl.compare(0, 8, "https://") == 0);
    const auto& startedAt = memberOf(json, "startedAtMs");
    CONTROL_CHECK(startedAt.isNumber() && startedAt.asDouble() >= processStart.asDouble()
                  && startedAt.asDouble() <= static_cast<double>(controlNowMs()));
    CONTROL_CHECK(memberOf(json, "flavor").text() == "standalone");
}

/** The log file is inside OPENTUNE_DATA_DIR (the logger hook works). */
void checkLogInsideDataDirectory()
{
    const auto file = AppLogger::getCurrentLogFile();
    CONTROL_CHECK(file != juce::File{} && file.isAChildOf(juceFileOf(isolatedDataDirectory() / "Logs")));
}

// --------------------------------------------------------------------------------------------------
// Scenarios
// --------------------------------------------------------------------------------------------------

/** Construction, asynchronous start, discovery and ready files, session.hello, destruction. */
void scenarioStartAndHello()
{
    // A class-layout mismatch between this test and the SharedCode would break everything below.
    CONTROL_CHECK(ControlService::processorClassSizeForTesting() == sizeof(OpenTuneAudioProcessor));
    const auto readyFile = isolatedDataDirectory() / "ready.json";
    std::error_code ignored;
    std::filesystem::remove(readyFile, ignored);

    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    CONTROL_CHECK(service.options().enabled && service.options().error.empty());
    CONTROL_CHECK(service.options().launchNonce == std::optional<std::string>(kLaunchNonce));
    // The start is posted by the constructor and runs from the message loop, after it returned.
    CONTROL_CHECK(!service.isRunning() && service.port() == 0);
    const auto started = Clock::now();
    if (!CONTROL_CHECK(pumpMessagesUntil([&service] { return service.isRunning(); }, kStartTimeoutMs)))
        return;
    note("listening " + std::to_string(millisecondsSince(started)) + " ms after construction");

    const auto discoveryPath = std::filesystem::u8path(service.discoveryFilePath());
    CONTROL_CHECK(discoveryPath == isolatedDiscoveryFile());
    const auto discovery = readDiscoveryFile(discoveryPath);
    if (!discovery)
        return;
    checkDiscoveryFile(*discovery, service, std::string(kLaunchNonce));
    const auto instanceId = memberOf(*discovery, "instanceId").text();
    const auto token = memberOf(*discovery, "token").text();
    const auto serverProof = memberOf(*discovery, "serverProof").text();
    CONTROL_CHECK(readFile(readyFile) == "{\"pid\":" + pidText() + ",\"instanceId\":\"" + instanceId + "\"}");

    const int port = service.port();
    TestClient client;
    const auto hello = authenticate(client, port, token);
    if (CONTROL_CHECK(hello.has_value())) {
        CONTROL_CHECK(memberOf(*hello, "protocol").text() == "1.0");
        CONTROL_CHECK(memberOf(*hello, "instanceId").text() == instanceId);
        CONTROL_CHECK(memberOf(*hello, "serverProof").text() == serverProof);
        CONTROL_CHECK(memberOf(*hello, "pid").asDouble() == static_cast<double>(controlCurrentProcessId()));
        CONTROL_CHECK(memberOf(*hello, "appVersion").text() == ControlService::buildIdentity().appVersion);
        // session.v1 and the capabilities of the methods the dispatcher implements.
        CONTROL_CHECK(
            toJsonString(memberOf(*hello, "capabilities"))
            == R"(["session.v1","read.v1","jobs.v1","import.v1","export.v1","ui.bridge","ui.onboardingClose","ui.workspace","edit.v1","history.v1","timing.v1","transport.v1","arrangement.v1","tracks.v1","project.io","project.copy","prefs.write","audio.device","app.quit"])");
    }
    // The runtime was initialised before the listener started: the logger writes under the data dir.
    checkLogInsideDataDirectory();

    processor.reset();
    CONTROL_CHECK(!std::filesystem::exists(discoveryPath));
    CONTROL_CHECK(!std::filesystem::exists(readyFile));
    std::size_t bytes = 0;
    CONTROL_CHECK(client.waitClosed(kCloseSeenMs, bytes));
    CONTROL_CHECK(!portAccepts(port));

    const auto log = logText();
    CONTROL_CHECK(log.find("control API: listening on 127.0.0.1:" + std::to_string(port)) != std::string::npos);
    CONTROL_CHECK(occurrences(log, "control API: stopped") == 1);
    CONTROL_CHECK(log.find(token) == std::string::npos && log.find(serverProof) == std::string::npos);
}

/** Not enabled at run time (OPENTUNE_CONTROL_API is not "1", no --control-api): nothing happens, and the
    other control-api options are ignored, even an invalid one. */
void scenarioDisabled()
{
    setEnvironmentVariable("OPENTUNE_CONTROL_API", std::string("0"));
    CONTROL_CHECK(controlSystemEnvironment("OPENTUNE_CONTROL_API") == std::optional<std::string>("0"));
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    CONTROL_CHECK(!service.options().enabled && service.options().error.empty());
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(!service.isRunning() && service.port() == 0 && service.discoveryFilePath().empty());
    processor.reset();
    pumpMessagesFor(kQuietPumpMs);
    const auto data = isolatedDataDirectory();
    CONTROL_CHECK(!std::filesystem::exists(data / "control"));
    // Nothing initialised the runtime or the logger, as with the upstream constructor and destructor.
    CONTROL_CHECK(AppLogger::getCurrentLogFile() == juce::File{});
    CONTROL_CHECK(!std::filesystem::exists(data / "Logs"));
    setEnvironmentVariable("OPENTUNE_CONTROL_API", std::nullopt);
}

/** ensureRuntimeInitialized() fails (injected): the failure is logged, nothing listens. */
void scenarioRuntimeInitFailure()
{
    int calls = 0;
    ControlService::setRuntimeInitialiserForTesting([&calls](OpenTuneAudioProcessor&) {
        ++calls;
        return false;
    });
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    CONTROL_CHECK(pumpMessagesUntil([&calls] { return calls > 0; }, kStartTimeoutMs));
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(calls == 1);
    CONTROL_CHECK(!service.isRunning() && service.port() == 0 && service.discoveryFilePath().empty());
    CONTROL_CHECK(!std::filesystem::exists(isolatedDiscoveryFile()));
    CONTROL_CHECK(!service.start()); // start() runs at most once
    CONTROL_CHECK(calls == 1);
    checkLogInsideDataDirectory();
    CONTROL_CHECK(logText().find("control API: not started: the OpenTune runtime could not be initialised")
                  != std::string::npos);
    processor.reset();
    ControlService::setRuntimeInitialiserForTesting({});
}

/** An invalid option with the API enabled: logged by name and rule (never the value), nothing listens,
    and the runtime is not initialised for it. */
void scenarioInvalidOption()
{
    int calls = 0;
    ControlService::setRuntimeInitialiserForTesting([&calls](OpenTuneAudioProcessor& processor) {
        ++calls;
        return processor.ensureRuntimeInitialized();
    });
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    CONTROL_CHECK(service.options().enabled && !service.options().error.empty());
    CONTROL_CHECK(pumpMessagesUntil([] { return !logText().empty(); }, kStartTimeoutMs));
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(calls == 0 && !service.isRunning() && service.port() == 0);
    CONTROL_CHECK(!std::filesystem::exists(isolatedDataDirectory() / "control"));
    const auto log = logText();
    CONTROL_CHECK(log.find("control API: not started: --control-api-dir is not an accepted absolute path")
                  != std::string::npos);
    CONTROL_CHECK(log.find("not/absolute") == std::string::npos && log.find("not\\absolute") == std::string::npos);
    processor.reset();
    ControlService::setRuntimeInitialiserForTesting({});
}

/** OPENTUNE_DATA_DIR redirects preferences, logs and discovery; without it (or with a value that is not
    an accepted absolute path) the hooks leave OpenTune's folders as upstream has them. */
void scenarioDataDirectory()
{
    const auto data = isolatedDataDirectory();
    const auto original = controlSystemEnvironment("OPENTUNE_DATA_DIR");
    CONTROL_CHECK(controlDataDirectory() == juceFileOf(data));
    {
        AppPreferences preferences; // the default storage options, as the processor uses them
        preferences.save();
    }
    CONTROL_CHECK(std::filesystem::exists(data / "app-preferences.settings"));
    AppLogger::log("ControlProcessorTests: data directory scenario");
    checkLogInsideDataDirectory();
    std::string error;
    const auto enabled = parseControlApiOptions({"--control-api"}, controlSystemEnvironment);
    CONTROL_CHECK(controlResolveDiscoveryDirectory(enabled, controlSystemEnvironment, controlCurrentPlatform(), error)
                  == (data / "control" / "instances").u8string());

    // No redirection. Nothing below may write: AppPreferences and the processor stay untouched here.
    const std::optional<std::string> notRedirected[] = {std::nullopt, std::string("relative/dir"),
#if defined(_WIN32)
                                                        std::string("\\\\server\\share\\dir"), std::string("C:relative")
#else
                                                        std::string("relative")
#endif
    };
    for (const auto& value : notRedirected) {
        setEnvironmentVariable("OPENTUNE_DATA_DIR", value);
        CONTROL_CHECK(controlDataDirectory() == juce::File{});
    }
    setEnvironmentVariable("OPENTUNE_DATA_DIR", std::nullopt);
    CONTROL_CHECK(!controlSystemEnvironment("OPENTUNE_DATA_DIR").has_value());
    const auto unset = parseControlApiOptions({"--control-api"}, controlSystemEnvironment);
    CONTROL_CHECK(unset.canStart());
    CONTROL_CHECK(controlResolveDiscoveryDirectory(unset, controlSystemEnvironment, controlCurrentPlatform(), error)
                  == realDiscoveryFileOfThisProcess().parent_path().u8string());
    setEnvironmentVariable("OPENTUNE_DATA_DIR", original);
    CONTROL_CHECK(controlDataDirectory() == juceFileOf(data));
}

/** The bridge interface: defaults report "not supported"; the processor's service keeps the pointer. */
void scenarioUiBridge()
{
    ControlUiBridge defaults;
    CONTROL_CHECK(!defaults.projectInfo() && !defaults.queryBusy() && !defaults.viewState());
    CONTROL_CHECK(!defaults.visibleTrackCount());
    CONTROL_CHECK(defaults.projectSession() == nullptr && defaults.projectWorkerPool() == nullptr
                  && defaults.audioDevice() == nullptr);
    defaults.markDirty();
    defaults.onStructureChanged();
    defaults.onUndoRedoApplied();
    defaults.onProjectReloaded();

    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    CONTROL_CHECK(service.uiBridge() == nullptr);
    TestUiBridge bridge;
    bridge.visibleTracks = 3;
    bridge.busy.modalOpen = true;
    service.setUiBridge(&bridge);
    ControlUiBridge* registered = service.uiBridge();
    if (CONTROL_CHECK(registered == &bridge)) {
        CONTROL_CHECK(registered->visibleTrackCount() == std::optional<int>(3));
        const auto busy = registered->queryBusy();
        CONTROL_CHECK(busy && busy->modalOpen && !busy->pointerDown && !busy->operationRunning);
        registered->markDirty();
        registered->onStructureChanged();
        CONTROL_CHECK(bridge.dirtyMarks == 1 && bridge.structureChanges == 1 && registered->projectInfo()->dirty);
    }
    service.setUiBridge(nullptr);
    CONTROL_CHECK(service.uiBridge() == nullptr);
    processor.reset();
}

/** The processor is destroyed before the message loop reached the start posted by its constructor:
    the queued start finds the liveness token dead and does nothing. */
void scenarioDestroyBeforeStart()
{
    int initialisations = 0;
    ControlService::setRuntimeInitialiserForTesting([&initialisations](OpenTuneAudioProcessor& processor) {
        ++initialisations;
        return processor.ensureRuntimeInitialized();
    });
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    CONTROL_CHECK(processor->getControlService().options().enabled);
    const auto messageThread = processor->getControlService().messageThread();
    CONTROL_CHECK(messageThread->isAlive() && !messageThread->isStopped());
    processor.reset();
    CONTROL_CHECK(messageThread->isStopped() && !messageThread->isAlive() && !messageThread->post([] {}));
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(initialisations == 0);
    CONTROL_CHECK(!std::filesystem::exists(isolatedDiscoveryFile()));

    // The same queue does deliver the start of a processor that lives.
    auto second = startedProcessor();
    CONTROL_CHECK(initialisations == 1 && second->getControlService().isRunning());
    second.reset();
    CONTROL_CHECK(!std::filesystem::exists(isolatedDiscoveryFile()));
    ControlService::setRuntimeInitialiserForTesting({});
}

/** stop() may be called any number of times; after it nothing starts, posts or waits again. */
void scenarioRepeatedStop()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const int port = service.port();
    const auto discoveryPath = std::filesystem::u8path(service.discoveryFilePath());
    const auto discovery = readDiscoveryFile(discoveryPath);
    if (!discovery)
        return;
    TestClient client;
    CONTROL_CHECK(authenticate(client, port, memberOf(*discovery, "token").text()).has_value());

    service.stop();
    CONTROL_CHECK(!service.isRunning() && service.isStopping() && service.port() == 0);
    CONTROL_CHECK(!std::filesystem::exists(discoveryPath));
    std::size_t bytes = 0;
    CONTROL_CHECK(client.waitClosed(kCloseSeenMs, bytes));
    CONTROL_CHECK(!portAccepts(port));

    service.stop();
    CONTROL_CHECK(!service.start());
    const auto messageThread = service.messageThread();
    CONTROL_CHECK(!messageThread->isAlive() && !messageThread->post([] {}));
    std::atomic<int> executed{0};
    auto result = ControlMessageThread::CallResult::Done;
    std::thread caller([&] { result = messageThread->call([&executed] { ++executed; }, kLongCallTimeoutMs); });
    caller.join();
    CONTROL_CHECK(result == ControlMessageThread::CallResult::Stopped && executed == 0);

    processor.reset(); // the destructor's stop() and the service's own are no-ops now
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(executed == 0);
    CONTROL_CHECK(occurrences(logText(), "control API: stopped") == 1);
}

/** Destroying the processor while an authenticated session, a silent connection and a half-sent line
    are open, a thread waits for the message thread, and functions are queued for it: done within
    kShutdownBoundMs, the waiter returns Stopped, and the queued functions never run. */
void scenarioShutdown()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const int port = service.port();
    const auto discoveryPath = std::filesystem::u8path(service.discoveryFilePath());
    const auto discovery = readDiscoveryFile(discoveryPath);
    if (!discovery)
        return;

    TestClient session;
    CONTROL_CHECK(authenticate(session, port, memberOf(*discovery, "token").text()).has_value());
    TestClient silent;
    CONTROL_CHECK(silent.connect(port));
    TestClient partial;
    CONTROL_CHECK(partial.connect(port) && partial.sendRaw(R"({"jsonrpc":"2.0","id":1,)"));

    const auto messageThread = service.messageThread();
    std::atomic<int> executed{0};
    std::atomic<int> waiterResult{-1};
    std::thread waiter([messageThread, &executed, &waiterResult] {
        const auto result = messageThread->call([&executed] { ++executed; }, kLongCallTimeoutMs);
        waiterResult = static_cast<int>(result);
    });
    CONTROL_CHECK(waitUntil([&messageThread] { return messageThread->waitingCalls() == 1; }, kWaiterSettleMs));
    for (int i = 0; i < kQueuedFunctions; ++i)
        CONTROL_CHECK(messageThread->post([&executed] { ++executed; }));

    const auto started = Clock::now();
    processor.reset();
    const auto elapsed = millisecondsSince(started);
    note("processor destroyed in " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed <= kShutdownBoundMs);

    CONTROL_CHECK(waitUntil([&waiterResult] { return waiterResult.load() >= 0; }, kCloseSeenMs));
    waiter.join();
    CONTROL_CHECK(waiterResult.load() == static_cast<int>(ControlMessageThread::CallResult::Stopped));
    CONTROL_CHECK(messageThread->waitingCalls() == 0);
    CONTROL_CHECK(!std::filesystem::exists(discoveryPath));
    std::size_t bytes = 0;
    CONTROL_CHECK(session.waitClosed(kCloseSeenMs, bytes));
    CONTROL_CHECK(silent.waitClosed(kCloseSeenMs, bytes) && bytes == 0);
    CONTROL_CHECK(partial.waitClosed(kCloseSeenMs, bytes) && bytes == 0);

    // The waiter's function and the posted ones reach the message loop now and find the token dead.
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(executed.load() == 0);
}

struct Scenario
{
    const char* name;
    void (*run)();
    /** Runs in a child process under the upstream lifecycle watchdog. */
    bool lifecycle;
};

const Scenario kScenarios[] = {
    {"StartAndHello", scenarioStartAndHello, false},
    {"Disabled", scenarioDisabled, false},
    {"RuntimeInitFailure", scenarioRuntimeInitFailure, false},
    {"InvalidOption", scenarioInvalidOption, false},
    {"DataDirectory", scenarioDataDirectory, false},
    {"UiBridge", scenarioUiBridge, false},
    {"DestroyBeforeStart", scenarioDestroyBeforeStart, true},
    {"RepeatedStop", scenarioRepeatedStop, true},
    {"Shutdown", scenarioShutdown, true},
    {"Reads", scenarioReads, false},
    {"ThreadProbe", scenarioThreadProbe, false},
    {"DispatchShutdown", scenarioDispatchShutdown, true},
    // Waits longer than dispatchTimeoutMs, beyond the watchdog's bound.
    {"DispatchTimeout", scenarioDispatchTimeout, false},
    {"Details", scenarioDetails, false},
    {"Completeness", scenarioCompleteness, false},
    {"Vectors", scenarioVectors, false},
    {"OffMainThread", scenarioOffMainThread, false},
    {"Jobs", scenarioJobs, false},
    {"JobShutdown", scenarioJobShutdown, true},
    {"WriteEnvelope", scenarioWriteEnvelope, false},
    {"Import", scenarioImport, false},
    {"ImportAfterOpen", scenarioImportAfterOpen, false},
    {"ImportCancel", scenarioImportCancel, false},
    {"ImportRevert", scenarioImportRevert, false},
    {"JobVectors", scenarioJobVectors, false},
    {"BusyMatrix", scenarioBusyMatrix, false},
    {"JobDisconnect", scenarioJobDisconnect, true},
    {"ImportFailures", scenarioImportFailures, false},
    {"Export", scenarioExport, false},
    {"ExportFailures", scenarioExportFailures, false},
    {"ExportDeferredRetry", scenarioExportDeferredRetry, false},
    {"ExportVectors", scenarioExportVectors, false},
    {"ExportTimeStretch", scenarioExportTimeStretch, false},
    {"ExportAfterEdit", scenarioExportAfterEdit, false},
    {"ExportCancelOverwrite", scenarioExportCancelOverwrite, false},
    {"ExportDestroy", scenarioExportDestroy, true},
    {"ExportUnicodePath", scenarioExportUnicodePath, false},
    {"ExportDryReasons", scenarioExportDryReasons, false},
    {"Undo", scenarioUndo, false},
    {"SetKey", scenarioSetKey, false},
    {"AutoTune", scenarioAutoTune, false},
    {"EditNotes", scenarioEditNotes, false},
    {"EditMultiNoteRender", scenarioEditMultiNoteRender, false},
    {"SetPitchShift", scenarioSetPitchShift, false},
    {"SnapNotes", scenarioSnapNotes, false},
    {"EditConflictRetry", scenarioEditConflictRetry, false},
    {"TokenAspects", scenarioTokenAspects, false},
    {"EditCapability", scenarioEditCapability, false},
    {"EditVectors", scenarioEditVectors, false},
    {"Timing", scenarioTiming, false},
    {"TimeGridSeed", scenarioTimeGridSeed, false},
    {"TimingTokenAspects", scenarioTimingTokenAspects, false},
    {"TimingVectors", scenarioTimingVectors, false},
    {"ProjectRoundTrip", scenarioProjectRoundTrip, false},
    {"ProjectOpenTrackCount", scenarioProjectOpenTrackCount, false},
    {"ProjectJobs", scenarioProjectJobs, false},
    {"ProjectWithoutEditor", scenarioProjectWithoutEditor, false},
    {"ProjectSaveCopy", scenarioProjectSaveCopy, false},
    {"ProjectSaveCopyChecks", scenarioProjectSaveCopyChecks, false},
    {"ProjectSaveCopyCapability", scenarioProjectSaveCopyCapability, false},
    {"ProjectReopenKeepsTuning", scenarioProjectReopenKeepsTuning, false},
    {"ProjectVectors", scenarioProjectVectors, false},
    {"Transport", scenarioTransport, false},
    {"Tracks", scenarioTracks, false},
    {"TrackBarrier", scenarioTrackBarrier, false},
    {"TrackVectors", scenarioTrackVectors, false},
    {"UiState", scenarioUiState, false},
    {"UiDismiss", scenarioUiDismiss, false},
    {"UiOnboardingWithheld", scenarioUiOnboardingWithheld, false},
    {"UiWorkspace", scenarioUiWorkspace, false},
    {"UiVectors", scenarioUiVectors, false},
    {"PrefsRoundTrip", scenarioPrefsRoundTrip, false},
    {"PrefsSchemeJob", scenarioPrefsSchemeJob, false},
    {"PrefsOptions", scenarioPrefsOptions, false},
    {"PrefsVocoderJob", scenarioPrefsVocoderJob, false},
    {"PrefsVocoderOverlap", scenarioPrefsVocoderOverlap, false},
    {"PrefsVectors", scenarioPrefsVectors, false},
    {"AudioDevice", scenarioAudioDevice, false},
    {"AudioVectors", scenarioAudioVectors, false},
    {"Arrangement", scenarioArrangement, false},
    {"ArrangementUndo", scenarioArrangementUndo, false},
    {"ArrangementCopy", scenarioArrangementCopy, false},
    {"ArrangementVectors", scenarioArrangementVectors, false},
    {"Selection", scenarioSelection, false},
    {"SelectionMatrix", scenarioSelectionMatrix, false},
    {"SelectionVectors", scenarioSelectionVectors, false},
    {"AppQuit", scenarioAppQuit, false},
    {"AppQuitWithoutWindow", scenarioAppQuitWithoutWindow, false},
    {"AppQuitResult", scenarioAppQuitResult, false},
    {"AppQuitSequence", scenarioAppQuitSequence, false},
    {"AppQuitStopping", scenarioAppQuitStopping, false},
    {"AppQuitVectors", scenarioAppQuitVectors, false},
    {"ClipIdSkip", scenarioClipIdSkip, false},
    {"ClipIdsAfterOpen", scenarioClipIdsAfterOpen, false},
    {"ClipIdsWithoutOpen", scenarioClipIdsWithoutOpen, false},
};

/** How much earlier than the ctest TIMEOUT of its test the watchdog of a scenario ends it. */
constexpr int kScenarioBoundMarginMs = 5000;
/** The bound of a scenario whose process was started without OPENTUNE_SCENARIO_TIMEOUT_SEC (not through ctest). */
constexpr int kDefaultScenarioBoundMs = 55000;

/**
 * The bound of the scenario of this process: kScenarioBoundMarginMs before the ctest TIMEOUT of its test, which
 * cmake/OpenTuneControl.cmake passes as OPENTUNE_SCENARIO_TIMEOUT_SEC (whole seconds), so that a scenario with a
 * longer TIMEOUT gets a longer bound with nothing else to change; kDefaultScenarioBoundMs without that variable.
 */
int scenarioBoundMs()
{
    const auto timeoutSeconds = scenarioTimeoutSeconds();
    if (!timeoutSeconds)
        return kDefaultScenarioBoundMs;
    const long long timeoutMs = *timeoutSeconds * 1000;
    return timeoutMs > 2LL * kScenarioBoundMarginMs ? static_cast<int>(timeoutMs - kScenarioBoundMarginMs)
                                                    : static_cast<int>(timeoutMs / 2);
}

/**
 * Ends the process with a failure that names the scenario when it has not finished within its bound, so that a
 * scenario blocked in a wait without a bound (in upstream code, for example) fails with a message rather than being
 * killed by ctest at its TIMEOUT without one. The flag is set and notified under the mutex the watchdog waits with.
 */
class ScenarioWatchdog
{
public:
    ScenarioWatchdog(const char* name, int boundMs)
        : thread_([this, name, boundMs] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::milliseconds(boundMs), [this] { return done_; }))
                return;
            std::cout << "[FAILED] " << name << ": not finished within " << boundMs
                      << " ms; a wait without a bound did not return" << std::endl;
            std::cerr << "    FAILED " << name << " did not finish within its bound" << std::endl;
            std::_Exit(1);
        })
    {}

    ~ScenarioWatchdog()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    ScenarioWatchdog(const ScenarioWatchdog&) = delete;
    ScenarioWatchdog& operator=(const ScenarioWatchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

int runScenario(const Scenario& scenario)
{
    const auto realDiscoveryFile = realDiscoveryFileOfThisProcess();
    std::error_code ignored;
    std::filesystem::create_directories(isolatedDataDirectory(), ignored);
    // Scenarios that need the API pass --control-api; a value inherited from the shell must not count.
    setEnvironmentVariable("OPENTUNE_CONTROL_API", std::nullopt);

    const auto started = Clock::now();
    std::cout << "[ RUN  ] " << scenario.name << std::endl;
    const ScenarioWatchdog watchdog(scenario.name, scenarioBoundMs());
    {
        // The message manager on this thread; no window, no audio device.
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        if (!ControlSocket::acquireNetworking()) {
            std::cerr << "cannot initialise networking" << std::endl;
            return 1;
        }
        scenario.run();
        // Before the message manager goes away: a dialog left open would be deleted at its shutdown,
        // where an upstream dialog uses its freed content (see closeLeftoverModalWindows).
        const int closedWindows = closeLeftoverModalWindows();
        CONTROL_CHECK(closedWindows >= 0);
        if (closedWindows > 0)
            note("closed " + std::to_string(closedWindows) + " modal window(s) left open");
        ControlSocket::releaseNetworking();
    }
    // Never the user's real discovery file; preferences and logs were checked inside the data dir.
    CONTROL_CHECK(!realDiscoveryFile.empty() && !std::filesystem::exists(realDiscoveryFile));
    CONTROL_CHECK(!isWithin(realDiscoveryFile, isolatedDataDirectory()));
    std::cout << (failureCount() == 0 ? "[  OK  ] " : "[FAILED] ") << scenario.name << " ("
              << millisecondsSince(started) << " ms)" << std::endl;
    return failureCount() == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    publishCommandLineToJuce(argc, argv);
    const Scenario* scenario = nullptr;
    if (argc > 1)
        for (const auto& candidate : kScenarios)
            if (std::strcmp(argv[1], candidate.name) == 0)
                scenario = &candidate;
    if (scenario == nullptr) {
        std::cerr << "usage: OpenTuneControlProcessorTests <scenario> [control API options]; scenarios:";
        for (const auto& candidate : kScenarios)
            std::cerr << " " << candidate.name;
        std::cerr << std::endl;
        return 2;
    }
    if (isolatedDataDirectory().empty()) {
        std::cerr << "OPENTUNE_DATA_DIR must be an absolute directory: without it the processor would write "
                     "the user's real preferences and logs"
                  << std::endl;
        return 2;
    }
    if (!scenario->lifecycle)
        return runScenario(*scenario);
    const std::string traceName = std::string("ControlProcessorTests.") + scenario->name;
    return OpenTuneTest::run(argc, argv, traceName.c_str(), [scenario](int, char**) { return runScenario(*scenario); });
}
