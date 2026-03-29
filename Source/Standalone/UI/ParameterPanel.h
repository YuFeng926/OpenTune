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
#include "OpenTuneLookAndFeel.h"
#include "UIColors.h"

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
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void retuneSpeedChanged(float speed) = 0;
        virtual void vibratoDepthChanged(float value) = 0;
        virtual void vibratoRateChanged(float value) = 0;
        virtual void noteSplitChanged(float value) = 0;
        virtual void toolSelected(int toolId) = 0;
        // 参数拖动完成回调（用于 Undo 记录，oldValue 是拖动开始前的值）
        virtual void parameterDragEnded(int paramId, float oldValue, float newValue) { juce::ignoreUnused(paramId, oldValue, newValue); }
    };

    ParameterPanel();
    ~ParameterPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    void setActiveTool(int toolId);
    
    // Setters for UI state
    void setRetuneSpeed(float speed);
    void setVibratoDepth(float value);
    void setVibratoRate(float value);
    void setNoteSplit(float value);
    void setF0Min(float value);
    void setF0Max(float value);

    void applyTheme();

    // Getters
    float getRetuneSpeed() const;
    float getVibratoDepth() const;
    float getVibratoRate() const;
    float getNoteSplit() const;
    float getF0Min() const;
    float getF0Max() const;

private:
    class ToolIconButton : public juce::Button
    {
    public:
        ToolIconButton(int toolId, const juce::String& name, const juce::String& tooltip);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        int getToolId() const { return toolId_; }
        void setIcon(const juce::Path& path, bool fill) { iconPath_ = path; fillIcon_ = fill; }
        void setTextIcon(const juce::String& iconText) { textIcon_ = iconText; }

    private:
        int toolId_ = 0;
        juce::Path iconPath_;
        bool fillIcon_ = false;
        juce::String textIcon_;

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

    // F0 Range Section (Hidden for now, but kept in code)
    juce::Slider f0MinSlider_;
    juce::Slider f0MaxSlider_;

    // Tools Section
    juce::Label toolsHeader_;
    std::unique_ptr<ToolIconButton> autoTuneToolButton_;
    std::unique_ptr<ToolIconButton> selectToolButton_;
    std::unique_ptr<ToolIconButton> drawNoteToolButton_;
    std::unique_ptr<ToolIconButton> lineAnchorToolButton_;
    std::unique_ptr<ToolIconButton> handDrawToolButton_;

    LargeKnobLookAndFeel largeKnobLookAndFeel_;

    // 参数拖动前的值（用于 Undo 记录）
    float dragStartRetuneSpeed_{15.0f};
    float dragStartVibratoDepth_{0.0f};
    float dragStartVibratoRate_{7.5f};
    float dragStartNoteSplit_{80.0f};
    
    // 参数 ID 常量
    static constexpr int kParamRetuneSpeed = 0;
    static constexpr int kParamVibratoDepth = 1;
    static constexpr int kParamVibratoRate = 2;
    static constexpr int kParamNoteSplit = 3;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};

} // namespace OpenTune
