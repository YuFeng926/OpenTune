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

private:
    static void logWithLevel(LogLevel level, const juce::String& message);
    static const char* levelToString(LogLevel level);
};

} // namespace OpenTune
