#include "AppLogger.h"

namespace OpenTune {

static juce::CriticalSection& getLoggerLock()
{
    static juce::CriticalSection lock;
    return lock;
}

static std::unique_ptr<juce::FileLogger>& getLoggerInstance()
{
    static std::unique_ptr<juce::FileLogger> logger;
    return logger;
}

static juce::File& getLogFileInstance()
{
    static juce::File file;
    return file;
}

static LogLevel& getLogLevelInstance()
{
    static LogLevel level = LogLevel::Info;
    return level;
}

const char* AppLogger::levelToString(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        default:                return "UNKNOWN";
    }
}

void AppLogger::logWithLevel(LogLevel level, const juce::String& message)
{
    LogLevel currentLevel = getLogLevel();
    int levelPriority[] = {0, 1, 2, 3}; // Debug, Info, Warning, Error
    if (levelPriority[static_cast<int>(level)] < levelPriority[static_cast<int>(currentLevel)]) {
        return;
    }
    log("[" + juce::String(levelToString(level)) + "] " + message);
}

void AppLogger::initialize()
{
    const juce::ScopedLock sl(getLoggerLock());
    auto& logger = getLoggerInstance();
    if (logger != nullptr) {
        return;
    }

    juce::File logDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("OpenTune")
        .getChildFile("Logs");
    if (!logDir.createDirectory()) {
        logDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("OpenTune")
            .getChildFile("Logs");
        logDir.createDirectory();
    }

    logger.reset(juce::FileLogger::createDateStampedLogger(
        logDir.getFullPathName(),
        "OpenTune",
        ".log",
        "OpenTune session start"
    ));

    if (logger != nullptr) {
        getLogFileInstance() = logger->getLogFile();
        juce::Logger::setCurrentLogger(logger.get());
        juce::Logger::writeToLog("Log file: " + getLogFileInstance().getFullPathName());
    }
}

void AppLogger::shutdown()
{
    const juce::ScopedLock sl(getLoggerLock());
    auto& logger = getLoggerInstance();
    if (juce::Logger::getCurrentLogger() == logger.get()) {
        juce::Logger::setCurrentLogger(nullptr);
    }
    logger.reset();
    getLogFileInstance() = juce::File{};
}

void AppLogger::log(const juce::String& message)
{
    AppLogger::initialize();
    juce::Logger::writeToLog(message);
}

void AppLogger::debug(const juce::String& message)
{
    logWithLevel(LogLevel::Debug, message);
}

void AppLogger::info(const juce::String& message)
{
    logWithLevel(LogLevel::Info, message);
}

void AppLogger::warn(const juce::String& message)
{
    logWithLevel(LogLevel::Warning, message);
}

void AppLogger::error(const juce::String& message)
{
    logWithLevel(LogLevel::Error, message);
}

void AppLogger::setLogLevel(LogLevel level)
{
    const juce::ScopedLock sl(getLoggerLock());
    getLogLevelInstance() = level;
}

LogLevel AppLogger::getLogLevel()
{
    const juce::ScopedLock sl(getLoggerLock());
    return getLogLevelInstance();
}

juce::File AppLogger::getCurrentLogFile()
{
    const juce::ScopedLock sl(getLoggerLock());
    return getLogFileInstance();
}

} // namespace OpenTune
