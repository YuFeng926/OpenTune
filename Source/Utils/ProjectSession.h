/**
 * 工程会话控制器（ProjectSession）
 *
 * 负责 Standalone 模式下工程生命周期管理：Open / Save / Save As / 未保存修改弹窗。
 * 持有当前工程路径、脏标记、工程快照抓取/应用的协调逻辑。
 *
 * 设计原则：
 *   - 只管理文件级别的生命周期，不触碰 UI 组件（UI 由 PluginEditor 处理）
 *   - 脏标记由外部通过 markDirty() / clearDirty() 设置
 *   - 媒体复制在 Save/Save As 时自动执行
 *   - 使用 Result<void> 统一成功/失败语义
 *
 * 依赖：
 *   - ProjectModel: 数据模型
 *   - ProjectPersistence: 序列化/反序列化
 *   - OpenTuneAudioProcessor: 运行时 shell（抓取/应用状态）
 *   - AppPreferences: 最近工程列表持久化
 */

#pragma once

#include <juce_core/juce_core.h>

#include "../PluginProcessor.h"
#include "ProjectModel.h"
#include "Error.h"

namespace OpenTune {

class AppPreferences;

class ProjectSession {
public:
    // ============================================================================
    // 媒体目录常量
    // ============================================================================

    /** 工程目录下的媒体子目录名 */
    static constexpr const char* kMediaDirectoryName = "Project_Media";

    // ============================================================================
    // 构造
    // ============================================================================

    explicit ProjectSession(OpenTuneAudioProcessor& processor, AppPreferences& appPreferences);
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    // ============================================================================
    // 路径查询
    // ============================================================================

    /** 当前是否有关联的工程文件路径 */
    bool hasProjectPath() const noexcept;

    /** 获取当前工程文件路径（可能为空 File） */
    const juce::File& getCurrentProjectFile() const noexcept;

    /** 获取当前工程名（无路径时返回 "Untitled"） */
    juce::String getProjectName() const;

    // ============================================================================
    // 脏状态
    // ============================================================================

    /** 当前工程是否有未保存修改 */
    bool isDirty() const noexcept;

    /** 标记工程为已修改 */
    void markDirty();

    /** 清除脏标记（保存成功后调用） */
    void clearDirty();

    /** 获取当前脏标记的 generation（用于后台保存后判断是否仍可安全清除脏标记） */
    uint64_t getDirtyGeneration() const noexcept;

    // ============================================================================
    // 两阶段工程 I/O
    // ============================================================================

    /**
     * 保存工作单元。
     * prepareSave(targetFile) 在消息线程构造此结构（captureSnapshot + 目标路径）；
     * executeSaveToFile() 在后台线程消费此结构（纯文件 I/O）。
     */
    struct SaveTask {
        ProjectSnapshot snapshot;
        juce::File targetFile;
        juce::File mediaDirectory;
    };

    /** 在消息线程调用：捕获目标文件快照。返回的 SaveTask 供后台线程使用。 */
    SaveTask prepareSave(const juce::File& targetFile);

    /**
     * 在后台线程调用：纯文件 I/O。
     * 复制媒体文件并写入 .otproj。不碰任何 ProjectSession 内部状态。
     */
    static Result<void> executeSaveToFile(SaveTask& task);

    /** 保存成功后在消息线程提交当前工程文件路径。 */
    void setCurrentProjectFile(const juce::File& file);

    struct PreparedProjectSource {
        uint64_t sourceId{0};
        OpenTuneAudioProcessor::PreparedImport preparedImport;
    };

    struct PreparedOpen {
        ProjectSnapshot snapshot;
        juce::File projectFile;
        std::vector<PreparedProjectSource> sources;
    };

    /** 在后台线程读取工程媒体并执行 canonical import 预处理。 */
    Result<PreparedOpen> prepareOpen(const juce::File& file);

    /** 在消息线程一次性提交已预处理的工程数据。 */
    Result<void> commitPreparedOpen(PreparedOpen&& preparedOpen);

    /** 从当前运行时状态抓取完整工程快照 */
    ProjectSnapshot captureSnapshot() const;

    // ============================================================================
    // 最近工程列表管理
    // ============================================================================

    /** 将工程路径推到最近工程列表顶部（MRU 去重、裁剪） */
    void pushRecentProject(const juce::File& file);

    /** 获取最近工程路径列表 */
    std::vector<juce::File> getRecentProjects() const;

    /** 清空最近工程列表 */
    void clearRecentProjects();

private:
    // ============================================================================
    // 媒体辅助
    // ============================================================================

    /** 复制所有引用媒体到指定媒体目录（静态，纯文件 I/O） */
    static Result<void> copyMediaToProjectDirectory(ProjectSnapshot& snapshot, const juce::File& mediaDir);

    /** 生成媒体文件的稳定目标文件名 */
    static juce::String generateMediaFileName(const ProjectSourceEntry& source);

    // ============================================================================
    // 成员
    // ============================================================================

    OpenTuneAudioProcessor& processorRef_;
    AppPreferences& appPreferencesRef_;
    juce::File currentProjectFile_;
    bool dirty_{false};
    uint64_t dirtyGeneration_{0};

    // 固化工程身份（首次保存生成，后续复用）
    mutable juce::String cachedProjectId_;
    mutable juce::String cachedCreatedAt_;
};

} // namespace OpenTune
