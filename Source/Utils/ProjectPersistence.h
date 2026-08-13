/**
 * 工程持久化层（ProjectPersistence）
 *
 * 负责 ProjectSnapshot 与 .otproj ValueTree/XML 之间的双向转换。
 * 不承担 UI 逻辑、文件选择或工程生命周期管理——那些属于 ProjectSession 的职责。
 *
 * 设计原则：
 *   - 纯函数式转换：toValueTree / fromValueTree 无副作用
 *   - 文件 I/O 通过 writeProjectFile / readProjectFile 封装
 *   - 版本校验：projectFormatVersion 不匹配时拒绝加载并返回错误
 *   - 缺失节点使用默认值回退（向前兼容）
 *   - 使用 Result<T> 模式统一错误处理
 */

#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ProjectModel.h"
#include "Error.h"

namespace OpenTune {

class ProjectPersistence {
public:
    ProjectPersistence() = default;

    // ============================================================================
    // ValueTree 转换
    // ============================================================================

    /** 将完整工程快照序列化为 ValueTree */
    juce::ValueTree toValueTree(const ProjectSnapshot& snapshot) const;

    /** 从 ValueTree 反序列化为工程快照。
     *  成功时返回包含快照的 Result，失败时包含 Error。 */
    Result<ProjectSnapshot> fromValueTree(const juce::ValueTree& tree) const;

    // ============================================================================
    // 文件 I/O
    // ============================================================================

    /** 将工程快照写入 .otproj 文件。
     *  返回 true 表示写入成功。 */
    bool writeProjectFile(const ProjectSnapshot& snapshot, const juce::File& file) const;

    /** 从 .otproj 文件读取工程快照。
     *  返回 Result<ProjectSnapshot>：成功时包含快照，失败时包含错误描述。 */
    Result<ProjectSnapshot> readProjectFile(const juce::File& file) const;

    // ============================================================================
    // 常量
    // ============================================================================

    static constexpr int kCurrentProjectFormatVersion = 5; // v5 adds per-note EQ settings
    static constexpr int kMinimumProjectFormatVersion = 3;
    static constexpr const char* kRootNodeName = "OpenTuneProject";
    static constexpr const char* kProjectFormatVersionAttr = "projectFormatVersion";
    static constexpr const char* kAppVersionAttr = "appVersion";

private:
    // 序列化辅助
    static juce::ValueTree settingsToValueTree(const ProjectSettings& settings);
    static juce::ValueTree sourceToValueTree(const ProjectSourceEntry& source);
    static juce::ValueTree contentToValueTree(const ProjectContentEntry& mat);
    static juce::ValueTree trackToValueTree(const ProjectTrackEntry& track);
    static juce::ValueTree referenceBindingToValueTree(const ProjectReferenceBinding& binding);

    // 反序列化辅助
    static ProjectSettings settingsFromValueTree(const juce::ValueTree& tree);
    static ProjectSourceEntry sourceFromValueTree(const juce::ValueTree& tree);
    static ProjectContentEntry contentFromValueTree(const juce::ValueTree& tree);
    static ProjectTrackEntry trackFromValueTree(const juce::ValueTree& tree);
    static ProjectReferenceBinding referenceBindingFromValueTree(const juce::ValueTree& tree);

    // 子节点反序列化
    static std::vector<Note> notesFromValueTree(const juce::ValueTree& tree);
    static std::vector<ProjectContentEntry::SegmentEntry> segmentsFromValueTree(const juce::ValueTree& tree);
    static ProjectContentEntry::TimeGridEntry timeGridFromValueTree(const juce::ValueTree& tree);
    static std::vector<ProjectPlacementEntry> placementsFromValueTree(const juce::ValueTree& tree);
    static std::vector<ProjectContentEntry::SilentGapEntry> silentGapsFromValueTree(const juce::ValueTree& tree);
    static ProjectContentEntry::ReferenceFeatureEntry referenceFeaturesFromValueTree(const juce::ValueTree& tree);
    static AutomationLane volumeEnvelopeFromValueTree(const juce::ValueTree& tree);

    // 子节点序列化
    static juce::ValueTree notesToValueTree(const std::vector<Note>& notes, const juce::String& nodeName);
    static juce::ValueTree segmentsToValueTree(const std::vector<ProjectContentEntry::SegmentEntry>& segments);
    static juce::ValueTree timeGridToValueTree(const ProjectContentEntry::TimeGridEntry& tg);
    static juce::ValueTree placementsToValueTree(const std::vector<ProjectPlacementEntry>& placements);
    static juce::ValueTree silentGapsToValueTree(const std::vector<ProjectContentEntry::SilentGapEntry>& gaps);
    static juce::ValueTree referenceFeaturesToValueTree(const ProjectContentEntry::ReferenceFeatureEntry& rf);
    static juce::ValueTree volumeEnvelopeToValueTree(const AutomationLane& envelope);

    // 属性读写辅助
    static void setOptionalProperty(juce::ValueTree& tree, const juce::Identifier& name, const juce::String& value);
    static juce::String getOptionalProperty(const juce::ValueTree& tree, const juce::Identifier& name, const juce::String& defaultValue);
    static void setColourProperty(juce::ValueTree& tree, const juce::Identifier& name, const juce::Colour& colour);
    static juce::Colour getColourProperty(const juce::ValueTree& tree, const juce::Identifier& name, const juce::Colour& defaultColour);
};

} // namespace OpenTune
