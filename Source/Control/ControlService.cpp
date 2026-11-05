// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlService.h"

#include "ControlDispatcher.h"
#include "ControlJobs.h"
#include "ControlServer.h"
#include "Edit/ControlOriginAction.h"

#include "PluginProcessor.h"
#include "Utils/AppLogger.h"

#include <juce_events/juce_events.h>

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// Build identity. cmake/OpenTuneControl.cmake defines these for this file only, so that a new commit
// recompiles nothing else; the fallbacks keep a build without them valid against the schema.
#ifndef OPENTUNE_CONTROL_BUILD_COMMIT
#define OPENTUNE_CONTROL_BUILD_COMMIT "0000000000000000000000000000000000000000"
#endif
#ifndef OPENTUNE_CONTROL_COMMIT_TAG
#define OPENTUNE_CONTROL_COMMIT_TAG ""
#endif
#ifndef OPENTUNE_CONTROL_BUILD_TAG
#define OPENTUNE_CONTROL_BUILD_TAG ""
#endif
#ifndef OPENTUNE_CONTROL_SOURCE_URL
#define OPENTUNE_CONTROL_SOURCE_URL "https://github.com/YuFeng926/OpenTune"
#endif

namespace OpenTune {

namespace {

ControlService::RuntimeInitialiser& runtimeInitialiserOverride()
{
    static ControlService::RuntimeInitialiser initialiser;
    return initialiser;
}

/** The process's command-line arguments (UTF-8, without the executable) as JUCE reads them:
    juce::JUCEApplicationBase::getCommandLineParameterArray(), on Windows parsed from GetCommandLineW(). */
std::vector<std::string> commandLineArguments()
{
    std::vector<std::string> arguments;
    for (const auto& argument : juce::JUCEApplicationBase::getCommandLineParameterArray())
        arguments.push_back(argument.toStdString());
    return arguments;
}

std::optional<std::string> nonEmpty(const char* text)
{
    if (text == nullptr || *text == '\0')
        return std::nullopt;
    return std::string(text);
}

juce::File fileOf(const std::string& utf8Path)
{
    return juce::File(juce::String::fromUTF8(utf8Path.c_str()));
}

} // namespace

/** The listener's diagnostic sink: forwards to AppLogger until close(). close() takes the mutex, so it
    also waits for a line being written; after it returns the sink never reaches AppLogger again. */
struct ControlService::LogGate
{
    std::mutex mutex;
    bool open = true;

    void write(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (open)
            AppLogger::logNoThrow(juce::String::fromUTF8(line.c_str()));
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
    }
};

ControlService::ControlService(OpenTuneAudioProcessor& processor)
    : processor_(processor)
    , options_(parseControlApiOptions(commandLineArguments(), controlSystemEnvironment))
    , messageThread_(std::make_shared<ControlMessageThread>())
    , logGate_(std::make_shared<LogGate>())
{}

ControlService::~ControlService()
{
    stop();
}

void ControlService::scheduleStart()
{
    // Not enabled at run time: nothing is posted, written or logged (PROTOCOL.md section 6.2).
    if (!options_.enabled || startScheduled_ || stopCalled_)
        return;
    startScheduled_ = true;
    // `this` is safe: the function runs only while the liveness token is alive, and stop() (called by
    // the destructor at the latest) kills it on the message thread, where the function runs too.
    if (!messageThread_->post([this] { start(); }))
        log("control API: not started: the start could not be queued on the message thread");
}

bool ControlService::start()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (startAttempted_ || stopCalled_ || !options_.enabled)
        return running_.load();
    startAttempted_ = true;
    try {
        return startListening();
    } catch (...) {
        server_.reset();
        log("control API: not started: unexpected error");
        return false;
    }
}

bool ControlService::startListening()
{
    if (!options_.error.empty()) {
        log("control API: not started: " + options_.error);
        return false;
    }
    // OpenTuneAudioProcessor::ensureRuntimeInitialized(): the same sequence as the host entry points
    // (createEditor, prepareToPlay), so that the services and preferences the API reads exist before
    // the first request. It works without an audio device and without a window.
    const auto& initialiser = runtimeInitialiserOverride();
    const bool runtimeReady = initialiser ? initialiser(processor_) : processor_.ensureRuntimeInitialized();
    if (!runtimeReady) {
        log("control API: not started: the OpenTune runtime could not be initialised");
        return false;
    }

    std::string error;
    const auto directory =
        controlResolveDiscoveryDirectory(options_, controlSystemEnvironment, controlCurrentPlatform(), error);
    if (directory.empty()) {
        log("control API: not started: " + error);
        return false;
    }
    ControlServerConfig config;
    config.discoveryDirectory = directory;
    config.readyFile = options_.readyFile;
    config.launchNonce = options_.launchNonce;
    config.build = buildIdentity();
    // The dispatcher answers every request after session.hello. It reaches the processor and the bridge
    // only on the message thread, while the liveness token is alive, so `this` is safe in the accessor.
    ControlDispatcher::Setup setup;
    setup.processor = &processor_;
    setup.messageThread = messageThread_;
    setup.limits = config.limits;
    setup.build = config.build;
    setup.uiBridge = [this] { return uiBridge_; };
    // The jobs live as long as the dispatcher can reach them; stop() stops them before the listener.
    auto jobs = std::make_shared<ControlJobs>(config.limits);
    setup.jobs = jobs;
    // The undo entries the API records keep it alive beyond the service (OpenTune's UndoManager outlives it).
    history_ = std::make_shared<ControlHistory>();
    setup.history = history_;
    auto dispatcher = std::make_shared<ControlDispatcher>(std::move(setup));
    config.handler = dispatcher;
    auto gate = logGate_;
    config.log = [gate](const std::string& line) { gate->write(line); };
    auto server = std::make_unique<ControlServer>(std::move(config));
    if (!server->start(error)) {
        jobs->stop();
        log("control API: not started: " + error);
        return false;
    }
    // Requests run on this thread, after this function returned, so they always see the instance id.
    dispatcher->setInstanceId(server->instanceId());
    dispatcher->setRedactionSecrets(server->secrets());
    dispatcher_ = std::move(dispatcher);
    jobs_ = std::move(jobs);
    discoveryFile_ = server->discoveryFilePath();
    port_ = server->port();
    if (options_.readyFile) {
        // The listener writes the ready file and only logs a failure; remember it only when it holds
        // this instance's content, so that stop() never deletes a file it did not write.
        const auto expected = controlReadyFileContent(controlCurrentProcessId(), server->instanceId());
        if (fileOf(*options_.readyFile).loadFileAsString().toStdString() == expected)
            readyFileContent_ = expected;
    }
    server_ = std::move(server);
    running_ = true;
    return true;
}

void ControlService::stop() noexcept
{
    if (stopCalled_)
        return;
    stopCalled_ = true;
    try {
        // 1. Stop flag: from now on requests are answered with SHUTTING_DOWN.
        stopping_ = true;
        // 2. Wake every thread waiting for the message thread; queued functions become no-ops.
        messageThread_->stop();
        // 3. The jobs: waiting job.get requests are answered with SHUTTING_DOWN, no job work runs again,
        //    and the work of every job is destroyed on this thread, joining whatever thread it owns.
        if (jobs_ != nullptr)
            jobs_->stop();
        // 4. The listener: its own stop flag and SHUTTING_DOWN for new requests, then the sessions and
        //    the listener close, the threads are joined with a bounded wait and the discovery file is
        //    deleted (ControlServer::stop()).
        if (server_ != nullptr) {
            server_->stop();
            server_.reset();
        }
        // 5. The ready file.
        withdrawReadyFile();
    } catch (...) {
        log("control API: unexpected error while stopping");
    }
    running_ = false;
    port_ = 0;
    // 6. No connection thread reaches AppLogger through this service after this point.
    logGate_->close();
}

void ControlService::withdrawReadyFile()
{
    if (readyFileContent_.empty() || !options_.readyFile)
        return;
    const auto file = fileOf(*options_.readyFile);
    if (file.loadFileAsString().toStdString() == readyFileContent_ && !file.deleteFile())
        log("control API: the ready file could not be deleted");
    readyFileContent_.clear();
}

void ControlService::log(const std::string& message) const
{
    AppLogger::logNoThrow(juce::String::fromUTF8(message.c_str()));
}

ControlBuildIdentity ControlService::buildIdentity()
{
    ControlBuildIdentity identity;
    identity.appVersion = OPENTUNE_VERSION;
    identity.commit = OPENTUNE_CONTROL_BUILD_COMMIT;
    identity.commitTag = nonEmpty(OPENTUNE_CONTROL_COMMIT_TAG);
    identity.buildTag = nonEmpty(OPENTUNE_CONTROL_BUILD_TAG);
    identity.sourceUrl = OPENTUNE_CONTROL_SOURCE_URL;
    return identity;
}

void ControlService::setRuntimeInitialiserForTesting(RuntimeInitialiser initialiser)
{
    runtimeInitialiserOverride() = std::move(initialiser);
}

std::size_t ControlService::processorClassSizeForTesting() noexcept
{
    return sizeof(OpenTuneAudioProcessor);
}

} // namespace OpenTune
