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

#include "ProjectModel.h"
#include "Error.h"

namespace OpenTune {

class OpenTuneAudioProcessor;
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
    // 工程操作
    // ============================================================================

    /**
     * 打开工程文件。
     * 调用方应先在外部判断是否需要保存当前工程。
     *
     * @param file .otproj 文件路径
     * @return Result<void> 成功或失败原因
     */
    Result<void> openProject(const juce::File& file);

    /**
     * 保存工程到当前关联路径。
     * 如果从未保存过（hasProjectPath() == false），应在外部先调用 saveProjectAs()。
     *
     * @return Result<void> 成功或失败原因
     */
    Result<void> saveProject();

    /**
     * 另存为工程到新路径。
     * 会创建新工程目录、复制媒体文件、更新工程路径。
     *
     * @param file 新 .otproj 文件路径
     * @return Result<void> 成功或失败原因
     */
    Result<void> saveProjectAs(const juce::File& file);

    // ============================================================================
    // 分步保存（分离 UI 线程和文件 I/O 线程）
    // ============================================================================

    /**
     * 保存工作单元。
     * prepareSave() 在消息线程构造此结构（captureSnapshot + 路径捕获）；
     * executeSaveToFile() 在后台线程消费此结构（纯文件 I/O）。
     */
    struct SaveTask {
        ProjectSnapshot snapshot;
        juce::File targetFile;
        juce::File mediaDirectory;
    };

    /** 在消息线程调用：捕获快照 + 路径。返回的 SaveTask 供后台线程使用。 */
    SaveTask prepareSave();

    /**
     * 在后台线程调用：纯文件 I/O。
     * 复制媒体文件并写入 .otproj。不碰任何 ProjectSession 内部状态。
     */
    static Result<void> executeSaveToFile(SaveTask& task);

    /** 设置当前工程文件路径（用于 saveProjectAs 的场景） */
    void setCurrentProjectFile(const juce::File& file);

    /**
     * 清空当前工程状态（新建工程）。
     * 会清除 tracks、placements、contents、sources，重置路径和脏标记。
     */
    void newProject();

    // ============================================================================
    // 快照抓取/应用（供序列化使用）
    // ============================================================================

    /** 从当前运行时状态抓取完整工程快照 */
    ProjectSnapshot captureSnapshot() const;

    /** 将工程快照应用到当前运行时状态 */
    Result<void> applySnapshot(const ProjectSnapshot& snapshot);

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

    /** 获取工程文件所在目录的媒体子目录 */
    juce::File getProjectMediaDirectory() const;

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
