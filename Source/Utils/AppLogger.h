#pragma once

/**
 * 应用日志管理器（进程寿命）
 *
 * 提供应用程序日志的初始化、记录和查询功能。
 * 日志文件保存在用户应用数据目录中。
 *
 * 生命周期合同：logger 是进程级后台资源（FileLogger 内部有独立写入线程），
 * 进程内所有线程（F0/GAME 推理 worker、渲染 worker、control worker）都可能
 * 在任何时刻写入日志。因此不存在"实例销毁关闭 logger"的路径：initialize()
 * 幂等，log 系列自动初始化；显式 shutdown 已删除，资源随进程退出由系统回收。
 * 多实例卸载场景下，后加载的实例继续复用同一个进程级 logger。
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
