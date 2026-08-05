#pragma once

/**
 * 参数面板组件
 * 
 * 显示和编辑音高校正参数的侧边面板：
 * - Retune Speed（校正速度）
 * - Vibrato Depth/Rate（颤音深度/速率）
 * - Note Split（音符分割阈值）
 * - 工具选择按钮
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <functional>
#include "OpenTuneLookAndFeel.h"
#include "UIColors.h"
#include "ToolIds.h"

namespace OpenTune {

class LargeKnobLookAndFeel : public OpenTuneLookAndFeel
{
public:
    LargeKnobLookAndFeel();

    juce::Font getSliderPopupFont(juce::Slider&) override
    {
        return UIColors::getUIFont(14.0f);
    }

    juce::Font getLabelFont(juce::Label&) override
    {
        return UIColors::getLabelFont(14.0f);
    }
    
    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = OpenTuneLookAndFeel::createSliderTextBox(slider);
        label->setFont(UIColors::getLabelFont(14.0f));
        return label;
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;
};

class ParameterPanel : public juce::Component
{
public:
    struct AutoButtonPresentation {
        enum class Mode : uint8_t {
            StandardAuto = 0,
            ReferenceAuto,
            ReferenceBoundButFallbackToAuto
        };

        Mode mode{Mode::StandardAuto};
        juce::String tooltip;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void retuneSpeedChanged(float speed) = 0;
        virtual void vibratoDepthChanged(float value) = 0;
        virtual void vibratoRateChanged(float value) = 0;
        virtual void noteSplitChanged(float value) = 0;
        virtual void toolSelected(int toolId) = 0;
        virtual void autoTuneRequested() {}
        virtual void pitchShiftRequested() {}
    };

    ParameterPanel();
    ~ParameterPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void setActiveTool(int toolId);
    void setExperimentalFeaturesEnabled(bool enabled);

    /** OpenDyne（NotesPrimary）模式：切换工具在上 / Pitch Shift 在底部布局。 */
    void setOpenDyneMode(bool enabled);
    bool isOpenDyneMode() const noexcept { return openDyneMode_; }

    /** Pitch Grid 全局开关回调：切换 No Snap / Chromatic / Key Scale 吸附模式。 */
    std::function<void(PitchGridMode)> onPitchGridModeChanged;

    /** 设置 AUTO 按钮的模式显示。
     *  @param hasReference  true: 主文本 "AUTO" + 副标题 "(Ref)"（小字在下方）, false: 仅 "AUTO" 居中
     */
    void setAutoButtonPresentation(const AutoButtonPresentation& presentation);
    
    // Setters for UI state
    void setRetuneSpeed(float speed);
    void setVibratoDepth(float value);
    void setVibratoRate(float value);
    void setNoteSplit(float value);
    void setPitchShiftIndicator(int semitone, int cents);

    void applyTheme();
    void refreshLocalizedText();  // 刷新本地化文本

    // ── 固定尺寸常量：resized() 绝对坐标排布的验收基准 ──
    // OpenDyne 模式内容需求高度：旋钮区 386（header 24 + spacing 12 + row1 143 + spacing 12 + row2 143 + spacing 12 + pitchShift 28 + spacing 12）+ 工具区 328（header 24 + gap 8 + 4×60 按钮 + 3×10 行距 = 302 + PitchGrid 4 + 22）
    static constexpr int kMinimumContentHeight = 714;
    // 面板最小高度 = 内容需求 + reduced(shadowMargin+innerPadding) 上下边距 40
    static constexpr int kMinimumPanelHeight = kMinimumContentHeight + 40;

    // Getters
    float getRetuneSpeed() const;
    float getVibratoDepth() const;
    float getVibratoRate() const;
    float getNoteSplit() const;

private:
    class ToolIconButton : public juce::Button
    {
    public:
        ToolIconButton(int toolId, const juce::String& name, const juce::String& tooltip);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        int getToolId() const { return toolId_; }
        void setIcon(const juce::Path& path, bool fill) { iconPath_ = path; fillIcon_ = fill; }
        void setTextIcon(const juce::String& iconText) { textIcon_ = iconText; repaint(); }
        void setSubTextIcon(const juce::String& subText) { subTextIcon_ = subText; repaint(); }

    private:
        int toolId_ = 0;
        juce::Path iconPath_;
        bool fillIcon_ = false;
        juce::String textIcon_;
        juce::String subTextIcon_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolIconButton)
    };

    void setupHeader(juce::Label& label, const juce::String& text);
    void setupLabel(juce::Label& label, const juce::String& text);
    void setupLargeKnob(juce::Slider& slider, double min, double max, double defaultVal, const juce::String& suffix);
    
    void onRetuneSpeedChanged();
    void onVibratoDepthChanged();
    void onVibratoRateChanged();
    void onNoteSplitChanged();
    void onToolClicked(int toolId);
    void rebuildAuroraSidebarSurface(juce::Rectangle<float> bounds);

    juce::ListenerList<Listener> listeners_;

    // Pitch Correction Section
    juce::Label pitchCorrectionHeader_;
    juce::Label retuneSpeedLabel_;
    juce::Slider retuneSpeedSlider_;
    
    juce::Label vibratoDepthLabel_;
    juce::Slider vibratoDepthSlider_;
    juce::Label vibratoRateLabel_;
    juce::Slider vibratoRateSlider_;
    juce::Label noteSplitLabel_;
    juce::Slider noteSplitSlider_;

    // Tools Section
    juce::Label toolsHeader_;
    std::unique_ptr<ToolIconButton> autoTuneToolButton_;
    std::unique_ptr<ToolIconButton> selectToolButton_;
    std::unique_ptr<ToolIconButton> drawNoteToolButton_;
    std::unique_ptr<ToolIconButton> lineAnchorToolButton_;
    std::unique_ptr<ToolIconButton> handDrawToolButton_;
    // ⚡️ vocal-time-stretch §8.4 — Time tool palette button (toolId=5)
    std::unique_ptr<ToolIconButton> timeToolButton_;
    // OpenDyne 工具按钮（toolId=6 Pitch / 7 VolumeEnvelope / 8 Scissors / 9 PitchModulation / 10 PitchDrift）
    std::unique_ptr<ToolIconButton> pitchToolButton_;
    std::unique_ptr<ToolIconButton> pitchModulationToolButton_;
    std::unique_ptr<ToolIconButton> pitchDriftToolButton_;
    std::unique_ptr<ToolIconButton> volumeEnvelopeToolButton_;
    std::unique_ptr<ToolIconButton> scissorsToolButton_;
    AutoButtonPresentation autoButtonPresentation_;
    std::unique_ptr<juce::TextButton> pitchShiftButton_;
    // OpenDyne Pitch Grid 全局开关（No Snap / Chromatic / Key Scale）
    juce::ComboBox pitchGridSelector_;

    LargeKnobLookAndFeel largeKnobLookAndFeel_;
    juce::Image auroraSidebarSurface_;
    float auroraSidebarSurfaceScale_ = 0.0f;

    bool experimentalFeaturesEnabled_ = false;
    bool openDyneMode_ = false;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};

} // namespace OpenTune
