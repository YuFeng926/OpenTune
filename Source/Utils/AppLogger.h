#pragma once

/**
 * 应用日志管理器
 * 
 * 提供应用程序日志的初始化、记录和查询功能。
 * 日志文件保存在用户应用数据目录中。
 */

#include <juce_core/juce_core.h>

namespace OpenTune {

enum class LogLevel {
    Debug,    
    Info,     
    Warning,  
    Error     
};

class AppLogger {
public:
    static void initialize();
    
    static void shutdown();
    
    static void log(const juce::String& message);
    
    static void debug(const juce::String& message);
    static void info(const juce::String& message);
    static void warn(const juce::String& message);
    static void error(const juce::String& message);
    
    static void setLogLevel(LogLevel level);
    static LogLevel getLogLevel();
    
    static juce::File getCurrentLogFile();
    
    static void logPerf(const juce::String& operation, double durationMs);

private:
    static void logWithLevel(LogLevel level, const juce::String& message);
    static const char* levelToString(LogLevel level);
};

/**
 * 性能计时器（RAII）
 */
class PerfTimer {
public:
    explicit PerfTimer(const juce::String& operation)
        : operation_(operation)
        , startTime_(juce::Time::getMillisecondCounterHiRes())
    {}
    
    ~PerfTimer() {
        // 性能日志已禁用
        return;
        double elapsed = juce::Time::getMillisecondCounterHiRes() - startTime_;
        AppLogger::logPerf(operation_, elapsed);
    }
    
private:
    juce::String operation_;
    double startTime_;
};

} // namespace OpenTune
