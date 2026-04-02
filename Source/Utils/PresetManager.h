#pragma once

/**
 * 预设管理器
 * 
 * 负责预设的保存、加载和管理。预设包含处理参数、UI状态等信息。
 */

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../PluginProcessor.h"

namespace OpenTune {

/**
 * PresetData - 预设数据结构
 * 
 * 包含预设的所有参数和状态信息。
 */
struct PresetData {
    juce::String name;              // 预设名称
    juce::String author;            // 作者
    juce::String description;       // 描述
    int64_t timestamp;              // 时间戳
    int version{1};                 // 版本号

    // 处理参数
    float retuneSpeed{15.0f};       // 重调速度
    int scaleRoot{0};               // 调式主音（0=C, 1=C#, 等）
    bool isMinorScale{false};       // 是否为小调

    // 高级参数
    float vibratoDepth{0.0f};       // 颤音深度
    float vibratoRate{6.0f};        // 颤音速率

    // UI状态
    bool showWaveform{true};        // 显示波形
    bool showLanes{true};           // 显示轨道
    double zoomLevel{1.0};          // 缩放级别
    double bpm{120.0};              // BPM

    PresetData() : timestamp(juce::Time::currentTimeMillis()) {}

    /**
     * 序列化为ValueTree
     */
    juce::ValueTree toValueTree() const;
    
    /**
     * 从ValueTree反序列化
     */
    static PresetData fromValueTree(const juce::ValueTree& tree);
};

/**
 * PresetManager - 预设管理器类
 * 
 * 提供预设的保存、加载、枚举等功能。
 */
class PresetManager {
public:
    PresetManager();
    ~PresetManager();

    /**
     * 保存预设到文件
     * @param preset 预设数据
     * @param file 目标文件
     * @return 是否成功
     */
    bool savePreset(const PresetData& preset, const juce::File& file);
    
    /**
     * 从文件加载预设
     * @param file 源文件
     * @return 预设数据
     */
    PresetData loadPreset(const juce::File& file);

    /**
     * 获取目录中的所有预设文件
     */
    std::vector<juce::File> getPresetFiles(const juce::File& directory);
    
    /**
     * 获取默认预设目录
     */
    juce::File getDefaultPresetDirectory();

    /**
     * 捕获当前处理器状态为预设
     */
    PresetData captureCurrentState(OpenTuneAudioProcessor& processor);
    
    /**
     * 应用预设到处理器
     */
    void applyPreset(const PresetData& preset, OpenTuneAudioProcessor& processor);

private:
    juce::File presetDirectory_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};

} // namespace OpenTune
