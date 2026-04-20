#pragma once

/**
 * 传输控制栏组件
 * 
 * 提供播放控制、BPM设置、调式选择等功能的工具栏：
 * - 播放/暂停/停止控制
 * - 循环开关
 * - BPM 显示和编辑
 * - 调式选择（大调/小调等）
 * - 视图切换
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <functional>
#include "UIColors.h"

namespace OpenTune {

// 统一工具栏按钮类，用于所有顶部栏图标（轨道、属性、传输等）
class UnifiedToolbarButton : public juce::Button
{
public:
    enum ConnectedEdge
    {
        None = 0,
        Left = 1 << 0,
        Right = 1 << 1,
        Top = 1 << 2,
        Bottom = 1 << 3
    };

    UnifiedToolbarButton(const juce::String& name, juce::Path iconPath, juce::Path toggledIconPath = {});
    
    void setIcon(juce::Path iconPath);
    void setToggledIcon(juce::Path iconPath);
    void setConnectedEdges(int edges);

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    juce::Path iconPath_;
    juce::Path toggledIconPath_;
    int connectedEdges_ = None;
};

class DigitalTimeDisplay : public juce::Component
{
public:
    DigitalTimeDisplay();
    void setTimeString(const juce::String& time);
    void paint(juce::Graphics& g) override;

private:
    void drawChar(juce::Graphics& g, juce::juce_wchar c, juce::Rectangle<float> area);
    void drawSegment(juce::Graphics& g, int segment, juce::Rectangle<float> area, bool active);

    juce::String timeString_ = "00:00";
};

class RoundedActionButton : public juce::Button
{
public:
    explicit RoundedActionButton(const juce::String& text, float fontSize = 13.0f);
    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    juce::String text_;
    float fontSize_ = 13.0f;
};

// 视图切换开关（轨道视图 vs 音高视图）
class ViewToggleSwitch : public juce::Button
{
public:
    ViewToggleSwitch();
    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void mouseDown(const juce::MouseEvent& e) override;
};

class BpmValueField : public juce::Component,
                      private juce::Timer
{
public:
    BpmValueField();
    void setValue(double value);
    double getValue() const;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusLost(juce::Component::FocusChangeType cause) override;

    std::function<void(double)> onCommit;

private:
    void timerCallback() override;
    void commit();
    void cancelEdit();

    juce::String text_ = "120";
    bool showCaret_ = false;
    int caretIndex_ = 0;
    bool isEditing_ = false;
    int lastValidValue_ = 120;
};

class TransportBarComponent : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playRequested() = 0;
        virtual void pauseRequested() = 0;
        virtual void stopRequested() = 0;
        virtual void loopToggled(bool enabled) = 0;
        virtual void returnToStartOnPauseToggled(bool enabled) = 0;
        virtual void bpmChanged(double newBpm) = 0;
        virtual void scaleChanged(int rootNote, int scaleType) = 0;
        virtual void viewToggled(bool workspaceView) = 0;
        virtual void audioSettingsRequested() {}
    };

    // Callback functions for Menu requests (File/Edit/View)
    std::function<void()> onFileMenuRequested;
    std::function<void()> onEditMenuRequested;
    std::function<void()> onViewMenuRequested;

    TransportBarComponent();
    ~TransportBarComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;  // 点击外部区域时让BpmField失去焦点

    void applyTheme();
    void setEmbeddedInTopBar(bool embedded);

    void addListener(Listener* l);
    void removeListener(Listener* l);

    void refreshLocalizedText();  // 刷新本地化文本

    void setPlaying(bool playing);
    bool isPlaying() const;

    void setLoopEnabled(bool enabled);
    bool isLoopEnabled() const;
    // Alias for compatibility
    void setLooping(bool looping) { setLoopEnabled(looping); }

    void setReturnToStartOnPause(bool enabled);
    bool isReturnToStartOnPause() const;

    void setBpm(double bpm);
    double getBpm() const;

    void setScale(int rootNote, int scaleType);

    void setPositionSeconds(double seconds);
    void setWorkspaceView(bool workspaceView);
    bool isWorkspaceView() const;
    void setRenderStatusText(const juce::String& text);

    juce::Component& getFileButton() { return fileButton_; }
    juce::Component& getEditButton() { return editButton_; }
    juce::Component& getViewButton() { return viewButton_; }

private:
    // Internal callbacks
    void onPlayClicked();
    void onPauseClicked();
    void onStopClicked();
    void onLoopToggled();
    void onReturnToStartToggled();
    void onBpmChanged();
    void onTapClicked();
    void onScaleChanged();
    void onTrackViewClicked();
    void onPianoViewClicked();

    juce::ListenerList<Listener> listeners_;

    // Menu Buttons (LOGO style)
    UnifiedToolbarButton fileButton_;
    UnifiedToolbarButton editButton_;
    UnifiedToolbarButton viewButton_;

    // Transport Controls
    UnifiedToolbarButton playButton_;
    UnifiedToolbarButton pauseButton_;
    UnifiedToolbarButton stopButton_;
    UnifiedToolbarButton loopButton_;
    UnifiedToolbarButton returnToStartButton_;  // 切换暂停时是否回到起点
    
    // Split View Buttons
    UnifiedToolbarButton trackViewButton_;
    UnifiedToolbarButton pianoViewButton_;
    
    // Labels & Editors
    DigitalTimeDisplay timeDisplay_;
    juce::Label bpmLabel_;
    BpmValueField bpmField_;
    UnifiedToolbarButton tapButton_;
    
    // Scale controls
    juce::Label scaleLabel_;
    juce::ComboBox scaleRootSelector_;
    juce::ComboBox scaleTypeSelector_;
    // State
    bool isPlaying_ = false;
    bool workspaceView_ = true;
    bool embeddedInTopBar_ = false;
    juce::String renderStatusText_;
    juce::Time lastTapTime_;
    std::vector<double> tapIntervals_;
    static const int maxTapSamples_ = 5;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBarComponent);
};

} // namespace OpenTune
