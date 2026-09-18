#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../Standalone/UI/UIColors.h"
#include "../Utils/LocalizationManager.h"

namespace OpenTune
{

/**
 * @brief 主题化文件选择器内容组件（替代 juce::FileChooser / FileChooserDialogBox）
 *
 * 仅复用 JUCE 的 DirectoryContentsList + TimeSliceThread + WildcardFileFilter 作为后台目录模型，
 * 目录/文件列表由自定义 ListBoxModel 逐行绘制（主题文件夹、文件图标），
 * 不包含任何 JUCE 默认文件浏览器视觉（无 FileBrowserComponent、无默认文件浏览器、无系统原生对话框）。
 *
 * 用法：
 *   ThemedFileChooserContent::openFile (this, u8"导入音频", lastFile, "*.wav;*.mp3",
 *       [this] (juce::Array<juce::File> files) { ... });
 *
 *   ThemedFileChooserContent::saveFile (this, u8"导出音频", lastFile, "*.wav",
 *       [this] (juce::Array<juce::File> files) { ... });
 *
 * 取消（取消按钮 / Esc / 关闭窗口）不会调用结果回调。
 */
class ThemedFileChooserContent final : public juce::Component,
                                       private juce::ChangeListener,
                                       private juce::ListBoxModel,
                                       private juce::ComponentListener
{
public:
    enum class Mode
    {
        OpenSingle,
        OpenMultiple,
        Save
    };

    /** 结果回调：打开模式返回选中的文件，保存模式返回唯一的目标文件；取消时不调用。 */
    using ResultCallback = std::function<void (juce::Array<juce::File>)>;

    // ============================================================================
    // Construction
    // ============================================================================

    ThemedFileChooserContent (Mode mode,
                              const juce::String& title,
                              const juce::File& initialFileOrDirectory,
                              const juce::String& wildcard,
                              ResultCallback onResult);

    ~ThemedFileChooserContent() override;

    // ============================================================================
    // Async launch API
    // ============================================================================

    /** 异步弹出文件选择器；parent 为空时按主显示器居中。 */
    static void launch (juce::Component* parent,
                        Mode mode,
                        const juce::String& title,
                        const juce::File& initialFileOrDirectory,
                        const juce::String& wildcard,
                        ResultCallback onResult);

    static void openFile (juce::Component* parent,
                          const juce::String& title,
                          const juce::File& initialFileOrDirectory,
                          const juce::String& wildcard,
                          ResultCallback onResult);

    static void openFiles (juce::Component* parent,
                           const juce::String& title,
                           const juce::File& initialFileOrDirectory,
                           const juce::String& wildcard,
                           ResultCallback onResult);

    static void saveFile (juce::Component* parent,
                          const juce::String& title,
                          const juce::File& initialFileOrDirectory,
                          const juce::String& wildcard,
                          ResultCallback onResult);

    // ============================================================================
    // Component overrides
    // ============================================================================

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    // ============================================================================
    // Layout / helpers
    // ============================================================================

    struct Layout
    {
        juce::Rectangle<int> title, pathRow, list, saveRow, status, buttons;
    };

    Layout makeLayout() const;
    void fitToDisplay (juce::Component* parent);

    /** launchAsync 会把键盘焦点先交给 DialogWindow，这里延后一拍取回内容焦点。 */
    void takeKeyboardFocus();

    /** 保存标签宽度：按当前字体和文案测量，窗口过窄时为输入框让位。 */
    int getSaveLabelWidth (int rowWidth) const;

    void updateUpButton();
    void updateAcceptButton();
    void updateStatusText();
    juce::String getStatusText() const;

    void setCurrentDirectory (const juce::File& directory);
    void navigateToParent();
    void navigateToTypedPath();
    void refreshList();
    void setSaveFileName (const juce::String& name);
    void activateRow (int row);
    void selectPendingFile();
    void setHoveredRow (int row);

    /** 选中行的稳定 full path；列表异步重建后据此恢复选择，避免行号漂移 */
    juce::String getRowFullPath (int row) const;
    void updateSelectedPathsFromRows();
    void restoreSelectionFromPaths();

    bool isSaveTargetValid() const;
    juce::File getSaveTargetFile() const;

    /** 读取当前选中行前先按稳定 full path 恢复选择，避免列表重建后行号漂移 */
    juce::Array<juce::File> getSelectedFiles();

    void accept();
    void cancel();
    void closeDialog();

    // ============================================================================
    // ListBoxModel
    // ============================================================================

    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override;
    void returnKeyPressed (int lastRowSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    // ============================================================================
    // ChangeListener / MouseListener
    // ============================================================================

    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // ============================================================================
    // ComponentListener（父组件生命周期）
    // ============================================================================

    void componentBeingDeleted (juce::Component& comp) override;

    void watchParent (juce::Component* parent);
    void unwatchParent();

    // ============================================================================
    // State
    // ============================================================================

    Mode mode_;
    juce::String title_;
    ResultCallback onResult_;

    juce::File currentDirectory_;
    juce::String pendingSelectName_;
    juce::String errorText_;
    int hoveredRow_ = -1;

    // 选中文件的稳定 full paths（多选时全部保存）；列表重建后据此恢复选择
    juce::StringArray selectedFullPaths_;
    bool restoringSelection_ = false;

    // 父组件生命周期监听（SafePointer，不保存裸指针跨异步）
    juce::Component::SafePointer<juce::Component> watchedParent_;

    // 后台目录扫描（filter_ 必须比 list_ 活得更久）
    juce::TimeSliceThread scanThread_ { "OpenTune file chooser" };
    std::unique_ptr<juce::WildcardFileFilter> filter_;
    std::unique_ptr<juce::DirectoryContentsList> list_;

    juce::TextButton upButton_;
    juce::TextButton refreshButton_;
    juce::TextEditor pathEditor_;
    juce::ListBox listBox_;
    juce::TextEditor saveNameEditor_;
    juce::TextButton cancelButton_;
    juce::TextButton acceptButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemedFileChooserContent)
};

} // namespace OpenTune
